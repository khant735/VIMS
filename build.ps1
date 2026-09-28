param([string]$OutputDirectory = 'build/Release')
$ErrorActionPreference = 'Stop'

function Require-File([string]$Path, [string]$Purpose) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Purpose missing: $Path" }
    return (Resolve-Path -LiteralPath $Path).Path
}
foreach ($name in @('LLVM_MINGW_ROOT','VULKAN_SDK','ONNXRUNTIME_ROOT')) {
    if (-not [Environment]::GetEnvironmentVariable($name)) { throw "Set `$env:$name to your local SDK folder." }
}
$compiler = Require-File (Join-Path $env:LLVM_MINGW_ROOT 'bin/x86_64-w64-mingw32-clang++.exe') 'LLVM-MinGW compiler'
$vkHeader = Require-File (Join-Path $env:VULKAN_SDK 'Include/vulkan/vulkan.h') 'Vulkan header'
$vkLib = Require-File (Join-Path $env:VULKAN_SDK 'Lib/vulkan-1.lib') 'Vulkan import library'
$ortHeader = Require-File (Join-Path $env:ONNXRUNTIME_ROOT 'include/onnxruntime_c_api.h') 'ONNX Runtime header'
$vkInclude = Split-Path -Parent (Split-Path -Parent $vkHeader)
$ortInclude = Split-Path -Parent $ortHeader
$output = Join-Path $PSScriptRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'VulkanImageMaskStudio.exe'
$sources = @('main','App','VulkanRenderer','SegmentationEngine','LearningStore','WicImage','CpuTopology','CompoundMask','PoseGif','CrashLog') | ForEach-Object { Join-Path $PSScriptRoot "src/$_.cpp" }
$compilerArgs = @('-std=c++20','-O2','-Wno-macro-redefined','-DUNICODE','-D_UNICODE','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-DVK_USE_PLATFORM_WIN32_KHR',"-I$vkInclude","-I$ortInclude") + $sources + @($vkLib,'-lcomdlg32','-lshell32','-luser32','-lgdi32','-lole32','-lwindowscodecs','-luuid','-lwinhttp','-lbcrypt','-lcomctl32','-municode','-mwindows','-o',$exe)
& $compiler @compilerArgs
if ($LASTEXITCODE -ne 0) { throw "Compiler returned exit code $LASTEXITCODE" }
foreach ($dll in @(
    (Join-Path $env:LLVM_MINGW_ROOT 'x86_64-w64-mingw32/bin/libc++.dll'),
    (Join-Path $env:LLVM_MINGW_ROOT 'x86_64-w64-mingw32/bin/libunwind.dll'),
    (Join-Path $env:ONNXRUNTIME_ROOT 'lib/onnxruntime.dll')
)) {
    if (Test-Path -LiteralPath $dll -PathType Leaf) { Copy-Item -LiteralPath $dll -Destination $output -Force }
    else { Write-Warning "Runtime file not copied: $dll" }
}
Write-Host "Built $exe"
Write-Host 'To run analysis: copy/download compatible ONNX models into models/ beside the executable.'
