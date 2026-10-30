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
    src/app_suspend.cpp   # 后台应用挂起/恢复
    src/app_cleanup.cpp   # 可见关闭后的残留进程清理
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

# 一次性提权清理助手：只由用户显式运行，用于停止 LocalSystem 应用服务。
add_executable(MyWigetsCleanup CONSOLE
    src/cleanup_admin.cpp
    src/app_cleanup.cpp
    src/app_suspend.cpp
)
target_compile_definitions(MyWigetsCleanup PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(MyWigetsCleanup PRIVATE
    advapi32
    user32
    dwmapi
)
target_link_options(MyWigetsCleanup PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
)

# 按需计划任务使用的服务守护 broker（不是常驻进程）。
add_executable(MyWigetsServiceGuard WIN32
    src/service_guard.cpp
    src/app_cleanup.cpp
    src/app_suspend.cpp
)
target_compile_definitions(MyWigetsServiceGuard PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(MyWigetsServiceGuard PRIVATE
    advapi32
    user32
    dwmapi
    shell32
)
target_link_options(MyWigetsServiceGuard PRIVATE
    "/ENTRY:wWinMainCRTStartup"
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
)
