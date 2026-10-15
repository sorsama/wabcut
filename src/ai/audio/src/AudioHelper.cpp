#include <wabcut/ai/audio/AudioHelper.h>

#include <wabcut/model/MediaLocator.h>

#include <atomic>
#include <mutex>
#include <optional>

#ifdef _WIN32
#include <process.h>
#define WABCUT_GETPID _getpid
#else
#include <unistd.h>
#define WABCUT_GETPID getpid
#endif

namespace wabcut::ai::audio {

namespace embed = search::embed;

namespace {

/// Deletes the block files of one call however it ends.
struct BlockFiles {
    std::filesystem::path input;
    std::filesystem::path output;
    ~BlockFiles() {
        std::error_code ec;
        std::filesystem::remove(input, ec);
        std::filesystem::remove(output, ec);
    }
};

bool retryable(const Error& e) {
    return e.code == ErrorCode::DecoderError && e.message.find("did not answer within") == std::string::npos;
}

} // namespace

struct AudioHelper::Impl {
    explicit Impl(AudioHelperOptions o)
        : options(std::move(o)),
          helper(search::EmbedHelperOptions{options.executable, options.arguments, options.log, options.startTimeout, options.lowPriority}) {}

    AudioHelperOptions options;
    search::EmbedHelper helper;
    int loadedSession = 0;
    bool speechLoaded = false;
    bool stemsLoaded = false;
    mutable std::mutex mutex; ///< device / timing reads from other threads
    std::string device;
    double milliseconds = 0.0;
    int counter = 0;

    std::filesystem::path workDir() const {
        if (!options.workDir.empty()) {
            return options.workDir;
        }
        std::error_code ec;
        return std::filesystem::temp_directory_path(ec) / "wabcut-audio";
    }

    BlockFiles files() {
        const std::filesystem::path dir = workDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string stem = "block-" + std::to_string(WABCUT_GETPID()) + "-" + std::to_string(++counter);
        return BlockFiles{dir / (stem + ".in.f32"), dir / (stem + ".out.f32")};
    }

    /// Loads the model a request needs into the running helper (again after a restart).
    std::expected<void, Error> load(bool speech, const std::stop_token& stop) {
        if (auto started = helper.start(stop); !started) {
            return std::unexpected(started.error());
        }
        if (helper.session() != loadedSession) {
            loadedSession = helper.session();
            speechLoaded = stemsLoaded = false;
        }
        if (speech ? speechLoaded : stemsLoaded) {
            return {};
        }
        const std::filesystem::path& model = speech ? options.speechModel : options.stemModel;
        if (model.empty()) {
            return std::unexpected(Error(ErrorCode::NotFound, speech ? "no speech model configured" : "no stem model configured"));
        }
        embed::Load request;
        request.id = helper.nextId();
        (speech ? request.speech : request.stems) = model;
        request.device = options.device;
        request.threads = options.threads;
        auto loaded = helper.call<embed::Loaded>(embed::Request{request}, request.id,
                                                 speech ? "loading the speech model" : "loading the stem model", options.loadTimeout, stop);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        {
            std::lock_guard lock(mutex);
            device = loaded->device;
        }
        (speech ? speechLoaded : stemsLoaded) = true;
        return {};
    }

    /// One request with its retry after a crash; the output file's samples (`outputs` blocks of the input's size).
    std::expected<std::vector<float>, Error> process(bool speech, std::span<const float> interleaved, int channels, int rate,
                                                     int outputs, const std::stop_token& stop) {
        if (channels <= 0 || interleaved.empty() || interleaved.size() % static_cast<std::size_t>(channels) != 0) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "an audio block must hold whole frames"));
        }
        std::optional<Error> last;
        for (int attempt = 0; attempt <= std::max(options.crashRetries, 0); ++attempt) {
            if (auto ready = load(speech, stop); !ready) {
                if (!retryable(ready.error()) || stop.stop_requested()) {
                    return std::unexpected(ready.error());
                }
                last = ready.error();
                continue;
            }
            BlockFiles block = files();
            auto written = embed::writeAudioBlock(block.input, interleaved, channels, rate);
            if (!written) {
                return std::unexpected(written.error());
            }
            const std::string id = helper.nextId();
            const embed::Request request = speech ? embed::Request{embed::EnhanceAudio{id, *written, block.output}}
                                                  : embed::Request{embed::SeparateAudio{id, *written, block.output}};
            auto done = helper.call<embed::AudioDone>(request, id, speech ? "enhancing speech" : "separating stems",
                                                      options.requestTimeout, stop);
            if (!done) {
                if (!retryable(done.error()) || stop.stop_requested()) {
                    return std::unexpected(done.error());
                }
                last = done.error();
                continue;
            }
            if (done->frames != written->frames || done->channels != channels || done->outputs != outputs) {
                return std::unexpected(helper.abandon(ErrorCode::DecoderError,
                                                      "wabcut-embed answered " + std::to_string(done->outputs) + " x " +
                                                          std::to_string(done->frames) + " frames for " + std::to_string(outputs) +
                                                          " x " + std::to_string(written->frames)));
            }
            embed::AudioBlock result = *written;
            result.path = block.output;
            auto samples = embed::readAudioBlock(result, outputs);
            if (!samples) {
                return std::unexpected(helper.abandon(ErrorCode::DecoderError, "wabcut-embed's output: " + samples.error().message));
            }
            {
                std::lock_guard lock(mutex);
                milliseconds += done->milliseconds;
                if (!done->device.empty()) {
                    device = done->device;
                }
            }
            return std::move(*samples);
        }
        return std::unexpected(*last);
    }
};

AudioHelper::AudioHelper(AudioHelperOptions options) : m_impl(std::make_unique<Impl>(std::move(options))) {}

AudioHelper::~AudioHelper() = default;

std::expected<std::vector<float>, Error> AudioHelper::enhance(std::span<const float> interleaved, int channels, std::stop_token stop) {
    if (m_impl->options.speechModel.empty()) {
        return std::unexpected(Error(ErrorCode::NotFound, "no speech model configured"));
    }
    return m_impl->process(true, interleaved, channels, 48000, 1, stop);
}

std::expected<std::vector<float>, Error> AudioHelper::separate(std::span<const float> interleaved, int rate, int stems,
                                                               std::stop_token stop) {
    if (m_impl->options.stemModel.empty()) {
        return std::unexpected(Error(ErrorCode::NotFound, "no stem model configured"));
    }
    if (stems <= 0 || stems > embed::kMaxAudioOutputs) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "a separation has 1 to " + std::to_string(embed::kMaxAudioOutputs) + " stems"));
    }
    return m_impl->process(false, interleaved, 2, rate, stems, stop);
}

std::string AudioHelper::device() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->device;
}

int AudioHelper::starts() const {
    return m_impl->helper.starts();
}

double AudioHelper::inferenceMilliseconds() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->milliseconds;
}

void AudioHelper::shutdown() {
    m_impl->helper.stop();
}

} // namespace wabcut::ai::audio
