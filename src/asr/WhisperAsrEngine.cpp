#include "asr/WhisperAsrEngine.h"

#include "core/Logging.h"

#include <whisper.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace vt {

namespace {

int physicalCoreCount() {
#ifdef __linux__
    // Count distinct (physical id, core id) pairs. hardware_concurrency()
    // returns logical CPUs, which over-counts on SMT machines.
    std::ifstream f("/proc/cpuinfo");
    if (f) {
        std::set<std::pair<int, int>> cores;
        int phys = 0, core = -1;
        std::string line;
        const auto valueAfterColon = [](const std::string& s) {
            const auto pos = s.find(':');
            return pos == std::string::npos ? 0 : std::atoi(s.c_str() + pos + 1);
        };
        while (std::getline(f, line)) {
            if (line.empty()) {
                if (core >= 0)
                    cores.insert({phys, core});
                phys = 0;
                core = -1;
            } else if (line.rfind("physical id", 0) == 0) {
                phys = valueAfterColon(line);
            } else if (line.rfind("core id", 0) == 0) {
                core = valueAfterColon(line);
            }
        }
        if (core >= 0)
            cores.insert({phys, core});
        if (!cores.empty())
            return static_cast<int>(cores.size());
    }
#endif
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 4 : static_cast<int>(hw);
}

int pickThreadCount(int requested) {
    if (requested > 0)
        return requested;
    // Prefer physical cores: the encoder is FP-heavy, and SMT siblings contend
    // for the same FPU, so e.g. 8 threads on a 4-core CPU runs slower than 4.
    // Capped at 8 to bound scheduler overhead on large machines.
    return std::clamp(physicalCoreCount(), 1, 8);
}

std::string trimmed(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// DEBUG (env VT_DUMP_WAV): dump the exact samples handed to whisper_full as a
// 16-bit PCM WAV, so a live repro can be replayed offline at any audio_ctx.
void dumpWavDebug(const std::vector<float>& samples, int sampleRate) {
    const char* dir = std::getenv("VT_DUMP_WAV");
    if (!dir || !*dir)
        return;
    static std::atomic<int> counter{0};
    const std::string path =
        std::string(dir) + "/vt_" + std::to_string(counter.fetch_add(1)) + ".wav";
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return;
    const auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size()) * 2;
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1);
    u32(static_cast<uint32_t>(sampleRate)); u32(static_cast<uint32_t>(sampleRate) * 2);
    u16(2); u16(16);
    f.write("data", 4); u32(dataBytes);
    for (float s : samples) {
        const float c = std::clamp(s, -1.0f, 1.0f);
        u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(c * 32767.0f))));
    }
    qCInfo(vtAsr) << "VT_DUMP_WAV: wrote" << samples.size() << "samples to" << QString::fromStdString(path);
}

// Loads a whisper context, swallowing C++ exceptions thrown by the compute
// backend during init. ggml-vulkan (Vulkan-Hpp) throws vk::SystemError when
// device / buffer / pipeline setup fails on a GPU that enumerated but can't
// actually run the model; without this the exception unwinds all the way into
// main() and terminates the app. Returns nullptr on failure instead.
// NOTE: this CANNOT catch a hard abort() such as CUDA's GGML_ABORT — that path
// is covered by the on-disk crash breadcrumb in AppController::buildAsrEngine.
whisper_context* initWhisperContext(const std::string& modelPath, bool useGpu,
                                    int gpuDevice, bool flashAttn) {
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = useGpu;
    cparams.gpu_device = gpuDevice;
    // Flash attention pinned explicitly (upstream flipped the default to ON in
    // v1.8.0) so a version bump never changes decode behaviour silently and the
    // two modes can be benchmarked head-to-head.
    cparams.flash_attn = flashAttn;
    try {
        return whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    } catch (const std::exception& e) {
        qCWarning(vtAsr) << "whisper init threw:" << e.what();
        return nullptr;
    } catch (...) {
        qCWarning(vtAsr) << "whisper init threw a non-std exception";
        return nullptr;
    }
}

