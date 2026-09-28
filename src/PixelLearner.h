#pragma once
#include "Image.h"
#include "MaskOrientation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

// Small trainable pixel head. The ONNX scene/face models still supply the
// parent region. This head learns only from explicitly approved pixel masks.
struct PixelLearner {
    static constexpr int Inputs=12,Hidden=12;
    std::array<float,Inputs*Hidden> first{};
    std::array<float,Hidden> bias{};
    std::array<float,Hidden> last{};
    float outputBias=0;
    static PixelLearner initial(){
        PixelLearner net;std::mt19937 rng(0x51982);std::uniform_real_distribution<float> d(-.16f,.16f);
        for(float& x:net.first)x=d(rng);for(float& x:net.last)x=d(rng);return net;
    }
    struct Features {std::array<float,Inputs> values;};
    static Features features(const ImageRGBA& image,int x,int y,int x0,int y0,int x1,int y1,
        const MaskFrame* parentFrame=nullptr){
        const auto* p=&image.pixels[(size_t(y)*image.width+x)*4];
        const float r=p[0]/255.f,g=p[1]/255.f,b=p[2]/255.f;
        const float saturation=std::max({r,g,b})-std::min({r,g,b});
        const float sum=r+g+b+.015f;
        auto luminance=[&](int xx,int yy){
            const auto* q=&image.pixels[(size_t(yy)*image.width+xx)*4];
            return (q[0]*.299f+q[1]*.587f+q[2]*.114f)/255.f;
        };
        const float here=luminance(x,y);
        const float l=luminance(std::max(0,x-1),y),rr=luminance(std::min(image.width-1,x+1),y);
        const float t=luminance(x,std::max(0,y-1)),bb=luminance(x,std::min(image.height-1,y+1));
        const float neighborhood=(here+l+rr+t+bb)/5.f;
        const auto local=parentFrame?parentFrame->coordinates(x+.5,y+.5):
          std::pair<double,double>{.5,.5};
        return {{{r-.5f,g-.5f,b-.5f,saturation-.5f,
            float(x-x0)/std::max(1,x1-x0)-.5f,float(y-y0)/std::max(1,y1-y0)-.5f,
            r/sum-1.f/3.f,g/sum-1.f/3.f,
            std::clamp(here/(neighborhood+.025f),0.f,2.f)-1.f,
            std::clamp(std::abs(rr-l)+std::abs(bb-t),0.f,1.f)-.5f,
            float(std::clamp(local.first,-.5,1.5)-.5),
            float(std::clamp(local.second,-.5,1.5)-.5)}}};
    }
    float probability(const Features& f) const {
        float z=outputBias;
        for(int h=0;h<Hidden;++h){float a=bias[h];for(int j=0;j<Inputs;++j)a+=first[h*Inputs+j]*f.values[j];z+=last[h]*std::tanh(a);}
        return 1.f/(1.f+std::exp(-std::clamp(z,-20.f,20.f)));
    }
    void update(const Features& f,bool target,float rate){
        std::array<float,Hidden> hidden{};
        float z=outputBias;
        for(int h=0;h<Hidden;++h){float a=bias[h];for(int j=0;j<Inputs;++j)a+=first[h*Inputs+j]*f.values[j];hidden[h]=std::tanh(a);z+=last[h]*hidden[h];}
        const float p=1.f/(1.f+std::exp(-std::clamp(z,-20.f,20.f)));
        const float delta=std::clamp(p-float(target),-1.f,1.f);
        for(int h=0;h<Hidden;++h){const float back=delta*last[h]*(1-hidden[h]*hidden[h]);
            last[h]-=rate*delta*hidden[h];bias[h]-=rate*back;
            for(int j=0;j<Inputs;++j)first[h*Inputs+j]-=rate*back*f.values[j];}
        outputBias-=rate*delta;
    }
};
