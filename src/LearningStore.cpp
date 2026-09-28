#include "LearningStore.h"
#include "PixelLearner.h"
#include "TrainingGuide.h"
#include "WicImage.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
struct Sample {ImageRGBA image;Mask truth,baseline,parent;std::string fingerprint;};
struct StoredModel {std::string label,parent;PixelLearner net;float score=0,baseline=0;};
std::filesystem::path wideAscii(const std::string& s){return std::filesystem::path(std::wstring(s.begin(),s.end()));}
std::string shortTrainingKey(const std::string& label){
    uint64_t digest=14695981039346656037ull;
    for(unsigned char c:label){digest^=c;digest*=1099511628211ull;}
    char key[24]{};std::snprintf(key,sizeof(key),"part_%016llx",static_cast<unsigned long long>(digest));return key;
}
Mask readMask(const std::filesystem::path& path){
    const ImageRGBA rgba=loadImageWic(path);Mask m{rgba.width,rgba.height,std::vector<uint8_t>(size_t(rgba.width)*rgba.height)};
    for(size_t i=0;i<m.pixels.size();++i)m.pixels[i]=rgba.pixels[i*4]>127?255:0;return m;
}
size_t area(const Mask& m){size_t n=0;for(auto v:m.pixels)n+=v!=0;return n;}
bool bbox(const Mask& m,int& x0,int& y0,int& x1,int& y1){
    x0=m.width;y0=m.height;x1=-1;y1=-1;
    for(int y=0;y<m.height;++y)for(int x=0;x<m.width;++x)if(m.pixels[size_t(y)*m.width+x]){
        x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);
    }return x1>=x0;
}
bool valid(const Sample& s){
    if(s.image.empty()||s.truth.empty()||s.baseline.empty()||s.parent.empty())return false;
    const auto n=size_t(s.image.width)*s.image.height;
    if(s.truth.pixels.size()!=n||s.baseline.pixels.size()!=n||s.parent.pixels.size()!=n)return false;
    const size_t positives=area(s.truth),region=area(s.parent);
    if(positives<16||region<positives||positives*10>=region*9)return false;
    size_t contained=0;for(size_t i=0;i<n;++i)contained+=s.truth.pixels[i]!=0&&s.parent.pixels[i]!=0;
    return contained*100>=positives*95;
}
void reduceForTraining(Sample& s){
    const int longest=std::max(s.image.width,s.image.height);
    if(longest<=1024)return;
    const int w=std::max(1,int(double(s.image.width)*1024/longest));
    const int h=std::max(1,int(double(s.image.height)*1024/longest));
    ImageRGBA image{w,h,std::vector<uint8_t>(size_t(w)*h*4)};
    Mask truth{w,h,std::vector<uint8_t>(size_t(w)*h)},baseline=truth,parent=truth;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        const int sx=std::min(s.image.width-1,int((x+.5)*s.image.width/w));
        const int sy=std::min(s.image.height-1,int((y+.5)*s.image.height/h));
        const size_t src=size_t(sy)*s.image.width+sx,dst=size_t(y)*w+x;
        for(int c=0;c<4;++c)image.pixels[dst*4+c]=s.image.pixels[src*4+c];
        truth.pixels[dst]=s.truth.pixels[src];baseline.pixels[dst]=s.baseline.pixels[src];parent.pixels[dst]=s.parent.pixels[src];
    }
    s.image=std::move(image);s.truth=std::move(truth);s.baseline=std::move(baseline);s.parent=std::move(parent);
}
std::vector<Sample> samplesFor(const std::filesystem::path& dir,const std::string& label,const std::string& parent){
    std::vector<Sample> samples;if(!std::filesystem::exists(dir))return samples;
    std::vector<std::filesystem::path> manifests;
    for(const auto& e:std::filesystem::directory_iterator(dir))if(e.is_regular_file()&&e.path().extension()==L".sample")manifests.push_back(e.path());
    std::sort(manifests.begin(),manifests.end(),[](const auto& a,const auto& b){return std::filesystem::last_write_time(a)>std::filesystem::last_write_time(b);});
    std::set<std::string> seen;
    for(const auto& file:manifests){try{
        if(std::filesystem::file_size(file)>1024)continue;
        std::ifstream f(file,std::ios::binary);std::string name,ancestor,hash;
        std::getline(f,name);std::getline(f,ancestor);std::getline(f,hash);
        if(name!=label||ancestor!=parent||hash.size()!=64||seen.count(hash)||
            !std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))continue;
        const auto stem=file.parent_path()/file.stem();
        Sample s;s.fingerprint=hash;s.image=loadImageWic(stem.wstring()+L"_image.png");
        s.truth=readMask(stem.wstring()+L"_truth.png");s.baseline=readMask(stem.wstring()+L"_baseline.png");
        s.parent=readMask(stem.wstring()+L"_parent.png");
        if(trainingImageSha256(s.image)!=hash)continue;
        reduceForTraining(s);
        if(valid(s)){seen.insert(hash);samples.push_back(std::move(s));}
        if(samples.size()>=16)break;
    }catch(const std::exception&){continue;}}
    return samples;
}
std::filesystem::path modelPath(const std::filesystem::path& appDir,const std::string& label){
    const auto old=appDir/L"LearnedModels"/std::filesystem::path(wideAscii(trainingGuideSafeLabel(label)).wstring()+L".pixelnet");
    if(std::filesystem::exists(old))return old;
    return appDir/L"LearnedModels"/std::filesystem::path(wideAscii(shortTrainingKey(label)).wstring()+L".pixelnet");
}
bool loadModel(const std::filesystem::path& path,StoredModel& m){
    std::ifstream f(path,std::ios::binary);std::string magic;
    if(!std::getline(f,magic)||(magic!="VIMS_PIXEL_LEARNER_V1"&&magic!="VIMS_PIXEL_LEARNER_V2"&&magic!="VIMS_PIXEL_LEARNER_V3"))return false;
    if(!std::getline(f,m.label)||!std::getline(f,m.parent)||m.label.size()>400||m.parent.size()>400)return false;
    if(!(f>>m.score>>m.baseline>>m.net.outputBias))return false;
    if(magic=="VIMS_PIXEL_LEARNER_V1"){
        // Preserve the six original inputs exactly; added light and chroma
        // features have zero weight until the next approved training run.
        for(int h=0;h<PixelLearner::Hidden;++h)for(int j=0;j<6;++j){
            float v;if(!(f>>v)||!std::isfinite(v)||std::abs(v)>100)return false;
            m.net.first[h*PixelLearner::Inputs+j]=v;
        }
    }else if(magic=="VIMS_PIXEL_LEARNER_V2"){
        for(int h=0;h<PixelLearner::Hidden;++h)for(int j=0;j<10;++j){
            float v;if(!(f>>v)||!std::isfinite(v)||std::abs(v)>100)return false;
            m.net.first[h*PixelLearner::Inputs+j]=v;
        }
    }else for(auto& v:m.net.first)if(!(f>>v)||!std::isfinite(v)||std::abs(v)>100)return false;
    for(auto& v:m.net.bias)if(!(f>>v)||!std::isfinite(v)||std::abs(v)>100)return false;
    for(auto& v:m.net.last)if(!(f>>v)||!std::isfinite(v)||std::abs(v)>100)return false;
    return std::isfinite(m.score)&&std::isfinite(m.baseline)&&std::isfinite(m.net.outputBias)&&
        m.score>=0&&m.score<=1&&m.baseline>=0&&m.baseline<=1;
}
bool saveModel(const std::filesystem::path& path,const StoredModel& m){
    std::filesystem::create_directories(path.parent_path());const auto temp=path.wstring()+L".pending";
    std::ofstream f(temp.c_str(),std::ios::binary|std::ios::trunc);if(!f)return false;
    f<<"VIMS_PIXEL_LEARNER_V3\n"<<m.label<<'\n'<<m.parent<<'\n'<<std::setprecision(9)
     <<m.score<<' '<<m.baseline<<' '<<m.net.outputBias<<'\n';
    for(float v:m.net.first)f<<v<<' ';f<<'\n';
    for(float v:m.net.bias)f<<v<<' ';f<<'\n';
    for(float v:m.net.last)f<<v<<' ';f<<'\n';f.close();
    if(!f)return false;
    return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
}
void train(PixelLearner& net,const std::vector<Sample>& examples,size_t heldOut,const std::string& parentName){
    net=PixelLearner::initial();std::mt19937 rng(0x27127);
    struct Points{const Sample* s;std::vector<size_t> positive,negative;int x0,y0,x1,y1;MaskFrame frame;};
    std::vector<Points> points;
    for(size_t j=0;j<examples.size();++j){if(j==heldOut)continue;
        const Sample& s=examples[j];Points p{&s,{},{},0,0,0,0};
        if(!bbox(s.parent,p.x0,p.y0,p.x1,p.y1))continue;
        p.frame=estimateMaskFrame(s.parent,verticallyOrientedPart(parentName,s.parent));
        for(size_t i=0;i<s.parent.pixels.size();++i)if(s.parent.pixels[i]){
            if(s.truth.pixels[i])p.positive.push_back(i);else p.negative.push_back(i);
        }
        if(!p.positive.empty()&&!p.negative.empty())points.push_back(std::move(p));
    }
    for(int epoch=0;epoch<36;++epoch)for(auto& p:points){
        for(int k=0;k<1300;++k){const bool positive=(k%2)==0;
            const auto& pool=positive?p.positive:p.negative;
            const size_t index=pool[rng()%pool.size()];
            const int x=int(index%p.s->image.width),y=int(index/p.s->image.width);
            const auto f=PixelLearner::features(p.s->image,x,y,p.x0,p.y0,p.x1,p.y1,&p.frame);
            net.update(f,positive,.025f/(1.f+epoch*.045f));
        }
    }
}
struct Score {float iou=0,precision=0,baseline=0;};
Score evaluate(const PixelLearner& net,const Sample& s,const std::string& parentName){
    int x0,y0,x1,y1;if(!bbox(s.parent,x0,y0,x1,y1))return {};
    const MaskFrame frame=estimateMaskFrame(s.parent,verticallyOrientedPart(parentName,s.parent));
    size_t tp=0,fp=0,fn=0,bTp=0,bFp=0,bFn=0;
    for(int y=y0;y<=y1;y+=2)for(int x=x0;x<=x1;x+=2){
        const size_t i=size_t(y)*s.image.width+x;if(!s.parent.pixels[i])continue;
        bool truth=s.truth.pixels[i]!=0,base=s.baseline.pixels[i]!=0;
        const float probability=net.probability(PixelLearner::features(s.image,x,y,x0,y0,x1,y1,&frame));
        const bool combined=base?probability>.10f:probability>=.80f;
        tp+=combined&&truth;fp+=combined&&!truth;fn+=!combined&&truth;
        bTp+=base&&truth;bFp+=base&&!truth;bFn+=!base&&truth;
    }
    return {float(tp)/std::max<size_t>(1,tp+fp+fn),float(tp)/std::max<size_t>(1,tp+fp),
        float(bTp)/std::max<size_t>(1,bTp+bFp+bFn)};
}
void mergeLearned(const PixelLearner& net,const ImageRGBA& image,const Mask& parent,Mask& mask,const std::string& parentName){
    int x0,y0,x1,y1;if(!bbox(parent,x0,y0,x1,y1))return;
    const MaskFrame frame=estimateMaskFrame(parent,verticallyOrientedPart(parentName,parent));
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
        size_t i=size_t(y)*image.width+x;
        if(!parent.pixels[i])continue;
        const float probability=net.probability(PixelLearner::features(image,x,y,x0,y0,x1,y1,&frame));
        if(mask.pixels[i]&&probability<=.10f)mask.pixels[i]=0;
        else if(!mask.pixels[i]&&probability>=.80f)mask.pixels[i]=255;
    }
}
}

