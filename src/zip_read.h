// Reading entries out of the game's zips (stored or deflated), without loading the whole zip.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ZipEntry {
    std::string name;   // as in the zip, '/' separated
    uint16_t method = 0;
    uint32_t csize = 0, usize = 0, local = 0;
};

class ZipReader {
public:
    ~ZipReader();
    bool open(const std::wstring& path);   // reads the central directory
    const std::vector<ZipEntry>& entries() const { return entries_; }
    bool read(const ZipEntry& e, std::vector<unsigned char>& out);

private:
    void* file_ = nullptr;
    std::vector<ZipEntry> entries_;
};
