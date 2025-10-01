#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <cassert>

/**
 * Mock implementation of FsFile that uses in-memory storage
 * Backed by std::vector<uint8_t> and doesn't support writing out of bounds
 */
class FsFile
{
private:
    std::vector<uint8_t> m_data;
    size_t m_position;

public:
    FsFile() : m_data(8*1024*1024), m_position(0) {}

    /**
     * Open file with specified flags and optional size
     * For mock purposes, creates a buffer with the specified size
     */
    void open(const char *, int)
    {
        m_position = 0;
    }

    /**
     * Read data from the current position
     */
    ssize_t read(void *buf, size_t count)
    {
        size_t bytes_to_read = std::min(count, m_data.size() - m_position);
        if (bytes_to_read == 0)
        {
            return 0; // EOF
        }

        std::memcpy(buf, m_data.data() + m_position, bytes_to_read);
        m_position += bytes_to_read;
        return static_cast<ssize_t>(bytes_to_read);
    }

    /**
     * Write data at the current position
     * Prevents writing beyond the fixed size
     */
    ssize_t write(const void *buf, size_t count)
    {
        // Check if write would go out of bounds
        if (m_position >= m_data.size())
        {
            return 0; // Can't write beyond fixed size
        }

        size_t bytes_to_write = std::min(count, m_data.size() - m_position);
        if (bytes_to_write == 0)
        {
            return 0;
        }

        std::memcpy(m_data.data() + m_position, buf, bytes_to_write);
        m_position += bytes_to_write;
        return static_cast<ssize_t>(bytes_to_write);
    }

    void seek(size_t position)
    {
        m_position = std::min(position, m_data.size());
    }

    size_t position() const
    {
        return m_position;
    }

    size_t size() const
    {
        return m_data.size();
    }

    std::vector<uint8_t> &data()
    {
        return m_data;
    }

    void set_data(const std::vector<uint8_t> &data)
    {
        m_data = data;
        m_position = 0;
    }

    void resize(size_t new_size)
    {
        m_data.resize(new_size, 0);
        m_position = std::min(m_position, new_size);
    }
};
