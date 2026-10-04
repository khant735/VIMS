param([switch]$CoreOnly,[switch]$SpecialistsOnly,[switch]$IncludeExperimental)
$ErrorActionPreference="Stop"
$ProgressPreference="SilentlyContinue"
$modelsDir=Join-Path $PSScriptRoot "models"
$logsDir=Join-Path $PSScriptRoot "Logs"
$report=Join-Path $logsDir "model_download_report.txt"
New-Item -ItemType Directory -Force -Path $modelsDir | Out-Null
New-Item -ItemType Directory -Force -Path $logsDir | Out-Null
"Vulkan Image Mask Studio model download report - $(Get-Date -Format o)" | Set-Content $report

function Model($Name,$File,$Url,[long]$Min,$Group,$Sha="") {
 [PSCustomObject]@{Name=$Name;File=$File;Url=$Url;Min=$Min;Group=$Group;Sha=$Sha}
}
$models=@(
 (Model "ADE20K Scene" "scene_ade20k.onnx" "https://huggingface.co/Xenova/segformer-b0-finetuned-ade-512-512/resolve/main/onnx/model.onnx?download=true" 10000000 "Core")
 (Model "Clothing/Human Parsing" "clothes_human_parsing.onnx" "https://huggingface.co/Xenova/segformer_b2_clothes/resolve/main/onnx/model.onnx?download=true" 50000000 "Core")
 (Model "Full Face Parsing" "face_parsing.onnx" "https://huggingface.co/jonathandinu/face-parsing/resolve/1f7e152/onnx/model.onnx?download=true" 300000000 "Specialist" "6d4e67af60ff78184745ebf74cc15163c0adc27d45cdeba31e3a03d1096fb8c3")
 (Model "Detailed Face / Teeth" "detailed_face_teeth.onnx" "https://huggingface.co/sadzip/EasyPortrait-ONNX/resolve/main/segformer_b0_fp_512.onnx?download=true" 10000000 "Specialist")
 (Model "Robust Video Matting" "strand_matting_rvm.onnx" "https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx" 10000000 "Experimental" "88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828")
 (Model "Human Whole-body Pose" "human_wholebody_pose.onnx" "https://huggingface.co/bukuroo/RTMPose-ONNX/resolve/a6e9fb8/rtmpose-m-wholebody.onnx?download=true" 70000000 "Experimental" "465259a2e6c0f434ec5a5097640d6a439106c423e942ddbab7c6eaa02cc00c8a")
 (Model "Hand Pose" "hand_pose.onnx" "https://huggingface.co/bukuroo/RTMPose-ONNX/resolve/a6e9fb8/rtmpose-m-hand.onnx?download=true" 54000000 "Experimental" "39e858936bca0f94c09847d4e70b68a51d6c0adac61f36b457fcadb54621cd29")
 (Model "Animal Pose AP-10K" "animal_pose_ap10k.onnx" "https://huggingface.co/hr16/UnJIT-DWPose/resolve/main/rtmpose-m_ap10k_256.onnx?download=true" 1000000 "Experimental" "1cfd1c86e0d9e5d5f95178bcd95ee9a4e8386a624cd3c57519f27ff58cac7f28")
 (Model "Fingernail Instance Segmentation" "fingernail_segmentation.onnx" "https://raw.githubusercontent.com/austingg/finger-nail-seg/main/models/nail-seg.onnx" 100000 "Experimental")
)
# Keep the Vulkan calibration shaders in sync when the in-app model
# downloader is used.  They are small runtime assets, so fetch them beside the
# executable rather than making the user update them manually.
$shaderFailures=0
$shaderAssets=@(
 [PSCustomObject]@{Name="Vulkan gear vertex shader";File="cog.vert.spv";Url="https://raw.githubusercontent.com/khant735/VIMS/main/runtime-shaders/cog.vert.spv"}
 [PSCustomObject]@{Name="Vulkan gear fragment shader";File="cog.frag.spv";Url="https://raw.githubusercontent.com/khant735/VIMS/main/runtime-shaders/cog.frag.spv"}
)
foreach($s in $shaderAssets){
 $dst=Join-Path $PSScriptRoot $s.File;$part="$dst.part"
 try{
  Write-Host "[download] $($s.Name)";Invoke-WebRequest -Uri $s.Url -OutFile $part -UseBasicParsing -MaximumRedirection 10
  if((Get-Item $part).Length-lt 20){throw "Downloaded shader is unexpectedly small."}
  Move-Item $part $dst -Force
  $x="[ok] $($s.Name)";Write-Host $x;Add-Content $report $x
 }catch{
  Remove-Item $part -Force -ErrorAction SilentlyContinue
  $shaderFailures++;$x="[failed] $($s.Name): $($_.Exception.Message)";Write-Warning $x;Add-Content $report $x
 }
}

$coreFailures=0;$optionalFailures=0
foreach($m in $models){
 if($m.Group-eq"Experimental" -and !$IncludeExperimental){continue}
 if($CoreOnly -and $m.Group-ne"Core"){continue};if($SpecialistsOnly -and $m.Group-ne"Specialist"){continue}
 $dst=Join-Path $modelsDir $m.File
 try{
  if(Test-Path $dst){
   $valid=(Get-Item $dst).Length-ge$m.Min
   if($valid -and $m.Sha){$valid=((Get-FileHash $dst -Algorithm SHA256).Hash.ToLowerInvariant()-eq$m.Sha)}
   if($valid){$x="[installed/verified] $($m.Name)";Write-Host $x;Add-Content $report $x;continue}
  }
  $part="$dst.part";Remove-Item $part -Force -ErrorAction SilentlyContinue
  $x="[download] $($m.Name)";Write-Host $x;Add-Content $report $x
  Invoke-WebRequest -Uri $m.Url -OutFile $part -UseBasicParsing -MaximumRedirection 10
  if((Get-Item $part).Length-lt$m.Min){throw "Downloaded file is unexpectedly small."}
  if($m.Sha){$got=(Get-FileHash $part -Algorithm SHA256).Hash.ToLowerInvariant();if($got-ne$m.Sha){throw "SHA-256 mismatch: $got"}}
  Move-Item $part $dst -Force;$x="[ok] $($m.Name)";Write-Host $x;Add-Content $report $x
 }catch{
  Remove-Item "$dst.part" -Force -ErrorAction SilentlyContinue
  $x="[failed] $($m.Name): $($_.Exception.Message)";Write-Warning $x;Add-Content $report $x
  if($m.Group-eq"Core"){$coreFailures++}else{$optionalFailures++}
 }
}
Add-Content $report "Core failures: $coreFailures; Optional failures: $optionalFailures; Shader failures: $shaderFailures"
if($coreFailures-gt 0){exit 10}
if($shaderFailures-gt 0){exit 11}
exit 0
