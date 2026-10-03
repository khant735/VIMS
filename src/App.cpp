#include "CompoundMask.h"
#include "App.h"
#include "WicImage.h"
#include "CrashLog.h"
#include "LearningStore.h"
#include "PoseGif.h"
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <cctype>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <unordered_map>
#include <sstream>
#include <vector>
#include <iterator>
#include <commctrl.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <iomanip>
#include <unordered_map>
#include <array>
#include <chrono>
#include <windowsx.h>
#include <cmath>

namespace {
constexpr double pi=3.14159265358979323846;
struct Vec2 {double x,y;};
Vec2 localPoint(const RECT& r,double angle,double x,double y){
    const double cx=(r.left+r.right)*.5,cy=(r.top+r.bottom)*.5,c=std::cos(angle),s=std::sin(angle);
    const double dx=x-cx,dy=y-cy;return {dx*c+dy*s,-dx*s+dy*c};
}
Vec2 worldPoint(const RECT& r,double angle,double x,double y){
    const double cx=(r.left+r.right)*.5,cy=(r.top+r.bottom)*.5,c=std::cos(angle),s=std::sin(angle);
    return {cx+x*c-y*s,cy+x*s+y*c};
}
bool withinGuide(const RECT& r,double angle,double x,double y){
    const Vec2 p=localPoint(r,angle,x,y);
    return std::abs(p.x)<=(r.right-r.left)*.5&&std::abs(p.y)<=(r.bottom-r.top)*.5;
}
enum : int {
    IDC_OPEN = 1001, IDC_ANALYSE, IDC_MASKLIST, IDC_BOUNDARY, IDC_TRANSPARENT_CUTOUT, IDC_EXPORT, IDC_EXPORT_ALL, IDC_CPUCOMBO, IDC_BACKENDCOMBO, IDC_REFINE_ENABLE, IDC_BOUNDARY_SLIDER, IDC_MATERIAL_SLIDER, IDC_COLOUR_SLIDER, IDC_RADIUS_SLIDER, IDC_FILL_HOLES, IDC_REMOVE_ISLANDS, IDC_PROTECT_SKIN, IDC_RAW_VIEW, IDC_REFINED_VIEW, IDC_RESET_REFINE, IDC_FACE_MODEL, IDC_DIAGNOSTICS, IDC_GUIDE, IDC_APPLY_GUIDE, IDC_RESET_GUIDE, IDC_CREATE_PART, IDC_APPROVE_MASK, IDC_POSE_GIF, IDC_ZOOM_IN, IDC_ZOOM_OUT, IDC_ZOOM_FIT
};

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}


}

App::App(HINSTANCE instance) : instance_(instance) {
    segmenter_ = std::make_unique<SegmentationEngine>(exeDir() / L"models");
}

std::filesystem::path App::exeDir() const {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::wstring App::widen(const std::string& s) { return utf8ToWide(s); }

int App::run(int showCmd) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES}; InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VulkanImageMaskStudioMain";
    RegisterClassW(&wc);

    WNDCLASSW vc{};
    vc.lpfnWndProc = viewProc;
    vc.hInstance = instance_;
    vc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    vc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    vc.lpszClassName = L"VulkanImageMaskStudioView";
    RegisterClassW(&vc);

    WNDCLASSW pc=wc;pc.lpfnWndProc=panelProc;pc.lpszClassName=L"VulkanImageMaskStudioPanel";RegisterClassW(&pc);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Vulkan Image Mask Studio 0.4.12.12",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1400, 860,
        nullptr, nullptr, instance_, this);
    if (!hwnd_) return 1;
    // Keep the standard top-level overlapped window style used by Windows
    // screen capture. The maximize/restore commands remain inactive below.
    if(HMENU menu=GetSystemMenu(hwnd_,FALSE)){
        EnableMenuItem(menu,SC_SIZE,MF_BYCOMMAND|MF_GRAYED);
        EnableMenuItem(menu,SC_MOVE,MF_BYCOMMAND|MF_GRAYED);
        EnableMenuItem(menu,SC_MAXIMIZE,MF_BYCOMMAND|MF_GRAYED);
    }
    createUi();
    ShowWindow(hwnd_, showCmd==SW_SHOWMINIMIZED?SW_SHOWMINIMIZED:SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd_);

    try {
        renderer_.initialize(view_);
        std::wstring s = L"Vulkan: "; s += utf8ToWide(renderer_.gpuName());
        setStatus(s); vulkanReady_ = true;
    } catch (const std::exception& e) {
        showError(L"Vulkan initialisation failed", widen(e.what()));
        setStatus(L"Vulkan unavailable; UI remains open for diagnostics.");
    }

    SetTimer(hwnd_, 1, 33, nullptr);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker_.request_stop(); modelWorker_.request_stop();
    renderer_.shutdown();
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK App::viewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if(m==WM_NCCREATE) SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    if(auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA)))
        if(m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_MBUTTONDOWN||m==WM_MBUTTONUP||m==WM_MOUSEMOVE||m==WM_CAPTURECHANGED||m==WM_MOUSEWHEEL)
            return app->handleView(h,m,w,l);
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK App::panelProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_NCCREATE)SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(app){
        if(m==WM_COMMAND||m==WM_NOTIFY||m==WM_HSCROLL)return SendMessageW(app->hwnd_,m,w,l);
        if(m==WM_VSCROLL){app->scrollPanel(LOWORD(w),HIWORD(w));return 0;}
        if(m==WM_MOUSEWHEEL){app->scrollPanel(SB_THUMBPOSITION,app->panelScroll_-(GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA)*60);return 0;}
    }
    return DefWindowProcW(h,m,w,l);
}

void App::createUi() {
    view_ = CreateWindowExW(0, L"VulkanImageMaskStudioView", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0,0,100,100, hwnd_, nullptr, instance_, this);

    rightPanel_=CreateWindowExW(WS_EX_COMPOSITED,L"VulkanImageMaskStudioPanel",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,100,100,hwnd_,nullptr,instance_,this);
    panelContent_=CreateWindowExW(0,L"VulkanImageMaskStudioPanel",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,rightPanel_,nullptr,instance_,this);
    panelScrollbar_=CreateWindowW(L"SCROLLBAR",nullptr,WS_CHILD|WS_VISIBLE|SBS_VERT|WS_CLIPSIBLINGS,0,0,32,100,rightPanel_,nullptr,instance_,nullptr);
    zoomOutBtn_=CreateWindowW(L"BUTTON",L"-",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,34,28,hwnd_,(HMENU)IDC_ZOOM_OUT,instance_,nullptr);
    zoomFitBtn_=CreateWindowW(L"BUTTON",L"Fit image",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,90,28,hwnd_,(HMENU)IDC_ZOOM_FIT,instance_,nullptr);
    zoomInBtn_=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,34,28,hwnd_,(HMENU)IDC_ZOOM_IN,instance_,nullptr);
    cpuText_ = CreateWindowW(L"STATIC", cpu_.description().c_str(), WS_CHILD | WS_VISIBLE,
        0,0,100,20, panelContent_, nullptr, instance_, nullptr);
    cpuCombo_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        0,0,100,200, panelContent_, reinterpret_cast<HMENU>(IDC_CPUCOMBO), instance_, nullptr);
    SendMessageW(cpuCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto CPU threads"));
    SendMessageW(cpuCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Physical cores only (SMT off)"));
    SendMessageW(cpuCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SMT 25% (some sibling threads)"));
    SendMessageW(cpuCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SMT 50% (half sibling threads)"));
    SendMessageW(cpuCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SMT 75% (most sibling threads)"));
    SendMessageW(cpuCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All logical processors (SMT 100%)"));
    SendMessageW(cpuCombo_, CB_SETCURSEL, 0, 0);
    backendCombo_=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,0,0,100,160,panelContent_,reinterpret_cast<HMENU>(IDC_BACKENDCOMBO),instance_,nullptr);
    SendMessageW(backendCombo_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"GPU: Auto (DirectML when available)"));
    SendMessageW(backendCombo_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"GPU: DirectML required"));
    SendMessageW(backendCombo_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"CPU only (no DirectML)"));
    SendMessageW(backendCombo_,CB_SETCURSEL,0,0);

    openBtn_ = CreateWindowW(L"BUTTON", L"Open image...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0,0,100,32, panelContent_, reinterpret_cast<HMENU>(IDC_OPEN), instance_, nullptr);
    analyseBtn_ = CreateWindowW(L"BUTTON", L"Analyse image", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0,0,100,32, panelContent_, reinterpret_cast<HMENU>(IDC_ANALYSE), instance_, nullptr);
    diagnosticsBtn_ = CreateWindowW(L"BUTTON", L"Runtime diagnostics", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0,0,100,32, panelContent_, reinterpret_cast<HMENU>(IDC_DIAGNOSTICS), instance_, nullptr);

    maskLabel_ = CreateWindowW(L"STATIC", L"Detected subjects and parts", WS_CHILD | WS_VISIBLE,
        0,0,100,20, panelContent_, nullptr, instance_, nullptr);
    maskList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
        0,0,100,300, panelContent_, reinterpret_cast<HMENU>(IDC_MASKLIST), instance_, nullptr);
    boundaryCheck_ = CreateWindowW(L"BUTTON", L"Show exact boundary", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0,0,100,24, panelContent_, reinterpret_cast<HMENU>(IDC_BOUNDARY), instance_, nullptr);
    SendMessageW(boundaryCheck_, BM_SETCHECK, BST_CHECKED, 0);

    cutoutCheck_ = CreateWindowW(L"BUTTON", L"Also export transparent PNG cutouts", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0,0,100,24, panelContent_, reinterpret_cast<HMENU>(IDC_TRANSPARENT_CUTOUT), instance_, nullptr);

    exportBtn_ = CreateWindowW(L"BUTTON", L"Export selected mask + boundary", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0,0,100,32, panelContent_, reinterpret_cast<HMENU>(IDC_EXPORT), instance_, nullptr);
    exportAllBtn_ = CreateWindowW(L"BUTTON", L"Export all masks", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0,0,100,32, panelContent_, reinterpret_cast<HMENU>(IDC_EXPORT_ALL), instance_, nullptr);
    poseGifBtn_ = CreateWindowW(L"BUTTON", L"Export 30-second pose GIFs", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0,0,100,32, panelContent_, reinterpret_cast<HMENU>(IDC_POSE_GIF), instance_, nullptr);
    guideBtn_ = CreateWindowW(L"BUTTON",L"Edit mask area",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        0,0,100,26,panelContent_,reinterpret_cast<HMENU>(IDC_GUIDE),instance_,nullptr);
    applyGuideBtn_ = CreateWindowW(L"BUTTON",L"Apply area",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        0,0,100,26,panelContent_,reinterpret_cast<HMENU>(IDC_APPLY_GUIDE),instance_,nullptr);
    resetGuideBtn_ = CreateWindowW(L"BUTTON",L"Reset area",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        0,0,100,26,panelContent_,reinterpret_cast<HMENU>(IDC_RESET_GUIDE),instance_,nullptr);
    createPartBtn_=CreateWindowW(L"BUTTON",L"Create missing mask",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        0,0,100,26,panelContent_,reinterpret_cast<HMENU>(IDC_CREATE_PART),instance_,nullptr);
    approveBtn_=CreateWindowW(L"BUTTON",L"Approve mask for learning",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        0,0,100,26,panelContent_,reinterpret_cast<HMENU>(IDC_APPROVE_MASK),instance_,nullptr);
    status_ = CreateWindowW(L"STATIC", L"Open an image to begin.", WS_CHILD | WS_VISIBLE | SS_LEFT,0,0,100,60, panelContent_, nullptr, instance_, nullptr);
    faceModelBtn_=CreateWindowW(L"BUTTON",L"Download Core + Face AI Models",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,100,30,panelContent_,(HMENU)IDC_FACE_MODEL,instance_,nullptr);
    faceModelStatus_=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,100,22,panelContent_,nullptr,instance_,nullptr);
    updateFaceModelStatus();
    refineGroup_=CreateWindowW(L"STATIC",L"Mask Refinement",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,100,100,panelContent_,nullptr,instance_,nullptr);
    refineEnable_=CreateWindowW(L"BUTTON",L"Enable refinement",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,100,22,panelContent_,(HMENU)IDC_REFINE_ENABLE,instance_,nullptr);
    boundarySlider_=CreateWindowW(TRACKBAR_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS,0,0,100,28,panelContent_,(HMENU)IDC_BOUNDARY_SLIDER,instance_,nullptr);
    materialSlider_=CreateWindowW(TRACKBAR_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS,0,0,100,28,panelContent_,(HMENU)IDC_MATERIAL_SLIDER,instance_,nullptr);
    colourSlider_=CreateWindowW(TRACKBAR_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS,0,0,100,28,panelContent_,(HMENU)IDC_COLOUR_SLIDER,instance_,nullptr);
    radiusSlider_=CreateWindowW(TRACKBAR_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS,0,0,100,28,panelContent_,(HMENU)IDC_RADIUS_SLIDER,instance_,nullptr);
    boundaryValue_=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,100,20,panelContent_,nullptr,instance_,nullptr); materialValue_=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,100,20,panelContent_,nullptr,instance_,nullptr); colourValue_=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,100,20,panelContent_,nullptr,instance_,nullptr); radiusValue_=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,100,20,panelContent_,nullptr,instance_,nullptr);
    fillHoles_=CreateWindowW(L"BUTTON",L"Fill holes",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,100,22,panelContent_,(HMENU)IDC_FILL_HOLES,instance_,nullptr); removeIslands_=CreateWindowW(L"BUTTON",L"Remove islands",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,100,22,panelContent_,(HMENU)IDC_REMOVE_ISLANDS,instance_,nullptr); protectSkin_=CreateWindowW(L"BUTTON",L"Protect skin",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,100,22,panelContent_,(HMENU)IDC_PROTECT_SKIN,instance_,nullptr);
    rawView_=CreateWindowW(L"BUTTON",L"Raw",WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON|WS_GROUP,0,0,70,22,panelContent_,(HMENU)IDC_RAW_VIEW,instance_,nullptr); refinedView_=CreateWindowW(L"BUTTON",L"Refined",WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,0,0,80,22,panelContent_,(HMENU)IDC_REFINED_VIEW,instance_,nullptr); resetRefine_=CreateWindowW(L"BUTTON",L"Reset defaults",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,100,25,panelContent_,(HMENU)IDC_RESET_REFINE,instance_,nullptr);
    for(HWND h:{boundarySlider_,materialSlider_,colourSlider_}){SendMessageW(h,TBM_SETRANGE,TRUE,MAKELONG(0,100));} SendMessageW(radiusSlider_,TBM_SETRANGE,TRUE,MAKELONG(1,24)); resetRefinementControls();
    for(HWND child=GetWindow(panelContent_,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        SetWindowLongPtrW(child,GWL_STYLE,GetWindowLongPtrW(child,GWL_STYLE)|WS_CLIPSIBLINGS);
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    }
    layout();
}

