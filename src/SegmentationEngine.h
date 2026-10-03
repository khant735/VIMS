#pragma once
#include "Image.h"
#include "MaskTypes.h"
#include "TrainingGuide.h"
#include <filesystem>
#include <string>
#include <vector>
struct RefinementSettings{bool enabled=true;int boundaryPrecision=70;int materialContinuity=65;int colourTolerance=55;int radius=8;bool fillHoles=true;bool removeIslands=true;bool protectSkin=true;};
struct AnalysisResult{std::vector<NamedMask> masks;std::vector<NamedMask> rawMasks;std::vector<TrainingGuide> appliedGuides;size_t localLearningApplied=0;std::string summary;std::string imageReport;std::string metricsCsv;std::vector<std::string> descriptiveNodes;std::vector<std::string> unavailableParts;double setupMs=0,sceneMs=0,clothingMs=0,faceMs=0,masksMs=0,refinementMs=0,totalMs=0;bool usedDirectML=false;};
class SegmentationEngine{public:enum class Backend{Auto,DirectML,CPU};explicit SegmentationEngine(const std::filesystem::path&);~SegmentationEngine();void setThreads(int);void setBackend(Backend b){backend_=b;}bool gpuActive()const{return gpuActive_;}void setRefinementSettings(const RefinementSettings& s){settings_=s;}AnalysisResult analyse(const ImageRGBA&);bool modelsPresent()const;static void exportMaskPng(const std::filesystem::path&,const Mask&);static void exportContoursJson(const std::filesystem::path&,const Mask&,const std::string&);public:struct Impl; private:Impl* p_;std::filesystem::path scene_,clothes_,face_,detail_,wholeBodyPose_,handPose_,animalPose_;int threads_=1;Backend backend_=Backend::Auto;bool gpuActive_=false;RefinementSettings settings_{};};
