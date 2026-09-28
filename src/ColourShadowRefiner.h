#pragma once
#include "Image.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Expands an existing mask a few pixels along locally continuous colour and
// shading. Parent and competing parts are fixed before calling this function.
// Never creates a mask without seed pixels, and never crosses the parent.
inline size_t refineColourShadowBoundary(const ImageRGBA& image,Mask& part,
    const Mask* parent,const std::vector<const Mask*>& competitors,int radius){
    if(image.empty()||part.empty()||part.width!=image.width||part.height!=image.height||
       (parent&&(parent->width!=image.width||parent->height!=image.height)))return 0;
    const int w=image.width,h=image.height;
    int x0=w,y0=h,x1=-1,y1=-1;size_t area=0;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){const size_t i=size_t(y)*w+x;
        if(!part.pixels[i])continue;
        ++area;x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);
    }
    if(area<24||area>size_t(w)*h*95/100)return 0;
    radius=std::clamp(radius,1,3);
    // A mask can include textured material; compare with an adjacent seed,
    // rather than assuming the entire part has one uniform colour.
    // Thin details can gain a full boundary row without gaining many pixels
    // relative to their length. Still cap total growth for broad regions.
    const size_t limit=std::max<size_t>(16,area/8);
    std::vector<uint8_t> working=part.pixels;
    std::vector<size_t> accepted;accepted.reserve(std::min<size_t>(limit,4096));
    auto light=[&](size_t i){const auto* p=&image.pixels[i*4];
        return .299f*p[0]+.587f*p[1]+.114f*p[2];};
    for(int step=0;step<radius;++step){
        std::vector<size_t> additions;
        for(int y=std::max(0,y0-radius);y<=std::min(h-1,y1+radius);++y)
         for(int x=std::max(0,x0-radius);x<=std::min(w-1,x1+radius);++x){
            const size_t i=size_t(y)*w+x;
            if(working[i]||(parent&&!parent->pixels[i])||image.pixels[i*4+3]<128)continue;
            bool occupied=false;
            for(const Mask* other:competitors)if(other&&other->pixels[i]){occupied=true;break;}
            if(occupied)continue;
            const auto* p=&image.pixels[i*4];const float lum=light(i);
            if(lum<10)continue;
            const float sum=float(p[0])+p[1]+p[2]+4.f;
            const size_t adjacent[]={x?i-1:i,x+1<w?i+1:i,y?i-size_t(w):i,y+1<h?i+size_t(w):i};
            bool continuous=false;
            for(size_t j:adjacent){
                if(j==i||!working[j])continue;
                const auto* q=&image.pixels[j*4];
                const float otherSum=float(q[0])+q[1]+q[2]+4.f;
                const float neighborLight=light(j);
                // Chroma is fairly stable across dimming. The luminance
                // bound allows gentle shade transitions but stops hard edges.
                if(std::abs(p[0]/sum-q[0]/otherSum)>.047f||
                   std::abs(p[1]/sum-q[1]/otherSum)>.047f||
                   std::abs(lum-neighborLight)>std::max(30.f,neighborLight*.38f))continue;
                continuous=true;break;
            }
            if(continuous)additions.push_back(i);
         }
        if(accepted.size()+additions.size()>limit)break;
        for(size_t i:additions)working[i]=255;
        accepted.insert(accepted.end(),additions.begin(),additions.end());
        if(additions.empty())break;
    }
    if(!accepted.empty())part.pixels=std::move(working);
    return accepted.size();
}
