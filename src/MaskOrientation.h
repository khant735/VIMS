#pragma once
#include "Image.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

// Image-space axes fitted to detected parent pixels. There is no assumption
// that an object keeps the same screen position or upright orientation.
struct MaskFrame {
    double centreX=0,centreY=0,angle=0;
    double cosine=1,sine=0;
    double minU=0,maxU=1,minV=0,maxV=1;
    bool reliable=false;
    std::pair<double,double> coordinates(double x,double y) const {
        const double dx=x-centreX,dy=y-centreY;
        const double u=dx*cosine+dy*sine,v=-dx*sine+dy*cosine;
        return {(u-minU)/std::max(1.0,maxU-minU),(v-minV)/std::max(1.0,maxV-minV)};
    }
};

inline bool verticallyOrientedPart(const std::string& name,const Mask& m){
    const auto slash=name.rfind('/');
    const std::string leaf=slash==std::string::npos?name:name.substr(slash+1);
    for(const char* label:{"Belt","Waist","Mouth","Eyebrow","Glasses"})
        if(leaf.find(label)!=std::string::npos)return false;
    for(const char* label:{"Head","Face","Body","body","Torso","Neck","arm","leg","Limb","Person","Animal","Scarf","Hair"})
        if(leaf.find(label)!=std::string::npos)return true;
    int x0=m.width,y0=m.height,x1=-1,y1=-1;
    for(int y=0;y<m.height;++y)for(int x=0;x<m.width;++x)if(m.pixels[size_t(y)*m.width+x]){
        x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);
    }
    return x1<x0||(y1-y0)>=(x1-x0);
}

inline MaskFrame estimateMaskFrame(const Mask& m,bool vertical){
    MaskFrame frame;if(m.empty())return frame;
    double n=0,sx=0,sy=0,xx=0,xy=0,yy=0;
    int x0=m.width,y0=m.height,x1=-1,y1=-1;
    for(int y=0;y<m.height;++y)for(int x=0;x<m.width;++x)if(m.pixels[size_t(y)*m.width+x]){
        ++n;sx+=x+.5;sy+=y+.5;xx+=(x+.5)*(x+.5);xy+=(x+.5)*(y+.5);yy+=(y+.5)*(y+.5);
        x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);
    }
    if(n<24)return frame;
    frame.centreX=sx/n;frame.centreY=sy/n;
    const double vx=xx/n-frame.centreX*frame.centreX;
    const double vy=yy/n-frame.centreY*frame.centreY;
    const double cross=xy/n-frame.centreX*frame.centreY;
    const double contrast=std::hypot(vx-vy,2*cross)/std::max(1.0,vx+vy);
    const double major=.5*std::atan2(2*cross,vx-vy);
    double angle=vertical?major-1.5707963267948966:major;
    while(angle>1.5707963267948966)angle-=3.1415926535897932;
    while(angle< -1.5707963267948966)angle+=3.1415926535897932;
    // Round or scattered masks do not determine a reliable direction.
    frame.reliable=contrast>.22&&std::abs(angle)<1.35;
    frame.angle=frame.reliable?angle:0;
    frame.cosine=std::cos(frame.angle);frame.sine=std::sin(frame.angle);
    frame.minU=frame.minV=1e30;frame.maxU=frame.maxV=-1e30;
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x)if(m.pixels[size_t(y)*m.width+x]){
        const double dx=x+.5-frame.centreX,dy=y+.5-frame.centreY;
        const double u=dx*frame.cosine+dy*frame.sine,v=-dx*frame.sine+dy*frame.cosine;
        frame.minU=std::min(frame.minU,u);frame.maxU=std::max(frame.maxU,u);
        frame.minV=std::min(frame.minV,v);frame.maxV=std::max(frame.maxV,v);
    }
    return frame;
}
