#include "asr/ComputeBackends.h"

#include "core/Logging.h"

#include <algorithm>
#include <cctype>

#ifdef VOICETYPER_WITH_WHISPER
#include <ggml-backend.h>
#endif

namespace vt {

namespace {

ComputeDevice cpuDevice() {
    return ComputeDevice{"cpu", "CPU", "CPU", /*isGpu=*/false, /*gpuIndex=*/-1};
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

std::vector<ComputeDevice> enumerateComputeDevices() {
    std::vector<ComputeDevice> devices;
    devices.push_back(cpuDevice());

#ifdef VOICETYPER_WITH_WHISPER
    // gpuIndex must match whisper_backend_init_gpu(): it counts only GPU-type
    // devices, in ggml_backend_dev_get() order, and uses that ordinal as
    // whisper_context_params.gpu_device.
    int gpuIndex = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (ggml_backend_dev_type(dev) != GGML_BACKEND_DEVICE_TYPE_GPU)
            continue;

        ComputeDevice cd;
        cd.isGpu = true;
        cd.gpuIndex = gpuIndex++;

        const char* desc = ggml_backend_dev_description(dev);
        cd.deviceName = desc ? desc : "GPU";

        std::string reg = "GPU";
        if (ggml_backend_reg_t r = ggml_backend_dev_backend_reg(dev)) {
            if (const char* rn = ggml_backend_reg_name(r))
                reg = rn;
        }
        cd.backendName = reg;
        cd.id = toLower(reg);

        devices.push_back(std::move(cd));
    }
#endif

    return devices;
}

ResolvedBackend resolveBackend(const std::string& wantedId) {
    // Explicit CPU: return BEFORE enumerating devices. enumerateComputeDevices()
    // initializes every compiled-in ggml backend — including CUDA registration,
    // which can abort() on a half-broken driver (device present but a property
    // query fails). A user who chose CPU must never pay that risk here.
    if (wantedId == "cpu")
        return {false, 0, "CPU"};

    const std::vector<ComputeDevice> devices = enumerateComputeDevices();

    const ComputeDevice* firstGpu = nullptr;
    for (const ComputeDevice& d : devices) {
        if (d.isGpu) {
            firstGpu = &d;
            break;
        }
    }

    // Empty / "auto": prefer the first GPU, else CPU.
    if (wantedId.empty() || wantedId == "auto") {
        if (firstGpu)
            return {true, firstGpu->gpuIndex,
                    firstGpu->backendName + " — " + firstGpu->deviceName};
        return {false, 0, "CPU"};
    }

    // Explicit GPU backend by id (e.g. "vulkan", "cuda").
    for (const ComputeDevice& d : devices) {
        if (d.isGpu && d.id == wantedId)
            return {true, d.gpuIndex, d.backendName + " — " + d.deviceName};
    }

    qCWarning(vtAsr) << "Requested compute backend"
                     << QString::fromStdString(wantedId)
                     << "is not available; falling back to CPU";
    return {false, 0, "CPU (requested backend unavailable)"};
}

} // namespace vt
