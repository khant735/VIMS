$ErrorActionPreference = "Stop"
$base = "https://raw.githubusercontent.com/khant735/VIMS/main/assets"
$files = @("cog_wood.ppm","cog_resin.ppm","spindle_metal.ppm")
$dest = Split-Path -Parent $MyInvocation.MyCommand.Path

foreach ($name in $files) {
    $url = "$base/$name"
    $out = Join-Path $dest $name
    $tmp = "$out.download"
    Write-Host "Downloading $name..."
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    $first = Get-Content -LiteralPath $tmp -TotalCount 1
    if ($first -ne "P3") {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        throw "$name is not a valid P3 PPM texture."
    }
    Move-Item -LiteralPath $tmp -Destination $out -Force
    Write-Host "Installed $name"
}
Write-Host "VIMS calibration textures are up to date."
