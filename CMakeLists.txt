cmake_minimum_required(VERSION 3.20)

project(DesktopClock VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_executable(DesktopClock WIN32 src/main.cpp)
add_executable(DesktopCalendar WIN32 src/calendar_main.cpp)
add_executable(DesktopTopBar WIN32 src/topbar_main.cpp)
add_executable(MyWigets WIN32 src/mywigets_main.cpp src/app.rc)
add_executable(DesktopLauncher WIN32 src/launcher_main.cpp src/app.rc)
add_executable(DesktopDock WIN32 src/dock_main.cpp)

foreach(target IN ITEMS DesktopClock DesktopCalendar DesktopTopBar)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    target_link_libraries(${target} PRIVATE
        gdiplus
        user32
        gdi32
        advapi32
    )
endforeach()

# DesktopTopBar 使用 Core Audio（按进程调音量），需要 COM 库；
# ws2_32：Chrome 标签同步的 WebSocket 服务端
target_link_libraries(DesktopTopBar PRIVATE
    ole32
    oleaut32
    ws2_32
)

target_compile_definitions(MyWigets PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(MyWigets PRIVATE
    user32
    advapi32
    shell32
    gdi32
)

target_compile_definitions(DesktopLauncher PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(DesktopLauncher PRIVATE
    gdiplus
    user32
    gdi32
    advapi32
    shell32
    comctl32
    ole32
)

# DesktopDock：类 macOS Dock 栏。
# dwmapi：幽灵窗口（UWP cloak）判断；version：exe 文件描述 → 显示名；
# uuid/ole32/shell32：IShellLink 解析 lnk、SHGetKnownFolderPath 展开托盘登记路径；
# gdiplus：逐像素透明渲染与图标绘制；
# uiautomationcore：Win11 XAML 托盘图标触发（UIA Invoke，托盘驻留应用唤醒）
target_compile_definitions(DesktopDock PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(DesktopDock PRIVATE
    gdiplus
    user32
    gdi32
    advapi32
    shell32
    ole32
    uuid
    version
    dwmapi
    uiautomationcore
    oleaut32
)
