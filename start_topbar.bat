@echo off
rem Restart DesktopTopBar: kill old instance, start new build
taskkill /IM DesktopTopBar-x64.exe /F >nul 2>&1
ping -n 2 127.0.0.1 >nul
start "" "C:\users\mayn\desktop\clock\bin\DesktopTopBar-x64.exe"
