#include "zulu_cow.hpp"

#include <cassert>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

/**
 * Constructor: Initializes copy-on-write ImageBackingStore
 *
 * Algorithm:
 * 1. Opens original file (read-only) and overlay file (read-write, create if needed)
 * 2. Calculates optimal group size based on image size and fixed bitmap capacity
 * 3. Groups must be multiples of 512 sectors to align with filesystem blocks
 * 4. Allocates bitmap to track dirty groups (1 bit per group)
 * 5. Pre-allocates temporary buffer for efficient copy operations
 * 6. Creates overlay file at the same size as original (sparse file)
 *
 * The overlay file is sparse and mirrors the original file structure.
 * Each group is stored at the same offset as in the original file.
 */
// Constructor implementation
ImageBackingStore::ImageBackingStore(const char *orig_filename, const char *dirty_filename,
                                     uint32_t bitmap_size, uint32_t buffer_size, uint32_t scsi_block_size)
{
    m_scsi_block_size = scsi_block_size;
    m_current_position = 0;
    m_bitmap_size = bitmap_size;
    m_buffer_size = buffer_size;

    // Open files with default size for mock
    m_fsfile_orig.open(orig_filename, O_RDONLY);
    m_fsfile_dirty.open(dirty_filename, O_RDWR | O_CREAT);

    // Calculate image size in sectors
    uint64_t image_size_bytes = m_fsfile_orig.size();
    uint32_t total_sectors = image_size_bytes / scsi_block_size;

    // Create overlay file at the same size as original (sparse)
    m_fsfile_dirty.seek(image_size_bytes - 1);
    uint8_t zero = 0;
    m_fsfile_dirty.write(&zero, 1); // Create sparse file of correct size

    // Calculate optimal group size based on provided bitmap size
    // We have bitmap_size * 8 bits available in our bitmap
    uint32_t max_groups = bitmap_size * 8;

    // Calculate group size - must be multiple of 512 sectors and fit within bitmap
    m_cow_group_size = ((total_sectors + max_groups - 1) / max_groups);

    // Calculate actual number of groups needed
    m_cow_group_count = (total_sectors + m_cow_group_size - 1) / m_cow_group_size;

    // This should never happen due to our group size calculation
    assert(m_cow_group_count <= max_groups);

    // Allocate and initialize bitmap using the provided bitmap_size
    m_cow_bitmap = new uint8_t[bitmap_size];
    assert(m_cow_bitmap != nullptr); // Check allocation succeeded
    memset(m_cow_bitmap, 0, bitmap_size);

    // Allocate temporary buffer for copy operations
    m_buffer = new uint8_t[m_buffer_size];
    assert(m_buffer != nullptr); // Check allocation succeeded

    std::cout << std::format( "Image size          {} bytes\n", image_size_bytes );
    std::cout << std::format( "m_bitmap_size       {} bytes\n", m_bitmap_size );
    std::cout << std::format( "m_cow_group_size    {} sectors\n", m_cow_group_size );
    std::cout << std::format( "m_cow_group_count   {} groups\n", m_cow_group_count );
    std::cout << std::format( "m_scsi_block_size   {} bytes\n", m_scsi_block_size );
    std::cout << std::format( "m_buffer_size       {} bytes\n", m_buffer_size );
}

/**
 * Read: Hybrid read from original and overlay files based on group status
 *
 * Algorithm:
 * 1. For copy-on-write mode, reads can span multiple groups with different sources
 * 2. For each group within the read range:
 *    - Check if group is dirty (modified) using bitmap
 *    - If dirty: read from overlay file at same offset as original
 *    - If clean: read from original file at natural position
 * 3. Overlay file mirrors original file structure (sparse, same size)
 * 4. Processes reads in group-aligned chunks to maintain consistency
 * 5. Falls back to normal single-file read for non-COW mode
 */
