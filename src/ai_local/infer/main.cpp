// wabcut-infer: runs whisper.cpp out of the editor's process and speaks the NDJSON protocol in
// wabcut/ai/InferProtocol.h on stdin/stdout. One request runs at a time; a reader thread keeps reading stdin, so
// "cancel" (and the end of input, when the editor goes away) aborts the running request at the next ggml
// computation step. Logs (whisper.cpp, ggml) go to stderr, which the parent sends to a file or discards.

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/InferProtocol.h>
#include <wabcut/ai/Timestamps.h>

#include <ggml-backend.h>
#include <whisper.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <wabcut/platform/DllSearch.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace infer = wabcut::ai::infer;

namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

/// Serialised writes of whole lines to stdout.
class Output {
public:
    void send(const infer::Response& response) {
        std::string line = infer::encode(response);
        line += '\n';
        std::lock_guard lock(m_mutex);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }

private:
    std::mutex m_mutex;
};

/// Requests from stdin. Cancel and end-of-input are handled on the reader thread (they must reach a request
/// that is running); everything else is queued for the main thread.
class Input {
public:
    explicit Input(Output& out) : m_out(out) {
        m_thread = std::thread([this] { readLoop(); });
        m_thread.detach(); // blocks in a read until the process exits
    }

    /// The next queued request; nullopt once the input ended or shutdown was requested and the queue is empty.
    std::optional<infer::Request> next() {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_queue.empty() || m_ended; });
        if (m_queue.empty()) {
            return std::nullopt;
        }
        infer::Request r = std::move(m_queue.front());
        m_queue.pop_front();
        return r;
    }

    /// Marks `id` as the running request (cancel targets it); "" when idle. Clears a stale abort.
    void running(const std::string& id) {
        std::lock_guard lock(m_mutex);
        m_running = id;
        m_abort.store(m_ended, std::memory_order_relaxed);
    }
    bool aborted() const { return m_abort.load(std::memory_order_relaxed); }
    std::atomic<bool>* abortFlag() { return &m_abort; }

private:
    void readLoop() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            auto request = infer::decodeRequest(line);
            if (!request) {
                m_out.send(infer::Failure{"", "bad_request", request.error().message});
                continue;
            }
            if (const auto* cancel = std::get_if<infer::Cancel>(&*request)) {
                std::lock_guard lock(m_mutex);
                if (cancel->id.empty() || cancel->id == m_running) {
                    m_abort.store(true, std::memory_order_relaxed);
                }
                continue;
            }
            if (std::holds_alternative<infer::Shutdown>(*request)) {
                break;
            }
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(*request));
            m_cv.notify_all();
        }
        // End of input (the editor closed the pipe or went away) or shutdown: stop whatever runs.
        std::lock_guard lock(m_mutex);
        m_ended = true;
        m_abort.store(true, std::memory_order_relaxed);
        m_cv.notify_all();
    }

    Output& m_out;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<infer::Request> m_queue;
    std::string m_running;
    bool m_ended = false;
    std::atomic<bool> m_abort{false};
};

std::expected<std::vector<float>, std::string> readPcm(const infer::PcmRange& range) {
    std::ifstream in(range.path, std::ios::binary);
    if (!in) {
        return std::unexpected("cannot open PCM file " + utf8(range.path));
    }
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::int64_t>(in.tellg());
    if (range.offset < 0 || range.count < 0 || (range.offset + range.count) * 4 > bytes) {
        return std::unexpected("PCM range outside the file");
    }
    if (range.count > std::numeric_limits<int>::max()) {
        return std::unexpected("PCM range too long");
    }
    std::vector<float> samples(static_cast<std::size_t>(range.count));
    in.seekg(range.offset * 4);
    in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(range.count * 4));
    if (!in) {
        return std::unexpected("cannot read PCM file " + utf8(range.path));
    }
    return samples;
}

const char* kindOf(ggml_backend_dev_t dev) {
    switch (ggml_backend_dev_type(dev)) {
    case GGML_BACKEND_DEVICE_TYPE_CPU:
        return "cpu";
    case GGML_BACKEND_DEVICE_TYPE_GPU:
        return "gpu";
    case GGML_BACKEND_DEVICE_TYPE_IGPU:
        return "igpu";
    default:
        return "accel";
    }
}

std::vector<infer::DeviceInfo> devices() {
    std::vector<infer::DeviceInfo> out;
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        out.push_back({ggml_backend_dev_name(dev), ggml_backend_dev_description(dev), kindOf(dev)});
    }
    return out;
}

/// The device whisper.cpp will put the model on: the n-th GPU (or integrated GPU) when allowed, else the CPU.
std::string modelDevice(bool useGpu, int gpuDevice) {
    if (useGpu) {
        int n = 0;
        for (const infer::DeviceInfo& d : devices()) {
            if ((d.kind == "gpu" || d.kind == "igpu") && n++ == gpuDevice) {
                return d.name + " (" + d.description + ")";
            }
        }
    }
    return "CPU";
}