// Forwards one ggml/whisper log line into the Qt logging system. Trailing
// newlines are stripped so FileLogging doesn't double them. ERROR/WARN land in
// the default (info-level) log; INFO/DEBUG are demoted to qCDebug so the chatty
// per-tensor backend-load output stays off unless the user opts in via
// QT_LOGGING_RULES="voicetyper.asr.debug=true".
void ggmlLogToQt(ggml_log_level level, const char* text, void* /*user*/) {
    if (!text || !*text)
        return;
    QString msg = QString::fromUtf8(text);
    while (msg.endsWith('\n') || msg.endsWith('\r'))
        msg.chop(1);
    if (msg.isEmpty())
        return;
    switch (level) {
    case GGML_LOG_LEVEL_ERROR:
    case GGML_LOG_LEVEL_WARN:
        qCWarning(vtAsr).noquote() << "[ggml]" << msg;
        break;
    default:
        qCDebug(vtAsr).noquote() << "[ggml]" << msg;
        break;
    }
}

// Last line before abort() tears the process down (e.g. a CUDA GGML_ABORT).
// Logged at warning level so it survives the default filter; FileLogging
// flushes per line, so it reaches disk before the process dies.
void ggmlAbortToQt(const char* message) {
    qCWarning(vtAsr).noquote()
        << "[ggml abort]" << (message ? QString::fromUtf8(message) : QString());
}

} // namespace

void WhisperAsrEngine::installDiagnostics() {
    static std::once_flag once;
    std::call_once(once, [] {
        ggml_log_set(ggmlLogToQt, nullptr);
        whisper_log_set(ggmlLogToQt, nullptr);
        ggml_set_abort_callback(ggmlAbortToQt);
    });
}

WhisperAsrEngine::WhisperAsrEngine(const std::string& modelPath, bool useGpu,
                                   int gpuDevice, bool flashAttn,
                                   std::string backendLabel)
    : modelPath_(modelPath), backendLabel_(std::move(backendLabel)) {
    if (modelPath.empty()) {
        qCWarning(vtAsr) << "WhisperAsrEngine: empty model path";
        return;
    }

    qCInfo(vtAsr) << "Whisper compute backend:"
                  << QString::fromStdString(
                         backendLabel_.empty()
                             ? (useGpu ? std::string("GPU") : std::string("CPU"))
                             : backendLabel_)
                  << "flash_attn:" << flashAttn;

    ctx_ = initWhisperContext(modelPath, useGpu, gpuDevice, flashAttn);

    // (B) GPU init failed but didn't hard-crash the process: retry on CPU so the
    // app stays usable instead of being left with no engine at all. The caller
    // reads gpuInitFailed() to persist the CPU choice and stop re-probing a GPU
    // that can't run the model on every launch.
    if (!ctx_ && useGpu) {
        gpuInitFailed_ = true;
        qCWarning(vtAsr) << "GPU compute init failed; retrying on CPU for"
                         << QString::fromStdString(modelPath);
        backendLabel_ = "CPU (GPU init failed)";
        ctx_ = initWhisperContext(modelPath, /*useGpu=*/false, /*gpuDevice=*/0,
                                  flashAttn);
    }

    if (!ctx_)
        qCWarning(vtAsr) << "Failed to load whisper model:"
                         << QString::fromStdString(modelPath);
    else
        qCInfo(vtAsr) << "Loaded whisper model:"
                      << QString::fromStdString(modelPath);
}

void WhisperAsrEngine::setOnFirstGpuInferenceDone(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onFirstGpuInferenceDone_ = std::move(cb);
}

WhisperAsrEngine::~WhisperAsrEngine() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
    }
}

bool WhisperAsrEngine::isReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ctx_ != nullptr;
}

std::string WhisperAsrEngine::backendName() const {
    std::string name = "whisper.cpp";
    if (!backendLabel_.empty())
        name += " [" + backendLabel_ + "]";
    return name + ": " + modelPath_;
}