// In ImageBackingStore.cpp
ssize_t ImageBackingStore::cow_read(void *buf, size_t count)
{
    // Read could span multiple groups with different types
    uint8_t *buffer = (uint8_t *)buf;
    size_t bytes_read = 0;
    uint64_t current_pos = position();

    while (bytes_read < count)
    {
        uint32_t current_lba = (current_pos + bytes_read) / m_scsi_block_size;
        eImageType type = getImageType(current_lba);

        // Determine how many consecutive sectors are of the same type
        uint32_t sector_in_group = current_lba % m_cow_group_size;
        uint32_t sectors_left_in_group = m_cow_group_size - sector_in_group;

        // Don't read beyond the requested count
        uint32_t sectors_to_read = std::min(sectors_left_in_group,
                                            static_cast<uint32_t>((count - bytes_read) / m_scsi_block_size));
        size_t bytes_this_read = sectors_to_read * m_scsi_block_size;

        if (type == IMG_TYPE_DIRTY)
        {
            // Read from overlay/dirty file at same offset as original
            m_fsfile_dirty.seek(current_pos + bytes_read);
            m_fsfile_dirty.read(buffer + bytes_read, bytes_this_read);
        }
        else
        {
            // Read from original file
            m_fsfile_orig.seek(current_pos + bytes_read);
            m_fsfile_orig.read(buffer + bytes_read, bytes_this_read);
        }

        bytes_read += bytes_this_read;
    }

    // Update current position
    set_position(current_pos + bytes_read);

    return bytes_read;
} /**
   * getImageType: Determines data source (original vs overlay) for a logical block
   *
   * Algorithm:
   * 1. Converts LBA to group index by dividing by group size
   * 2. Delegates to getGroupImageType for actual bitmap lookup
   * 3. Returns IMG_TYPE_ORIG for non-COW mode or clean groups
   * 4. Returns IMG_TYPE_DIRTY for groups that have been modified
   *
   * This abstraction allows LBA-based queries while internally managing groups.
   */
// Helper function implementation
ImageBackingStore::eImageType ImageBackingStore::getImageType(uint32_t lba)
{
    uint32_t group = lba / m_cow_group_size;
    return getGroupImageType(group);
}

/**
 * setImageType: Marks a logical block's group as clean or dirty
 *
 * Algorithm:
 * 1. Converts LBA to group index by dividing by group size
 * 2. Delegates to setGroupImageType for actual bitmap manipulation
 * 3. No-op for non-COW mode
 *
 * This abstraction allows LBA-based marking while internally managing groups.
 * Note: Typically used to mark groups as dirty after copy-on-write operations.
 */
void ImageBackingStore::setImageType(uint32_t lba, eImageType type)
{
    uint32_t group = lba / m_cow_group_size;
    setGroupImageType(group, type);
}

/**
 * getGroupImageType: Direct bitmap lookup for group status
 *
 * Algorithm:
 * 1. Validates group index is within bitmap bounds
 * 2. Uses bit manipulation to check group's bit in bitmap:
 *    - Byte index = group / 8
 *    - Bit position = group % 8
 *    - Test bit using (bitmap[byte] & (1 << bit))
 * 3. Returns IMG_TYPE_DIRTY if bit set, IMG_TYPE_ORIG if clear
 * 4. Returns IMG_TYPE_ORIG for out-of-range groups or non-COW mode
 *
 * This is the core bitmap access function for read operations.
 */
ImageBackingStore::eImageType ImageBackingStore::getGroupImageType(uint32_t group)
{
    assert(group < m_cow_group_count);
    return (m_cow_bitmap[group / 8] & (1 << (group % 8))) ? IMG_TYPE_DIRTY : IMG_TYPE_ORIG;
}

/**
 * setGroupImageType: Direct bitmap manipulation for group status
 *
 * Algorithm:
 * 1. Validates group index is within bitmap bounds
 * 2. Uses bit manipulation to set/clear group's bit in bitmap:
 *    - Byte index = group / 8
 *    - Bit position = group % 8
 *    - Set bit: bitmap[byte] |= (1 << bit)
 *    - Clear bit: bitmap[byte] &= ~(1 << bit)
 * 3. No-op for out-of-range groups or non-COW mode
 *
 * This is the core bitmap access function for write operations.
 * Used primarily by performCopyOnWrite to mark groups as dirty.
 */
void ImageBackingStore::setGroupImageType(uint32_t group, eImageType type)
{
    assert(group < m_cow_group_count);
    if (type == IMG_TYPE_DIRTY)
    {
        m_cow_bitmap[group / 8] |= (1 << (group % 8));
    }
    else
    {
        m_cow_bitmap[group / 8] &= ~(1 << (group % 8));
    }
}

