cmake_minimum_required(VERSION 3.21)

# Must be set before project(): otherwise Clang defaults to the installed
# SDK's own version (e.g. "26.0" on a machine running today's macOS), which
# makes every build silently refuse to launch on any older system even though
# Qt 6.11 itself only requires 13.0 (Ventura) — the real floor here. Override
# with -DCMAKE_OSX_DEPLOYMENT_TARGET=... if you need something else.
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

project(voiceTyper
    VERSION 0.3.0
    DESCRIPTION "Local voice typing utility (Qt6 + whisper.cpp)"
    LANGUAGES CXX)

# Objective-C++ is only needed for the small AppKit shim
# (src/ui/MacWindowUtils.mm) that Qt's cross-platform API can't reach.
if(APPLE)
    enable_language(OBJCXX)
endif()

# Patch = git commit count so every commit auto-increments it without
# touching CMakeLists.txt. Falls back to 0 in shallow/detached checkouts.
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE VT_COMMIT_COUNT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT VT_COMMIT_COUNT MATCHES "^[0-9]+$")
    set(VT_COMMIT_COUNT "0")
endif()
set(VT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${VT_COMMIT_COUNT}")
message(STATUS "voiceTyper ${VT_VERSION}")

# ----------------------------------------------------------------------------
# Global settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ----------------------------------------------------------------------------
# Dependencies
# ----------------------------------------------------------------------------
# Pass -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/<kit>  e.g. ~/Qt/6.11.1/gcc_64
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Network)

qt_standard_project_setup()

include(WhisperCpp)
voicetyper_add_whisper()

# ----------------------------------------------------------------------------
# Sources
# ----------------------------------------------------------------------------
set(VOICETYPER_SOURCES
    src/main.cpp

    src/app/AppController.cpp
    src/app/AppController.h
    src/app/RecordingController.cpp
    src/app/RecordingController.h

    src/audio/AudioRecorder.cpp
    src/audio/AudioRecorder.h

    src/asr/IAsrEngine.h
    src/asr/TranscriptionTypes.h
    src/asr/NullAsrEngine.h
    src/asr/ComputeBackends.cpp
    src/asr/ComputeBackends.h

    src/commands/CommandTypes.h
    src/commands/CommandConfig.cpp
    src/commands/CommandConfig.h
    src/commands/CommandEngine.cpp
    src/commands/CommandEngine.h
    src/commands/CommandDetectionLoop.cpp
    src/commands/CommandDetectionLoop.h

    src/clipboard/ClipboardPasteService.cpp
    src/clipboard/ClipboardPasteService.h
    src/clipboard/KeyboardPaster.h

    src/hotkey/HotkeyService.h
    src/hotkey/HotkeyParsing.cpp
    src/hotkey/HotkeyParsing.h

    src/postprocess/TextPostProcessor.h
    src/postprocess/NoOpTextPostProcessor.h
    src/postprocess/HttpTextPostProcessor.h

    src/settings/SettingsStore.cpp
    src/settings/SettingsStore.h

    src/core/AudioBuffer.h
    src/core/FileLogging.cpp
    src/core/FileLogging.h
    src/core/Logging.h

    src/ui/OverlayWindow.cpp
    src/ui/OverlayWindow.h
    src/ui/ToastOverlay.cpp
    src/ui/ToastOverlay.h
    src/ui/TrayController.cpp
    src/ui/TrayController.h
    src/ui/SettingsWindow.cpp
    src/ui/SettingsWindow.h
)

# Whisper-backed engine only when the backend is enabled.
if(VOICETYPER_WITH_WHISPER)
    list(APPEND VOICETYPER_SOURCES
        src/asr/WhisperAsrEngine.cpp
        src/asr/WhisperAsrEngine.h)
endif()

# ----------------------------------------------------------------------------
# Platform-specific input synthesis (global hotkey + paste keystroke)
# ----------------------------------------------------------------------------
if(WIN32)
    list(APPEND VOICETYPER_SOURCES
        src/hotkey/HotkeyService_win.cpp
        src/clipboard/KeyboardPaster_win.cpp)
elseif(APPLE)
    list(APPEND VOICETYPER_SOURCES
        src/hotkey/HotkeyService_mac.mm
        src/clipboard/KeyboardPaster_mac.cpp
        src/ui/MacWindowUtils.mm)
else()
    list(APPEND VOICETYPER_SOURCES
        src/hotkey/HotkeyService_x11.cpp
        src/clipboard/KeyboardPaster_x11.cpp)
endif()

# Embed the application icon into the .exe via a Windows resource script. The
# .ico is generated from the voicetyper_*x*.png sources by
# scripts/build-windows.ps1 (New-AppIcon) before configure; guard on its
# presence so a checkout without the generated icon still builds.
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/resources/voicetyper.ico")
    enable_language(RC)
    list(APPEND VOICETYPER_SOURCES resources/voicetyper.rc)
