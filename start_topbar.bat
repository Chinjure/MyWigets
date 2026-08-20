@echo off
rem 重启 DesktopTopBar：结束旧实例 -> 启动新构建
taskkill /IM DesktopTopBar-x64.exe /F >nul 2>&1
timeout /t 1 /nobreak >nul
start "" "C:\users\mayn\desktop\clock\bin\DesktopTopBar-x64.exe"
