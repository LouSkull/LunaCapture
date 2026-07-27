cmake_minimum_required(VERSION 3.24)
project(LunaCaptureNative VERSION 1.0.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Luna Capture Native targets Windows 10/11 only.")
endif()

add_executable(LunaCapture WIN32
    src/main.cpp
    src/App.cpp
    src/AudioMixer.cpp
    src/CaptureEngine.cpp
    src/CoreUtils.cpp
    src/OverlayWindow.cpp
    src/RegionEditor.cpp
    src/RecordingEngine.cpp
    src/SettingsStore.cpp
    src/SoundPlayer.cpp
    src/TrayMenu.cpp
    src/UiGfx.cpp
    src/UiIcons.cpp
    src/WicEncoder.cpp
    src/WinUtil.cpp
    resources.rc
)

target_include_directories(LunaCapture PRIVATE include)
target_compile_features(LunaCapture PRIVATE cxx_std_20)
target_compile_definitions(LunaCapture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
set_property(TARGET LunaCapture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    target_compile_options(LunaCapture PRIVATE /W4 /permissive- /EHsc /utf-8 /MP /Zc:__cplusplus /guard:cf)
    target_link_options(LunaCapture PRIVATE
        /SUBSYSTEM:WINDOWS
        /DYNAMICBASE
        /NXCOMPAT
        /HIGHENTROPYVA
        /guard:cf
        /MANIFEST:EMBED
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/assets/app.manifest"
    )
endif()

target_link_libraries(LunaCapture PRIVATE
    d3d11
    dxgi
    windowscodecs
    dwmapi
    shell32
    shlwapi
    ole32
    uuid
    winmm
    msimg32
    gdiplus
    mfplat
    mfreadwrite
    mfuuid
    avrt
    mmdevapi
    propsys
    user32
    gdi32
    advapi32
    uxtheme
)

set_target_properties(LunaCapture PROPERTIES
    OUTPUT_NAME "LunaCapture"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

add_library(LunaShotWindowsHeaderSmoke OBJECT tests/windows_header_smoke.cpp)
target_include_directories(LunaShotWindowsHeaderSmoke PRIVATE include)
target_compile_features(LunaShotWindowsHeaderSmoke PRIVATE cxx_std_20)
target_compile_definitions(LunaShotWindowsHeaderSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(LunaShotWindowsHeaderSmoke PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(LunaShotCoreTests tests/core_tests.cpp src/CoreUtils.cpp)
    target_include_directories(LunaShotCoreTests PRIVATE include)
    target_compile_features(LunaShotCoreTests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(LunaShotCoreTests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME LunaShotCoreTests COMMAND LunaShotCoreTests)

    add_executable(LunaShotRecordingSmoke EXCLUDE_FROM_ALL
        tests/recording_smoke.cpp
        src/AudioMixer.cpp
        src/CaptureEngine.cpp
        src/RecordingEngine.cpp
        src/WinUtil.cpp
        src/CoreUtils.cpp
        src/WicEncoder.cpp
    )
    target_include_directories(LunaShotRecordingSmoke PRIVATE include)
    target_compile_features(LunaShotRecordingSmoke PRIVATE cxx_std_20)
    target_compile_definitions(LunaShotRecordingSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    if(MSVC)
        target_compile_options(LunaShotRecordingSmoke PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    endif()
    target_link_libraries(LunaShotRecordingSmoke PRIVATE
        d3d11 dxgi windowscodecs dwmapi shell32 shlwapi ole32 uuid user32 gdi32
        mfplat mfreadwrite mfuuid avrt mmdevapi propsys uxtheme
    )
endif()

install(TARGETS LunaCapture RUNTIME DESTINATION .)