void App::layout() {
    if (!hwnd_ || !view_) return;
    RECT rc{}; GetClientRect(hwnd_, &rc);
    const int cw=std::max(1,int(rc.right)), ch=std::max(1,int(rc.bottom));
    UINT dpi=96;
    if(HMODULE u=GetModuleHandleW(L"user32.dll")){
        using GetDpiForWindowFn=UINT (WINAPI*)(HWND);
        if(auto fn=reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(u,"GetDpiForWindow"))) dpi=fn(hwnd_);
    }
    const double ds=std::max(0.75, double(dpi)/96.0);
    auto S=[&](int v){return std::max(1,int(v*ds+0.5));};

    // Reserve readable controls even at high display scaling.
    int panel=std::clamp(int(cw*0.31),S(400),S(590));
    if(cw-panel<S(320)) panel=std::max(S(300),cw-S(320));
    const int pad=S(10), gap=S(8), vw=std::max(S(160),cw-panel);
    const int toolbarH=S(50),previewH=std::max(1,ch-toolbarH);
    RECT existingView{};GetClientRect(view_,&existingView);
    const bool viewChanged=existingView.right!=vw||existingView.bottom!=previewH;
    if(viewChanged){
        // Reserve a dedicated strip above the Vulkan surface. Sibling buttons
        // painted over a Vulkan swapchain can disappear on the next frame.
        MoveWindow(view_,0,toolbarH,vw,previewH,TRUE);
        MoveWindow(zoomOutBtn_,S(10),S(10),S(36),S(28),TRUE);
        MoveWindow(zoomFitBtn_,S(50),S(10),S(90),S(28),TRUE);
        MoveWindow(zoomInBtn_,S(144),S(10),S(36),S(28),TRUE);
    }
    RECT existingPanel{};GetWindowRect(rightPanel_,&existingPanel);
    if(existingPanel.right-existingPanel.left!=cw-vw||existingPanel.bottom-existingPanel.top!=ch)
        MoveWindow(rightPanel_,vw,0,std::max(1,cw-vw),ch,TRUE);
    RECT panelRect{};GetClientRect(rightPanel_,&panelRect);
    const int scrollWidth=S(32);
    const int contentWidth=std::max(1,int(panelRect.right)-scrollWidth);
    MoveWindow(panelScrollbar_,contentWidth,0,scrollWidth,ch,TRUE);
    const int available=std::max(1,contentWidth-pad*2);
    // The children never move relative to one another. Scroll by shifting
    // the content window as a single clipped surface.
    SendMessageW(panelContent_,WM_SETREDRAW,FALSE,0);
    HDWP batch=BeginDeferWindowPos(48);
    auto place=[&](HWND control,int xx,int yy,int ww,int hh){
        if(batch)batch=DeferWindowPos(batch,control,nullptr,xx,yy,ww,hh,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
    };
    int x=pad,y=pad,w=available;
    const int row=S(24), btn=S(30), labelW=std::clamp(int(w*.48),S(175),S(245));

    place(cpuText_,x,y,w,S(36)); y+=S(38);
    place(cpuCombo_,x,y,w,S(200)); y+=S(30);
    place(backendCombo_,x,y,w,S(160)); y+=S(32);
    place(openBtn_,x,y,(w-gap)/2,btn);
    place(analyseBtn_,x+(w+gap)/2,y,(w-gap)/2,btn);y+=S(36);
    place(diagnosticsBtn_,x,y,w,btn);y+=S(36);

    const int groupH=S(300);
    place(refineGroup_,x,y,w,S(20));
    int gy=y+S(22);
    place(refineEnable_,x+pad,gy,w-pad*2,S(20)); gy+=S(25);
    auto sliderRow=[&](HWND label,HWND slider){
        place(label,x+pad,gy,labelW,S(20));
        place(slider,x+pad+labelW,gy-S(3),std::max(S(80),w-pad*3-labelW),S(25));
        gy+=S(28);
    };
    sliderRow(boundaryValue_,boundarySlider_); sliderRow(materialValue_,materialSlider_);
    sliderRow(colourValue_,colourSlider_); sliderRow(radiusValue_,radiusSlider_);

    const int half=(w-pad*2-gap)/2;
    place(fillHoles_,x+pad,gy,half,S(24));
    place(removeIslands_,x+pad+half+gap,gy,half,S(24)); gy+=S(28);
    place(protectSkin_,x+pad,gy,w-pad*2,S(24));gy+=S(30);
    const int radioW=S(82);
    place(rawView_,x+pad,gy,radioW,S(22));
    place(refinedView_,x+pad+radioW,gy,S(100),S(22));
    const int resetX=x+pad+radioW+S(104);
    place(resetRefine_,resetX,gy-S(2),std::max(S(100),x+w-pad-resetX),S(26));
    y+=groupH+S(6);

    place(faceModelBtn_,x,y,w,btn); y+=S(33);
    place(faceModelStatus_,x,y,w,S(34)); y+=S(38);
    place(maskLabel_,x,y,w,S(20)); y+=S(22);

    int listH=S(200);
    place(maskList_,x,y,w,listH); y+=listH+S(4);
    place(guideBtn_,x,y,w,S(26));y+=S(30);
    place(applyGuideBtn_,x,y,(w-gap)/2,S(26));
    place(resetGuideBtn_,x+(w+gap)/2,y,(w-gap)/2,S(26));y+=S(30);
    place(createPartBtn_,x,y,w,S(26));y+=S(30);
    place(approveBtn_,x,y,w,S(26));y+=S(30);
    place(boundaryCheck_,x,y,w,S(22)); y+=S(24);
    place(cutoutCheck_,x,y,w,S(22)); y+=S(24);
    place(exportBtn_,x,y,w,btn);y+=S(34);
    place(exportAllBtn_,x,y,w,btn);y+=S(34);
    place(poseGifBtn_,x,y,w,btn);y+=S(34);
    place(status_,x,y,w,S(58));y+=S(64);
    if(batch)EndDeferWindowPos(batch);
    panelContentHeight_=y+pad;
    panelScroll_=std::clamp(panelScroll_,0,std::max(0,panelContentHeight_-ch));
    SetWindowPos(panelContent_,nullptr,0,-panelScroll_,contentWidth,panelContentHeight_,
        SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
    SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS};si.nMin=0;si.nMax=panelContentHeight_-1;si.nPage=ch;si.nPos=panelScroll_;
    SetScrollInfo(panelScrollbar_,SB_CTL,&si,TRUE);
    SendMessageW(panelContent_,WM_SETREDRAW,TRUE,0);
    RedrawWindow(rightPanel_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    if(vulkanReady_&&viewChanged) {clampPreviewPan();renderer_.resized();updatePreview();}
}

void App::scrollPanel(int code,int value){
    if(!rightPanel_||!panelContent_||!panelScrollbar_)return;
    RECT rc{};GetClientRect(rightPanel_,&rc);
    int next=panelScroll_;
    if(code==SB_LINEUP)next-=32;else if(code==SB_LINEDOWN)next+=32;
    else if(code==SB_PAGEUP)next-=std::max(64,int(rc.bottom));
    else if(code==SB_PAGEDOWN)next+=std::max(64,int(rc.bottom));
    else if(code==SB_THUMBTRACK){
        SCROLLINFO track{sizeof(track),SIF_TRACKPOS};
        next=GetScrollInfo(panelScrollbar_,SB_CTL,&track)?track.nTrackPos:value;
    }else if(code==SB_THUMBPOSITION)next=value;
    else if(code==SB_TOP)next=0;
    else if(code==SB_BOTTOM)next=panelContentHeight_;
    next=std::clamp(next,0,std::max(0,panelContentHeight_-int(rc.bottom)));
    if(next==panelScroll_)return;
    panelScroll_=next;
    SCROLLINFO si{sizeof(si),SIF_POS};si.nPos=next;SetScrollInfo(panelScrollbar_,SB_CTL,&si,TRUE);
    SetWindowPos(panelContent_,nullptr,0,-next,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
    RedrawWindow(panelContent_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    RedrawWindow(rightPanel_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
}

LRESULT App::handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: layout(); return 0;
    case WM_NCLBUTTONDBLCLK: if(w==HTCAPTION)return 0;break;
    case WM_SYSCOMMAND:
        if((w&0xfff0)==SC_RESTORE){if(IsIconic(h))ShowWindow(h,SW_SHOWMAXIMIZED);return 0;}
        if((w&0xfff0)==SC_SIZE||(w&0xfff0)==SC_MOVE||(w&0xfff0)==SC_MAXIMIZE)return 0;
        break;
    case WM_TIMER: if(vulkanReady_) renderer_.draw(); return 0;
    case WM_HSCROLL: updateRefinementLabels(); return 0;
    case WM_COMMAND: {
        const int id = LOWORD(w), code = HIWORD(w);
        if (id == IDC_ZOOM_FIT && code == BN_CLICKED){zoom_=1;panX_=panY_=0;updatePreview();}
        else if (id == IDC_ZOOM_IN && code == BN_CLICKED)zoomPreview(1.5);
        else if (id == IDC_ZOOM_OUT && code == BN_CLICKED)zoomPreview(1/1.5);
        else if (id == IDC_OPEN && code == BN_CLICKED) openImage();
        else if (id == IDC_ANALYSE && code == BN_CLICKED) analyse();
        else if (id == IDC_DIAGNOSTICS && code == BN_CLICKED) runtimeDiagnostics(true);
        else if (id == IDC_EXPORT && code == BN_CLICKED) exportSelected();
        else if (id == IDC_EXPORT_ALL && code == BN_CLICKED) exportAll();
        else if (id == IDC_POSE_GIF && code == BN_CLICKED) exportPoseGifs();
        else if (id == IDC_GUIDE && code == BN_CLICKED) {guideMode_=SendMessageW(guideBtn_,BM_GETCHECK,0,0)==BST_CHECKED;updatePreview();setStatus(guideMode_?L"Drag box to move/resize/rotate. Shift+drag adds mask pixels; Ctrl+drag erases. Approve when correct.":L"Area editing off.");}
        else if (id == IDC_APPLY_GUIDE && code == BN_CLICKED) applyGuide();
        else if (id == IDC_RESET_GUIDE && code == BN_CLICKED) {const int i=selectedMask();if(i>=0&&i<(int)guides_.size()){guides_[i]={0,0,image_.width,image_.height};guideAngles_[i]=0;guideEdited_[i]=true;updatePreview();setStatus(L"Area reset to the whole image. Apply area to use it.");}}
        else if (id == IDC_CREATE_PART && code == BN_CLICKED) createMissingMask();
        else if (id == IDC_APPROVE_MASK && code == BN_CLICKED) approveMask();
        else if (id == IDC_BOUNDARY && code == BN_CLICKED) updatePreview();
        else if (id == IDC_RAW_VIEW && code == BN_CLICKED) { showRaw_=true; updatePreview(); }
        else if (id == IDC_REFINED_VIEW && code == BN_CLICKED) { showRaw_=false; updatePreview(); }
        else if (id == IDC_RESET_REFINE && code == BN_CLICKED) resetRefinementControls();
        else if (id == IDC_FACE_MODEL && code == BN_CLICKED) downloadFaceModel();
        return 0;
    }
    case WM_NOTIFY:
        if(reinterpret_cast<NMHDR*>(l)->idFrom == IDC_MASKLIST &&
           reinterpret_cast<NMHDR*>(l)->code == TVN_SELCHANGEDW){
            updatePreview();
            const int index=selectedMask();
            if(index<0)setStatus(L"Information row: no exportable mask for this entry.");
            else if(analysis_.masks[index].provenance==MaskProvenance::OcclusionProjected)
                setStatus(L"Projected hidden surface: estimated 2D location only. No unseen pixels or texture were detected.");
        }
        return 0;
    case WM_ANALYSIS_DONE: analysisDone(); return 0;
    case WM_FACE_MODEL_DONE: faceModelDone(w != 0); return 0;
    case WM_CLOSE:
        if (analysing_) worker_.request_stop();
        DestroyWindow(hwnd_); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

RefinementSettings App::refinementSettings() const {RefinementSettings r;r.enabled=SendMessageW(refineEnable_,BM_GETCHECK,0,0)==BST_CHECKED;r.boundaryPrecision=(int)SendMessageW(boundarySlider_,TBM_GETPOS,0,0);r.materialContinuity=(int)SendMessageW(materialSlider_,TBM_GETPOS,0,0);r.colourTolerance=(int)SendMessageW(colourSlider_,TBM_GETPOS,0,0);r.radius=(int)SendMessageW(radiusSlider_,TBM_GETPOS,0,0);r.fillHoles=SendMessageW(fillHoles_,BM_GETCHECK,0,0)==BST_CHECKED;r.removeIslands=SendMessageW(removeIslands_,BM_GETCHECK,0,0)==BST_CHECKED;r.protectSkin=SendMessageW(protectSkin_,BM_GETCHECK,0,0)==BST_CHECKED;return r;}
void App::updateRefinementLabels(){wchar_t b[80];swprintf(b,80,L"Boundary Precision: %ld",SendMessageW(boundarySlider_,TBM_GETPOS,0,0));SetWindowTextW(boundaryValue_,b);swprintf(b,80,L"Material Continuity: %ld",SendMessageW(materialSlider_,TBM_GETPOS,0,0));SetWindowTextW(materialValue_,b);swprintf(b,80,L"Colour Tolerance: %ld",SendMessageW(colourSlider_,TBM_GETPOS,0,0));SetWindowTextW(colourValue_,b);swprintf(b,80,L"Refinement Radius: %ld px",SendMessageW(radiusSlider_,TBM_GETPOS,0,0));SetWindowTextW(radiusValue_,b);}
void App::resetRefinementControls(){SendMessageW(refineEnable_,BM_SETCHECK,BST_CHECKED,0);SendMessageW(boundarySlider_,TBM_SETPOS,TRUE,70);SendMessageW(materialSlider_,TBM_SETPOS,TRUE,65);SendMessageW(colourSlider_,TBM_SETPOS,TRUE,55);SendMessageW(radiusSlider_,TBM_SETPOS,TRUE,8);SendMessageW(fillHoles_,BM_SETCHECK,BST_CHECKED,0);SendMessageW(removeIslands_,BM_SETCHECK,BST_CHECKED,0);SendMessageW(protectSkin_,BM_SETCHECK,BST_CHECKED,0);SendMessageW(refinedView_,BM_SETCHECK,BST_CHECKED,0);SendMessageW(rawView_,BM_SETCHECK,BST_UNCHECKED,0);showRaw_=false;updateRefinementLabels();}

void App::setStatus(const std::wstring& s) { SetWindowTextW(status_, s.c_str()); }
void App::showError(const std::wstring& title, const std::wstring& message) { MessageBoxW(hwnd_, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR); }

void App::openImage() {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.webp\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    try { image_ = loadImageWic(std::filesystem::path(file)); } catch (const std::exception& e) { showError(L"Open image", widen(e.what())); return; }
    analysis_ = {};
    guides_.clear();guideAngles_.clear();guideEdited_.clear();guideMode_=false;SendMessageW(guideBtn_,BM_SETCHECK,BST_UNCHECKED,0);
    TreeView_DeleteAllItems(maskList_);
    zoom_=1;panX_=panY_=0;updatePreview();
    std::wstringstream ss;
    ss << L"Loaded " << image_.width << L" x " << image_.height << L" image.";
    setStatus(ss.str());
}


static std::wstring winErrorText(DWORD e) {
    wchar_t* p=nullptr;
    DWORD n=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,e,0,reinterpret_cast<LPWSTR>(&p),0,nullptr);
    std::wstring s=(n&&p)?std::wstring(p,n):L"Unknown Windows loader error";
    if(p) LocalFree(p); while(!s.empty()&&(s.back()==L'\r'||s.back()==L'\n'))s.pop_back(); return s;
}

bool App::runtimeDiagnostics(bool interactive) {
    const int backend=static_cast<int>(SendMessageW(backendCombo_,CB_GETCURSEL,0,0));
    std::wstringstream out; bool ok=true;
    out << L"Vulkan Image Mask Studio runtime diagnostics\r\n\r\n";
    auto checkSystem=[&](const wchar_t* name, bool required){
        SetLastError(0); HMODULE h=LoadLibraryExW(name,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(h){out<<L"[OK] "<<name<<L" (Windows system runtime)\r\n";FreeLibrary(h);return true;}
        DWORD e=GetLastError();out<<(required?L"[FAIL] ":L"[WARN] ")<<name<<L" - "<<winErrorText(e)<<L" (error "<<e<<L")\r\n";
        if(required)ok=false;return false;
    };
    checkSystem(L"VCRUNTIME140.dll",true); checkSystem(L"VCRUNTIME140_1.dll",true); checkSystem(L"MSVCP140.dll",true); checkSystem(L"MSVCP140_1.dll",true); checkSystem(L"d3d12.dll",backend==1);
    out<<L"\r\nApplication runtimes\r\n";
    auto checkLocal=[&](const wchar_t* name, const char* symbol, bool required){
        auto path=exeDir()/name;
        if(!std::filesystem::exists(path)){out<<(required?L"[FAIL] ":L"[WARN] ")<<name<<L" - file is missing beside the executable.\r\n";if(required)ok=false;return false;}
        SetLastError(0); HMODULE h=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!h){DWORD e=GetLastError();out<<L"[FAIL] "<<name<<L" exists but Windows cannot load it: "<<winErrorText(e)<<L" (error "<<e<<L")\r\n";ok=false;return false;}
        bool symok=!symbol||GetProcAddress(h,symbol)!=nullptr;
        out<<(symok?L"[OK] ":L"[FAIL] ")<<name;
        if(symbol)out<<(symok?L" - required API export found.":L" - required API export is missing.");out<<L"\r\n";
        if(!symok)ok=false;FreeLibrary(h);return symok;
    };
    checkLocal(L"vulkan-1.dll",nullptr,true); checkLocal(L"libc++.dll",nullptr,true); checkLocal(L"libunwind.dll",nullptr,true);
    bool dml=checkLocal(L"DirectML.dll",nullptr,backend==1);
    bool ort=checkLocal(L"onnxruntime.dll","OrtGetApiBase",true);
    if(ort){
        auto path=exeDir()/L"onnxruntime.dll"; HMODULE h=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        bool ep=h&&GetProcAddress(h,"OrtSessionOptionsAppendExecutionProvider_DML");
        out<<(ep?L"[OK] ":backend==1?L"[FAIL] ":L"[WARN] ")<<L"ONNX Runtime DirectML execution-provider export"<<L"\r\n";
        if(!ep&&backend==1)ok=false;if(h)FreeLibrary(h);
    }
    out<<L"\r\n"<<(ok?(backend==2?L"Overall status: READY for CPU inference.":backend==1?L"Overall status: READY for DirectML initialization.":L"Overall status: READY for automatic GPU/CPU selection."):L"Overall status: NOT READY. Analysis has been blocked to prevent a runtime crash.");
    if(interactive) MessageBoxW(hwnd_,out.str().c_str(),L"Runtime diagnostics",MB_OK|(ok?MB_ICONINFORMATION:MB_ICONERROR));
    if(!ok) setStatus(L"Runtime diagnostics failed. Click Runtime diagnostics for details.");
    return ok;
}

void App::analyse() {
    CrashLog::write("Analyse requested");
    if (analysing_) return;
    const int backend=static_cast<int>(SendMessageW(backendCombo_,CB_GETCURSEL,0,0));
    if (!runtimeDiagnostics(false)) { CrashLog::write("Runtime diagnostics FAILED; analysis blocked"); runtimeDiagnostics(true); return; }
    CrashLog::write("Runtime diagnostics passed");
    if (image_.empty()) { showError(L"Analyse", L"Open an image first."); return; }
    if (!segmenter_->modelsPresent()) {
        showError(L"Models missing", L"The ONNX models are not installed. Run download_models.ps1 in the application folder, then try again.");
        return;
    }

    int sel = static_cast<int>(SendMessageW(cpuCombo_, CB_GETCURSEL, 0, 0));
    CpuTopology::Mode mode = CpuTopology::Mode::Auto;
    if (sel == 1) mode = CpuTopology::Mode::PhysicalCoresOnly;
    if (sel == 2) mode = CpuTopology::Mode::Smt25;
    if (sel == 3) mode = CpuTopology::Mode::Smt50;
    if (sel == 4) mode = CpuTopology::Mode::Smt75;
    if (sel == 5) mode = CpuTopology::Mode::AllLogicalProcessors;
    const int threads = cpu_.apply(mode);
    segmenter_->setThreads(threads);
    segmenter_->setBackend(backend==1?SegmentationEngine::Backend::DirectML:backend==2?SegmentationEngine::Backend::CPU:SegmentationEngine::Backend::Auto);
    segmenter_->setRefinementSettings(refinementSettings());
    analysis_ = {}; // Release masks from the previous run before starting the next.
    analysing_ = true;
    guides_.clear();guideAngles_.clear();guideEdited_.clear();guideMode_=false;SendMessageW(guideBtn_,BM_SETCHECK,BST_UNCHECKED,0);
    EnableWindow(analyseBtn_, FALSE);
    setStatus(backend==2?L"Analysing on CPU...":backend==1?L"Analysing with DirectML...":L"Analysing with automatic device selection...");

    ImageRGBA input = image_;
    const std::string cpuChoice=sel==1?"Physical cores (SMT off)":sel==2?"SMT 25%":sel==3?"SMT 50%":sel==4?"SMT 75%":sel==5?"SMT 100%":"Auto CPU threads";
    const std::string gpuChoice=backend==1?"DirectML required":backend==2?"CPU only":"Auto";
    worker_ = std::jthread([this, input = std::move(input), cpuChoice, gpuChoice, threads](std::stop_token st) {
        using Clock=std::chrono::steady_clock;const auto analysisStart=Clock::now();
        CrashLog::write("Analysis worker started; entering SegmentationEngine::analyse");
        try {
            if (!st.stop_requested()) analysis_ = segmenter_->analyse(input);
            CrashLog::write("SegmentationEngine::analyse returned");
            if(!st.stop_requested()){
                auto fmt=[](double ms){long long n=std::max(0LL,(long long)(ms+0.5));char b[40];snprintf(b,sizeof(b),"%02lld:%02lld.%03lld",n/60000,(n/1000)%60,n%1000);return std::string(b);};
                const double wallMs=std::chrono::duration<double,std::milli>(Clock::now()-analysisStart).count();
                const auto reportPath=exeDir()/L"Logs"/L"analysis_timings.log";
                std::ofstream report(reportPath,std::ios::app);
                if(report){SYSTEMTIME now{};GetLocalTime(&now);char when[40];snprintf(when,sizeof(when),"%04u-%02u-%02u %02u:%02u:%02u.%03u",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
                    report<<"Analysis "<<when<<" | Image "<<input.width<<"x"<<input.height<<" | CPU "<<cpuChoice<<" ("<<threads<<" threads) | GPU "<<gpuChoice<<" | Actual "<<(analysis_.usedDirectML?"DirectML":"CPU")<<"\n";
                    report<<"  Setup:       "<<fmt(analysis_.setupMs)<<"\n  Scene model: "<<fmt(analysis_.sceneMs)<<"\n  Clothing:    "<<fmt(analysis_.clothingMs)<<"\n  Face model:  "<<fmt(analysis_.faceMs)<<"\n  Masks:       "<<fmt(analysis_.masksMs)<<"\n  Refinement:  "<<fmt(analysis_.refinementMs)<<"\n  Total:       "<<fmt(analysis_.totalMs)<<" (worker wall "<<fmt(wallMs)<<")\n\n";
                    report.flush();CrashLog::write("Analysis timing report appended: Logs/analysis_timings.log");
                }else CrashLog::write("Could not open Logs/analysis_timings.log");
                std::ofstream imageReport(exeDir()/L"Logs"/L"image_properties.log",std::ios::app);
                if(imageReport){imageReport<<"Image "<<input.width<<"x"<<input.height<<"\n"
                    <<analysis_.imageReport<<"\n";imageReport.flush();}
                std::ofstream measurements(exeDir()/L"Logs"/L"mask_measurements.csv",
                    std::ios::out|std::ios::trunc);
                if(measurements){measurements<<analysis_.metricsCsv;measurements.flush();}
            }
        } catch (const std::exception& e) {
            analysis_ = {};
            CrashLog::write(std::string("Analysis C++ exception: ")+e.what());
            const double failedMs=std::chrono::duration<double,std::milli>(Clock::now()-analysisStart).count();
            const long long n=std::max(0LL,(long long)(failedMs+0.5));
            std::ofstream report(exeDir()/L"Logs"/L"analysis_timings.log",std::ios::app);
            if(report){report<<"FAILED | CPU "<<cpuChoice<<" ("<<threads<<" threads) | GPU "<<gpuChoice<<" | Elapsed "<<n/60000<<":"<<std::setfill('0')<<std::setw(2)<<(n/1000)%60<<"."<<std::setw(3)<<n%1000<<" | "<<e.what()<<"\n\n";report.flush();}
            analysis_.summary = std::string("ERROR:") + e.what();
        }
        if (!st.stop_requested()) PostMessageW(hwnd_, WM_ANALYSIS_DONE, 0, 0);
    });
}

void App::analysisDone() {
    analysing_ = false;
    EnableWindow(analyseBtn_, TRUE);
    TreeView_DeleteAllItems(maskList_);
    if (analysis_.summary.rfind("ERROR:", 0) == 0) {
        showError(L"Analysis failed", widen(analysis_.summary.substr(6)));
        setStatus(L"Analysis failed."); return;
    }
    resetGuides();
    std::unordered_map<std::string,HTREEITEM> nodes;
    HTREEITEM firstMask=nullptr;
    auto insertPath=[&](const std::string& path,LPARAM maskIndex,const wchar_t* suffix){
        HTREEITEM parent=TVI_ROOT;
        size_t pos=0;
        while(pos<path.size()){
            const size_t slash=path.find('/',pos);
            const bool leaf=slash==std::string::npos;
            const std::string key=path.substr(0,leaf?path.size():slash);
            auto it=nodes.find(key);
            if(it==nodes.end()){
                auto label=widen(path.substr(pos,leaf?path.size()-pos:slash-pos));
                if(leaf&&suffix)label+=suffix;
                TVINSERTSTRUCTW item{};item.hParent=parent;item.hInsertAfter=TVI_LAST;
                item.item.mask=TVIF_TEXT | TVIF_PARAM;
                item.item.pszText=label.data();item.item.lParam=leaf?maskIndex:0;
                HTREEITEM h=TreeView_InsertItem(maskList_,&item);
                nodes.emplace(key,h);parent=h;
                if(leaf&&maskIndex>0&&!firstMask)firstMask=h;
            }else parent=it->second;
            if(leaf)break;
            pos=slash+1;
        }
    };
    for(size_t i=0;i<analysis_.masks.size();++i)
        insertPath(analysis_.masks[i].name,static_cast<LPARAM>(i+1),
            analysis_.masks[i].provenance==MaskProvenance::OcclusionProjected?L" (estimated, unseen)":nullptr);
    for(const auto& path:analysis_.unavailableParts)
        insertPath(path,0,L" (no detector)");
    // Measurements and scene descriptions remain in AnalysisResult and its
    // CSV/log output, but the subject tree only displays masks and unavailable
    // parts. Measurement rows made every part needlessly hard to browse.
    for(const char* branch:{"People","People/Human","People/Human/Person 1","Animals","Environment"}){
        auto it=nodes.find(branch);if(it!=nodes.end())TreeView_Expand(maskList_,it->second,TVE_EXPAND);
    }
    if(firstMask)TreeView_SelectItem(maskList_,firstMask);
    setStatus(widen(analysis_.summary));
    updatePreview();
}

int App::selectedMask() const {
    HTREEITEM selected=TreeView_GetSelection(maskList_);
    if(!selected)return -1;
    TVITEMW item{};item.mask=TVIF_PARAM;item.hItem=selected;
    if(!TreeView_GetItem(maskList_,&item)||item.lParam<=0)return -1;
    const int index=static_cast<int>(item.lParam-1);
    return index<static_cast<int>(analysis_.masks.size())?index:-1;
}

namespace {
double cubic(double x){x=std::abs(x);if(x<1)return 1.5*x*x*x-2.5*x*x+1;if(x<2)return -.5*x*x*x+2.5*x*x-4*x+2;return 0;}
}
void App::updatePreview() {
    if(image_.empty()||!vulkanReady_)return;
    RECT rc{};GetClientRect(view_,&rc);const int w=std::max(1,int(rc.right)),h=std::max(1,int(rc.bottom));
    const double scale=std::min(double(w)/image_.width,double(h)/image_.height)*zoom_;
    ImageRGBA preview;preview.width=w;preview.height=h;preview.pixels.resize(size_t(w)*h*4);
    const int idx=selectedMask();const auto& sourceMasks=(showRaw_&&analysis_.rawMasks.size()==analysis_.masks.size())?analysis_.rawMasks:analysis_.masks;
    const Mask* mask=idx>=0&&idx<(int)sourceMasks.size()?&sourceMasks[idx].mask:nullptr;
    if(mask&&(mask->width!=image_.width||mask->height!=image_.height))mask=nullptr;
    const bool projected=mask&&analysis_.masks[idx].provenance==MaskProvenance::OcclusionProjected;
    const bool boundary=mask&&SendMessageW(boundaryCheck_,BM_GETCHECK,0,0)==BST_CHECKED;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        uint8_t* out=&preview.pixels[(size_t(y)*w+x)*4];out[0]=out[1]=out[2]=25;out[3]=255;
        const double sx=(x+.5-w*.5)/scale+image_.width*.5+panX_-.5;
        const double sy=(y+.5-h*.5)/scale+image_.height*.5+panY_-.5;
        if(sx<-.5||sy<-.5||sx>=image_.width-.5||sy>=image_.height-.5)continue;
        const int ix=int(std::floor(sx)),iy=int(std::floor(sy));double rgb[3]{},alpha=0,weight=0;
        for(int dy=-1;dy<=2;++dy){int row=std::clamp(iy+dy,0,image_.height-1);double wy=cubic(sy-iy-dy);
          for(int dx=-1;dx<=2;++dx){int col=std::clamp(ix+dx,0,image_.width-1);double k=wy*cubic(sx-ix-dx);
            const uint8_t* pixel=&image_.pixels[(size_t(row)*image_.width+col)*4];double a=pixel[3]/255.;
            for(int c=0;c<3;++c)rgb[c]+=k*a*pixel[c];alpha+=k*a;weight+=k;
          }
        }
        const double a=std::clamp(alpha/std::max(.001,weight),0.,1.);
        for(int c=0;c<3;++c)out[c]=uint8_t(std::clamp(rgb[c]/std::max(.001,weight)+(1-a)*25,0.,255.));
        if(mask){const int mx=std::clamp(int(std::lround(sx)),0,image_.width-1),my=std::clamp(int(std::lround(sy)),0,image_.height-1);
          const size_t i=size_t(my)*image_.width+mx;
          if(mask->pixels[i]){double tint=projected?(((mx+my)/6)%2?.13:.48):.45;
            out[0]=uint8_t(out[0]*(1-tint)+(projected?30:230)*tint);
            out[1]=uint8_t(out[1]*(1-tint)+(projected?185:55)*tint);
            out[2]=uint8_t(out[2]*(1-tint)+(projected?245:55)*tint);
            if(boundary&&(mx==0||my==0||mx+1==image_.width||my+1==image_.height||!mask->pixels[i-1]||!mask->pixels[i+1]||!mask->pixels[i-image_.width]||!mask->pixels[i+image_.width])){
              out[0]=projected?45:255;out[1]=projected?235:235;out[2]=projected?255:40;
            }
          }
        }
    }
    if(guideMode_&&mask&&idx<(int)guides_.size()){
        const RECT r=guides_[idx];const double angle=guideAngles_[idx];
        auto transform=[&](Vec2 v){return Vec2{(v.x-image_.width*.5-panX_)*scale+w*.5,(v.y-image_.height*.5-panY_)*scale+h*.5};};
        auto dot=[&](int x,int y){if(x<0||y<0||x>=w||y>=h)return;auto* q=&preview.pixels[(size_t(y)*w+x)*4];q[0]=0;q[1]=235;q[2]=255;q[3]=255;};
        auto line=[&](Vec2 p,Vec2 q){int n=std::max(1,int(std::hypot(q.x-p.x,q.y-p.y)));for(int j=0;j<=n;++j)for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)dot(int(std::lround(p.x+(q.x-p.x)*j/n))+dx,int(std::lround(p.y+(q.y-p.y)*j/n))+dy);};
        const double hw=(r.right-r.left)*.5,hh=(r.bottom-r.top)*.5;
        Vec2 corners[4]={transform(worldPoint(r,angle,-hw,-hh)),transform(worldPoint(r,angle,hw,-hh)),transform(worldPoint(r,angle,hw,hh)),transform(worldPoint(r,angle,-hw,hh))};
        for(int j=0;j<4;++j)line(corners[j],corners[(j+1)%4]);
        int radius=5;for(int j=0;j<3;++j)for(int k=0;k<3;++k){Vec2 q=transform(worldPoint(r,angle,(j-1)*hw,(k-1)*hh));for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx)dot(int(std::lround(q.x))+dx,int(std::lround(q.y))+dy);}
        Vec2 stem=transform(worldPoint(r,angle,0,-hh)),handle=transform(worldPoint(r,angle,0,-hh-std::max(18.0,hh*.22)));line(stem,handle);
        for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx)if(dx*dx+dy*dy<=radius*radius)dot(int(std::lround(handle.x))+dx,int(std::lround(handle.y))+dy);
    }
    renderer_.setImage(preview);
}

