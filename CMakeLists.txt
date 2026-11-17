cmake_minimum_required(VERSION 3.20)
project(hotd_overkill_vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "HOTD_NG.exe is 32-bit: configure with vcvars32 (build.cmd).")
endif()

# The probe: a stand-in XINPUT1_3.dll that the game loads from its own folder.
add_library(hovr_probe SHARED
    src/probe/dllmain.cpp
    src/probe/util.cpp
    src/probe/folder.cpp
    src/probe/window.cpp
    src/probe/xinput.cpp
    src/probe/shaders.cpp
    src/probe/d3d.cpp
    src/probe/control.cpp
    src/probe/dinput.cpp
    src/probe/gunhook.cpp
    src/probe/audio_mute.cpp
    src/probe/png.cpp)
set_target_properties(hovr_probe PROPERTIES OUTPUT_NAME XINPUT1_3)
target_compile_definitions(hovr_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(hovr_probe PRIVATE /W4 /wd4100 /utf-8)
target_link_libraries(hovr_probe PRIVATE ole32 windowscodecs uuid dxguid)
