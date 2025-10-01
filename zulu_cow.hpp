#pragma once

#include "fsfile_mock.h"

#include <cstdint>
#include <cstring>

class ImageBackingStore
{
private:
    bool m_iscopyonwrite;
    FsFile m_fsfile_orig;       // Original/pristine image file
    FsFile m_fsfile_dirty;      // Overlay file with modified sectors
    uint8_t *m_cow_bitmap;      // Bitmap tracking which groups are dirty   (typically 1024 bytes = 8192 groups)
    uint32_t m_bitmap_size;     // Size of bitmap in bytes
    uint32_t m_cow_group_size;  // Size of each group in sectors         (10 for a disk of 81920 sectors -- 40.96 Mb)
    uint32_t m_cow_group_count; // Total number of groups               (Number of bits in the bitmap)
    uint32_t m_scsi_block_size; // SCSI block size in bytes
    uint8_t *m_temp_buffer;     // Pre-allocated buffer for copy operations

    static const uint32_t kBufSize = 2048; // Buffer size in bytes (2KB)

    enum eImageType
    {
        IMG_TYPE_ORIG = 0,
        IMG_TYPE_DIRTY = 1
    };

public:
    // Constructor for copy-on-write setup
    ImageBackingStore(const char *orig_filename, const char *dirty_filename,
                      uint32_t bitmap_size = 1024, uint32_t scsi_block_size = 512);

    // Copy-on-write bitmap management
    eImageType getImageType(uint32_t lba);
    void setImageType(uint32_t lba, eImageType type);
    eImageType getGroupImageType(uint32_t group);
    void setGroupImageType(uint32_t group, eImageType type);

    // Copy-on-write I/O operations
    ssize_t cow_read(void *buf, size_t count);
    ssize_t cow_write(const void *buf, size_t count);

private:
    // Helper methods
    void performCopyOnWrite(uint32_t group);
    uint64_t position() const { return m_current_position; }
    void set_position(uint64_t pos) { m_current_position = pos; }

    uint64_t m_current_position = 0; // Track current file position
};
