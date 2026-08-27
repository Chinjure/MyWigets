@echo off
rem Restart DesktopDock: kill old instance, start new build
taskkill /IM DesktopDock-x64.exe /F >nul 2>&1
ping -n 2 127.0.0.1 >nul
start "" "%~dp0bin\DesktopDock-x64.exe"
