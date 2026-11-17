cmake_minimum_required(VERSION 3.20)
project(hotd_overkill_vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "HOTD_NG.exe is 32-bit: configure with vcvars32 (build.cmd).")
endif()

# The mod: a stand-in XINPUT1_3.dll that the game loads from its own folder (grown from the probe;
# the development harness - save redirect, minimized/muted window, command file - lives in it too).
add_library(hovr SHARED
    src/mod/dllmain.cpp
    src/mod/util.cpp
    src/mod/config.cpp
    src/mod/buildcheck.cpp
    src/mod/folder.cpp
    src/mod/window.cpp
    src/mod/xinput.cpp
    src/mod/shaders.cpp
    src/mod/d3d.cpp
    src/mod/stereo.cpp
    src/mod/engine.cpp
    src/mod/control.cpp
    src/mod/dinput.cpp
    src/mod/gun.cpp
    src/mod/gunmodel.cpp
    src/mod/vrinput.cpp
    src/mod/files.cpp
    src/mod/audio_mute.cpp
    src/mod/png.cpp
    src/mod/handoff.cpp
    src/mod/d3d9ex.cpp
    src/mod/gamestate.cpp
    src/mod/mirror.cpp
    src/mod/vrsys.cpp
    src/vr/stereo.cpp
    src/vr/xform.cpp
    src/vr/aim.cpp
    src/vr/gunpose.cpp)
set_target_properties(hovr PROPERTIES OUTPUT_NAME XINPUT1_3)
target_compile_definitions(hovr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(hovr PRIVATE /W4 /wd4100 /utf-8)
target_link_libraries(hovr PRIVATE ole32 windowscodecs uuid dxguid d3d11 dxgi)
# OpenVR (32-bit 2.15.6, copied from k7-vr), delay-loaded: openvr_api.dll is only loaded when
# [vr] mode=openvr.
target_include_directories(hovr PRIVATE third_party/openvr/headers)
target_link_libraries(hovr PRIVATE ${CMAKE_SOURCE_DIR}/third_party/openvr/lib/win32/openvr_api.lib delayimp)
target_link_options(hovr PRIVATE /DELAYLOAD:openvr_api.dll)
add_custom_command(TARGET hovr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/third_party/openvr/bin/win32/openvr_api.dll $<TARGET_FILE_DIR:hovr>)

# Unit tests for the VR math (no game needed): ctest --test-dir out
enable_testing()
add_executable(hovr_tests tests/test_stereo.cpp src/vr/stereo.cpp src/vr/xform.cpp src/vr/aim.cpp src/vr/gunpose.cpp)
target_compile_options(hovr_tests PRIVATE /W4 /utf-8)
add_test(NAME stereo COMMAND hovr_tests)
