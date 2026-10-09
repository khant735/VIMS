param([int]$WaitPid=0)
$ErrorActionPreference='Stop'
$base=Split-Path -Parent $MyInvocation.MyCommand.Path
$api='https://api.github.com/repos/khant735/VIMS/releases/tags/development-latest'
$headers=@{'User-Agent'='VIMS-Updater';'Accept'='application/vnd.github+json'}
function Fetch([string]$url,[string]$dest) { Invoke-WebRequest -Uri $url -Headers $headers -OutFile $dest -MaximumRedirection 5 }
function ParseManifest([string]$file) {
  $map=@{}
  foreach($line in Get-Content -LiteralPath $file) {
    if(!$line.Trim()){continue}
    $parts=$line -split "`t",2
    if($parts.Count -ne 2 -or $parts[1] -notmatch '^[a-fA-F0-9]{64}$'){throw "Invalid manifest entry: $line"}
    $name=$parts[0].Replace('/','\')
    if([IO.Path]::IsPathRooted($name) -or $name -match '(^|\\)\.\.(\\|$)' -or $name -match '[:*?"<>|]' -or $map.ContainsKey($name)){throw "Unsafe manifest path: $name"}
    $map[$name]=$parts[1].ToLowerInvariant()
  }
  return $map
}
$work=Join-Path $env:TEMP ('VIMS-update-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
  $release=Invoke-RestMethod -Uri $api -Headers $headers
  $assets=@{}
  foreach($a in $release.assets){$assets[$a.name]=$a.browser_download_url}
  foreach($name in @('distribution-manifest.tsv','distribution-previous.tsv','VIMS-Update-Files.zip','VIMS-Full-Windows-x64.zip')) {
    if(!$assets.ContainsKey($name)){throw "Update release missing $name"}
  }
  $latestFile=Join-Path $work 'latest.tsv';Fetch $assets['distribution-manifest.tsv'] $latestFile
  $latest=ParseManifest $latestFile
  $localFile=Join-Path $base 'distribution-manifest.tsv'
  $local=@{}
  if(Test-Path $localFile){$local=ParseManifest $localFile}
  $prevFile=Join-Path $work 'previous.tsv';Fetch $assets['distribution-previous.tsv'] $prevFile
  $prev=ParseManifest $prevFile
  $useDelta=$local.Count -gt 0 -and $local.Count -eq $prev.Count
  if($useDelta){foreach($key in $local.Keys){if(!$prev.ContainsKey($key) -or $prev[$key] -ne $local[$key]){$useDelta=$false;break}}}
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  # A delta is safe only when every installed previous-version file matches.
  if($useDelta){
    foreach($name in $prev.Keys){
      $installed=Join-Path $base $name
      if(!(Test-Path -LiteralPath $installed -PathType Leaf) -or
         (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash.ToLowerInvariant() -ne $prev[$name]){
        $useDelta=$false;break
      }
    }
  }
  $changed=@($latest.Keys | Where-Object { !(Test-Path (Join-Path $base $_)) -or (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $base $_)).Hash.ToLowerInvariant() -ne $latest[$_] })
  if($changed.Count -eq 0){[void][System.Windows.Forms.MessageBox]::Show('VIMS files are up to date.','Updates!');return}
  $mode=if($useDelta){'VIMS-Update-Files.zip'}else{'VIMS-Full-Windows-x64.zip'}
  Add-Type -AssemblyName System.Windows.Forms
  $answer=[System.Windows.Forms.MessageBox]::Show("Update $($changed.Count) file(s) using $mode? VIMS will need to close.","VIMS Updates!",'YesNo','Question')
  if($answer -ne 'Yes'){return}
  $zip=Join-Path $work 'payload.zip';Fetch $assets[$mode] $zip
  $stage=Join-Path $work 'stage';[IO.Compression.ZipFile]::ExtractToDirectory($zip,$stage)
  foreach($name in $changed) {
    $source=Join-Path $stage $name
    if(!(Test-Path -LiteralPath $source -PathType Leaf)){throw "Update payload missing: $name"}
    if((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash.ToLowerInvariant() -ne $latest[$name]){throw "SHA-256 mismatch: $name"}
  }
  # Only the manifest-listed files are installed. User-created files are preserved.
  if($WaitPid -gt 0){try {Wait-Process -Id $WaitPid -Timeout 120 -ErrorAction Stop}catch{throw 'VIMS did not exit within 120 seconds; update cancelled'}}
  $backup=Join-Path $work 'backup';New-Item -ItemType Directory $backup | Out-Null
  $installed=New-Object 'System.Collections.Generic.List[string]'
  try {
    foreach($name in $changed){
      $target=Join-Path $base $name
      $destDir=Split-Path $target -Parent
      New-Item -ItemType Directory -Force -Path $destDir | Out-Null
      if(Test-Path -LiteralPath $target){
        $old=Join-Path $backup $name
        New-Item -ItemType Directory -Force -Path (Split-Path $old -Parent) | Out-Null
        Copy-Item -LiteralPath $target -Destination $old -Force
      }
      $installed.Add($name)
      Copy-Item -LiteralPath (Join-Path $stage $name) -Destination $target -Force
    }
    Copy-Item $latestFile $localFile -Force
  }catch{
    foreach($name in $installed){
      $old=Join-Path $backup $name;$target=Join-Path $base $name
      if(Test-Path $old){Copy-Item $old $target -Force}else{Remove-Item $target -Force -ErrorAction SilentlyContinue}
    }
    throw
  }
  [System.Windows.Forms.MessageBox]::Show('Update installed and SHA-256 verified. Restart VIMS.','Updates!') | Out-Null
}catch{
  Add-Type -AssemblyName System.Windows.Forms
  [System.Windows.Forms.MessageBox]::Show($_.Exception.Message,'VIMS update failed','OK','Error') | Out-Null
  exit 1
}finally{
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
