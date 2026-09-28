#include "App.h"
#include <windows.h>
#include "CrashLog.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    CrashLog::initialize();
    CrashLog::write("Application startup");
    int rc=1;
    try { App app(hInstance); rc=app.run(nCmdShow); }
    catch(const std::exception& e){ CrashLog::write(std::string("Top-level C++ exception: ")+e.what()); MessageBoxA(nullptr,e.what(),"Vulkan Image Mask Studio fatal error",MB_OK|MB_ICONERROR); }
    catch(...){ CrashLog::write("Top-level unknown C++ exception"); }
    CrashLog::shutdownClean();
    return rc;
}