/// Silero is a tiny LSTM run window by window: thread hand-offs cost more than they save (measured on 277 s of
/// speech: 0.7 s with 1 thread, 3.1 s with 4, 10.6 s with 8).
int vadThreads() { return 1; }

struct Engine {
    Output& out;
    Input& in;
    whisper_context* ctx = nullptr;
    whisper_vad_context* vad = nullptr;
    std::string vadPath;

    ~Engine() {
        if (vad != nullptr) {
            whisper_vad_free(vad);
        }
        if (ctx != nullptr) {
            whisper_free(ctx);
        }
    }

    void handle(const infer::Hello&) {
        out.send(infer::HelloReply{infer::kProtocolVersion, "whisper.cpp", whisper_version(), devices()});
    }

    void handle(const infer::LoadModel& r) {
        if (ctx != nullptr) {
            whisper_free(ctx);
            ctx = nullptr;
        }
        whisper_context_params params = whisper_context_default_params();
        params.use_gpu = r.useGpu;
        params.gpu_device = r.gpuDevice;
        params.flash_attn = true;
        ctx = whisper_init_from_file_with_params(utf8(r.path).c_str(), params);
        if (ctx == nullptr) {
            out.send(infer::Failure{r.id, "model", "cannot load the model " + utf8(r.path)});
            return;
        }
        std::string info = whisper_model_type_readable(ctx);
        info += whisper_is_multilingual(ctx) != 0 ? ", multilingual" : ", English only";
        out.send(infer::Loaded{r.id, modelDevice(r.useGpu, r.gpuDevice), info});
    }

    void handle(const infer::LoadVad& r) {
        if (vad != nullptr) {
            whisper_vad_free(vad);
            vad = nullptr;
        }
        whisper_vad_context_params params = whisper_vad_default_context_params();
        params.n_threads = vadThreads();
        params.use_gpu = false; // tiny model: the CPU is faster than a round trip to the GPU
        vad = whisper_vad_init_from_file_with_params(utf8(r.path).c_str(), params);
        if (vad == nullptr) {
            out.send(infer::Failure{r.id, "model", "cannot load the VAD model " + utf8(r.path)});
            return;
        }
        vadPath = utf8(r.path);
        out.send(infer::Loaded{r.id, "CPU", "Silero VAD"});
    }

    void handle(const infer::VadRequest& r) {
        if (vad == nullptr) {
            out.send(infer::Failure{r.id, "bad_request", "no VAD model loaded"});
            return;
        }
        auto samples = readPcm(r.pcm);
        if (!samples) {
            out.send(infer::Failure{r.id, "io", samples.error()});
            return;
        }
        whisper_vad_params params = whisper_vad_default_params();
        params.threshold = r.threshold;
        params.min_speech_duration_ms = r.minSpeechMs;
        params.min_silence_duration_ms = r.minSilenceMs;
        params.speech_pad_ms = r.padMs;
        whisper_vad_segments* segments =
            whisper_vad_segments_from_samples(vad, params, samples->data(), static_cast<int>(samples->size()));
        if (segments == nullptr) {
            out.send(infer::Failure{r.id, "inference", "voice activity detection failed"});
            return;
        }
        infer::VadResult result{r.id, {}};
        for (int i = 0; i < whisper_vad_segments_n_segments(segments); ++i) {
            // Centiseconds (whisper's unit), as floats.
            result.segments.emplace_back(static_cast<std::int64_t>(whisper_vad_segments_get_segment_t0(segments, i) + 0.5f),
                                         static_cast<std::int64_t>(whisper_vad_segments_get_segment_t1(segments, i) + 0.5f));
        }
        whisper_vad_free_segments(segments);
        out.send(result);
    }

    struct Callbacks {
        Engine* engine;
        const std::string* id;
        int lastPercent = -1;
    };

