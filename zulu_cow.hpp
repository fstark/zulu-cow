#pragma once

#include "fsfile_mock.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <format>

class ImageBackingStore
{
private:
    FsFile m_fsfile_orig;       // Original/pristine image file
    FsFile m_fsfile_dirty;      // Overlay file with modified sectors
    uint8_t *m_cow_bitmap;      // Bitmap tracking which groups are dirty   (typically 1024 bytes = 8192 groups)
    uint32_t m_bitmap_size;     // Size of bitmap in bytes
    uint32_t m_cow_group_size;  // Size of each group in sectors         (10 for a disk of 81920 sectors -- 40.96 Mb)
    uint32_t m_cow_group_count; // Total number of groups               (Number of bits in the bitmap)
    uint32_t m_scsi_block_size; // SCSI block size in bytes
    uint8_t *m_buffer;          // Pre-allocated buffer for copy operations
    uint32_t m_buffer_size;

    enum eImageType
    {
        IMG_TYPE_ORIG = 0,
        IMG_TYPE_DIRTY = 1
    };

public:
    // Constructor for copy-on-write setup
    ImageBackingStore(const char *orig_filename, const char *dirty_filename,
                      uint32_t bitmap_size = 1024, uint32_t buffer_size = 2048, uint32_t scsi_block_size = 512);

    // For testing
    FsFile &getOriginalFile() { return m_fsfile_orig; }
    FsFile &getDirtyFile() { return m_fsfile_dirty; }
    std::vector<uint8_t> recreate()
    {
        std::vector<uint8_t> data(m_fsfile_orig.size());
        uint32_t group_size_bytes = m_cow_group_size * m_scsi_block_size;

            // Loop over the bitmap, copying orginal or dirty data as needed
        for (uint32_t group = 0; group < m_cow_group_count; ++group)
        {
            uint64_t pos = group * group_size_bytes;

// std::cout << std::format("Group {} at pos {} is {}\n", group, pos, (getGroupImageType(group) == IMG_TYPE_DIRTY) ? "DIRTY" : "ORIG");

            if (getGroupImageType(group) == IMG_TYPE_DIRTY)
            {
                std::copy( m_fsfile_dirty.data().data() + pos,
                           m_fsfile_dirty.data().data() + pos + group_size_bytes,
                           data.data() + pos );
            }
            else
            {
                // std::cout << std::format("  Copying ORIG data from overlay at pos {} size {}\n", pos, group_size_bytes);
                std::copy( m_fsfile_orig.data().data() + pos,
                           m_fsfile_orig.data().data() + pos + group_size_bytes,
                           data.data() + pos );
            }
        }
        return data;
    }

    // Copy-on-write bitmap management
    eImageType getImageType(uint32_t lba);
    void setImageType(uint32_t lba, eImageType type);
    eImageType getGroupImageType(uint32_t group);
    void setGroupImageType(uint32_t group, eImageType type);

    // Copy-on-write I/O operations
    ssize_t cow_read(void *buf, size_t count);
    ssize_t cow_write(const void *buf, size_t count);
    void set_position(uint64_t pos) { m_current_position = pos; }

private:
    // Helper methods
    void performCopyOnWrite(uint32_t group);
    uint64_t position() const { return m_current_position; }

    uint64_t m_current_position = 0; // Track current file position
};
