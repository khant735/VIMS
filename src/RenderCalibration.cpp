#include "RenderCalibration.h"
#include <cmath>
#include <algorithm>

namespace {
constexpr float PI=3.14159265358979323846f;
void quad(CalibrationMesh& m, CalibrationVertex a, CalibrationVertex b, CalibrationVertex c, CalibrationVertex d){
    uint32_t n=(uint32_t)m.vertices.size();m.vertices.insert(m.vertices.end(),{a,b,c,d});
    m.indices.insert(m.indices.end(),{n,n+1,n+2,n,n+2,n+3});
}
}

CalibrationMesh makeCalibrationGear(int teeth,float root,float tip,float hole,float thick){
    CalibrationMesh m; teeth=std::max(teeth,6); const int seg=teeth*4; const float hz=thick*.5f;
    auto radius=[&](int i){int q=i&3;return (q==1||q==2)?tip:root;};
    for(int i=0;i<seg;++i){
        int j=(i+1)%seg;float a=2*PI*i/seg,b=2*PI*j/seg;float ra=radius(i),rb=radius(j);
        float ca=std::cos(a),sa=std::sin(a),cb=std::cos(b),sb=std::sin(b);
        CalibrationVertex ofa{ra*ca,ra*sa,hz,0,0,1},ofb{rb*cb,rb*sb,hz,0,0,1};
        CalibrationVertex iha{hole*ca,hole*sa,hz,0,0,1},ihb{hole*cb,hole*sb,hz,0,0,1};
        quad(m,iha,ofa,ofb,ihb);
        CalibrationVertex oba{ra*ca,ra*sa,-hz,0,0,-1},obb{rb*cb,rb*sb,-hz,0,0,-1};
        CalibrationVertex iba{hole*ca,hole*sa,-hz,0,0,-1},ibb{hole*cb,hole*sb,-hz,0,0,-1};
        quad(m,ibb,obb,oba,iba);
        float mx=(ca+cb)*.5f,my=(sa+sb)*.5f,ml=std::sqrt(mx*mx+my*my);if(ml<.001f)ml=1;
        CalibrationVertex oa{ra*ca,ra*sa,-hz,mx/ml,my/ml,0},ob{rb*cb,rb*sb,-hz,mx/ml,my/ml,0};
        CalibrationVertex ota{ra*ca,ra*sa,hz,mx/ml,my/ml,0},otb{rb*cb,rb*sb,hz,mx/ml,my/ml,0};
        quad(m,oa,ob,otb,ota);
        CalibrationVertex ha{hole*ca,hole*sa,-hz,-ca,-sa,0},hb{hole*cb,hole*sb,-hz,-cb,-sb,0};
        CalibrationVertex hta{hole*ca,hole*sa,hz,-ca,-sa,0},htb{hole*cb,hole*sb,hz,-cb,-sb,0};
        quad(m,hb,ha,hta,htb);
    }
    return m;
}

CalibrationScene makeCalibrationScene(){
    CalibrationScene s;
    s.largeGear=makeCalibrationGear(14,0.82f,1.0f,0.27f,0.30f);
    s.smallGear=makeCalibrationGear(10,0.58f,0.72f,0.20f,0.30f);
    return s;
}
void animateCalibrationScene(CalibrationScene& s,float t){
    s.largeAngle=t*1.5f;
    s.smallAngle=-t*1.5f*(14.0f/10.0f)+0.15707963f;
    s.cameraAngle=t*0.55f;
}

const std::vector<CalibrationBackendInfo>& calibrationBackends(){
    static const std::vector<CalibrationBackendInfo> b={
        {CalibrationBackend::CpuSoftware,L"CPU Software",true,false,true,false},
        {CalibrationBackend::Vulkan,L"Vulkan",true,false,true,false},
        {CalibrationBackend::Direct3D12,L"Direct3D 12",true,false,true,false},
        {CalibrationBackend::Direct3D11,L"Direct3D 11",true,false,true,false},
        {CalibrationBackend::Direct3D9,L"Direct3D 9",true,false,true,false},
        {CalibrationBackend::OpenGL,L"OpenGL",true,false,true,false},
        {CalibrationBackend::OpenGLES,L"OpenGL ES",false,true,true,true},
        {CalibrationBackend::WebGPU,L"WebGPU",false,true,true,true},
        {CalibrationBackend::WebGL2,L"WebGL 2",false,true,true,true},
        {CalibrationBackend::WebGL1,L"WebGL 1",false,true,true,true},
        {CalibrationBackend::Ps2Gs,L"PlayStation 2 / PSX - GS",false,true,true,true},
        {CalibrationBackend::Ps3Rsx,L"PlayStation 3 - RSX",false,true,true,true},
        {CalibrationBackend::Ps4Gnm,L"PlayStation 4 - GNM/GNMX",false,true,true,true},
        {CalibrationBackend::Ps5Gnm,L"PlayStation 5 - GNM",false,true,true,true},
        {CalibrationBackend::DreamcastPvr,L"Dreamcast - PowerVR/PVR",false,true,true,true},
        {CalibrationBackend::GameCubeGx,L"GameCube - GX",false,true,true,true},
        {CalibrationBackend::WiiGx,L"Wii - GX",false,true,true,true},
        {CalibrationBackend::WiiUGx2,L"Wii U - GX2",false,true,true,true},
        {CalibrationBackend::SwitchNvn,L"Nintendo Switch - NVN",false,true,true,true},
        {CalibrationBackend::SwitchVulkan,L"Nintendo Switch - Vulkan",false,true,true,true},
        {CalibrationBackend::XboxD3D8,L"Xbox - Direct3D 8 family",false,true,true,true},
        {CalibrationBackend::Xbox360D3D9,L"Xbox 360 - Direct3D 9 family",false,true,true,true},
        {CalibrationBackend::XboxOneD3D11,L"Xbox One - Direct3D 11 family",false,true,true,true},
        {CalibrationBackend::XboxSeriesD3D12,L"Xbox Series - Direct3D 12 family",false,true,true,true},
        {CalibrationBackend::PspGu,L"PSP - GU",false,true,true,true},
        {CalibrationBackend::PsVitaGxm,L"PlayStation Vita - GXM",false,true,true,true},
        {CalibrationBackend::PsTvGxm,L"PlayStation TV - GXM",false,true,true,true},
        {CalibrationBackend::NvidiaShieldOpenGLES,L"NVIDIA SHIELD - OpenGL ES",false,true,true,true},
        {CalibrationBackend::NvidiaShieldVulkan,L"NVIDIA SHIELD - Vulkan",false,true,true,true},
        {CalibrationBackend::AppleMetal,L"Apple - Metal",false,true,true,true},
        {CalibrationBackend::AppleVulkanMoltenVK,L"Apple - Vulkan via MoltenVK/Metal",false,true,true,true}
    };
    return b;
}