void App::resetGuides(){
    guides_.clear();guideAngles_.clear();guideEdited_.clear();
    for(const auto& item:analysis_.masks){
        RECT r{image_.width,image_.height,0,0};
        const Mask& mask=item.mask;
        if(!mask.empty()&&mask.width==image_.width&&mask.height==image_.height)
            for(int y=0;y<mask.height;++y)for(int x=0;x<mask.width;++x)if(mask.pixels[size_t(y)*mask.width+x]){
                r.left=std::min<int>(r.left,x);r.top=std::min<int>(r.top,y);r.right=std::max<int>(r.right,x+1);r.bottom=std::max<int>(r.bottom,y+1);
            }
        if(r.right<=r.left||r.bottom<=r.top)r={0,0,image_.width,image_.height};
        else {const int px=std::max<int>(4,(r.right-r.left)/12),py=std::max<int>(4,(r.bottom-r.top)/12);r.left=std::max<int>(0,r.left-px);r.right=std::min<int>(image_.width,r.right+px);r.top=std::max<int>(0,r.top-py);r.bottom=std::min<int>(image_.height,r.bottom+py);}
        guides_.push_back(r);guideAngles_.push_back(0);guideEdited_.push_back(false);
        for(const auto& guide:analysis_.appliedGuides)if(guide.label==trainingGuideSafeLabel(item.name)){
            guides_.back()={guide.left,guide.top,guide.right,guide.bottom};
            guideAngles_.back()=guide.angleDegrees*pi/180.0;
        }
    }
}

