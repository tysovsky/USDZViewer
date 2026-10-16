// Reader for USD's binary "crate" file format (.usdc).
#pragma once

#include "layer.h"

namespace usd {

// Returns true if `data` starts with the crate magic "PXR-USDC".
bool IsCrateFile(const uint8_t* data, size_t size);

// Parses a crate file into `layer`. Values that fail to decode are skipped and reported in `warnings`.
bool ReadCrate(const uint8_t* data, size_t size, Layer& layer, std::string& err, std::vector<std::string>& warnings);

}  // namespace usd
