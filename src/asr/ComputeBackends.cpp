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
    return ComputeDevice{"cpu", "cpu", "CPU", "CPU", /*isGpu=*/false,
                         /*gpuIndex=*/-1};
}

ResolvedBackend useDevice(const ComputeDevice& d) {
    return {true, d.gpuIndex, d.backendName + " — " + d.deviceName, d.id};
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
        const auto type = ggml_backend_dev_type(dev);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU)
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
        cd.backendId = toLower(reg);

        // Keyed by name, not ordinal: CUDA and Vulkan order the same cards
        // differently, and an ordinal shifts when a card is added or removed.
        const std::string base = cd.backendId + "|" + cd.deviceName;
        const auto sameName = std::count_if(
            devices.begin(), devices.end(), [&](const ComputeDevice& other) {
                return other.isGpu && other.backendId == cd.backendId &&
                       other.deviceName == cd.deviceName;
            });
        cd.id = sameName == 0 ? base : base + "|" + std::to_string(sameName + 1);

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
        return {false, 0, "CPU", "cpu"};

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
            return useDevice(*firstGpu);
        return {false, 0, "CPU", "cpu"};
    }

    // Explicit device ("cuda|NVIDIA GeForce RTX 2080 Ti").
    for (const ComputeDevice& d : devices) {
        if (d.isGpu && d.id == wantedId)
            return useDevice(d);
    }

    // Bare backend id ("cuda", as older versions stored it), or a device that
    // is gone: the first device of that backend.
    const std::string backendId = wantedId.substr(0, wantedId.find('|'));
    for (const ComputeDevice& d : devices) {
        if (d.isGpu && d.backendId == backendId) {
            if (backendId != wantedId)
                qCWarning(vtAsr) << "Requested compute device"
                                 << QString::fromStdString(wantedId)
                                 << "is not available; using"
                                 << QString::fromStdString(d.id);
            return useDevice(d);
        }
    }

    qCWarning(vtAsr) << "Requested compute backend"
                     << QString::fromStdString(wantedId)
                     << "is not available; falling back to CPU";
    return {false, 0, "CPU (requested backend unavailable)", "cpu"};
}

} // namespace vt