POINT App::viewToImage(POINT p) const {
    RECT rc{};GetClientRect(view_,&rc);
    if(image_.empty())return {0,0};
    const double scale=std::min(double(std::max(1,int(rc.right)))/image_.width,double(std::max(1,int(rc.bottom)))/image_.height)*zoom_;
    const int margin=std::max(image_.width,image_.height);
    return {std::clamp(int((p.x+.5-rc.right*.5)/scale+image_.width*.5+panX_),-margin,image_.width+margin),
            std::clamp(int((p.y+.5-rc.bottom*.5)/scale+image_.height*.5+panY_),-margin,image_.height+margin)};
}
void App::clampPreviewPan(){
    if(image_.empty())return;
    if(zoom_<=1.000001){zoom_=1;panX_=panY_=0;return;}
    RECT rc{};GetClientRect(view_,&rc);
    const double fitted=std::min(double(std::max(1,int(rc.right)))/image_.width,
                                 double(std::max(1,int(rc.bottom)))/image_.height);
    const double scale=fitted*zoom_;
    const double limitX=std::max(0.,(image_.width-double(rc.right)/scale)*.5);
    const double limitY=std::max(0.,(image_.height-double(rc.bottom)/scale)*.5);
    panX_=std::clamp(panX_,-limitX,limitX);
    panY_=std::clamp(panY_,-limitY,limitY);
}
void App::zoomPreview(double factor,POINT* anchor){
    if(image_.empty())return;
    RECT rc{};GetClientRect(view_,&rc);const double old=zoom_;
    zoom_=std::clamp(zoom_*factor,1.,16.);
    if(anchor&&zoom_!=old){
        const double fitted=std::min(double(std::max(1,int(rc.right)))/image_.width,
                                     double(std::max(1,int(rc.bottom)))/image_.height);
        panX_+=(anchor->x-rc.right*.5)/fitted*(1/old-1/zoom_);
        panY_+=(anchor->y-rc.bottom*.5)/fitted*(1/old-1/zoom_);
    }
    clampPreviewPan();
    updatePreview();
}

