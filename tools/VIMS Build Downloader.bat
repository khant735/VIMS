@echo off
setlocal
title VIMS Build Downloader

:MENU
cls
echo ==========================================
echo          VIMS Build Downloader
echo ==========================================
echo.
echo  1. Login and Download
echo  2. Download with Saved Login
echo  3. Exit
echo.
choice /C 123 /N /M "Select an option [1-3]: "

if errorlevel 3 goto :EOF
if errorlevel 2 goto SAVED
if errorlevel 1 goto LOGIN

:LOGIN
cls
echo Running: VIMS - Login and Download.ps1
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0VIMS - Login and Download.ps1"
goto DONE

:SAVED
cls
echo Running: VIMS - Download with Saved Login.ps1
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0VIMS - Download with Saved Login.ps1"
goto DONE

:DONE
echo.
echo ==========================================
echo Script finished.
echo ==========================================
echo.
pause
goto MENU