    void handle(const infer::TranscribeRequest& r) {
        if (ctx == nullptr) {
            out.send(infer::Failure{r.id, "bad_request", "no model loaded"});
            return;
        }
        auto samples = readPcm(r.pcm);
        if (!samples) {
            out.send(infer::Failure{r.id, "io", samples.error()});
            return;
        }
        const bool beam = r.beamSize > 1;
        whisper_full_params params = whisper_full_default_params(beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
        params.n_threads = std::max(1, r.threads);
        params.language = r.language.empty() ? "auto" : r.language.c_str();
        params.detect_language = false;
        params.translate = false;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.print_special = false;
        params.token_timestamps = true;
        params.max_len = 0;
        if (beam) {
            params.beam_search.beam_size = r.beamSize;
        }
        params.initial_prompt = r.initialPrompt.empty() ? nullptr : r.initialPrompt.c_str();
        params.vad = r.vad && !vadPath.empty();
        params.vad_model_path = params.vad ? vadPath.c_str() : nullptr;

        Callbacks callbacks{this, &r.id};
        params.new_segment_callback = [](whisper_context* c, whisper_state* state, int n, void* user) {
            auto* cb = static_cast<Callbacks*>(user);
            const int total = whisper_full_n_segments_from_state(state);
            const whisper_token eot = whisper_token_eot(c);
            const char* lang = whisper_lang_str(whisper_full_lang_id_from_state(state));
            for (int i = std::max(0, total - n); i < total; ++i) {
                infer::Segment s;
                s.id = *cb->id;
                s.language = lang != nullptr ? lang : "";
                s.segment.t0 = whisper_full_get_segment_t0_from_state(state, i);
                s.segment.t1 = whisper_full_get_segment_t1_from_state(state, i);
                s.segment.speakerTurnNext = whisper_full_get_segment_speaker_turn_next_from_state(state, i);
                for (int t = 0; t < whisper_full_n_tokens_from_state(state, i); ++t) {
                    if (whisper_full_get_token_id_from_state(state, i, t) >= eot) {
                        continue; // special and timestamp tokens
                    }
                    s.segment.tokens.push_back({whisper_full_get_token_text_from_state(c, state, i, t),
                                                whisper_full_get_token_t0_from_state(state, i, t),
                                                whisper_full_get_token_t1_from_state(state, i, t),
                                                whisper_full_get_token_p_from_state(state, i, t)});
                }
                cb->engine->out.send(s);
            }
        };
        params.new_segment_callback_user_data = &callbacks;
        params.progress_callback = [](whisper_context*, whisper_state*, int percent, void* user) {
            auto* cb = static_cast<Callbacks*>(user);
            if (percent != cb->lastPercent) {
                cb->lastPercent = percent;
                cb->engine->out.send(infer::Progress{*cb->id, percent});
            }
        };
        params.progress_callback_user_data = &callbacks;
        params.abort_callback = [](void* user) { return static_cast<const std::atomic<bool>*>(user)->load(std::memory_order_relaxed); };
        params.abort_callback_user_data = in.abortFlag();
        params.encoder_begin_callback = [](whisper_context*, whisper_state*, void* user) {
            return !static_cast<const std::atomic<bool>*>(user)->load(std::memory_order_relaxed);
        };
        params.encoder_begin_callback_user_data = in.abortFlag();

        whisper_reset_timings(ctx);
        const auto started = std::chrono::steady_clock::now();
        const int rc = whisper_full(ctx, params, samples->data(), static_cast<int>(samples->size()));
        const double wallMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (in.aborted()) {
            out.send(infer::Failure{r.id, "cancelled", "cancelled"});
            return;
        }
        if (rc != 0) {
            out.send(infer::Failure{r.id, "inference", "whisper_full failed (" + std::to_string(rc) + ")"});
            return;
        }
        infer::Result result;
        result.id = r.id;
        const char* lang = whisper_lang_str(whisper_full_lang_id(ctx));
        result.language = lang != nullptr ? lang : "";
        result.segments = whisper_full_n_segments(ctx);
        result.stats["wallMs"] = wallMs;
        result.stats["audioMs"] = static_cast<double>(samples->size()) * 1000.0 / wabcut::ai::kTranscriptionSampleRate;
        if (const whisper_timings* t = whisper_get_timings(ctx)) {
            result.stats["encodeMs"] = t->encode_ms;
            result.stats["decodeMs"] = t->decode_ms + t->batchd_ms;
        }
        out.send(result);
    }

    void handle(const infer::Cancel&) {}   // handled by the reader thread
    void handle(const infer::Shutdown&) {} // handled by the reader thread
};

} // namespace

int main(int argc, char** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (argc >= 2 && std::string_view(argv[1]) == "--version") {
        std::printf("wabcut-infer (whisper.cpp %s, protocol %d)\n", whisper_version(), infer::kProtocolVersion);
        return 0;
    }
    // Backends are run-time modules next to this executable (ggml-cpu-<level>.dll, ggml-vulkan.dll, ...).
    ggml_backend_load_all_from_path(utf8(wabcut::ai::currentExecutableDir()).c_str());
    if (argc >= 2 && std::string_view(argv[1]) == "--devices") {
        for (const infer::DeviceInfo& d : devices()) {
            std::printf("%s\t%s\t%s\n", d.kind.c_str(), d.name.c_str(), d.description.c_str());
        }
        return 0;
    }

    Output out;
    Input in(out);
    Engine engine{out, in};
    while (auto request = in.next()) {
        std::visit(
            [&](const auto& r) {
                if constexpr (requires { r.id; }) {
                    in.running(r.id);
                } else {
                    in.running("");
                }
                engine.handle(r);
                in.running("");
            },
            *request);
    }
    return 0;
}
