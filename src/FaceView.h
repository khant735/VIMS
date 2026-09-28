#pragma once
#include "Image.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

struct FaceView {
    double roll=0; // radians, screen clockwise from upright
    double yawHint=0; // signed image-space nose offset, not a 3D pose measurement
    bool leftEye=false,rightEye=false,nose=false,profile=false;
    double eyeX=0,eyeY=0,noseX=0,noseY=0;
    double leftX=0,leftY=0,rightX=0,rightY=0;
    int x0=0,y0=0,width=1,height=1;

    std::pair<double,double> coordinates(double x,double y) const {
        const double dx=x-(x0+width*.5),dy=y-(y0+height*.5);
        const double c=std::cos(roll),s=std::sin(roll);
        return {0.5+(dx*c+dy*s)/width,0.5+(-dx*s+dy*c)/height};
    }
};

inline FaceView estimateFaceView(const Mask& face,const std::vector<uint16_t>& labels){
    FaceView view;
    if(face.empty())return view;
    int x0=face.width,y0=face.height,x1=-1,y1=-1;
    for(int y=0;y<face.height;++y)for(int x=0;x<face.width;++x)
        if(face.pixels[size_t(y)*face.width+x]){
            x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);
        }
    if(x1<x0)return view;
    view.x0=x0;view.y0=y0;view.width=x1-x0+1;view.height=y1-y0+1;
    if(labels.size()!=face.pixels.size())return view;
    struct Point {double x=0,y=0;size_t count=0;};
    Point left,right,nose;
    const int padX=std::max(2,view.width/6),padY=std::max(2,view.height/8);
    for(int y=std::max(0,y0-padY);y<=std::min(face.height-1,y1+padY);++y)
      for(int x=std::max(0,x0-padX);x<=std::min(face.width-1,x1+padX);++x){
        const auto id=labels[size_t(y)*face.width+x];
        Point* p=id==4?&left:id==5?&right:id==2?&nose:nullptr;
        if(p){p->x+=x;p->y+=y;++p->count;}
      }
    auto finish=[](Point& p){if(p.count){p.x/=p.count;p.y/=p.count;}};
    finish(left);finish(right);finish(nose);
    const size_t minEye=std::max<size_t>(5,size_t(view.width)*view.height/6500);
    view.leftEye=left.count>=minEye;view.rightEye=right.count>=minEye;
    if(view.leftEye){view.leftX=left.x;view.leftY=left.y;}
    if(view.rightEye){view.rightX=right.x;view.rightY=right.y;}
    view.nose=nose.count>=std::max<size_t>(7,minEye);
    if(view.nose){view.noseX=nose.x;view.noseY=nose.y;}
    if(view.leftEye&&view.rightEye){
        // The eye line rotates the anatomical fallback windows with the head.
        const double dx=left.x-right.x,dy=left.y-right.y;
        if(std::abs(dx)>view.width*.12 && std::abs(dy)<std::abs(dx)*.65){
            view.roll=std::clamp(std::atan(dy/dx),-.45,.45);
            if(view.nose)view.yawHint=std::clamp((nose.x-(left.x+right.x)*.5)/
                std::max(1.0,std::abs(dx)),-.8,.8);
        }
    }else if((view.leftEye||view.rightEye)&&view.nose){
        const Point& eye=view.leftEye?left:right;
        if(std::abs(eye.x-nose.x)>view.width*.06){
            view.profile=true;view.eyeX=eye.x;view.eyeY=eye.y;
            view.yawHint=std::clamp((nose.x-eye.x)/std::max(1.0,double(view.width)),-.8,.8);
        }
    }
    return view;
}
