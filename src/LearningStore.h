#pragma once
#include "SegmentationEngine.h"
#include <filesystem>
#include <string>

struct LearningOutcome {
    size_t approvedImages=0;
    bool activated=false;
    float baselineIou=0,learnedIou=0;
    std::string message;
};

LearningOutcome approveLearningSample(const std::filesystem::path& appDir,
    const ImageRGBA& image,const NamedMask& corrected,const Mask& original,const Mask& parent);

// Called after regular model inference; adds only predictions from models that
// passed validation against a separate approved image.
size_t applyLearnedModels(const std::filesystem::path& appDir,const ImageRGBA& image,AnalysisResult& result);