TranscriptionResult WhisperAsrEngine::transcribe(
    const AudioBuffer& audio, const TranscriptionOptions& options) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    std::lock_guard<std::mutex> lock(mutex_);

    TranscriptionResult result;
    if (!ctx_) {
        result.ok = false;
        return result;
    }
    if (audio.samples.empty()) {
        result.ok = true;
        return result;
    }

    whisper_full_params params =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;
    params.translate = options.translate;
    params.n_threads = pickThreadCount(options.threads);
    params.no_context = true;     // independent passes (good for the detect loop)
    params.suppress_blank = true;

    // Language: "auto"/empty => let whisper detect; otherwise force it.
    if (options.language.empty() || options.language == "auto")
        params.language = "auto";
    else
        params.language = options.language.c_str();

    if (options.fastMode) {
        // Latency-focused pass for the command-detection loop.
        params.single_segment = true;
        params.n_threads = std::min(params.n_threads, 4);
        params.temperature = 0.0f;
    }

    // Cooperative cancellation: ggml polls this during compute, so a caller can
    // abort a slow pass instead of blocking until it finishes.
    if (options.abortFlag) {
        params.abort_callback = [](void* user) -> bool {
            return static_cast<std::atomic<bool>*>(user)->load(
                std::memory_order_relaxed);
        };
        params.abort_callback_user_data = options.abortFlag.get();
    }

    // Scale the encoder context to the actual audio length. By default whisper
    // pads every clip to 30 s and encodes all 1500 frames, so a 2 s clip pays
    // the full 30 s cost. There are ~50 encoder frames per second of audio.
    //
    // The headroom is +3 s (~150 frames) with a 300-frame floor, NOT +1 s.
    // The distilled turbo decoder hallucinates a repeat of the utterance when
    // audio_ctx sits just above the speech content and the clip has trailing
    // silence: it re-decodes the silence region and emits the phrase twice (the
    // "duplicated text" bug). Empirically the minimum safe headroom shrinks with
    // clip length (~+150 frames at 2.5 s, +75 at 4.3 s, ~0 by 8 s), so a flat
    // +150 plus a generous floor for short clips clears it. Too-tight values are
    // also paradoxically slower, because the bad decode triggers temperature
    // fallback retries. Clamped to the model max, so clips near 30 s fall back
    // to full context.
    const double audioSeconds = audio.durationSeconds();
    const int modelMaxCtx = whisper_model_n_audio_ctx(ctx_);
    params.audio_ctx = std::clamp(
        static_cast<int>(std::ceil((audioSeconds + 3.0) * 50.0)), 300,
        modelMaxCtx);

    qCDebug(vtAsr) << "transcribe: audio" << audioSeconds << "s, threads"
                   << params.n_threads << ", audio_ctx" << params.audio_ctx
                   << "/" << modelMaxCtx;

    dumpWavDebug(audio.samples, audio.sampleRate);

    const int rc = whisper_full(ctx_, params, audio.samples.data(),
                                static_cast<int>(audio.samples.size()));

    // whisper_full() returned — the GPU didn't abort on this call. Clear the
    // first-inference crash breadcrumb (set by AppController after GPU init).
    if (!firstGpuInferenceDone_ && onFirstGpuInferenceDone_) {
        firstGpuInferenceDone_ = true;
        onFirstGpuInferenceDone_();
    }

    if (rc != 0) {
        const bool aborted = options.abortFlag &&
                             options.abortFlag->load(std::memory_order_relaxed);
        if (aborted)
            qCDebug(vtAsr) << "transcribe aborted (rc =" << rc << ")";
        else
            qCWarning(vtAsr) << "whisper_full failed, rc =" << rc;
        result.ok = false;
        return result;
    }

    std::string text;
    const int segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < segments; ++i) {
        const char* seg = whisper_full_get_segment_text(ctx_, i);
        if (seg)
            text += seg;
    }

    result.text = trimmed(std::move(text));
    result.ok = true;
    result.durationSeconds =
        std::chrono::duration<double>(clock::now() - t0).count();
    return result;
}

} // namespace vt
