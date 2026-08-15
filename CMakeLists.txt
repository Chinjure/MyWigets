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
add_executable(MyWigets WIN32 src/mywigets_main.cpp src/app.rc)
add_executable(DesktopLauncher WIN32 src/launcher_main.cpp src/app.rc)

foreach(target IN ITEMS DesktopClock DesktopCalendar)
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