LearningOutcome approveLearningSample(const std::filesystem::path& appDir,const ImageRGBA& image,
    const NamedMask& corrected,const Mask& original,const Mask& parent){
    LearningOutcome outcome;
    Sample sample{image,corrected.mask,original,parent,trainingImageSha256(image)};
    if(!valid(sample)||sample.fingerprint.empty()){
        outcome.message="Approval needs a nonempty part mask inside a detected parent; use Add/Erase brush to correct it first.";return outcome;
    }
    // Keep a fixed filename length even for deeply nested anatomical labels.
    const std::string label=trainingGuideSafeLabel(corrected.name);
    const auto folder=appDir/L"TrainingSamples"/wideAscii(shortTrainingKey(corrected.name));
    std::filesystem::create_directories(folder);
    const auto base=folder/wideAscii(sample.fingerprint);
    const auto png=base.wstring();
    Mask all{image.width,image.height,std::vector<uint8_t>(size_t(image.width)*image.height,255)};
    // Save the complete approved example. The learner never labels its own
    // predictions as ground truth without an explicit user approval.
    saveCutoutPngWic(png+L"_image.png",image,all);
    saveMaskPngWic(png+L"_truth.png",corrected.mask);
    saveMaskPngWic(png+L"_baseline.png",original);
    saveMaskPngWic(png+L"_parent.png",parent);
    const auto manifestPath=png+L".sample";
    std::ofstream manifest(manifestPath.c_str(),std::ios::binary|std::ios::trunc);
    manifest<<corrected.name<<'\n'<<corrected.parent<<'\n'<<sample.fingerprint<<'\n';manifest.close();
    if(!manifest)throw std::runtime_error("Could not finish approved training sample");
    auto examples=samplesFor(folder,corrected.name,corrected.parent);
    const auto oldFolder=appDir/L"TrainingSamples"/wideAscii(label);
    if(oldFolder!=folder&&std::filesystem::exists(oldFolder)){
        auto older=samplesFor(oldFolder,corrected.name,corrected.parent);
        for(auto& e:older){bool duplicate=false;for(const auto& current:examples)if(current.fingerprint==e.fingerprint){duplicate=true;break;}
            if(!duplicate)examples.push_back(std::move(e));}
    }
    outcome.approvedImages=examples.size();
    if(examples.size()<3){outcome.message="Approved mask saved. Need three different approved images of this part before cross-image training.";return outcome;}
    // The newest approved image is withheld from training.
    const size_t heldOut=0;PixelLearner candidate;train(candidate,examples,heldOut,corrected.parent);
    const auto score=evaluate(candidate,examples[heldOut],corrected.parent);
    StoredModel existing;const auto path=modelPath(appDir,corrected.name);
    const bool hasExisting=loadModel(path,existing)&&existing.label==corrected.name&&existing.parent==corrected.parent;
    const auto existingScore=hasExisting?evaluate(existing.net,examples[heldOut],corrected.parent):Score{};
    if(score.iou>=std::max(.60f,score.baseline+.03f)&&score.precision>=.75f&&
       (!hasExisting||score.iou>=existingScore.iou+.01f)){
        StoredModel next{corrected.name,corrected.parent,candidate,score.iou,score.baseline};
        if(!saveModel(path,next))throw std::runtime_error("Could not save validated learned model");
        outcome.activated=true;outcome.baselineIou=score.baseline;outcome.learnedIou=score.iou;
        outcome.message="Cross-image pixel model validated and activated for this part.";
    }else{
        // Keep the last validated model only if it still passes this newer
        // approved example; otherwise pause it while preserving its weights.
        if(hasExisting&&(existingScore.iou<existingScore.baseline+.03f||existingScore.precision<.75f)){
            const auto paused=path.wstring()+L".paused";
            if(!MoveFileExW(path.c_str(),paused.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Could not pause a regressing learned model");
        }
        outcome.message="Approved mask saved; candidate did not beat the base prediction on the held-out image. No new model activated.";
    }
    return outcome;
}

size_t applyLearnedModels(const std::filesystem::path& appDir,const ImageRGBA& image,AnalysisResult& result){
    size_t applied=0;std::vector<std::string> approvedHere;
    const auto sampleRoot=appDir/L"TrainingSamples";
    if(std::filesystem::exists(sampleRoot)){
        const std::string fingerprint=trainingImageSha256(image);
        for(const auto& folder:std::filesystem::directory_iterator(sampleRoot)){
            try{
                if(!folder.is_directory())continue;
                const auto manifest=folder.path()/wideAscii(fingerprint+".sample");
                if(!std::filesystem::exists(manifest))continue;
                std::ifstream f(manifest,std::ios::binary);std::string name,ancestor,hash;
                std::getline(f,name);std::getline(f,ancestor);std::getline(f,hash);
                if(hash!=fingerprint||trainingGuideSafeLabel(name)!=folder.path().filename().string() && folder.path().filename().string().rfind("part_",0)!=0)continue;
                const auto path=folder.path()/wideAscii(fingerprint);
                Mask verified=readMask(path.wstring()+L"_truth.png");
                if(verified.width!=image.width||verified.height!=image.height)continue;
                auto it=std::find_if(result.masks.begin(),result.masks.end(),[&](const NamedMask& m){return m.name==name;});
                if(it!=result.masks.end()){
                    it->mask=std::move(verified);it->source+="+user-approved mask (same image)";
                }else{
                    NamedMask m;m.name=name;m.parent=ancestor;m.mask=std::move(verified);
                    m.category="User-approved region";m.source="approved pixel mask replay";
                    m.provenance=MaskProvenance::LandmarkDerived;m.confidence=1;
                    result.masks.push_back(std::move(m));
                    NamedMask raw;raw.name=name;raw.parent=ancestor;
                    raw.mask={image.width,image.height,std::vector<uint8_t>(size_t(image.width)*image.height)};
                    raw.source="raw ONNX model has no prediction";raw.confidence=0;
                    result.rawMasks.push_back(std::move(raw));
                }
                approvedHere.push_back(name);++applied;
            }catch(const std::exception&){continue;}
        }
    }
    const auto folder=appDir/L"LearnedModels";if(!std::filesystem::exists(folder))return applied;
    for(const auto& e:std::filesystem::directory_iterator(folder)){
        try{
            if(!e.is_regular_file()||e.path().extension()!=L".pixelnet"||e.file_size()>8192)continue;
            StoredModel model;if(!loadModel(e.path(),model))continue;
            if(std::find(approvedHere.begin(),approvedHere.end(),model.label)!=approvedHere.end())continue;
            auto parent=std::find_if(result.masks.begin(),result.masks.end(),[&](const NamedMask& m){return m.name==model.parent;});
            if(parent==result.masks.end()||parent->mask.empty())continue;
            const Mask parentMask=parent->mask;
            auto it=std::find_if(result.masks.begin(),result.masks.end(),[&](const NamedMask& m){return m.name==model.label;});
            Mask prediction{image.width,image.height,std::vector<uint8_t>(size_t(image.width)*image.height)};
            if(it!=result.masks.end()&&it->mask.width==image.width&&it->mask.height==image.height)prediction=it->mask;
            mergeLearned(model.net,image,parentMask,prediction,model.parent);
            const size_t parentArea=area(parentMask);
            if(area(prediction)>parentArea*6/10)continue;
            if(!area(prediction))continue;
            if(it!=result.masks.end()){
                it->mask=std::move(prediction);it->source+="+validated local pixel learner";
            }else{
                NamedMask m;m.name=model.label;m.parent=model.parent;m.mask=std::move(prediction);
                m.category="Locally learned region";m.source="approved-mask pixel learner";
                m.provenance=MaskProvenance::LandmarkDerived;m.confidence=.55f;
                result.masks.push_back(std::move(m));
                NamedMask raw;raw.name=model.label;raw.parent=model.parent;raw.mask={image.width,image.height,std::vector<uint8_t>(size_t(image.width)*image.height)};
                raw.category="No ONNX prediction";raw.source="raw model has no detector";raw.confidence=0;
                result.rawMasks.push_back(std::move(raw));
            }
            ++applied;
        }catch(const std::exception&){continue;}
    }
    return applied;
}