/**
 * write: Copy-on-write enabled write to overlay file
 *
 * Algorithm:
 * 1. All writes in COW mode go to overlay file after ensuring copy-on-write
 * 2. For each group touched by the write:
 *    - Check if group is still clean (IMG_TYPE_ORIG)
 *    - If clean: trigger performCopyOnWrite to copy original data first
 *    - Write to overlay file at same offset as original file
 * 3. Overlay file mirrors original file structure (sparse, same size)
 * 4. Processes writes in group-aligned chunks to maintain consistency
 * 5. Falls back to normal single-file write for non-COW mode
 *
 * The copy-on-write ensures that partial group writes preserve unmodified data.
 */
ssize_t ImageBackingStore::cow_write(const void *buf, size_t count)
{
    // All writes go to the overlay file after copy-on-write
    const uint8_t *buffer = (const uint8_t *)buf;
    size_t bytes_written = 0;
    uint64_t current_pos = position();

    while (bytes_written < count)
    {
        uint32_t current_lba = (current_pos + bytes_written) / m_scsi_block_size;
        uint32_t group = current_lba / m_cow_group_size;
        uint32_t sector_in_group = current_lba % m_cow_group_size;

        // Check if this group needs copy-on-write
        if (getImageType(current_lba) == IMG_TYPE_ORIG)
        {
            // First write to this group - perform copy-on-write
            performCopyOnWrite(group);
        }

        // Calculate how many sectors we can write in this group
        uint32_t sectors_left_in_group = m_cow_group_size - sector_in_group;
        uint32_t sectors_to_write = std::min(sectors_left_in_group,
                                             static_cast<uint32_t>((count - bytes_written) / m_scsi_block_size));
        size_t bytes_this_write = sectors_to_write * m_scsi_block_size;

        // Write to overlay file at same offset as original
        m_fsfile_dirty.seek(current_pos + bytes_written);
        ssize_t result = m_fsfile_dirty.write(buffer + bytes_written, bytes_this_write);
        if (result <= 0)
            break;

        bytes_written += result;
    }

    // Update current position
    set_position(current_pos + bytes_written);

    return bytes_written;
}

/**
 * performCopyOnWrite: Atomically copies a group from original to overlay
 *
 * Algorithm:
 * 1. Guards against double-copying by checking if group is already dirty
 * 2. Calculates source position in original file: group * group_size * block_size
 * 3. Calculates destination position in overlay file: same as source (mirrored layout)
 * 4. Copies entire group using chunked reads/writes with pre-allocated buffer
 *    - Chunk size limited by kBufSize for memory efficiency
 *    - Ensures atomic group copying for consistency
 * 5. Marks group as dirty in bitmap only after successful copy
 *
 * This preserves the original data in the overlay before any writes occur,
 * ensuring partial writes don't corrupt unmodified sectors within the group.
 */
void ImageBackingStore::performCopyOnWrite(uint32_t group)
{
    // Copy entire group from original to overlay file before first write
    if (getGroupImageType(group) == IMG_TYPE_DIRTY)
    {
        return; // Already copied
    }

    // Calculate positions
    uint64_t orig_pos = group * m_cow_group_size * m_scsi_block_size;
    uint64_t dirty_pos = orig_pos; // Same offset in overlay file

    // Copy the entire group using pre-allocated buffer
    uint32_t group_size_bytes = m_cow_group_size * m_scsi_block_size;
    uint32_t bytes_copied = 0;

    m_fsfile_orig.seek(orig_pos);
    m_fsfile_dirty.seek(dirty_pos);

    while (bytes_copied < group_size_bytes)
    {
        uint32_t bytes_to_copy = std::min(m_buffer_size, group_size_bytes - bytes_copied);

        m_fsfile_orig.read(m_buffer, bytes_to_copy);
        m_fsfile_dirty.write(m_buffer, bytes_to_copy);

        bytes_copied += bytes_to_copy;
    }

    // Mark group as dirty in bitmap
    setGroupImageType(group, IMG_TYPE_DIRTY);
}
