#pragma once
#include "Image.h"
#include "WicImage.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct TrainingGuide {
    std::string label;
    int left=0,top=0,right=0,bottom=0;
    double angleDegrees=0;
};

// Hash the decoded pixels, not the filename: a re-encoded copy of an image
// can reuse its bounds, while a different image of the same size cannot.
inline std::string trainingImageSha256(const ImageRGBA& im){
    if(im.empty())return {};
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)!=0)return {};
    DWORD objectLen=0,digestLen=0,bytes=0;
    bool ok=BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&objectLen,sizeof(objectLen),&bytes,0)==0&&
            BCryptGetProperty(alg,BCRYPT_HASH_LENGTH,(PUCHAR)&digestLen,sizeof(digestLen),&bytes,0)==0&&digestLen==32;
    std::vector<UCHAR> object(ok?objectLen:0),digest(ok?digestLen:0);
    if(ok)ok=BCryptCreateHash(alg,&hash,object.data(),objectLen,nullptr,0,0)==0;
    std::array<unsigned char,8> dims{};
    for(int j=0;j<4;++j){dims[j]=static_cast<unsigned char>(unsigned(im.width)>>(j*8));dims[j+4]=static_cast<unsigned char>(unsigned(im.height)>>(j*8));}
    if(ok)ok=BCryptHashData(hash,dims.data(),static_cast<ULONG>(dims.size()),0)==0;
    for(size_t i=0;ok&&i<im.pixels.size();){
        const ULONG n=static_cast<ULONG>(std::min<size_t>(im.pixels.size()-i,1<<20));
        ok=BCryptHashData(hash,const_cast<PUCHAR>(im.pixels.data()+i),n,0)==0;i+=n;
    }
    if(ok)ok=BCryptFinishHash(hash,digest.data(),digestLen,0)==0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);
    if(!ok)return {};
    std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto b:digest)out<<std::setw(2)<<unsigned(b);return out.str();
}

inline bool trainingGuideContains(const TrainingGuide& guide,double x,double y){
    const double cx=(guide.left+guide.right)*.5,cy=(guide.top+guide.bottom)*.5;
    const double a=guide.angleDegrees*3.14159265358979323846/180.0,c=std::cos(a),s=std::sin(a);
    const double dx=x-cx,dy=y-cy;
    return std::abs(dx*c+dy*s)<=(guide.right-guide.left)*.5&&
           std::abs(-dx*s+dy*c)<=(guide.bottom-guide.top)*.5;
}

inline std::string trainingGuideSafeLabel(std::string s){
    for(char& c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))c='_';
    return s;
}

// Read only our fixed JSON fields; reject malformed, oversized, or unrelated
// files. Older guides are matched by decoding their saved PNG once.
inline std::vector<TrainingGuide> loadTrainingGuides(const std::filesystem::path& dir,const ImageRGBA& im){
    std::vector<TrainingGuide> out;
    if(im.empty()||!std::filesystem::exists(dir))return out;
    const std::string fingerprint=trainingImageSha256(im);if(fingerprint.empty())return out;
    std::vector<std::filesystem::path> paths;
    for(const auto& entry:std::filesystem::directory_iterator(dir)){
        if(!entry.is_regular_file())continue;
        const auto filename=entry.path().filename().wstring();
        if(filename.size()>11&&filename.ends_with(L"_guide.json")&&entry.file_size()<=65536)paths.push_back(entry.path());
    }
    std::sort(paths.begin(),paths.end());
    std::map<std::string,TrainingGuide> latest;
    for(const auto& path:paths){
        try{
            std::ifstream f(path,std::ios::binary);std::string json((std::istreambuf_iterator<char>(f)),{});
            auto str=[&](const char* key)->std::string{
                const std::string needle=std::string("\"")+key+"\":\"";
                const auto p=json.find(needle);if(p==std::string::npos)return {};
                const auto start=p+needle.size(),end=json.find('"',start);
                return end==std::string::npos?std::string():json.substr(start,end-start);
            };
            auto number=[&](const char* key,double fallback)->double{
                const std::string needle=std::string("\"")+key+"\":";
                const auto p=json.find(needle);if(p==std::string::npos)return fallback;
                size_t used=0;double n=std::stod(json.substr(p+needle.size()),&used);
                return used&&std::isfinite(n)?n:fallback;
            };
            const auto label=str("label"),storedHash=str("image_sha256");
            if(label.empty()||label.size()>400||trainingGuideSafeLabel(label)!=label)continue;
            if(int(number("width",-1))!=im.width||int(number("height",-1))!=im.height)continue;
            if(!storedHash.empty()){
                if(storedHash!=fingerprint)continue;
            }else{
                const std::string imageName=str("image");
                if(imageName.empty()||imageName.find_first_of("/\\:")!=std::string::npos||imageName.find("..")!=std::string::npos||
                   !imageName.ends_with(".png")||!std::all_of(imageName.begin(),imageName.end(),[](unsigned char c){return
                    (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';}))continue;
                const ImageRGBA saved=loadImageWic(dir/std::filesystem::path(std::wstring(imageName.begin(),imageName.end())));
                if(trainingImageSha256(saved)!=fingerprint)continue;
            }
            const auto pos=json.find("\"area\":[");if(pos==std::string::npos)continue;
            const auto first=pos+8,last=json.find(']',first);if(last==std::string::npos||last-first>100)continue;
            int l,t,r,b;char comma1,comma2,comma3;
            std::istringstream area(json.substr(first,last-first));
            if(!(area>>l>>comma1>>t>>comma2>>r>>comma3>>b)||comma1!=','||comma2!=','||comma3!=',')continue;
            if(l< -im.width||t< -im.height||r>im.width*2||b>im.height*2||l>=r||t>=b)continue;
            const double angle=number("rotation_degrees",0);
            if(std::abs(angle)>360)continue;
            latest[label]={label,l,t,r,b,angle};
        }catch(const std::exception&){continue;}
    }
    for(auto& pair:latest)out.push_back(std::move(pair.second));
    return out;
}
