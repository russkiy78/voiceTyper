# WhisperCpp.cmake
#
# Integrates whisper.cpp as a bundled, statically linked dependency.
#
# Resolution order:
#   1. -DWHISPER_CPP_SOURCE_DIR=/path/to/whisper.cpp   (local checkout)
#   2. third_party/whisper.cpp                          (vendored copy / submodule)
#   3. FetchContent from GitHub at WHISPER_CPP_GIT_TAG  (requires network at configure time)
#
# When VOICETYPER_WITH_WHISPER is OFF the app builds without any ASR backend
# (NullAsrEngine is used) so the rest of the application can be compiled and
# tested without pulling the model runtime.

option(VOICETYPER_WITH_WHISPER "Build with bundled whisper.cpp local ASR" ON)

set(WHISPER_CPP_SOURCE_DIR "" CACHE PATH
    "Path to a local whisper.cpp checkout (overrides FetchContent)")
set(WHISPER_CPP_GIT_TAG "v1.7.6" CACHE STRING
    "whisper.cpp git tag to fetch when no local source is provided")

function(voicetyper_add_whisper)
    if(NOT VOICETYPER_WITH_WHISPER)
        message(STATUS "voiceTyper: building WITHOUT whisper.cpp (NullAsrEngine only)")
        return()
    endif()

    # whisper.cpp build knobs: we only want the static library, no extras.
    set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)

    # ------------------------------------------------------------------
    # GPU backends. Auto-detect the toolchains present on THIS build host and
    # compile in whatever is available. The user then picks CPU / Vulkan / CUDA
    # at runtime (ComputeBackends enumerates live devices), so a backend only
    # appears where both the build and the client's GPU/driver support it.
    # Force-disable a backend with -DVOICETYPER_WITH_VULKAN=OFF / _WITH_CUDA=OFF.
    # ------------------------------------------------------------------
    option(VOICETYPER_WITH_VULKAN "Enable whisper.cpp Vulkan backend when toolchain is present" ON)
    option(VOICETYPER_WITH_CUDA   "Enable whisper.cpp CUDA backend when toolchain is present"   ON)

    if(VOICETYPER_WITH_VULKAN)
        # ggml-vulkan needs the Vulkan headers/loader and the glslc shader compiler.
        find_package(Vulkan QUIET COMPONENTS glslc)
        if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
            set(GGML_VULKAN ON CACHE BOOL "" FORCE)
            message(STATUS "voiceTyper: Vulkan backend ENABLED (glslc: ${Vulkan_GLSLC_EXECUTABLE})")
        else()
            set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
            message(STATUS "voiceTyper: Vulkan backend disabled — install libvulkan-dev + glslc to enable")
        endif()
    endif()

    if(VOICETYPER_WITH_CUDA)
        # ggml-cuda needs the CUDA toolkit (nvcc). Absent on CPU-only hosts.
        include(CheckLanguage)
        check_language(CUDA)
        if(CMAKE_CUDA_COMPILER)
            set(GGML_CUDA ON CACHE BOOL "" FORCE)
            message(STATUS "voiceTyper: CUDA backend ENABLED (nvcc: ${CMAKE_CUDA_COMPILER})")
        else()
            set(GGML_CUDA OFF CACHE BOOL "" FORCE)
            message(STATUS "voiceTyper: CUDA backend disabled — no CUDA toolkit found")
        endif()
    endif()

    set(_vendored "${CMAKE_SOURCE_DIR}/third_party/whisper.cpp")

    if(WHISPER_CPP_SOURCE_DIR)
        message(STATUS "voiceTyper: using whisper.cpp from WHISPER_CPP_SOURCE_DIR=${WHISPER_CPP_SOURCE_DIR}")
        add_subdirectory("${WHISPER_CPP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/whisper_cpp_build")
    elseif(EXISTS "${_vendored}/CMakeLists.txt")
        message(STATUS "voiceTyper: using vendored whisper.cpp at ${_vendored}")
        add_subdirectory("${_vendored}" "${CMAKE_BINARY_DIR}/whisper_cpp_build")
    else()
        message(STATUS "voiceTyper: fetching whisper.cpp ${WHISPER_CPP_GIT_TAG} via FetchContent")
        include(FetchContent)
        FetchContent_Declare(whisper_cpp
            GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
            GIT_TAG        ${WHISPER_CPP_GIT_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(whisper_cpp)
    endif()

    if(NOT TARGET whisper)
        message(FATAL_ERROR
            "voiceTyper: expected a 'whisper' CMake target from whisper.cpp but none was found.")
    endif()
endfunction()
