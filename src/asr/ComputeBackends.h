#pragma once

#include <string>
#include <vector>

namespace vt {

// A compute device whisper.cpp can run on. CPU is always available; GPU entries
// appear only when the matching ggml backend was compiled in AND a live device
// is present at runtime (so the same binary offers Vulkan/CUDA only where the
// hardware/driver actually supports it).
struct ComputeDevice {
    // Stable selector stored in settings: "cpu", or "<backend>|<device name>"
    // for a GPU, e.g. "cuda|NVIDIA GeForce RTX 2080 Ti". A second device with
    // the same name gets "|2" appended, a third "|3", ...
    std::string id;
    std::string backendId;   // "cpu", "vulkan", "cuda", ...
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
    std::string label;    // what was actually selected (for logging / UI)
    std::string deviceId; // ComputeDevice::id of that device ("cpu" for CPU)
};

// Resolve a stored backend id to whisper params. Accepted ids: "cpu", a device
// id ("cuda|NVIDIA GeForce RTX 2080 Ti"), a bare backend id ("vulkan"/"cuda",
// stored by older versions: its first device), or empty/"auto" (the first GPU,
// else CPU). A device that is gone falls back to the first device of the same
// backend; a backend that is unavailable falls back to CPU.
ResolvedBackend resolveBackend(const std::string& wantedId);

} // namespace vt
