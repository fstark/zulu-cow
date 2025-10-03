#include "zulu_cow.hpp"

#include <cassert>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>

// Initializes copy-on-write store: bitmap_size (dirty tracking), buffer_size (I/O chunks), scsi_block_size (sector size)
ImageBackingStore::ImageBackingStore(const char *orig_filename, const char *dirty_filename,
                                     uint32_t bitmap_max_size, uint32_t buffer_size, uint32_t scsi_block_size)
{
    m_scsi_block_size = scsi_block_size;
    m_current_position = 0;
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
    ssize_t written = m_fsfile_dirty.write(&zero, 1); // Create sparse file of correct size
    if (written != 1)
    {
        throw std::runtime_error("Failed to initialize dirty file: write operation failed");
    }

    // Calculate optimal group size based on provided bitmap size
    // We have bitmap_size * 8 bits available in our bitmap
    uint32_t max_groups = bitmap_max_size * 8;

    // Calculate group size - must be multiple of 512 sectors and fit within bitmap
    m_cow_group_size = ((total_sectors + max_groups - 1) / max_groups);
    m_cow_group_size_bytes = m_cow_group_size * m_scsi_block_size;

    // Calculate actual number of groups needed
    m_cow_group_count = (total_sectors + m_cow_group_size - 1) / m_cow_group_size;

    // This should never happen due to our group size calculation
    assert(m_cow_group_count <= max_groups);

    m_bitmap_size = (m_cow_group_count + 7) / 8;

    // Allocate and initialize bitmap using the provided bitmap_size
    m_cow_bitmap = new uint8_t[m_bitmap_size];
    assert(m_cow_bitmap != nullptr); // Check allocation succeeded
    memset(m_cow_bitmap, 0, m_bitmap_size);

    // Allocate temporary buffer for copy operations
    m_buffer = new uint8_t[m_buffer_size];
    assert(m_buffer != nullptr); // Check allocation succeeded

    std::cout << std::format("Image size          {} bytes\n", image_size_bytes);
    std::cout << std::format("m_bitmap_size       #groups = {}, real size = {} (requested: {})\n", m_cow_group_count, m_bitmap_size, bitmap_max_size);
    std::cout << std::format("m_cow_group_size    {} sectors ({} bytes)\n", m_cow_group_size, m_cow_group_size_bytes);
    std::cout << std::format("m_scsi_block_size   {} bytes\n", m_scsi_block_size);
    std::cout << std::format("m_buffer_size       {} bytes\n", m_buffer_size);

    resetStats();
}

// Destructor: cleans up allocated memory
ImageBackingStore::~ImageBackingStore()
{
    dumpstats();
    delete[] m_cow_bitmap;
    delete[] m_buffer;
}

std::string ImageBackingStore::stats() const
{
    double over_read = 0;
    double over_write = 0;
    if (m_bytes_requested_read > 0)
    {
        over_read = 100.0 * (static_cast<double>(m_bytes_read_original + m_bytes_read_dirty) / m_bytes_requested_read - 1);
    }
    if (m_bytes_requested_write > 0)
    {
        over_write = 100.0 * (static_cast<double>(m_bytes_read_original_cow + m_bytes_written_dirty) / m_bytes_requested_write - 1);
    }

    return std::format("Over-read: {:.2f}%, Over-write: {:.2f}%", over_read, over_write);
}

// Dumps detailed I/O statistics
void ImageBackingStore::dumpstats() const
{
    std::cout << std::format("=== I/O Statistics ===\n");
    std::cout << std::format("Bytes requested to read:  {}\n", m_bytes_requested_read);
    std::cout << std::format("Bytes read from dirty:    {}\n", m_bytes_read_dirty);
    std::cout << std::format("Bytes read from original: {}\n", m_bytes_read_original);
    std::cout << std::format("Bytes requested to write: {}\n", m_bytes_requested_write);
    std::cout << std::format("Bytes written to dirty:   {}\n", m_bytes_written_dirty);
    std::cout << std::format("Bytes read from original COW: {}\n", m_bytes_read_original_cow);
    std::cout << std::format("======================\n");

    if (m_bytes_requested_read > 0)
    {
        double over_read = 100.0 * (static_cast<double>(m_bytes_read_original + m_bytes_read_dirty) / m_bytes_requested_read - 1);
        std::cout << std::format(" Over-read  : {:.2f}%\n", over_read);
    }
    if (m_bytes_requested_write > 0)
    {
        double over_write = 100.0 * (static_cast<double>(m_bytes_read_original_cow + m_bytes_written_dirty) / m_bytes_requested_write - 1);
        std::cout << std::format(" Over-write : {:.2f}%\n", over_write);
    }
}

