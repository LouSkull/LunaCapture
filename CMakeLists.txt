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
    src/CaptureEngine.cpp
    src/CoreUtils.cpp
    src/Localization.cpp
    src/OverlayWindow.cpp
    src/RegionEditor.cpp
    src/SettingsStore.cpp
    src/SoundPlayer.cpp
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
    d3dcompiler
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
    COMMENT "Copying offline documentation"
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
    d3d11 dxgi dwmapi shell32 shlwapi ole32 oleaut32 uuid gdiplus gdi32 user32 advapi32 winhttp)
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
    add_executable(LunaCapturePrivacyTests tests/capture_privacy_tests.cpp tests/live_desktop_probe.cpp
        src/CaptureEngine.cpp src/WicEncoder.cpp
        src/OverlayWindow.cpp src/TrayMenu.cpp src/RegionEditor.cpp src/WinUtil.cpp
        src/UiGfx.cpp src/UiIcons.cpp src/CoreUtils.cpp src/Localization.cpp)
    target_include_directories(LunaCapturePrivacyTests PRIVATE include)
    target_compile_features(LunaCapturePrivacyTests PRIVATE cxx_std_20)
    target_compile_definitions(LunaCapturePrivacyTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    if(MSVC)
        target_compile_options(LunaCapturePrivacyTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    target_link_libraries(LunaCapturePrivacyTests PRIVATE windowsapp windowscodecs d3d11 dxgi gdiplus user32 gdi32 dwmapi shell32 shlwapi ole32 uuid uxtheme msimg32 advapi32)
    add_test(NAME LunaCapturePrivacyTests COMMAND LunaCapturePrivacyTests)
    set_tests_properties(LunaCapturePrivacyTests PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(LunaShotBindingTests tests/binding_tests.cpp)
    target_include_directories(LunaShotBindingTests PRIVATE include)
    target_compile_features(LunaShotBindingTests PRIVATE cxx_std_20)
    add_test(NAME LunaShotBindingTests COMMAND LunaShotBindingTests)
    add_executable(LunaShotCoreTests tests/core_tests.cpp src/CoreUtils.cpp src/Localization.cpp)
    target_include_directories(LunaShotCoreTests PRIVATE include)
    target_compile_features(LunaShotCoreTests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(LunaShotCoreTests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME LunaShotCoreTests COMMAND LunaShotCoreTests)

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

endif()

install(TARGETS LunaCapture RUNTIME DESTINATION .)
install(DIRECTORY docs DESTINATION .)
