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
