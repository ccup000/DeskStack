cmake_minimum_required(VERSION 3.15)
project(DeskStack VERSION 2.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8)
endif()
add_definitions(-DUNICODE -D_UNICODE)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

add_executable(DeskStack WIN32
    src/main.cpp
    src/container.cpp
    src/panel.cpp
    src/layered.cpp
    src/config.cpp
    src/manager.cpp
    src/desktop.cpp
    src/iconlib.cpp
    src/uinp.cpp
    src/app.rc
)

target_include_directories(DeskStack PRIVATE src src/thirdparty)

# 合并应用 manifest（Per-Monitor V2 + 兼容 Win8 以支持 WS_EX_LAYERED 子窗口）
target_link_options(DeskStack PRIVATE
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")

target_link_libraries(DeskStack PRIVATE
    user32 gdi32 gdiplus ole32 oleaut32 shell32 shlwapi comctl32 comdlg32 uuid
    advapi32 windowscodecs)
set_target_properties(DeskStack PROPERTIES WIN32_EXECUTABLE TRUE)
