// Image decoding through the Windows Imaging Component (PNG, JPEG, BMP, TIFF, GIF, and any installed codecs).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace render {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // 8-bit RGBA, straight alpha, top row first
    bool hasAlpha = false;
    std::string error;
    bool Ok() const { return width > 0 && height > 0; }
};

// Decodes an encoded image file held in memory. Safe to call from worker threads (initializes COM).
DecodedImage DecodeImage(const uint8_t* data, size_t size);

}  // namespace render
