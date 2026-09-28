#pragma once
#include "MaskTypes.h"
#include <filesystem>
#include <string>
#include <vector>

struct PoseGifResult {
    int frames=0;
    int durationCentiseconds=0;
    bool approximateLimbs=false;
};

// Builds a 30-second, 8 fps transparent GIF from the current image and
// visible exported-mask geometry. Hidden surface projections are excluded.
PoseGifResult exportPoseGif(const std::filesystem::path& path,
    const ImageRGBA& image,const std::vector<NamedMask>& masks,
    const std::string& subjectName);
