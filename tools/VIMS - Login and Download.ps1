$token = Read-Host "Paste GitHub token" -AsSecureString
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($token)
$plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
$plain | & "C:\Program Files\GitHub CLI\gh.exe" auth login --hostname github.com --with-token
[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
Remove-Variable plain, token

& "C:\Program Files\GitHub CLI\gh.exe" auth status

& "C:\Program Files\GitHub CLI\gh.exe" run download --repo khant735/VIMS --name VIMS-development-Windows-x64-EXE