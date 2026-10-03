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