endif()

qt_add_executable(voiceTyper WIN32 ${VOICETYPER_SOURCES})

# ARC for the static NSRunningApplication* in MacWindowUtils.mm (frontmost-app
# capture/restore around the paste keystroke) so it's retained/released
# correctly without manual bookkeeping.
if(APPLE)
    set_source_files_properties(src/ui/MacWindowUtils.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
endif()

target_include_directories(voiceTyper PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(voiceTyper PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network)

target_compile_definitions(voiceTyper PRIVATE VT_VERSION="${VT_VERSION}")

if(VOICETYPER_WITH_WHISPER)
    target_link_libraries(voiceTyper PRIVATE whisper)
    target_compile_definitions(voiceTyper PRIVATE VOICETYPER_WITH_WHISPER=1)
endif()

# CUDA build identity (set by voicetyper_add_whisper() when the CUDA backend is
# compiled in) so the running app can log which toolkit/arches the release was
# built with — see WhisperAsrEngine::installDiagnostics.
if(DEFINED VT_CUDA_BUILD_ARCHES)
    target_compile_definitions(voiceTyper PRIVATE
        VT_CUDA_BUILD_TOOLKIT="${VT_CUDA_BUILD_TOOLKIT}"
        VT_CUDA_BUILD_ARCHES="${VT_CUDA_BUILD_ARCHES}")
endif()

# ----------------------------------------------------------------------------
# Platform link dependencies
# ----------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(voiceTyper PRIVATE user32)
elseif(APPLE)
    find_library(CARBON_LIBRARY Carbon)
    find_library(APPSERVICES_LIBRARY ApplicationServices)
    find_library(APPKIT_LIBRARY AppKit)
    target_link_libraries(voiceTyper PRIVATE ${CARBON_LIBRARY} ${APPSERVICES_LIBRARY} ${APPKIT_LIBRARY})
else()
    # Linux / X11. Requires: libx11-dev libxtst-dev libxcb1-dev
    find_package(X11 REQUIRED)
    find_library(XTST_LIBRARY Xtst)
    find_library(XCB_LIBRARY xcb)
    if(NOT XTST_LIBRARY)
        message(FATAL_ERROR "libXtst not found. Install libxtst-dev.")
    endif()
    if(NOT XCB_LIBRARY)
        message(FATAL_ERROR "libxcb not found. Install libxcb1-dev.")
    endif()
    target_include_directories(voiceTyper PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(voiceTyper PRIVATE ${X11_LIBRARIES} ${XTST_LIBRARY} ${XCB_LIBRARY})
endif()

# ----------------------------------------------------------------------------
# Bundle default resources next to the binary (config + models dir layout)
# ----------------------------------------------------------------------------
add_custom_command(TARGET voiceTyper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/commands.default.json"
            "$<TARGET_FILE_DIR:voiceTyper>/commands.default.json"
    COMMENT "Copying default command config next to the binary")

# ----------------------------------------------------------------------------
# macOS: keep the dev .app bundle's binary in sync and codesigned.
#
# The bundle at ${CMAKE_BINARY_DIR}/voiceTyper.app isn't produced by this
# CMake target (no MACOSX_BUNDLE), so without this it silently goes stale on
# every rebuild. Signing matters too: an ad-hoc signature changes on every
# rebuild, which resets any Accessibility/Input Monitoring permission grant
# (needed for the global hotkey) every time. Signing with a stable local
# identity — see README's macOS setup — keeps that grant valid across
# rebuilds. Falls back to ad-hoc signing if that identity isn't present
# (e.g. a fresh checkout that hasn't created one yet).
# ----------------------------------------------------------------------------
if(APPLE)
    set(VT_MAC_BUNDLE "${CMAKE_BINARY_DIR}/voiceTyper.app")
    set(VT_CODESIGN_IDENTITY "voiceTyper Dev" CACHE STRING
        "Code signing identity for the dev .app bundle (macOS)")
    add_custom_command(TARGET voiceTyper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VT_MAC_BUNDLE}/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:voiceTyper>"
                "${VT_MAC_BUNDLE}/Contents/MacOS/voiceTyper"
        COMMAND bash -c "codesign --force --sign '${VT_CODESIGN_IDENTITY}' '${VT_MAC_BUNDLE}/Contents/MacOS/voiceTyper' 2>/dev/null || codesign --force --sign - '${VT_MAC_BUNDLE}/Contents/MacOS/voiceTyper'"
        COMMENT "Refreshing and codesigning voiceTyper.app"
        VERBATIM)
endif()

install(TARGETS voiceTyper RUNTIME DESTINATION bin)
install(FILES config/commands.default.json DESTINATION bin)
