#pragma once
#include "Image.h"
#include <string>
#include <vector>

enum class MaskProvenance { SemanticModel, SpecialistModel, LandmarkDerived, AlphaMatte, TextureEdgeDerived, OcclusionProjected };
struct MaskComponent {
    std::string instanceId;
    Mask mask;
    size_t pixelArea=0;
    bool thinStructure=false;
};
struct NamedMask {
    std::string name;
    Mask mask;
    std::string category;
    std::string parent;
    std::string source;
    float confidence=1.0f;
    MaskProvenance provenance=MaskProvenance::SemanticModel;
    std::vector<MaskComponent> components;
};
