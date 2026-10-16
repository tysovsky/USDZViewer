// Read-only zip archive access for .usdz packages.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace usd {

class Archive {
public:
    struct Entry {
        std::string name;
        uint64_t dataOffset = 0;
        uint64_t compressedSize = 0;
        uint64_t size = 0;
        uint16_t method = 0;  // 0 = stored (the only method the usdz spec allows), 8 = deflate
    };

    // Parses the zip directory of `bytes`. `displayPath` is the UTF-8 path of the file on disk.
    static std::shared_ptr<Archive> Load(std::vector<uint8_t> bytes, std::string displayPath, std::string& err);

    const std::vector<Entry>& Entries() const { return entries_; }
    // Looks up a normalized package-relative path (case-insensitive fallback).
    const Entry* Find(std::string_view path) const;
    bool Extract(const Entry& e, std::vector<uint8_t>& out, std::string& err) const;
    const std::string& DisplayPath() const { return displayPath_; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
    std::string displayPath_;
};

// Raw deflate decoder (RFC 1951). Returns false on malformed input.
bool Inflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out, size_t expectedSize);

// Removes "./" and resolves ".." in a '/'-separated relative path; converts '\\' to '/'.
std::string NormalizeRelativePath(std::string_view p);

}  // namespace usd