// Returns whether a group is stored in original or dirty file by checking bitmap
ImageBackingStore::eImageType ImageBackingStore::getGroupImageType(uint32_t group)
{
    assert(group < m_cow_group_count);
    return (m_cow_bitmap[group / 8] & (1 << (group % 8))) ? IMG_TYPE_DIRTY : IMG_TYPE_ORIG;
}

// Sets group type in bitmap by setting or clearing the corresponding bit
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

// Reads from a single image type (original or dirty) for given byte range
// Used for implementation the high-level read
ssize_t ImageBackingStore::cow_read_single(uint32_t from, uint32_t count, void *buf)
{
    if (getGroupImageType(groupFromOffset(from)) == IMG_TYPE_DIRTY)
    {
        // Read from overlay/dirty file at same offset as original
        m_bytes_read_dirty += count;
        m_fsfile_dirty.seek(from);
        return m_fsfile_dirty.read(buf, count);
    }
    // Read from original file
    m_bytes_read_original += count;
    m_fsfile_orig.seek(from);
    return m_fsfile_orig.read(buf, count);
}

/*
    Reads across multiple groups, switching between original and dirty files as needed

|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)
                           |                          |                          |                          |      Groups (3 sectors each)
           DIRTY           |          CLEAN           |          CLEAN           |          DIRTY           |      Group state before write
          [---------------------------------------------------------------------------------------]         |      Read 10 blocs, spanning 4 groups
          [  DIRTY READ   ] [                      CLEAN READ                   ] [  DIRTY READ   ]|        |      Underlying chunks from alternating sources
|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)

    Idea is we repeatedly create a "chunk" that extends from the current read position
    to the next transition between original and dirty, or to the end of the read request
*/
ssize_t ImageBackingStore::cow_read(uint32_t from, uint32_t to, void *buf)
{
    ssize_t total_bytes_read = 0;
    uint8_t *buffer_ptr = static_cast<uint8_t *>(buf);
    uint32_t current_offset = from;

    while (current_offset < to)
    {
        // Find the end of the current chunk (either 'to' or where image type changes)
        uint32_t current_group = groupFromOffset(current_offset);
        eImageType current_type = getGroupImageType(current_group);
        uint32_t chunk_end = current_offset;

        // Extend chunk while image type remains the same and we haven't reached 'to'
        while (chunk_end < to && groupFromOffset(chunk_end) < m_cow_group_count &&
               getGroupImageType(groupFromOffset(chunk_end)) == current_type)
        {
            uint32_t next_group_offset = offsetFromGroup(groupFromOffset(chunk_end) + 1);
            chunk_end = std::min(to, next_group_offset);
        }

        // Read this chunk using cow_read_single
        ssize_t bytes_read = cow_read_single(current_offset, chunk_end - current_offset, buffer_ptr);
        if (bytes_read <= 0)
            break;

        total_bytes_read += bytes_read;
        buffer_ptr += bytes_read;
        current_offset = chunk_end;
    }

    return total_bytes_read;
}

// Wrapper for cow_read that uses current file position and updates it
ssize_t ImageBackingStore::cow_read(void *buf, size_t count)
{
    m_bytes_requested_read += count;

    uint32_t from = static_cast<uint32_t>(m_current_position);
    uint32_t to = from + static_cast<uint32_t>(count);

    ssize_t bytes_read = cow_read(from, to, buf);

    // Update current position
    if (bytes_read > 0)
    {
        set_position(m_current_position + bytes_read);
    }

    return bytes_read;
}

