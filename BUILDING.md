# Building Vulkan Image Mask Studio 0.4.12.12

The tested output is a Windows x64 executable built with LLVM-MinGW's C++20 Clang compiler. The repository does not include compiler binaries, the Vulkan SDK, ONNX Runtime headers, or ONNX model weights.

## Prerequisites

- LLVM-MinGW x86_64 for Windows (with `x86_64-w64-mingw32-clang++.exe`, `libc++.dll`, and `libunwind.dll`). The current release was built with LLVM-MinGW 20260908 UCRT. Other versions may work but are untested.
- Vulkan SDK for Windows x64, with `Include/vulkan/vulkan.h` and `Lib/vulkan-1.lib`. The release used Vulkan headers 1.4.357.
- ONNX Runtime Windows x64 **development** package with `include/onnxruntime_c_api.h`; the release compiled against version 1.30.0 headers. Choose a runtime `onnxruntime.dll` compatible with those headers and the desired execution provider.
- PowerShell. A Vulkan-capable driver is needed to run the compiled app, and ONNX model weights are needed to analyse images.

Set these three environment variables in a PowerShell window to the folders you installed:

```powershell
$env:LLVM_MINGW_ROOT = 'C:\path\to\llvm-mingw'
$env:VULKAN_SDK = 'C:\VulkanSDK\1.4.357.0'
$env:ONNXRUNTIME_ROOT = 'C:\path\to\onnxruntime-win-x64-1.30.0'
.\build.ps1
```

`build.ps1` checks the compiler, headers and Vulkan import library, compiles the sources with the same compiler/link options as this release, and copies available LLVM and ONNX Runtime DLLs beside `build/Release/VulkanImageMaskStudio.exe`. It does not download dependencies. To package the GUI with DirectML, provide a DirectML-enabled ONNX Runtime build and its compatible `DirectML.dll`; the plain CPU package provides CPU inference only. The optional Vulkan loader may be supplied by the GPU driver or copied from the official SDK according to its distribution terms.

The `build.ps1` output has a generic executable name. The published release asset instead contains the versioned binary that was built and ZIP-checked for version 0.4.12.12.

After building, copy `download_models.ps1` and the `models/` folder beside the executable, then run the downloader or use the in-app model button. All runtime files and mutable `Logs`, `Exports`, `TrainingGuides`, `TrainingSamples`, and `LearnedModels` folders belong beside the executable in a writable location.

### Linux cross-compilation

The original artifact was compiled on Linux using LLVM-MinGW's `x86_64-w64-mingw32-clang++` and a Windows Vulkan loader import library. Use the same command and definitions shown in `build.ps1` with Linux paths to the Vulkan headers, ONNX Runtime headers and import library. `build.ps1` itself is written for Windows PowerShell.
