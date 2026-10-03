$gh = "C:\Program Files\GitHub CLI\gh.exe"

& $gh auth status

& $gh run download `
    --repo khant735/VIMS `
    --name VIMS-development-Windows-x64-EXE
