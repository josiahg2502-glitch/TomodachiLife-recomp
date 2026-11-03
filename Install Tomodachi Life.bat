@echo off
rem Opens the launcher (build\work\TomodachiLauncher.exe, part of the release): its INSTALL screen sets everything up.
rem No PowerShell here. README.md has the steps of the older installer window by hand.
cd /d "%~dp0"
if exist "build\work\TomodachiLauncher.exe" (
    start "" "build\work\TomodachiLauncher.exe"
    exit /b 0
)
echo The launcher (build\work\TomodachiLauncher.exe) is not in this folder.
echo Get Tomodachi-Life-PC-Setup.exe from https://github.com/josiahg2502-glitch/TomodachiLife-recomp/releases
echo or see README.md to build everything from the sources.
pause
