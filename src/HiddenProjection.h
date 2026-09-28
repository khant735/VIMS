#pragma once
#include "MaskOrientation.h"
#include <algorithm>
#include <cstddef>

// Project a plausible footprint for an occluded surface into image space.
// The pixels describe a possible 2D location only; they contain no observed
// colour, depth, back texture, or evidence that the hidden surface is visible.
enum class ProjectionBand { Centre, ImageLeft, ImageRight };
inline Mask projectHiddenFootprint(const Mask& anchor,const Mask& subject,bool vertical,
    ProjectionBand band=ProjectionBand::Centre){
    Mask result{anchor.width,anchor.height,std::vector<uint8_t>(anchor.pixels.size())};
    if(anchor.empty()||subject.empty()||anchor.width!=subject.width||anchor.height!=subject.height)return result;
    const MaskFrame frame=estimateMaskFrame(anchor,vertical);
    size_t seedCount=0,projected=0;
    for(int y=0;y<anchor.height;++y)for(int x=0;x<anchor.width;++x){
        const size_t i=size_t(y)*anchor.width+x;
        if(!anchor.pixels[i]||!subject.pixels[i])continue;
        ++seedCount;
        const auto [u,v]=frame.coordinates(x+.5,y+.5);
        // Keep the candidate inside the measured silhouette. This inset
        // intentionally avoids claiming a precise unseen outline.
        const bool inside=band==ProjectionBand::Centre?u>=.09&&u<=.91:
            band==ProjectionBand::ImageLeft?u>=.07&&u<=.34:u>=.66&&u<=.93;
        if(inside&&v>=.09&&v<=.91){result.pixels[i]=255;++projected;}
    }
    if(seedCount<48||projected<24||projected>seedCount*9/10)
        std::fill(result.pixels.begin(),result.pixels.end(),0);
    return result;
}
