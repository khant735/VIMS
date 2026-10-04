$ErrorActionPreference = 'Stop'

$base = 'https://raw.githubusercontent.com/khant735/VIMS/main/runtime-shaders'
$files = @('cog.vert.spv','cog.frag.spv')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$report = Join-Path $root 'shader_download_report.txt'
"VIMS Vulkan shader download/verification report - $(Get-Date -Format o)" | Set-Content $report -Encoding utf8

foreach ($name in $files) {
    $part = Join-Path $root ($name + '.part')
    $dest = Join-Path $root $name
    $hashPart = Join-Path $root ($name + '.sha256.part')
    try {
        Write-Host "Downloading $name..."
        Invoke-WebRequest -UseBasicParsing -Uri "$base/$name" -OutFile $part
        Invoke-WebRequest -UseBasicParsing -Uri "$base/$name.sha256" -OutFile $hashPart
        $expected = ((Get-Content -LiteralPath $hashPart -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
        $actual = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($expected -notmatch '^[0-9a-f]{64}$' -or $actual -ne $expected) {
            throw "SHA-256 mismatch for $name (expected $expected, got $actual)"
        }
        [byte[]]$bytes = [IO.File]::ReadAllBytes($part)
        if ($bytes.Length -lt 20 -or ($bytes.Length % 4) -ne 0 -or [BitConverter]::ToUInt32($bytes,0) -ne 0x07230203) {
            throw "Downloaded file is not valid SPIR-V: $name"
        }
        Move-Item -LiteralPath $part -Destination $dest -Force
        Remove-Item -LiteralPath $hashPart -Force -ErrorAction SilentlyContinue
        "OK  $name  SHA256=$actual" | Add-Content $report -Encoding utf8
        Write-Host "Verified $name"
    } catch {
        Remove-Item -LiteralPath $part,$hashPart -Force -ErrorAction SilentlyContinue
        "FAIL  $name  $($_.Exception.Message)" | Add-Content $report -Encoding utf8
        throw
    }
}
Write-Host 'Vulkan shaders downloaded and verified successfully.'
