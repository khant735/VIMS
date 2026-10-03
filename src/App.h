#pragma once
#include "VulkanRenderer.h"
#include "SegmentationEngine.h"
#include "CpuTopology.h"
#include <windows.h>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

class App {
public:
    explicit App(HINSTANCE instance);
    int run(int showCmd);

private:
    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND view_{};
    HWND rightPanel_{},panelContent_{},panelScrollbar_{},zoomInBtn_{},zoomOutBtn_{},zoomFitBtn_{};
    int panelScroll_=0,panelContentHeight_=0;
    double zoom_=1,panX_=0,panY_=0;
    bool panning_=false;
    UINT panButton_=0;
    POINT panStart_{};
    double panOriginX_=0,panOriginY_=0;
    HWND cpuText_{};
    HWND gpuText_{};
    HWND operationLabel_{}, operationProgress_{};
    HWND cpuCombo_{};
    HWND backendCombo_{};
    HWND openBtn_{};
    HWND analyseBtn_{};
    HWND diagnosticsBtn_{};
    HWND maskLabel_{};
    HWND maskList_{};
    HWND boundaryCheck_{};
    HWND cutoutCheck_{};
    HWND exportBtn_{};
    HWND exportAllBtn_{};
    HWND poseGifBtn_{};
    HWND status_{};
    HWND refineGroup_{}, refineEnable_{}, boundarySlider_{}, materialSlider_{}, colourSlider_{}, radiusSlider_{};
    HWND boundaryValue_{}, materialValue_{}, colourValue_{}, radiusValue_{};
    HWND fillHoles_{}, removeIslands_{}, protectSkin_{}, rawView_{}, refinedView_{}, resetRefine_{};
    HWND faceModelBtn_{}, faceModelStatus_{};
    HWND guideBtn_{}, applyGuideBtn_{}, resetGuideBtn_{};
    HWND createPartBtn_{}, approveBtn_{};
    std::vector<RECT> guides_;
    std::vector<double> guideAngles_;
    std::vector<bool> guideEdited_;
    bool guideMode_ = false;
    int dragMode_ = 0, dragMask_ = -1;
    POINT dragStart_{};
    RECT dragOriginal_{};
    double dragAngle_ = 0.0;
    POINT brushPrevious_{};
    int brushRadius_ = 5;
    bool showRaw_ = false;
    bool vulkanReady_ = false;
    VulkanRenderer renderer_;
    CpuTopology cpu_;
    std::unique_ptr<SegmentationEngine> segmenter_;
    ImageRGBA image_;
    std::filesystem::path loadedImagePath_;
    AnalysisResult analysis_;
    std::jthread worker_;
    bool analysing_ = false;
    int analysisProgress_ = 0;
    bool downloadingFaceModel_ = false;
    std::jthread modelWorker_;

    static constexpr UINT WM_ANALYSIS_DONE = WM_APP + 10;
    static constexpr UINT WM_FACE_MODEL_DONE = WM_APP + 11;
    static constexpr UINT WM_ANALYSIS_PROGRESS = WM_APP + 12;
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK panelProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK viewProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);
    void createUi();
    RefinementSettings refinementSettings() const;
    void resetRefinementControls();
    void updateRefinementLabels();
    void layout();
    void scrollPanel(int,int);
    void zoomPreview(double,POINT* = nullptr);
    void clampPreviewPan();
    void openImage();
    void analyse();
    bool runtimeDiagnostics(bool interactive);
    void analysisDone();
    void updatePreview();
    void resetGuides();
    void applyGuide();
    void createMissingMask();
    void approveMask();
    void paintMask(POINT,int,int);
    LRESULT handleView(HWND, UINT, WPARAM, LPARAM);
    POINT viewToImage(POINT) const;
    void exportSelected();
    void exportAll();
    void exportPoseGifs();
    void downloadFaceModel();
    void faceModelDone(bool ok);
    void updateFaceModelStatus();
    int selectedMask() const;
    std::filesystem::path exeDir() const;
    void setStatus(const std::wstring& s);
    void beginOperation(const std::wstring& label, int percent=-1);
    void updateOperation(const std::wstring& label, int percent=-1);
    void endOperation();
    void showError(const std::wstring& title, const std::wstring& message);
    static std::wstring widen(const std::string& s);
    static std::string safeFileName(std::string s);
};
