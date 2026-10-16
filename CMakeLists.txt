cmake_minimum_required(VERSION 3.20)
project(USDZViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "USDZ Viewer is a Windows application and must be built with MSVC")
endif()

# Static CRT so the executables run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /permissive- /utf-8 /MP /wd4100 /wd4201)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# USD parsing, composition and scene extraction (no rendering, no UI).
add_library(usdcore STATIC
    src/usd/archive.cpp
    src/usd/crate.cpp
    src/usd/fileio.cpp
    src/usd/layer.cpp
    src/usd/stage.cpp
    src/usd/usda.cpp
    src/scene/scene.cpp
    src/scene/describe.cpp
)
target_include_directories(usdcore PUBLIC src)

# Command line inspector, also used by the regression tests in tests/.
add_executable(usdzinfo tools/usdzinfo.cpp)
target_link_libraries(usdzinfo PRIVATE usdcore)

# The viewer: Win32 UI + Direct3D 11 + WIC.
add_executable(USDZViewer WIN32
    src/app/main.cpp
    src/app/app.rc
    src/render/image.cpp
    src/render/renderer.cpp
)
target_link_libraries(USDZViewer PRIVATE usdcore d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs comctl32 uxtheme shlwapi dwmapi ole32 shell32)
# The manifest (visual styles, per-monitor DPI awareness) comes from app.rc.
target_link_options(USDZViewer PRIVATE /MANIFEST:NO)
