#pragma once
#include "Image.h"
#include <filesystem>
#include <string>
// Export 8-connected, pixel-exact instances with stable IDs and per-instance PNGs.
void exportCompoundMask(const std::filesystem::path& folder, const std::string& label, const Mask& mask, const std::string& provenance, float confidence);
