#include <iostream>
#include <format>
#include <vector>
#include <cstdint>
#include <random>
#include <iomanip>
#include "fsfile_mock.h"

// Include the implementation
#include "zulu_cow.hpp"

FsFile fs;

std::mt19937 gen(1);

// Function to dump vector data in hex format, 32 bytes per line
void dump(const std::vector<uint8_t> &data)
{
    const size_t bytesPerLine = 32;

    for (size_t i = 0; i < data.size(); i += bytesPerLine)
    {
        // Print offset
        std::cout << std::hex << std::setw(8) << std::setfill('0') << i << ": ";

        // Print hex bytes
        for (size_t j = 0; j < bytesPerLine && (i + j) < data.size(); ++j)
        {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(data[i + j]) << " ";
        }

        std::cout << std::endl;
    }
    std::cout << std::dec; // Reset to decimal output
}

// Function to diff two vectors and dump differences in 32-byte aligned chunks
void diff(const std::vector<uint8_t> &data1, const std::vector<uint8_t> &data2)
{
    if (data1.size() != data2.size())
    {
        std::cout << "Vector sizes differ: " << data1.size() << " vs " << data2.size() << std::endl;
        return;
    }

    const size_t bytesPerLine = 32;
    bool foundDifferences = false;

    // Track which 32-byte chunks have differences
    for (size_t i = 0; i < data1.size(); i += bytesPerLine)
    {
        bool chunkHasDiff = false;

        // Check if this 32-byte chunk has any differences
        for (size_t j = 0; j < bytesPerLine && (i + j) < data1.size(); ++j)
        {
            if (data1[i + j] != data2[i + j])
            {
                chunkHasDiff = true;
                break;
            }
        }

        // If chunk has differences, dump both versions
        if (chunkHasDiff)
        {
            foundDifferences = true;

            // Print first vector
            std::cout << "A " << std::hex << std::setw(8) << std::setfill('0') << i << ": ";
            for (size_t j = 0; j < bytesPerLine && (i + j) < data1.size(); ++j)
            {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned int>(data1[i + j]) << " ";
            }
            std::cout << std::endl;

            // Print second vector
            std::cout << "B " << std::hex << std::setw(8) << std::setfill('0') << i << ": ";
            for (size_t j = 0; j < bytesPerLine && (i + j) < data1.size(); ++j)
            {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned int>(data2[i + j]) << " ";
            }
            std::cout << std::endl;

            // Print difference markers
            std::cout << "  " << std::hex << std::setw(8) << std::setfill('0') << i << ": ";
            for (size_t j = 0; j < bytesPerLine && (i + j) < data1.size(); ++j)
            {
                if (data1[i + j] != data2[i + j])
                    std::cout << "^^ ";
                else
                    std::cout << "   ";
            }
            std::cout << std::endl
                      << std::endl;
        }
    }

    if (!foundDifferences)
    {
        std::cout << "No differences found." << std::endl;
    }

    std::cout << std::dec; // Reset to decimal output
}

/* Compare the contents of fs and bs */
void check_integrity(ImageBackingStore &bs)
{
    if (fs.data() == bs.recreate())
        return;

    std::cout << std::format("\n\n\nfs size: {}, bs size: {}\n", fs.size(), bs.getOriginalFile().size());

    diff(fs.data(), bs.recreate());

    exit(1);
}

// Function to return a random integer in range [low, high]
uint32_t rand_int(uint32_t low, uint32_t high)
{
    std::uniform_int_distribution<uint32_t> dis(low, high);
    return dis(gen);
}

// Function to return a random sector count between 1 and 64
uint32_t rand_sector()
{
    auto max_sectors = std::min(64u, static_cast<uint32_t>(fs.size() / 512));
    return rand_int(1, max_sectors);
}

// Function to fill a vector with pseudo random uint8_t values using C++ random
void fillWithPseudoRandom(std::vector<uint8_t> &vec)
{
    std::uniform_int_distribution<uint16_t> dis(0, 255);

    for (int i = 0; i < vec.size(); i += 512)
    {
        uint8_t v = static_cast<uint8_t>(dis(gen));
        for (int j = 0; j < 512 && (i + j) < vec.size(); ++j)
        {
            vec[i + j] = v;
        }
    }
}

std::tuple<u_int32_t, u_int32_t> rand_start_and_size()
{
    // We pick a random number of sectors to write
    const uint32_t num_sectors = rand_sector();
    const uint32_t start_sector = rand_int(0, fs.size() / 512 - num_sectors);
    return {start_sector * 512, num_sectors * 512};
}

void one_write(ImageBackingStore &bs)
{
    auto [start_byte, size] = rand_start_and_size();

    std::cout << std::format("Write at {} size {} ", start_byte, size);

    std::vector<uint8_t> buffer(size);
    fillWithPseudoRandom(buffer);
    // Write to both fs and bs
    fs.seek(start_byte);
    fs.write(buffer.data(), size);
    bs.set_position(start_byte);
    bs.cow_write(buffer.data(), size);
}

void one_read(ImageBackingStore &bs)
{
    auto [start_byte, size] = rand_start_and_size();

    std::cout << std::format("Read  at {} size {}  ", start_byte, size);

    std::vector<uint8_t> buffer1(size);
    std::vector<uint8_t> buffer2(size);
    // Write to both fs and bs
    fs.seek(start_byte);
    fs.read(buffer1.data(), size);
    bs.set_position(start_byte);
    bs.cow_read(buffer2.data(), size);

    if (buffer1 != buffer2)
    {
        std::cout << "Read for read " << start_byte << " size " << size << std::endl;
        diff(buffer1, buffer2);
        exit(1);
    }
}

int main()
{
    ImageBackingStore bs("", "");

    //  Start with identical data in both fs and bs original file
    gen.seed(1);
    fillWithPseudoRandom(fs.data());
    gen.seed(1);
    fillWithPseudoRandom(bs.getOriginalFile().data());

    check_integrity(bs);

    for (int i = 0; i < 1000; i++)
    {
        std::cout << std::format("{}: ", i);
        one_write(bs);
        // check_integrity(bs);
        one_read(bs);
        std::cout << std::format("{}\n", bs.stats());
    }

    return 0;
}
