$token = Read-Host "Paste GitHub token" -AsSecureString
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($token)
$plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
$plain | & "C:\Program Files\GitHub CLI\gh.exe" auth login --hostname github.com --with-token
[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
Remove-Variable plain, token

& "C:\Program Files\GitHub CLI\gh.exe" auth status

$gh = "C:\Program Files\GitHub CLI\gh.exe"
# Download the EXE-only artifact from the newest successful workflow run.
# Its CRC32 suffix changes on every build, so discover the exact artifact name first.
$runId = (& $gh run list --repo khant735/VIMS --workflow windows-release.yml --status success --limit 1 --json databaseId --jq '.[0].databaseId').Trim()
if (-not $runId) { throw "No successful VIMS Windows workflow run was found." }

$artifactNames = @(& $gh api "repos/khant735/VIMS/actions/runs/$runId/artifacts" --jq '.artifacts[].name')
if ($LASTEXITCODE -ne 0) { throw "Could not read artifacts for workflow run $runId." }

$artifactName = $artifactNames |
    Where-Object { $_ -like 'VIMS-development-Windows-x64-EXE-*' } |
    Select-Object -First 1

if ([string]::IsNullOrWhiteSpace($artifactName)) {
    throw "No CRC32-named VIMS EXE artifact was found in workflow run $runId."
}
$artifactName = $artifactName.Trim()

Write-Host "Downloading $artifactName from workflow run $runId..."
# gh refuses to extract an artifact over files that already exist. Download
# into a temporary directory, then deliberately replace the installed runtime
# files so updating an existing VIMS folder is reliable.
$tempDir = Join-Path $env:TEMP ("VIMS-download-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
try {
    & $gh run download $runId --repo khant735/VIMS --name $artifactName --dir $tempDir
    if ($LASTEXITCODE -ne 0) { throw "GitHub CLI download failed with exit code $LASTEXITCODE." }
    Get-ChildItem -LiteralPath $tempDir -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $PSScriptRoot $_.Name) -Force
        Write-Host "Updated $($_.Name)"
    }
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
