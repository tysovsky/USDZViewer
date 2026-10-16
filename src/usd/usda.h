// Reader for USD's text file format (.usda).
#pragma once

#include "layer.h"

namespace usd {

bool IsUsdaFile(const uint8_t* data, size_t size);

bool ReadUsda(const char* text, size_t size, Layer& layer, std::string& err, std::vector<std::string>& warnings);

}  // namespace usd
