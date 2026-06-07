cmake_minimum_required(VERSION 3.21)

project(voiceTyper
    VERSION 0.1.0
    DESCRIPTION "Local voice typing utility (Qt6 + whisper.cpp)"
    LANGUAGES CXX)

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
    src/core/Logging.h

    src/ui/OverlayWindow.cpp
    src/ui/OverlayWindow.h
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
        src/hotkey/HotkeyService_mac.cpp
        src/clipboard/KeyboardPaster_mac.cpp)
else()
    list(APPEND VOICETYPER_SOURCES
        src/hotkey/HotkeyService_x11.cpp
        src/clipboard/KeyboardPaster_x11.cpp)
endif()

qt_add_executable(voiceTyper WIN32 ${VOICETYPER_SOURCES})

target_include_directories(voiceTyper PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(voiceTyper PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network)

if(VOICETYPER_WITH_WHISPER)
    target_link_libraries(voiceTyper PRIVATE whisper)
    target_compile_definitions(voiceTyper PRIVATE VOICETYPER_WITH_WHISPER=1)
endif()

# ----------------------------------------------------------------------------
# Platform link dependencies
# ----------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(voiceTyper PRIVATE user32)
elseif(APPLE)
    find_library(CARBON_LIBRARY Carbon)
    find_library(APPSERVICES_LIBRARY ApplicationServices)
    target_link_libraries(voiceTyper PRIVATE ${CARBON_LIBRARY} ${APPSERVICES_LIBRARY})
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

install(TARGETS voiceTyper RUNTIME DESTINATION bin)
install(FILES config/commands.default.json DESTINATION bin)
