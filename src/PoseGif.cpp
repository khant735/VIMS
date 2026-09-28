#include "PoseGif.h"
#include "MaskOrientation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace {
constexpr double pi=3.14159265358979323846;
void word(std::ofstream& file,int v){file.put(char(v&255));file.put(char((v>>8)&255));}
class GifWriter {
    std::ofstream file_;
    int width_,height_;
public:
    GifWriter(const std::filesystem::path& path,int w,int h):file_(path,std::ios::binary),width_(w),height_(h){
        if(!file_)throw std::runtime_error("Cannot create animated GIF");
        file_.write("GIF89a",6);word(file_,w);word(file_,h);
        file_.put(char(0xF7));file_.put(char(255));file_.put(0);
        for(int i=0;i<256;++i){
            file_.put(char(i==254?255:((i>>5)&7)*255/7));
            file_.put(char(i==254?255:((i>>2)&7)*255/7));
            file_.put(char(i==254?255:(i&3)*255/3));
        }
        // Repeat the 30-second sequence when the GIF is viewed in a browser.
        const unsigned char loop[]={0x21,0xFF,11,'N','E','T','S','C','A','P','E','2','.','0',3,1,0,0,0};
        file_.write(reinterpret_cast<const char*>(loop),sizeof(loop));
    }
    void frame(const std::vector<uint8_t>& pixels,int delay){
        if(pixels.size()!=size_t(width_)*height_)throw std::runtime_error("Incorrect GIF frame dimensions");
        file_.put(0x21);file_.put(char(0xF9));file_.put(4);file_.put(9);
        word(file_,delay);file_.put(char(255));file_.put(0);
        file_.put(0x2C);word(file_,0);word(file_,0);word(file_,width_);word(file_,height_);file_.put(0);
        file_.put(8); // GIF LZW minimum code size for the global palette.
        std::vector<uint8_t> packed;packed.reserve(pixels.size()/3+256);
        uint32_t bits=0;int count=0;
        auto code=[&](int value){bits|=uint32_t(value)<<count;count+=9;
            while(count>=8){packed.push_back(uint8_t(bits));bits>>=8;count-=8;}};
        constexpr int clear=256,end=257;
        std::unordered_map<uint32_t,int> dictionary;dictionary.reserve(256);
        int next=258;code(clear);int prefix=pixels.front();
        for(size_t i=1;i<pixels.size();++i){
            const int symbol=pixels[i];const uint32_t key=(uint32_t(prefix)<<8)|uint32_t(symbol);
            auto found=dictionary.find(key);
            if(found!=dictionary.end()){prefix=found->second;continue;}
            code(prefix);
            // Clear before crossing the 9-bit code limit. This bounded LZW
            // dictionary compresses flat masks while keeping decoding simple.
            if(next>=500){code(clear);dictionary.clear();next=258;}
            else dictionary.emplace(key,next++);
            prefix=symbol;
        }
        code(prefix);code(end);
        if(count)packed.push_back(uint8_t(bits));
        for(size_t offset=0;offset<packed.size();offset+=255){
            const auto n=std::min<size_t>(255,packed.size()-offset);
            file_.put(char(n));file_.write(reinterpret_cast<const char*>(packed.data()+offset),n);
        }
        file_.put(0);
        if(!file_)throw std::runtime_error("Could not write GIF frame");
    }
    void finish(){file_.put(0x3B);file_.close();if(!file_)throw std::runtime_error("Could not finish GIF");}
};

struct Layer {
    const Mask* mask=nullptr;
    double pivotX=0,pivotY=0,centerX=0,centerY=0,restAngle=0;
    enum Kind{Leg,Head,Arm} kind=Arm;
};
bool extent(const Mask& mask,int& x0,int& y0,int& x1,int& y1){
    x0=mask.width;y0=mask.height;x1=-1;y1=-1;
    for(int y=0;y<mask.height;++y)for(int x=0;x<mask.width;++x)
     if(mask.pixels[size_t(y)*mask.width+x]){
      x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);
     }
    return x1>=x0;
}
double smooth(double a,double b,double t){t=std::clamp((t-a)/(b-a),0.0,1.0);return t*t*(3-2*t);}
double poseWeight(double t,int pose){
    // Six distinct holds connected by short eased transitions.
    constexpr double keys[]={0,4,8,12,16,20,24,28,30};
    if(t<=keys[pose])return pose==0?1:0;
    if(t>=keys[pose+2])return 0;
    return t<keys[pose+1]?smooth(keys[pose],keys[pose+1],t):
        1-smooth(keys[pose+1],keys[pose+2],t);
}
double wrap(double a){while(a>pi)a-=2*pi;while(a< -pi)a+=2*pi;return a;}
double bicubic(double v){v=std::abs(v);if(v<1)return 1.5*v*v*v-2.5*v*v+1;if(v<2)return -.5*v*v*v+2.5*v*v-4*v+2;return 0;}
uint8_t colour(const uint8_t* rgba){
    int index=(int(rgba[0])&224)|(int(rgba[1])>>5)*4|(int(rgba[2])>>6);
    return uint8_t(std::min(254,index)); // 255 is reserved for transparency.
}
}

