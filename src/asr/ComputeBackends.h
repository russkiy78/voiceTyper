#pragma once

#include <string>
#include <vector>

namespace vt {

// A compute device whisper.cpp can run on. CPU is always available; GPU entries
// appear only when the matching ggml backend was compiled in AND a live device
// is present at runtime (so the same binary offers Vulkan/CUDA only where the
// hardware/driver actually supports it).
struct ComputeDevice {
    std::string id;          // stable selector: "cpu", "vulkan", "cuda", ...
    std::string backendName; // ggml registry name: "CPU", "Vulkan", "CUDA"
    std::string deviceName;  // human description, e.g. "NVIDIA GeForce GTX 1050 Ti"
    bool isGpu = false;
    int gpuIndex = -1;       // index among GPU devices (whisper gpu_device); -1 = CPU
};

// CPU first, then GPU devices in ggml registration order.
std::vector<ComputeDevice> enumerateComputeDevices();

// What backend selection resolves to for whisper_context_params.
struct ResolvedBackend {
    bool useGpu = false;
    int gpuDevice = 0;
    std::string label; // what was actually selected (for logging / UI)
};

// Resolve a stored backend id to whisper params. Accepted ids: "cpu", a GPU id
// ("vulkan"/"cuda"/...), or empty/"auto" (prefer the first GPU, else CPU).
// Falls back to CPU when the requested GPU backend is not available.
ResolvedBackend resolveBackend(const std::string& wantedId);

} // namespace vt
