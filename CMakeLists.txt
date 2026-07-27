cmake_minimum_required(VERSION 3.24)

# Single source of truth for the app version: the VERSION file at the repo root.
# Everything else (C++ kAppVersion, the .rc file properties, dist\version.txt, the
# website badges) derives from it - bump only that one file for a release.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LUNA_VERSION)
string(STRIP "${LUNA_VERSION}" LUNA_VERSION)
project(LunaCaptureNative VERSION ${LUNA_VERSION} LANGUAGES CXX RC)
# Re-run the configure step whenever the VERSION file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)
set(LUNA_VERSION_TWEAK "${PROJECT_VERSION_TWEAK}")
if(LUNA_VERSION_TWEAK STREQUAL "")
    set(LUNA_VERSION_TWEAK 0)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Luna Capture Native targets Windows 10/11 only.")
endif()

add_executable(LunaCapture WIN32
    src/main.cpp
    src/App.cpp
    src/AudioMixer.cpp
    src/CaptureEngine.cpp
    src/CoreUtils.cpp
    src/Localization.cpp
    src/LunaScript.cpp
    src/MediaViewerWindow.cpp
    src/OverlayWindow.cpp
    src/PluginInstallPrompt.cpp
    src/PluginSystem.cpp
    src/QuickPanel.cpp
    src/RegionEditor.cpp
    src/RecordingEngine.cpp
    src/SettingsStore.cpp
    src/SoundPlayer.cpp
    src/ThumbnailCache.cpp
    src/TrayMenu.cpp
    src/UiGfx.cpp
    src/UiIcons.cpp
    src/UpdateCheck.cpp
    src/WicEncoder.cpp
    src/WinUtil.cpp
    resources.rc
)

# The .rc file cannot reliably take comma/quote-containing defines on the rc.exe command
# line, so the version macros go through a generated header instead.
configure_file(cmake/AppVersionRc.h.in "${CMAKE_BINARY_DIR}/generated/AppVersionRc.h")

target_include_directories(LunaCapture PRIVATE include "${CMAKE_BINARY_DIR}/generated")
target_compile_features(LunaCapture PRIVATE cxx_std_20)
target_compile_definitions(LunaCapture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00
    APP_VERSION_STRING=L\"${PROJECT_VERSION}\")
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
    mfplay
    mfuuid
    avrt
    mmdevapi
    propsys
    user32
    gdi32
    advapi32
    uxtheme
    winhttp
)

set_target_properties(LunaCapture PROPERTIES
    OUTPUT_NAME "LunaCapture"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET LunaCapture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/docs"
            "$<TARGET_FILE_DIR:LunaCapture>/docs"
    COMMENT "Copying the offline LunaScript SDK"
)

# ---------------------------------------------------------------------------
# Standalone installer / uninstaller. It never links the app: it downloads a
# published release and registers the OS integration points. The same binary
# runs as the uninstaller when launched with --uninstall.
# ---------------------------------------------------------------------------
add_executable(LunaSetup WIN32
    installer/InstallerMain.cpp
    installer/InstallerCore.cpp
    installer/installer.rc
    src/UiGfx.cpp
    src/UiIcons.cpp
    src/Localization.cpp
)
target_include_directories(LunaSetup PRIVATE include installer "${CMAKE_BINARY_DIR}/generated")
target_compile_features(LunaSetup PRIVATE cxx_std_20)
target_compile_definitions(LunaSetup PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00
    APP_VERSION_STRING=L\"${PROJECT_VERSION}\")
set_property(TARGET LunaSetup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(LunaSetup PRIVATE /W4 /permissive- /EHsc /utf-8 /MP /Zc:__cplusplus /guard:cf)
    target_link_options(LunaSetup PRIVATE
        /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf)
endif()
target_link_libraries(LunaSetup PRIVATE
    d3d11 dxgi dwmapi shell32 shlwapi ole32 oleaut32 uuid gdiplus gdi32 user32 advapi32 winhttp mfplat)
set_target_properties(LunaSetup PROPERTIES
    OUTPUT_NAME "LunaSetup"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
add_custom_command(TARGET LunaSetup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/installer/installer.ini"
            "$<TARGET_FILE_DIR:LunaSetup>/installer.ini"
    COMMENT "Copying the installer version pin"
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
    add_executable(LunaShotCoreTests tests/core_tests.cpp src/CoreUtils.cpp src/Localization.cpp src/LunaScript.cpp)
    target_include_directories(LunaShotCoreTests PRIVATE include)
    target_compile_features(LunaShotCoreTests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(LunaShotCoreTests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME LunaShotCoreTests COMMAND LunaShotCoreTests)

    add_executable(LunaShotPluginDispatchTests tests/plugin_dispatch_tests.cpp)
    target_include_directories(LunaShotPluginDispatchTests PRIVATE include)
    target_compile_features(LunaShotPluginDispatchTests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(LunaShotPluginDispatchTests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME LunaShotPluginDispatchTests COMMAND LunaShotPluginDispatchTests)

    add_executable(LunaShotPluginExampleTests tests/plugin_examples_tests.cpp src/LunaScript.cpp)
    target_include_directories(LunaShotPluginExampleTests PRIVATE include)
    target_compile_features(LunaShotPluginExampleTests PRIVATE cxx_std_20)
    target_compile_definitions(LunaShotPluginExampleTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00
        LUNA_PLUGIN_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/plugins"
    )
    if(MSVC)
        target_compile_options(LunaShotPluginExampleTests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME LunaShotPluginExampleTests COMMAND LunaShotPluginExampleTests)

    add_executable(LunaShotThemeTests
        tests/theme_tests.cpp
        src/UiGfx.cpp
        src/Localization.cpp
    )
    target_include_directories(LunaShotThemeTests PRIVATE include)
    target_compile_features(LunaShotThemeTests PRIVATE cxx_std_20)
    target_compile_definitions(LunaShotThemeTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00
    )
    if(MSVC)
        target_compile_options(LunaShotThemeTests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    endif()
    target_link_libraries(LunaShotThemeTests PRIVATE gdiplus user32 gdi32)
    add_test(NAME LunaShotThemeTests COMMAND LunaShotThemeTests)

    add_executable(LunaShotRecordingSmoke EXCLUDE_FROM_ALL
        tests/recording_smoke.cpp
        src/AudioMixer.cpp
        src/CaptureEngine.cpp
        src/Localization.cpp
        src/RecordingEngine.cpp
        src/UiGfx.cpp
        src/UiIcons.cpp
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
        d3d11 dxgi windowscodecs dwmapi shell32 shlwapi ole32 uuid user32 gdi32 gdiplus
        mfplat mfreadwrite mfuuid avrt mmdevapi propsys uxtheme
    )

    add_executable(LunaShotMediaPlayerSmoke EXCLUDE_FROM_ALL tests/media_player_smoke.cpp)
    target_compile_features(LunaShotMediaPlayerSmoke PRIVATE cxx_std_20)
    target_compile_definitions(LunaShotMediaPlayerSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    if(MSVC)
        target_compile_options(LunaShotMediaPlayerSmoke PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    endif()
    target_link_libraries(LunaShotMediaPlayerSmoke PRIVATE mfplat mfplay mfuuid ole32 uuid user32)
endif()

install(TARGETS LunaCapture RUNTIME DESTINATION .)
install(DIRECTORY docs DESTINATION .)