LRESULT App::handleView(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_CAPTURECHANGED){dragMode_=0;panning_=false;return 0;}
    if(!image_.empty()&&(m==WM_MBUTTONDOWN||(m==WM_LBUTTONDOWN&&!guideMode_))){
        panning_=true;panButton_=m==WM_MBUTTONDOWN?WM_MBUTTONUP:WM_LBUTTONUP;
        panStart_={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        panOriginX_=panX_;panOriginY_=panY_;
        SetCapture(h);SetCursor(LoadCursor(nullptr,IDC_SIZEALL));return 0;
    }
    if(panning_&&m==WM_MOUSEMOVE){
        RECT rc{};GetClientRect(view_,&rc);
        const double scale=std::min(double(std::max(1,int(rc.right)))/image_.width,
                                    double(std::max(1,int(rc.bottom)))/image_.height)*zoom_;
        panX_=panOriginX_-(GET_X_LPARAM(l)-panStart_.x)/scale;
        panY_=panOriginY_-(GET_Y_LPARAM(l)-panStart_.y)/scale;
        clampPreviewPan();updatePreview();return 0;
    }
    if(panning_&&m==panButton_){panning_=false;if(GetCapture()==h)ReleaseCapture();SetCursor(LoadCursor(nullptr,IDC_ARROW));return 0;}
    if(m==WM_MOUSEWHEEL&&!(guideMode_&&(GET_KEYSTATE_WPARAM(w)&MK_SHIFT))){
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&p);
        zoomPreview(GET_WHEEL_DELTA_WPARAM(w)>0?1.25:.8,&p);return 0;
    }
    const int idx=selectedMask();if(!guideMode_||image_.empty()||idx<0||idx>=(int)guides_.size())return 0;
    if(m==WM_MOUSEWHEEL){brushRadius_=std::clamp(brushRadius_+(GET_WHEEL_DELTA_WPARAM(w)>0?1:-1),1,64);
        setStatus(L"Paint brush radius: "+std::to_wstring(brushRadius_)+L" image pixels. Shift+drag adds; Ctrl+drag erases.");return 0;}
    POINT p=viewToImage({GET_X_LPARAM(l),GET_Y_LPARAM(l)});
    if(m==WM_LBUTTONDOWN){
        if(GetKeyState(VK_SHIFT)&0x8000){dragMode_=128;dragMask_=idx;brushPrevious_=p;paintMask(p,idx,255);SetCapture(h);updatePreview();return 0;}
        if(GetKeyState(VK_CONTROL)&0x8000){dragMode_=256;dragMask_=idx;brushPrevious_=p;paintMask(p,idx,0);SetCapture(h);updatePreview();return 0;}
        RECT r=guides_[idx];dragOriginal_=r;dragStart_=p;dragMask_=idx;dragAngle_=guideAngles_[idx];
        const int e=std::max<int>(5,std::min<int>(image_.width,image_.height)/70);
        const double hw=(r.right-r.left)*.5,hh=(r.bottom-r.top)*.5;
        Vec2 local=localPoint(r,dragAngle_,p.x,p.y);
        const double rotY=-hh-std::max(18.0,hh*.22);
        int edges=0;
        if((std::hypot(local.x,local.y-rotY)<=e*2)||((GetKeyState(VK_MENU)&0x8000)&&withinGuide(r,dragAngle_,p.x,p.y)))dragMode_=64;
        else {
            if(std::abs(local.x+hw)<=e&&std::abs(local.y)<=hh+e)edges|=1;
            else if(std::abs(local.x-hw)<=e&&std::abs(local.y)<=hh+e)edges|=2;
            if(std::abs(local.y+hh)<=e&&std::abs(local.x)<=hw+e)edges|=4;
            else if(std::abs(local.y-hh)<=e&&std::abs(local.x)<=hw+e)edges|=8;
            if(edges)dragMode_=edges;
            else if(withinGuide(r,dragAngle_,p.x,p.y))dragMode_=16;
            else {dragMode_=32;dragAngle_=guideAngles_[idx]=0;dragOriginal_={std::clamp<int>(p.x,0,image_.width-1),std::clamp<int>(p.y,0,image_.height-1),0,0};dragOriginal_.right=dragOriginal_.left+1;dragOriginal_.bottom=dragOriginal_.top+1;guides_[idx]=dragOriginal_;}
        }
        SetCapture(h);return 0;
    }
    if(m==WM_MOUSEMOVE&&dragMode_&&GetCapture()==h&&dragMask_==idx){
        if(dragMode_==128||dragMode_==256){
            const int steps=std::max(1,int(std::hypot(double(p.x-brushPrevious_.x),double(p.y-brushPrevious_.y))/std::max(1,brushRadius_/2)));
            const POINT from=brushPrevious_;
            for(int i=1;i<=steps;++i){POINT q{from.x+(p.x-from.x)*i/steps,from.y+(p.y-from.y)*i/steps};paintMask(q,idx,dragMode_==128?255:0);}
            brushPrevious_=p;updatePreview();return 0;
        }
        const int dx=p.x-dragStart_.x,dy=p.y-dragStart_.y;RECT r=dragOriginal_;
        if(dragMode_==64){const double cx=(r.left+r.right)*.5,cy=(r.top+r.bottom)*.5;
            const double start=std::atan2(dragStart_.y-cy,dragStart_.x-cx),now=std::atan2(p.y-cy,p.x-cx);
            double angle=dragAngle_+now-start;while(angle>pi)angle-=2*pi;while(angle< -pi)angle+=2*pi;
            guideAngles_[idx]=angle;guideEdited_[idx]=true;updatePreview();return 0;}
        if(dragMode_==16){r.left=std::clamp<int>(r.left+dx,0,image_.width-(r.right-r.left));r.right=r.left+(dragOriginal_.right-dragOriginal_.left);
            r.top=std::clamp<int>(r.top+dy,0,image_.height-(r.bottom-r.top));r.bottom=r.top+(dragOriginal_.bottom-dragOriginal_.top);}
        else if(dragMode_==32){r.left=std::clamp<int>(std::min<int>(dragStart_.x,p.x),0,image_.width-1);r.right=std::clamp<int>(std::max<int>(dragStart_.x,p.x)+1,r.left+1,image_.width);r.top=std::clamp<int>(std::min<int>(dragStart_.y,p.y),0,image_.height-1);r.bottom=std::clamp<int>(std::max<int>(dragStart_.y,p.y)+1,r.top+1,image_.height);}
        else {
            const double c=std::cos(dragAngle_),s=std::sin(dragAngle_);
            const int lx=int(std::lround(dx*c+dy*s)),ly=int(std::lround(-dx*s+dy*c));
            int left=0,right=r.right-r.left,top=0,bottom=r.bottom-r.top;
            if(dragMode_&1)left=std::min<int>(right-1,lx);
            if(dragMode_&2)right=std::max<int>(left+1,right+lx);
            if(dragMode_&4)top=std::min<int>(bottom-1,ly);
            if(dragMode_&8)bottom=std::max<int>(top+1,bottom+ly);
            const double mx=(left+right-(dragOriginal_.right-dragOriginal_.left))*.5,my=(top+bottom-(dragOriginal_.bottom-dragOriginal_.top))*.5;
            const int cx=int(std::lround((dragOriginal_.left+dragOriginal_.right)*.5+mx*c-my*s));
            const int cy=int(std::lround((dragOriginal_.top+dragOriginal_.bottom)*.5+mx*s+my*c));
            const int newW=right-left,newH=bottom-top;
            r={cx-newW/2,cy-newH/2,cx-newW/2+newW,cy-newH/2+newH};
        }
        guides_[idx]=r;guideEdited_[idx]=true;updatePreview();return 0;
    }
    if(m==WM_LBUTTONUP){if(GetCapture()==h)ReleaseCapture();dragMode_=0;return 0;}
    return 0;
}

