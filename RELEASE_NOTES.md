# v0.4.12.12 preview

A native Windows x64 image mask analyser using Vulkan for the preview and ONNX Runtime for model inference.

- Detects and exports scene, human, clothing, face and derived anatomical masks when supported by installed models.
- Provides fitted and bicubic zoomable preview, drag to pan, independent right-side control scrolling, rough mask area editing, and explicit approval of corrected masks for local learning.
- Exports PNG masks, transparent cutouts, boundary metadata and approximate 1280 × 720 pose GIFs for detected people or animals.
- Stores diagnostic and timing logs beside the executable.

**Download:** Choose the `VIMS_v0.4.12.12_Windows_x64.zip` release asset, extract the entire archive, download the core ONNX models with `download_models.ps1`, and run `VulkanImageMaskStudio_v0.4.12.12.exe`. The automatically generated source archives do not contain a runnable executable.

**Testing status:** The executable was cross-compiled and the ZIP checked, but recent fixes to panel scrolling and Windows Snipping Tool compatibility have not been confirmed on Windows. Face and part detections are also model-dependent approximations; projected hidden regions are not recovered pixels.
