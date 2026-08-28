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

# 单进程宿主：全部组件（时钟/日历/应用管理/顶栏/Dock）都以线程形式
# 运行在 MyWigets 内，不再产出任何独立的组件 exe。
add_executable(MyWigets WIN32
    src/mywigets_main.cpp
    src/main.cpp          # 时钟
    src/calendar_main.cpp # 日历
    src/launcher_main.cpp # 应用管理
    src/topbar_main.cpp   # 顶栏
    src/dock_main.cpp     # Dock 栏
    src/app.rc
)

target_compile_definitions(MyWigets PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(MyWigets PRIVATE
    gdiplus
    user32
    gdi32
    advapi32
    shell32
    ole32
    oleaut32
    ws2_32
    uuid
    version
    propsys
    dwmapi
    uiautomationcore
    comctl32
    Mmdevapi
)
