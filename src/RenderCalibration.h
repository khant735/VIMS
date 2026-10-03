#pragma once
#include <vector>
#include <cstdint>

struct CalibrationVertex { float x,y,z,nx,ny,nz; };
struct CalibrationMesh {
    std::vector<CalibrationVertex> vertices;
    std::vector<uint32_t> indices;
};
struct CalibrationScene {
    CalibrationMesh largeGear;
    CalibrationMesh smallGear;
    float largeAngle=0.0f;
    float smallAngle=0.0f;
    float cameraAngle=0.0f;
};

CalibrationMesh makeCalibrationGear(int teeth,float rootRadius,float tipRadius,float holeRadius,float thickness);
CalibrationScene makeCalibrationScene();
void animateCalibrationScene(CalibrationScene& scene,float seconds);

enum class CalibrationBackend {
    CpuSoftware,
    Vulkan,
    Direct3D12,
    Direct3D11,
    Direct3D9,
    OpenGL,
    OpenGLES,
    WebGPU,
    WebGL2,
    WebGL1
};
struct CalibrationBackendInfo {
    CalibrationBackend backend;
    const wchar_t* name;
    bool nativeWindows;
    bool browserOrMobile;
};
const std::vector<CalibrationBackendInfo>& calibrationBackends();
