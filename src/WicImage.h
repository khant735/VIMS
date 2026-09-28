#pragma once
#include "Image.h"
#include <filesystem>
ImageRGBA loadImageWic(const std::filesystem::path& path);
void saveMaskPngWic(const std::filesystem::path& path,const Mask& mask);
void saveCutoutPngWic(const std::filesystem::path& path,const ImageRGBA& image,const Mask& mask);
