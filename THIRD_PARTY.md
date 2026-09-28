# Third-party components

The C++ application source is MIT licensed (see `LICENSE`). The release ZIP includes binary components that retain their own terms:

| Component | Role | Upstream rights / source |
| --- | --- | --- |
| ONNX Runtime (`onnxruntime.dll`) | Neural inference, with optional DirectML execution provider | Microsoft ONNX Runtime MIT license and bundled third-party notices; https://github.com/microsoft/onnxruntime |
| DirectML (`DirectML.dll`) | Optional GPU inference | Microsoft's DirectML redistributable; consult the license and notices supplied with the exact Microsoft package used; https://www.nuget.org/packages/Microsoft.AI.DirectML/ |
| LLVM-MinGW runtime (`libc++.dll`, `libunwind.dll`) | C++ and unwind libraries | LLVM/MinGW license supplied with the compiler distribution, including Apache 2.0 with LLVM exceptions; https://github.com/mstorsjo/llvm-mingw |
| Vulkan loader (`vulkan-1.dll`) | GPU image presentation | Khronos Vulkan loader licensing; https://github.com/KhronosGroup/Vulkan-Loader |
| ONNX model weights | Scene, clothing, face and optional specialist analysis | Downloaded separately from URLs in `download_models.ps1`; inspect each model's own license before redistributing any weights. |

Source publication and binary redistribution are separate steps. Before uploading a public release, confirm that each included DLL comes from its official distribution package and include that package's required license/notice documents. No ONNX weights or local training samples are part of this source repository or prepared release ZIP.