//  Helper function for cow_write
//  Copies original data to overlay (dirty) file for a specific byte range
//  Request never spans multiple groups
ssize_t ImageBackingStore::performCopyOnWrite(uint32_t from_offset, uint32_t to_offset)
{
    // Verify both offsets are in the same group
    assert(groupFromOffset(from_offset) == groupFromOffset(to_offset - 1));

    uint32_t bytes_to_copy = to_offset - from_offset;
    uint32_t bytes_copied = 0;

    m_fsfile_orig.seek(from_offset);
    m_fsfile_dirty.seek(from_offset);

    while (bytes_copied < bytes_to_copy)
    {
        uint32_t chunk_size = std::min(m_buffer_size, bytes_to_copy - bytes_copied);

        ssize_t bytes_read = m_fsfile_orig.read(m_buffer, chunk_size);
        if (bytes_read < 0)
        {
            return bytes_read; // Return read error immediately
        }
        if (static_cast<uint32_t>(bytes_read) != chunk_size)
        {
            return -1; // Unexpected partial read
        }
        m_bytes_read_original_cow += chunk_size;

        ssize_t bytes_written = m_fsfile_dirty.write(m_buffer, chunk_size);
        if (bytes_written < 0)
        {
            return bytes_written; // Return write error immediately
        }
        if (static_cast<uint32_t>(bytes_written) != chunk_size)
        {
            return -1; // Unexpected partial write
        }
        m_bytes_written_dirty += chunk_size;

        bytes_copied += chunk_size;
    }

    return bytes_to_copy; // Return total bytes copied
}

/*
    Writes data performing copy-on-write for unmodified portions at begining and end

|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)
                  |                          |                          |                          |      Groups (3 sectors each)
  CLEAN           |          CLEAN           |          CLEAN           |          CLEAN           |      Group state before write
                  |                  [---------------------------------------------------]         |      Write 6 blocs, spanning 3 groups
                  |[ COPY...COPY...] [ WRITE...WRITE...WRITE...WRITE...WRITE...WRITE...  ] [ COPY ]|      Actions taken (1), (2), (3)
  CLEAN           |          DIRTY           |          DIRTY           |          DIRTY           |      Group market dirty after write (4)
|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|--------|----- Sectors (512 bytes each)

    Implementation follows the above pattern:
    - (1) Handle first group: if clean and write doesn't start at group beginning, copy original data
    - (2) Write the main data to dirty file
    - (3) Handle last group: if clean and write doesn't end at group end, copy original data
    - (4) Mark all affected groups as dirty
*/
ssize_t ImageBackingStore::cow_write(uint32_t from, uint32_t to, const void *buf)
{
    size_t count = to - from;

    uint32_t first_group = groupFromOffset(from);
    uint32_t last_group = groupFromOffset(to - 1); // Last byte affected

    // Handle first group - copy-on-write if needed and write doesn't start at group beginning
    if (getGroupImageType(first_group) == IMG_TYPE_ORIG)
    {
        uint32_t group_start = offsetFromGroup(first_group);
        if (from > group_start)
        {
            // Need to preserve data before the write
            ssize_t cow_result = performCopyOnWrite(group_start, from);
            if (cow_result < 0)
            {
                return cow_result; // Return COW error immediately
            }
        }
    }

    // Handle copy in the dirty file
    m_fsfile_dirty.seek(from);
    ssize_t bytes_written = m_fsfile_dirty.write(buf, count);
    if (bytes_written <= 0)
    {
        return bytes_written;
    }

    m_bytes_written_dirty += count;

    // Handle last group - copy-on-write if needed and write doesn't end at group end
    if (getGroupImageType(last_group) == IMG_TYPE_ORIG)
    {
        uint32_t group_end = offsetFromGroup(last_group + 1);
        if (to < group_end)
        {
            // Need to preserve data after the write
            ssize_t cow_result = performCopyOnWrite(to, group_end);
            if (cow_result < 0)
            {
                return cow_result; // Return COW error immediately
            }
        }
    }

    // Mark all affected groups as dirty
    for (uint32_t group = first_group; group <= last_group; ++group)
    {
        setGroupImageType(group, IMG_TYPE_DIRTY);
    }

    return bytes_written;
}

// Wrapper for cow_write that uses current file position and updates it
ssize_t ImageBackingStore::cow_write(const void *buf, size_t count)
{
    m_bytes_requested_write += count;

    uint32_t from = static_cast<uint32_t>(m_current_position);
    uint32_t to = from + static_cast<uint32_t>(count);

    ssize_t bytes_written = cow_write(from, to, buf);

    // Update current position
    if (bytes_written > 0)
    {
        set_position(m_current_position + bytes_written);
    }

    return bytes_written;
}
