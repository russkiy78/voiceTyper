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
set(WHISPER_CPP_GIT_TAG "v1.8.6" CACHE STRING
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
    # Portable CPU baseline. ggml's GGML_NATIVE defaults ON, which compiles the
    # CPU backend for the *build host's* exact ISA: -march=native on GCC/Clang,
    # and on MSVC FindSIMD.cmake probes the build machine and silently flips to
    # /arch:AVX512 if that machine has AVX-512. The resulting binary then hits an
    # illegal instruction (Windows: 0xC000001D STATUS_ILLEGAL_INSTRUCTION; a hard
    # crash try/catch cannot catch) the moment it runs on a client CPU missing
    # that ISA — e.g. any consumer Alder/Raptor Lake laptop (AVX2 but no AVX-512).
    # That was the "crashes on some Win11 machines, not others" + crash-quarantine
    # report. Pin a fixed baseline so the release never inherits the build host.
    # AVX2 (Haswell, 2013+) covers effectively all current x86; drop AVX2/FMA/F16C
    # to GGML_AVX / GGML_SSE42 if pre-AVX2 CPUs must be supported.
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    set(GGML_AVX2   ON  CACHE BOOL "" FORCE)
    set(GGML_FMA    ON  CACHE BOOL "" FORCE)
    set(GGML_F16C   ON  CACHE BOOL "" FORCE)
    set(GGML_AVX512 OFF CACHE BOOL "" FORCE)

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

            # Pin a portable GPU arch baseline — the GPU analogue of the AVX2 CPU
            # pin above. ggml-cuda's default arch list depends on the toolkit
            # version AND on GGML_NATIVE; worse, a host-arch probe (CMake's
            # "native", or our build-windows.ps1 nvidia-smi auto-detect) bakes in
            # ONLY the build box's GPU arch. The release then has no kernel image
            # for any other arch: model load + Vulkan still work (allocations are
            # arch-agnostic), but the first whisper_full() kernel launch aborts
            # with "no kernel image available for execution on the device" ->
            # GGML_ABORT. That is the "crashes on a 1080 Ti (Pascal) under CUDA,
            # fine under Vulkan, dies on the 2nd second of recording" report.
            #
            # The list is toolkit-version-aware so a fixed pin never breaks
            # configure: Pascal/Maxwell/Volta (sm_50/61/70) ONLY compile on CUDA
            # <= 12.x — CUDA 13 removed them, so emitting 61-virtual there is a
            # hard configure error. Blackwell (sm_120) needs CUDA >= 12.8. Net:
            #   - CUDA 12.8 / 12.9  -> Maxwell..Blackwell (widest; keeps GTX 10xx)
            #   - CUDA 12.0 .. 12.7 -> Maxwell..Hopper
            #   - CUDA 13.x         -> Turing..Blackwell (NO Pascal; build the CUDA
            #                          release with CUDA 12.x to keep GTX 10xx)
            # -virtual ships PTX (JIT-forward onto the client GPU on first run);
            # -real ships SASS for the common consumer cards so they skip the JIT.
            # Only set it when the caller hasn't (build-windows.ps1 may override).
            if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
                find_package(CUDAToolkit QUIET)
                set(_vt_cuda_archs "")
                if(CUDAToolkit_VERSION AND CUDAToolkit_VERSION VERSION_LESS "13")
                    list(APPEND _vt_cuda_archs 50-virtual 61-virtual 70-virtual)
                endif()
                list(APPEND _vt_cuda_archs 75-virtual 80-virtual 86-real 89-real 90-virtual)
                if(CUDAToolkit_VERSION AND CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8")
                    list(APPEND _vt_cuda_archs 120a-real)
                endif()
                set(CMAKE_CUDA_ARCHITECTURES "${_vt_cuda_archs}"
                    CACHE STRING "Portable CUDA arch baseline" FORCE)
                message(STATUS "voiceTyper: pinned CMAKE_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES} (toolkit ${CUDAToolkit_VERSION})")
                if(NOT CUDAToolkit_VERSION VERSION_LESS "13")
                    message(STATUS "voiceTyper: NOTE — CUDA ${CUDAToolkit_VERSION} cannot target Pascal (GTX 10xx); use a CUDA 12.x toolkit if you need it.")
                endif()
            else()
                message(STATUS "voiceTyper: CMAKE_CUDA_ARCHITECTURES preset to ${CMAKE_CUDA_ARCHITECTURES} (caller override)")
            endif()

            # Export the CUDA build identity to the app target so it lands in the
            # client's log at startup (see WhisperAsrEngine::installDiagnostics).
            # The Pascal "no kernel image" abort is decided by which arches are
            # compiled in HERE, not by the client's driver — so logging the build
            # toolkit + arch list is what lets a crash report tell "12.x (has
            # sm_61) vs 13.x (dropped Pascal)" apart at a glance. find_package is
            # repeated because the override branch above skips it.
            find_package(CUDAToolkit QUIET)
            string(REPLACE ";" "," _vt_cuda_archs_csv "${CMAKE_CUDA_ARCHITECTURES}")
            set(VT_CUDA_BUILD_TOOLKIT "${CUDAToolkit_VERSION}" PARENT_SCOPE)
            set(VT_CUDA_BUILD_ARCHES  "${_vt_cuda_archs_csv}" PARENT_SCOPE)
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
