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
    WebGL1,
    Ps2Gs,
    Ps3Rsx,
    Ps4Gnm,
    Ps5Gnm,
    DreamcastPvr,
    GameCubeGx,
    WiiGx,
    WiiUGx2,
    SwitchNvn,
    SwitchVulkan,
    XboxD3D8,
    Xbox360D3D9,
    XboxOneD3D11,
    XboxSeriesD3D12,
    PspGu,
    PsVitaGxm,
    PsTvGxm,
    NvidiaShieldOpenGLES,
    NvidiaShieldVulkan,
    AppleMetal,
    AppleVulkanMoltenVK
};
struct CalibrationBackendInfo {
    CalibrationBackend backend;
    const wchar_t* name;
    bool nativeWindows;
    bool browserOrMobile;
    bool internetEligible;
    bool theoreticalProbeOnly;
};
const std::vector<CalibrationBackendInfo>& calibrationBackends();

enum class CalibrationResultKind { RenderPass, RenderFail, TheoreticalCompatible, TranslationRequired, TheoreticalIncompatible, Unavailable };
struct CalibrationResult {
    CalibrationBackend backend;
    CalibrationResultKind kind;
    const wchar_t* detail;
};
const wchar_t* calibrationResultName(CalibrationResultKind kind);
std::vector<CalibrationResult> theoreticalCalibrationResults();