void App::applyGuide(){
    const int idx=selectedMask();if(image_.empty()||idx<0||idx>=(int)guides_.size()){showError(L"Mask area",L"Analyse an image and select a mask first.");return;}
    if(!guideEdited_[idx]){setStatus(L"Move, resize or rotate the area before applying it.");return;}
    const RECT r=guides_[idx];
    auto& mask=analysis_.masks[idx].mask;
    if(mask.empty()||mask.width!=image_.width||mask.height!=image_.height){showError(L"Mask area",L"Selected mask dimensions do not match the image.");return;}
    try {
        const auto dir=exeDir()/L"TrainingGuides";std::filesystem::create_directories(dir);
        SYSTEMTIME t{};GetLocalTime(&t);static unsigned serial=0;
        wchar_t timestamp[80]{};swprintf(timestamp,80,L"%04u%02u%02u_%02u%02u%02u_%03u_%u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,++serial);
        const auto stem=std::wstring(timestamp)+L"_"+widen(safeFileName(analysis_.masks[idx].name));
        const auto imagePath=dir/(stem+L"_image.png"),maskPath=dir/(stem+L"_mask.png"),jsonPath=dir/(stem+L"_guide.json");
        Mask original=mask;Mask all{image_.width,image_.height,std::vector<uint8_t>(size_t(image_.width)*image_.height,255)};
        saveCutoutPngWic(imagePath,image_,all);SegmentationEngine::exportMaskPng(maskPath,original);
        std::ofstream record(jsonPath,std::ios::binary);if(!record)throw std::runtime_error("Could not save area metadata");
        const auto fingerprint=trainingImageSha256(image_);
        if(fingerprint.empty())throw std::runtime_error("Could not fingerprint loaded image");
        record<<"{\"schema\":2,\"label\":\""<<safeFileName(analysis_.masks[idx].name)<<"\",\"image_sha256\":\""<<fingerprint<<"\",\"image\":\""<<imagePath.filename().string()<<"\",\"prediction\":\""<<maskPath.filename().string()
              <<"\",\"width\":"<<image_.width<<",\"height\":"<<image_.height<<",\"area\":["<<r.left<<","<<r.top<<","<<r.right<<","<<r.bottom<<"],\"rotation_degrees\":"<<std::setprecision(10)<<(guideAngles_[idx]*180/pi)<<",\"rotation_origin\":["<<(r.left+r.right)*.5<<","<<(r.top+r.bottom)*.5<<"],\"annotation\":\"rough_bounds_only\",\"pixels_inside_verified\":false}\n";
        if(!record)throw std::runtime_error("Could not complete area metadata");
        for(int y=0;y<mask.height;++y)for(int x=0;x<mask.width;++x)if(mask.pixels[size_t(y)*mask.width+x]&&!withinGuide(r,guideAngles_[idx],x+.5,y+.5))mask.pixels[size_t(y)*mask.width+x]=0;
        guideEdited_[idx]=false;showRaw_=false;SendMessageW(refinedView_,BM_SETCHECK,BST_CHECKED,0);SendMessageW(rawView_,BM_SETCHECK,BST_UNCHECKED,0);
        updatePreview();setStatus(L"Area applied to selected mask. Rough guide saved; no model retraining has occurred.");
    }catch(const std::exception& e){showError(L"Mask area",widen(e.what()));}
}

void App::paintMask(POINT p,int index,int value){
    if(index<0||index>=int(analysis_.masks.size()))return;
    Mask& mask=analysis_.masks[index].mask;
    if(mask.empty()||mask.width!=image_.width||mask.height!=image_.height)return;
    const int r=brushRadius_;
    for(int y=std::max(0,int(p.y)-r);y<=std::min(mask.height-1,int(p.y)+r);++y)
        for(int x=std::max(0,int(p.x)-r);x<=std::min(mask.width-1,int(p.x)+r);++x)
            if((x-p.x)*(x-p.x)+(y-p.y)*(y-p.y)<=r*r&&image_.pixels[(size_t(y)*mask.width+x)*4+3])
                mask.pixels[size_t(y)*mask.width+x]=static_cast<uint8_t>(value);
    showRaw_=false;
    SendMessageW(refinedView_,BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(rawView_,BM_SETCHECK,BST_UNCHECKED,0);
}

void App::createMissingMask(){
    if(analysing_||image_.empty()){setStatus(L"Analyse an image before creating a missing mask.");return;}
    HTREEITEM item=TreeView_GetSelection(maskList_);
    if(!item){setStatus(L"Select a missing part marked (no detector) in the list first.");return;}
    TVITEMW selected{};wchar_t label[256]{};selected.hItem=item;
    selected.mask=TVIF_TEXT|TVIF_PARAM;selected.pszText=label;selected.cchTextMax=256;
    if(!TreeView_GetItem(maskList_,&selected)||selected.lParam>0){setStatus(L"Select a missing part marked (no detector) first.");return;}
    std::wstring suffix=L" (no detector)",leaf(label);
    if(!leaf.ends_with(suffix)){setStatus(L"Select a missing part marked (no detector) first.");return;}
    leaf.erase(leaf.size()-suffix.size());std::vector<std::wstring> parts{leaf};
    for(HTREEITEM parent=TreeView_GetParent(maskList_,item);parent;parent=TreeView_GetParent(maskList_,parent)){
        TVITEMW node{};wchar_t text[256]{};node.mask=TVIF_TEXT;node.hItem=parent;node.pszText=text;node.cchTextMax=256;
        if(TreeView_GetItem(maskList_,&node))parts.push_back(text);
    }
    std::reverse(parts.begin(),parts.end());std::wstring path;
    for(const auto& part:parts){if(!path.empty())path+=L"/";path+=part;}
    const int bytes=WideCharToMultiByte(CP_UTF8,0,path.c_str(),int(path.size()),nullptr,0,nullptr,nullptr);
    if(bytes<=0)return;std::string name(bytes,0);
    WideCharToMultiByte(CP_UTF8,0,path.c_str(),int(path.size()),name.data(),bytes,nullptr,nullptr);
    if(std::any_of(analysis_.masks.begin(),analysis_.masks.end(),[&](const NamedMask& m){return m.name==name;}))return;
    NamedMask newMask;newMask.name=name;const auto slash=name.rfind('/');
    newMask.parent=slash==std::string::npos?"":name.substr(0,slash);
    newMask.category="User annotation";newMask.source="blank in-app mask; no trained detector";
    newMask.confidence=0;newMask.provenance=MaskProvenance::LandmarkDerived;
    newMask.mask={image_.width,image_.height,std::vector<uint8_t>(size_t(image_.width)*image_.height)};
    analysis_.masks.push_back(newMask);analysis_.rawMasks.push_back(std::move(newMask));
    guides_.push_back({0,0,image_.width,image_.height});guideAngles_.push_back(0);guideEdited_.push_back(false);
    selected.lParam=static_cast<LPARAM>(analysis_.masks.size());
    selected.pszText=leaf.data();selected.cchTextMax=int(leaf.size()+1);
    SetWindowTextW(guideBtn_,L"Edit mask area");
    TreeView_SetItem(maskList_,&selected);
    guideMode_=true;SendMessageW(guideBtn_,BM_SETCHECK,BST_CHECKED,0);
    updatePreview();setStatus(L"Blank mask created. Shift+drag adds pixels, Ctrl+drag erases; mouse wheel adjusts brush. Approve after correction.");
}

void App::approveMask(){
    if(analysing_||image_.empty())return;
    const int idx=selectedMask();if(idx<0){setStatus(L"Select a corrected mask before approving it for learning.");return;}
    const NamedMask& chosen=analysis_.masks[idx];
    if(chosen.provenance==MaskProvenance::OcclusionProjected){
        setStatus(L"Hidden-surface projections cannot train the detector; unseen pixels are not verified labels.");return;
    }
    std::string ancestor=chosen.parent;
    const NamedMask* parent=nullptr;
    while(!ancestor.empty()){
        auto found=std::find_if(analysis_.masks.begin(),analysis_.masks.end(),[&](const NamedMask& m){return m.name==ancestor&&!m.mask.empty()&&std::any_of(m.mask.pixels.begin(),m.mask.pixels.end(),[](uint8_t p){return p!=0;});});
        if(found!=analysis_.masks.end()){parent=&*found;break;}
        const auto slash=ancestor.rfind('/');if(slash==std::string::npos)break;ancestor.resize(slash);
    }
    if(!parent){setStatus(L"A detected parent region is required to train this mask on other images.");return;}
    try{
        NamedMask example=chosen;example.parent=parent->name;
        const Mask& raw=(idx<int(analysis_.rawMasks.size()))?analysis_.rawMasks[idx].mask:chosen.mask;
        const auto outcome=approveLearningSample(exeDir(),image_,example,raw,parent->mask);
        setStatus(widen(outcome.message));
    }catch(const std::exception& e){showError(L"Learning sample",widen(e.what()));}
}

std::string App::safeFileName(std::string s) {
    for (char& c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c=='-' || c=='_')) c = '_';
    return s;
}

void App::exportSelected() {
    int idx = selectedMask();
    if (idx < 0 || idx >= static_cast<int>(analysis_.masks.size())) { showError(L"Export", L"Select a mask first."); return; }
    const auto exportsDir = exeDir() / L"Exports";
    std::filesystem::create_directories(exportsDir);
    const bool projected=analysis_.masks[idx].provenance==MaskProvenance::OcclusionProjected;
    const auto defaultBase = safeFileName(analysis_.masks[idx].name) + (projected?"_projected.png":".png");
    const auto defaultPath = exportsDir / widen(defaultBase);
    wchar_t file[MAX_PATH]{};
    wcsncpy_s(file, MAX_PATH, defaultPath.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"PNG mask\0*.png\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png"; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    try {
        const auto p = std::filesystem::path(file);
        const auto& sourceMasks=(showRaw_&&analysis_.rawMasks.size()==analysis_.masks.size())?analysis_.rawMasks:analysis_.masks;
        SegmentationEngine::exportMaskPng(p, sourceMasks[idx].mask);
        if (!projected&&SendMessageW(cutoutCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED)
            saveCutoutPngWic(p.parent_path() / (p.stem().wstring() + L"_cutout.png"), image_, sourceMasks[idx].mask);
        auto json = p; json.replace_extension(L".boundary.json");
        SegmentationEngine::exportContoursJson(json, sourceMasks[idx].mask, analysis_.masks[idx].name);
        exportCompoundMask(p.parent_path()/(safeFileName(analysis_.masks[idx].name)+"_components"),analysis_.masks[idx].name,sourceMasks[idx].mask,analysis_.masks[idx].source,analysis_.masks[idx].confidence);
        setStatus(projected?L"Exported estimated 2D projection mask and boundary; no source-image cutout.":
            SendMessageW(cutoutCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? L"Exported mask, transparent cutout, boundary and individual components."
            : L"Exported mask, boundary and individual components.");
    } catch (const std::exception& e) { showError(L"Export failed", widen(e.what())); }
}

void App::exportAll() {
    if (analysis_.masks.empty()) { showError(L"Export", L"Analyse an image first."); return; }
    try {
        const std::filesystem::path exportsDir = exeDir() / L"Exports";
        std::filesystem::create_directories(exportsDir);
        std::wstring imageStem = loadedImagePath_.empty() ? L"image" : loadedImagePath_.stem().wstring();
        for (auto& c : imageStem) if (c==L'<'||c==L'>'||c==L':'||c==L'"'||c==L'/'||c==L'\\'||c==L'|'||c==L'?'||c==L'*') c=L'_';
        if(imageStem.empty()) imageStem=L"image";
        const auto stage = exportsDir / (L"." + imageStem + L"_export_pending");
        const auto zipPath = exportsDir / (imageStem + L".zip");
        const auto pendingZip = exportsDir / (imageStem + L".pending.zip");
        std::error_code ignored;
        std::filesystem::remove_all(stage,ignored);
        std::filesystem::remove(pendingZip,ignored);
        std::filesystem::create_directories(stage);
        struct ExportEntry { std::string name; Mask mask; bool projected=false; };
        std::vector<ExportEntry> out;
        std::unordered_map<std::string,size_t> byFile;
        for (const auto& m : analysis_.masks) {
            const auto base = safeFileName(m.name);
            auto it=byFile.find(base);
            if(it==byFile.end()){byFile.emplace(base,out.size());out.push_back({m.name,m.mask,m.provenance==MaskProvenance::OcclusionProjected});}
            else {
                auto& dst=out[it->second].mask;
                if(dst.width==m.mask.width&&dst.height==m.mask.height)
                    for(size_t p=0;p<dst.pixels.size();++p) dst.pixels[p]|=m.mask.pixels[p];
            }
        }
        const bool cutouts = SendMessageW(cutoutCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        for (const auto& e : out) {
            const auto name = safeFileName(e.name);
            SegmentationEngine::exportMaskPng(stage / (name + ".png"), e.mask);
            if (cutouts&&!e.projected) saveCutoutPngWic(stage / (name + "_cutout.png"), image_, e.mask);
        }
        auto psQuote=[](std::wstring v){size_t p=0;while((p=v.find(L'\'',p))!=std::wstring::npos){v.replace(p,1,L"''");p+=2;}return L"'"+v+L"'";};
        const auto sourcePattern=(stage/L"*").wstring();
        const std::wstring command=L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Compress-Archive -Path " +
            psQuote(sourcePattern) + L" -DestinationPath " + psQuote(pendingZip.wstring()) + L" -CompressionLevel Optimal -Force\"";
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmd(command.begin(),command.end());cmd.push_back(0);
        if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))
            throw std::runtime_error("Could not start Windows PowerShell ZIP compressor");
        WaitForSingleObject(pi.hProcess,INFINITE);DWORD exitCode=1;GetExitCodeProcess(pi.hProcess,&exitCode);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        if(exitCode!=0||!std::filesystem::exists(pendingZip)) throw std::runtime_error("ZIP compression failed");
        std::filesystem::remove(zipPath,ignored);
        if(!MoveFileExW(pendingZip.c_str(),zipPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not finish export ZIP");
        std::filesystem::remove_all(stage,ignored);
        setStatus(L"Exported current image to " + zipPath.wstring() + L" using maximum ZIP compression; all mask files are contained inside the archive.");
    } catch (const std::exception& e) { showError(L"Export failed", widen(e.what())); }
}

void App::exportPoseGifs(){
    if(analysing_||image_.empty()||analysis_.masks.empty()){
        setStatus(L"Analyse an image before exporting pose GIFs.");return;
    }
    std::vector<std::string> subjects;
    for(const auto& m:analysis_.masks){
        if(m.provenance==MaskProvenance::OcclusionProjected)continue;
        const bool human=m.name.rfind("People/Human/Person ",0)==0&&m.name.find('/',20)==std::string::npos;
        const bool animal=m.name.rfind("Animals/Animal ",0)==0&&m.name.find('/',15)==std::string::npos;
        if(human||animal)subjects.push_back(m.name);
    }
    if(subjects.empty()){setStatus(L"No detected human or animal silhouette is available for a GIF.");return;}
    const auto folder=exeDir()/L"Exports"/L"PoseGIFs";
    std::filesystem::create_directories(folder);
    std::filesystem::path chosen;
    if(subjects.size()==1){
        wchar_t filename[4096]{};
        const auto suggested=widen(safeFileName(subjects.front())+"_30s_poses.gif");
        wcsncpy_s(filename,suggested.c_str(),_TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=hwnd_;
        dialog.lpstrFilter=L"Animated GIF (*.gif)\0*.gif\0All files (*.*)\0*.*\0";
        dialog.lpstrFile=filename;dialog.nMaxFile=4096;dialog.lpstrInitialDir=folder.c_str();
        dialog.lpstrDefExt=L"gif";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
        if(!GetSaveFileNameW(&dialog))return;
        chosen=filename;
    }
    EnableWindow(poseGifBtn_,FALSE);
    SetCursor(LoadCursor(nullptr,IDC_WAIT));
    size_t completed=0,approximate=0;std::string errors;
    for(const auto& subject:subjects){
        setStatus(L"Rendering 30-second pose GIF for "+widen(subject)+L"...");
        UpdateWindow(hwnd_);
        const auto dest=chosen.empty()?folder/widen(safeFileName(subject)+"_30s_poses.gif"):chosen;
        const auto temp=std::filesystem::path(dest.wstring()+L".pending");
        try{
            const auto result=exportPoseGif(temp,image_,analysis_.masks,subject);
            if(result.frames!=240||result.durationCentiseconds!=3000)
                throw std::runtime_error("GIF frame count or duration is incorrect");
            if(!MoveFileExW(temp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Could not finish GIF in Exports/PoseGIFs");
            ++completed;approximate+=result.approximateLimbs;
        }catch(const std::exception& e){
            std::error_code ignored;std::filesystem::remove(temp,ignored);
            if(!errors.empty())errors+="; ";errors+=subject+": "+e.what();
        }
    }
    SetCursor(LoadCursor(nullptr,IDC_ARROW));EnableWindow(poseGifBtn_,TRUE);
    if(!errors.empty())showError(L"Pose GIF export",widen(errors));
    if(completed)setStatus(L"Saved "+std::to_wstring(completed)+L" transparent 1280 x 720, 30-second GIF(s) to "+(chosen.empty()?folder.wstring():chosen.wstring())+L". " +
        (approximate?std::to_wstring(approximate)+L" used approximate limb motion.":L""));
    else setStatus(L"No pose GIF could be exported.");
}

void App::updateFaceModelStatus() {
    const auto p=exeDir()/L"models"/L"face_parsing.onnx";
    std::wstring t=std::filesystem::exists(p)?L"Face model: installed":L"Face model: absent";
    t+=std::filesystem::exists(exeDir()/L"models"/L"detailed_face_teeth.onnx")?L" | Detailed face: installed":L" | Detailed face: absent";
    if(faceModelStatus_) SetWindowTextW(faceModelStatus_,t.c_str());
}

static std::string sha256FileHex(const std::filesystem::path& p) {
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; DWORD objLen=0,cb=0,hashLen=0;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)!=0) throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&objLen,sizeof(objLen),&cb,0);
    BCryptGetProperty(alg,BCRYPT_HASH_LENGTH,(PUCHAR)&hashLen,sizeof(hashLen),&cb,0);
    std::vector<UCHAR> obj(objLen),digest(hashLen);
    if(BCryptCreateHash(alg,&hash,obj.data(),objLen,nullptr,0,0)!=0){BCryptCloseAlgorithmProvider(alg,0);throw std::runtime_error("BCryptCreateHash failed");}
    std::ifstream f(p,std::ios::binary); if(!f) throw std::runtime_error("Cannot open downloaded model");
    std::array<char,1<<20> buf{}; while(f){f.read(buf.data(),buf.size()); auto n=f.gcount(); if(n>0&&BCryptHashData(hash,(PUCHAR)buf.data(),(ULONG)n,0)!=0) throw std::runtime_error("SHA-256 update failed");}
    if(BCryptFinishHash(hash,digest.data(),hashLen,0)!=0) throw std::runtime_error("SHA-256 finish failed");
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg,0);
    std::ostringstream os; os<<std::hex<<std::setfill('0'); for(auto b:digest) os<<std::setw(2)<<(unsigned)b; return os.str();
}

static void downloadHttpsFile(const std::wstring& host,const std::wstring& path,const std::filesystem::path& out) {
    HINTERNET ses=WinHttpOpen(L"VulkanImageMaskStudio/0.4.2",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!ses) throw std::runtime_error("WinHttpOpen failed");
    DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS; WinHttpSetOption(ses,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy));
    HINTERNET con=WinHttpConnect(ses,host.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0); if(!con){WinHttpCloseHandle(ses);throw std::runtime_error("WinHttpConnect failed");}
    HINTERNET req=WinHttpOpenRequest(con,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE); if(!req){WinHttpCloseHandle(con);WinHttpCloseHandle(ses);throw std::runtime_error("WinHttpOpenRequest failed");}
    if(!WinHttpSendRequest(req,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(req,nullptr)){WinHttpCloseHandle(req);WinHttpCloseHandle(con);WinHttpCloseHandle(ses);throw std::runtime_error("HTTPS request failed");}
    DWORD status=0,len=sizeof(status); WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&len,WINHTTP_NO_HEADER_INDEX); if(status<200||status>=300){WinHttpCloseHandle(req);WinHttpCloseHandle(con);WinHttpCloseHandle(ses);throw std::runtime_error("HTTP download failed");}
    std::ofstream f(out,std::ios::binary|std::ios::trunc); if(!f) throw std::runtime_error("Cannot create model .part file");
    std::vector<char> b(1<<20); for(;;){DWORD got=0;if(!WinHttpReadData(req,b.data(),(DWORD)b.size(),&got))throw std::runtime_error("Download read failed");if(!got)break;f.write(b.data(),got);if(!f)throw std::runtime_error("Model write failed");}
    f.close(); WinHttpCloseHandle(req);WinHttpCloseHandle(con);WinHttpCloseHandle(ses);
}

void App::downloadFaceModel() {
    if (downloadingFaceModel_) return;
    const auto script = exeDir() / L"download_models.ps1";
    if (!std::filesystem::exists(script)) {
        showError(L"Model downloader", L"download_models.ps1 is missing beside the application.");
        return;
    }
    downloadingFaceModel_ = true;
    EnableWindow(faceModelBtn_, FALSE);
    setStatus(L"Running model downloader... A PowerShell window shows per-model progress.");
    modelWorker_ = std::jthread([this,script](std::stop_token st) {
        bool ok=false;
        const auto launchLog = exeDir() / L"model_downloader_launch.log";
        { std::wofstream lf(launchLog, std::ios::trunc); lf << L"Starting PowerShell downloader. Script: " << script.wstring() << L"\\n"; }
        std::wstring cmd=L"& { & '" + script.wstring() + L"' *>&1 | Tee-Object -FilePath '" + (exeDir()/L"model_downloader_console.log").wstring() + L"'; exit $LASTEXITCODE }";
        std::wstring args=L"-NoProfile -ExecutionPolicy Bypass -Command \"" + cmd + L"\"";
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask=SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb=L"open";
        sei.lpFile=L"powershell.exe";
        sei.lpParameters=args.c_str();
        const auto launchDirectory=exeDir().wstring();
        sei.lpDirectory=launchDirectory.c_str();
        sei.nShow=SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            while (!st.stop_requested()) {
                DWORD wr=WaitForSingleObject(sei.hProcess,250);
                if (wr==WAIT_OBJECT_0) { DWORD code=1; GetExitCodeProcess(sei.hProcess,&code); ok=(code==0); break; }
            }
            CloseHandle(sei.hProcess);
        }
        if (!st.stop_requested()) PostMessageW(hwnd_,WM_FACE_MODEL_DONE,ok?1:0,0);
    });
}

void App::faceModelDone(bool ok) {
    downloadingFaceModel_=false; EnableWindow(faceModelBtn_,TRUE); updateFaceModelStatus();
    if(ok){ setStatus(L"Model download pass completed. Installed models are available to the analysis pipeline."); MessageBoxW(hwnd_,L"The model download pass completed. Check the PowerShell output for any optional models that need retrying.",L"Model pack installed",MB_OK|MB_ICONINFORMATION); }
    else { setStatus(L"Model pack download/verification failed."); showError(L"Model pack",L"The model downloader could not complete its required models. See model_downloader_launch.log, model_downloader_console.log and model_download_report.txt beside the application."); }
}