PoseGifResult exportPoseGif(const std::filesystem::path& path,const ImageRGBA& image,
    const std::vector<NamedMask>& masks,const std::string& subjectName){
    if(image.empty())throw std::runtime_error("Load an image before exporting a GIF");
    auto find=[&](const std::string& name)->const Mask*{
        for(const auto& item:masks)if(item.name==name&&item.provenance!=MaskProvenance::OcclusionProjected&&
            item.mask.width==image.width&&item.mask.height==image.height)return &item.mask;
        return nullptr;
    };
    const Mask* subject=find(subjectName);
    if(!subject)throw std::runtime_error("No visible subject mask for pose GIF");
    int x0,y0,x1,y1;if(!extent(*subject,x0,y0,x1,y1))throw std::runtime_error("Subject mask has no pixels");
    constexpr int width=1280,height=720,frameCount=240;
    const double centerX=(x0+x1)*.5,centerY=(y0+y1)*.5;
    const double scale=std::min(1080.0/(x1-x0+1),620.0/(y1-y0+1));
    const bool human=subjectName.rfind("People/Human/Person ",0)==0;
    const std::string body=subjectName+"/Body";
    std::vector<Mask> inferred;inferred.reserve(6);
    std::vector<Layer> legs,head,arms;
    auto add=[&](const Mask* mask,Layer::Kind kind){
        if(!mask||mask->width!=image.width||mask->height!=image.height)return;
        int l,t,r,b;if(!extent(*mask,l,t,r,b))return;
        Layer part;part.mask=mask;part.kind=kind;part.centerX=(l+r)*.5;part.centerY=(t+b)*.5;
        const double px=kind==Layer::Head?centerX:
            centerX+(part.centerX>=centerX?1:-1)*(x1-x0)*.13;
        const double py=kind==Layer::Head?b:kind==Layer::Arm?
            y0+(y1-y0)*.33:y0+(y1-y0)*.62;
        double distance=1e30;
        for(int y=t;y<=b;++y)for(int x=l;x<=r;++x)if(mask->pixels[size_t(y)*image.width+x]){
            const double d=(x-px)*(x-px)+(y-py)*(y-py);
            if(d<distance){distance=d;part.pivotX=x;part.pivotY=y;}
        }
        part.restAngle=std::atan2(part.centerY-part.pivotY,part.centerX-part.pivotX);
        if(kind==Layer::Arm){
            // Align the arm's long axis to the pose target. The centroid can
            // be offset by sleeves or hands and make a T pose diagonal.
            const bool longVertical=(b-t)>=(r-l);
            const MaskFrame axis=estimateMaskFrame(*mask,longVertical);
            if(axis.reliable){
                part.restAngle=longVertical?pi/2+axis.angle:axis.angle;
                if(longVertical&&part.centerY<part.pivotY)part.restAngle-=pi;
                if(!longVertical&&part.centerX<part.pivotX)part.restAngle+=pi;
            }
        }
        if(kind==Layer::Leg)legs.push_back(part);
        else if(kind==Layer::Head)head.push_back(part);
        else arms.push_back(part);
    };
    if(human){
        add(find(subjectName+"/Head"),Layer::Head);
        add(find(body+"/Upper body/Left arm"),Layer::Arm);
        add(find(body+"/Upper body/Right arm"),Layer::Arm);
        add(find(body+"/Lower body/Legs/Left leg"),Layer::Leg);
        add(find(body+"/Lower body/Legs/Right leg"),Layer::Leg);
    }
    PoseGifResult outcome;
    if(arms.size()<2&&human){
        // A broad silhouette can supply only rough arm strips; do not present
        // this as anatomically rigged data in the UI or metadata.
        for(int side=0;side<2;++side){
            Mask strip{image.width,image.height,std::vector<uint8_t>(subject->pixels.size())};
            for(int y=y0+(y1-y0)*23/100;y<y0+(y1-y0)*58/100;++y)
             for(int x=x0;x<=x1;++x){const size_t i=size_t(y)*image.width+x;
              if(subject->pixels[i]&&(side==0?x<x0+(x1-x0)*27/100:x>x0+(x1-x0)*73/100))strip.pixels[i]=255;
             }
            inferred.push_back(std::move(strip));add(&inferred.back(),Layer::Arm);
        }
        outcome.approximateLimbs=true;
    }
    if(!human&&legs.empty()){
        // Animal parsing may supply only a whole-animal silhouette. Divide
        // its lower edge into two broad, explicitly approximate moving parts.
        for(int side=0;side<2;++side){
            Mask strip{image.width,image.height,std::vector<uint8_t>(subject->pixels.size())};
            for(int y=y0+(y1-y0)*63/100;y<=y1;++y)
             for(int x=x0;x<=x1;++x){const size_t i=size_t(y)*image.width+x;
              if(subject->pixels[i]&&(side==0?x<centerX:x>=centerX))strip.pixels[i]=255;
             }
            inferred.push_back(std::move(strip));add(&inferred.back(),Layer::Leg);
        }
        outcome.approximateLimbs=true;
    }
    Mask base=*subject;
    for(const auto& layer:legs)for(size_t i=0;i<base.pixels.size();++i)
        if(layer.mask->pixels[i])base.pixels[i]=0;
    for(const auto& layer:head)for(size_t i=0;i<base.pixels.size();++i)
        if(layer.mask->pixels[i])base.pixels[i]=0;
    for(const auto& layer:arms)for(size_t i=0;i<base.pixels.size();++i)
        if(layer.mask->pixels[i])base.pixels[i]=0;
    std::filesystem::create_directories(path.parent_path());GifWriter gif(path,width,height);
    std::vector<uint8_t> frame(size_t(width)*height,255);
    for(int f=0;f<frameCount;++f){
        const double seconds=f/8.0;
        const int scene=std::min(6,int(seconds/4));
        const double blend=smooth(scene*4+3,(scene+1)*4,seconds);
        const double bodyAngle=.034*std::sin(seconds*2*pi/3);
        const double bob=((human?2.5:5)*std::sin(seconds*2*pi/2.1)+
            (scene==6?5:0))/scale;
        std::fill(frame.begin(),frame.end(),255);
        auto paint=[&](const Mask& mask,const Layer* layer){
            double angle=0;
            if(layer){
                if(layer->kind==Layer::Arm){
                    const bool right=layer->centerX>=centerX;
                    const double targetT=right?0:pi;
                    const double targetUp=right?-pi/2:3*pi/2;
                    const double poses[]={0,wrap(targetT-layer->restAngle),
                        wrap(targetUp-layer->restAngle),.23*std::sin(seconds*3+(right?0:pi)),
                        (right?.45*std::sin(seconds*8):wrap(targetUp-layer->restAngle)),
                        wrap(targetT-layer->restAngle),-.15,0};
                    angle=poses[scene]*(1-blend)+poses[scene+1]*blend;
                }else if(layer->kind==Layer::Leg){
                    const double sign=layer->centerX>=centerX?1:-1;
                    angle=(scene==3||scene==6?.22:.08)*sign*std::sin(seconds*3.2);
                }else angle=.075*std::sin(seconds*2*pi/4);
            }
            const double ca=std::cos(angle),sa=std::sin(angle);
            const double cb=std::cos(bodyAngle),sb=std::sin(bodyAngle);
            for(int y=0;y<height;++y)for(int x=0;x<width;++x){
                const double dx=(x+.5-width*.5)/scale,dy=(y+.5-height*.5)/scale-bob;
                const double bx=centerX+dx*cb+dy*sb;
                const double by=centerY-dx*sb+dy*cb;
                const double sx=layer?layer->pivotX+(bx-layer->pivotX)*ca+(by-layer->pivotY)*sa:bx;
                const double sy=layer?layer->pivotY-(bx-layer->pivotX)*sa+(by-layer->pivotY)*ca:by;
                const int ix=int(std::floor(sx+.5)),iy=int(std::floor(sy+.5));
                if(ix<0||iy<0||ix>=image.width||iy>=image.height)continue;
                const size_t i=size_t(iy)*image.width+ix;
                if(!mask.pixels[i]||!subject->pixels[i])continue;
                if(image.pixels[i*4+3]<128)continue;
                // Smooth source pixels and their mask edges when the cutout is
                // enlarged beyond its native resolution. Clamp cubic ringing.
                if(scale>1.01){
                    const int floorX=int(std::floor(sx)),floorY=int(std::floor(sy));
                    double rgb[3]{},opacity=0,weights=0;
                    for(int yy=-1;yy<=2;++yy){const int py=std::clamp(floorY+yy,0,image.height-1);const double wy=bicubic(sy-(floorY+yy));
                        for(int xx=-1;xx<=2;++xx){const int px=std::clamp(floorX+xx,0,image.width-1);
                            const size_t pos=size_t(py)*image.width+px;const double k=wy*bicubic(sx-(floorX+xx));
                            const double a=(mask.pixels[pos]&&subject->pixels[pos]?1.:0.)*image.pixels[pos*4+3]/255.;
                            for(int c=0;c<3;++c)rgb[c]+=k*a*image.pixels[pos*4+c];opacity+=k*a;weights+=k;
                        }
                    }
                    if(opacity/std::max(.001,weights)<.5)continue;
                    uint8_t smoothPixel[4]{};for(int c=0;c<3;++c)smoothPixel[c]=uint8_t(std::clamp(rgb[c]/std::max(.001,opacity),0.,255.));
                    frame[size_t(y)*width+x]=colour(smoothPixel);
                }else frame[size_t(y)*width+x]=colour(&image.pixels[i*4]);
            }
        };
        for(const auto& layer:legs)paint(*layer.mask,&layer);
        paint(base,nullptr);
        for(const auto& layer:head)paint(*layer.mask,&layer);
        for(const auto& layer:arms)paint(*layer.mask,&layer);
        const int delay=(f%2)?13:12;
        gif.frame(frame,delay);
        ++outcome.frames;outcome.durationCentiseconds+=delay;
    }
    gif.finish();return outcome;
}
