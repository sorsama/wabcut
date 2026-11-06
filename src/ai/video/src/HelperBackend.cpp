#include <wabcut/ai/video/HelperBackend.h>

#include <wabcut/ai/search/EmbedHelper.h>
#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/ai/video/Gaze.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>

namespace wabcut::ai::video {

namespace embed = search::embed;

namespace {

search::EmbedHelperOptions helperOptions(const HelperBackendOptions& o) {
    search::EmbedHelperOptions h;
    h.executable = o.executable;
    h.arguments = o.arguments;
    h.log = o.log;
    h.startTimeout = o.startTimeout;
    h.lowPriority = false; // the user waits for it (the run has a progress bar and Cancel)
    return h;
}

std::filesystem::path workDirOf(const HelperBackendOptions& o) {
    if (!o.workDir.empty()) {
        return o.workDir;
    }
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "wabcut-embed";
}

} // namespace

struct HelperBackend::Impl {
    explicit Impl(HelperBackendOptions o) : options(std::move(o)), helper(helperOptions(options)), dir(workDirOf(options)) {}

    HelperBackendOptions options;
    search::EmbedHelper helper;
    std::filesystem::path dir;
    std::mutex mutex; ///< one request at a time
    mutable std::mutex infoMutex;
    std::string device;
    int loadedSession = 0;
    std::set<std::string> loaded; ///< model keys loaded in the current helper session
    bool cpuOnly = false;
    int restarts = 0;
    std::atomic<int> counter{0};

    std::expected<void, Error> load(Model model, const std::stop_token& stop) {
        const ModelDescription& d = describe(model);
        // The weights first: no helper is started for a model that is not on this computer.
        std::optional<std::filesystem::path> file;
        if (options.resolveModel) {
            file = options.resolveModel(model);
        } else if (const ModelInfo* info = findModel(d.storeId); info != nullptr) {
            // The store's folder can change while the app runs (Settings > AI > Local models > Storage folder).
            const ModelStore store(options.modelRoot.empty() ? ModelStore::defaultRoot() : options.modelRoot);
            if (store.isInstalled(*info)) {
                file = store.pathFor(*info);
            }
        }
        if (!file) {
            return std::unexpected(Error(ErrorCode::NotFound, std::string(d.name) + " is not downloaded"));
        }
        if (auto started = helper.start(stop); !started) {
            return started;
        }
        if (loadedSession != helper.session()) {
            loaded.clear();
            loadedSession = helper.session();
        }
        if (loaded.contains(std::string(d.key))) {
            return {};
        }
        const auto send = [&](const std::string& on) {
            embed::Load request;
            request.id = helper.nextId();
            request.pictures.push_back({std::string(d.key), *file});
            request.device = on;
            request.threads = options.threads;
            return helper.call<embed::Loaded>(embed::Request{request}, request.id, std::string("loading ") + std::string(d.name),
                                              options.loadTimeout, stop);
        };
        auto answer = send(cpuOnly ? std::string("cpu") : options.device);
        if (!answer && !cpuOnly && options.device != "cpu" && !helper.running() && !stop.stop_requested()) {
            // The GPU provider took the helper down while creating the session: once more on the CPU.
            cpuOnly = true;
            if (auto restarted = helper.start(stop); !restarted) {
                return restarted;
            }
            loaded.clear();
            loadedSession = helper.session();
            answer = send("cpu");
        }
        if (!answer) {
            return std::unexpected(answer.error());
        }
        loaded.insert(std::string(d.key));
        std::lock_guard lock(infoMutex);
        device = answer->device;
        return {};
    }

    /// One model run. `second` (frame interpolation): the later frame, `timestep` where between the two the answer lies.
    std::expected<StepResult, Error> call(const Picture& in, const Step& step, const std::stop_token& stop,
                                          const Picture* second = nullptr, double timestep = 0.5, bool retryReleased = true) {
        if (!in.valid() || (second != nullptr && (!second->valid() || second->width != in.width || second->height != in.height))) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "no picture to process"));
        }
        const ModelDescription& d = describe(step.model);
        if (auto ready = load(step.model, stop); !ready) {
            return std::unexpected(ready.error());
        }
        if (step.model == Model::FaceParse || step.model == Model::FaceLandmarks) { // 0.7 AI Face Retouch / Eye Contact: the model looks at the faces the detector finds, in the same request
            if (auto detector = load(Model::FaceDetect, stop); !detector) {
                return std::unexpected(detector.error());
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string stem = "picture-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" + std::to_string(counter++);
        const std::filesystem::path inputFile = dir / (stem + ".in16");
        const std::filesystem::path outputFile = dir / (stem + ".out16");
        const std::filesystem::path secondFile = dir / (stem + ".in2-16");
        struct Cleanup {
            std::filesystem::path a, b, c;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove(a, e);
                std::filesystem::remove(b, e);
                std::filesystem::remove(c, e);
            }
        } cleanup{inputFile, outputFile, secondFile};
        auto written = embed::writePictureFile(inputFile, in.width, in.height, in.rgb);
        if (!written) {
            return std::unexpected(written.error());
        }
        embed::ProcessPicture request;
        if (second != nullptr) {
            auto writtenSecond = embed::writePictureFile(secondFile, second->width, second->height, second->rgb);
            if (!writtenSecond) {
                return std::unexpected(writtenSecond.error());
            }
            request.input2 = *writtenSecond;
            request.timestep = std::clamp(timestep, 0.0, 1.0);
        }
        request.id = helper.nextId();
        request.model = std::string(d.key);
        request.input = *written;
        request.output = outputFile;
        request.scale = d.scale;
        request.tile = std::clamp(tileFor(step.model, step.tile), embed::kMinTile, embed::kMaxTile);
        request.pad = kTilePad;
        request.sigma = std::clamp(step.sigma, 0.0, 1.0);
        const double outputMegapixels = static_cast<double>(in.width) * in.height * d.scale * d.scale / 1e6;
        const auto deadline = options.requestBase + std::chrono::milliseconds(static_cast<long long>(
                                                        outputMegapixels * static_cast<double>(options.requestPerMegapixel.count())));
        auto answer = helper.call<embed::PictureDone>(embed::Request{request}, request.id, std::string("running ") + std::string(d.name), deadline, stop);
        if (!answer) {
            if (retryReleased && answer.error().message.find("is not loaded") != std::string::npos) {
                loaded.clear(); // the helper keeps a few models and released this one: load them again, once
                return call(in, step, stop, second, timestep, false);
            }
            return std::unexpected(answer.error());
        }
        if (step.model == Model::FaceLandmarks) { // 0.7 AI Eye Contact: the answer is a table (Gaze.h): kGazeSlotCount wide, a row per face
            if (answer->width != kGazeSlotCount || answer->height < 1 || answer->height > 16) {
                return std::unexpected(helper.abandon(ErrorCode::DecoderError, "wabcut-embed returned a landmark table of the wrong size"));
            }
        } else if (answer->width != in.width * d.scale || answer->height != in.height * d.scale) {
            return std::unexpected(helper.abandon(ErrorCode::DecoderError, "wabcut-embed returned a picture of the wrong size"));
        }
        auto rgb = embed::readPictureFile({outputFile, answer->width, answer->height});
        if (!rgb) {
            return std::unexpected(rgb.error());
        }
        StepResult result;
        result.picture.width = answer->width;
        result.picture.height = answer->height;
        result.picture.rgb = std::move(*rgb);
        result.device = answer->device;
        result.milliseconds = answer->milliseconds;
        result.tiles = answer->tiles;
        {
            std::lock_guard lock(infoMutex);
            device = answer->device;
        }
        return result;
    }
};

HelperBackend::HelperBackend(HelperBackendOptions options) : m(std::make_unique<Impl>(std::move(options))) {}

HelperBackend::~HelperBackend() = default;

namespace {

/// One request with the helper's restart policy: a crash, a timeout or nonsense restarts it on the CPU and asks again, up to
/// maxRestarts times.
template <class Call>
std::expected<StepResult, Error> withRestarts(std::mutex& mutex, int& restarts, bool& cpuOnly, const HelperBackendOptions& options,
                                              search::EmbedHelper& helper, const std::stop_token& stop, Call&& call) {
    std::lock_guard lock(mutex);
    restarts = 0;
    while (true) {
        const bool onCpu = cpuOnly || options.device == "cpu";
        auto r = call();
        if (r || helper.running() || stop.stop_requested() || r.error().code == ErrorCode::InvalidArgument ||
            r.error().code == ErrorCode::Cancelled || r.error().code == ErrorCode::NotFound || onCpu) {
            return r; // done, or nothing left to try (the CPU failed too)
        }
        // The helper is gone (crashed, timed out, answered nonsense): again, on the CPU, with fresh models.
        if (++restarts > options.maxRestarts) {
            return std::unexpected(Error(r.error().code, "the AI helper keeps failing: " + r.error().message));
        }
        cpuOnly = cpuOnly || options.device != "cpu"; // the GPU provider is the likely culprit
    }
}

} // namespace

std::expected<StepResult, Error> HelperBackend::run(const Picture& in, const Step& step, std::stop_token stop) {
    return withRestarts(m->mutex, m->restarts, m->cpuOnly, m->options, m->helper, stop, [&] { return m->call(in, step, stop); });
}

std::expected<StepResult, Error> HelperBackend::interpolate(const Picture& a, const Picture& b, double timestep, std::stop_token stop) {
    Step step;
    step.model = Model::Interpolate;
    return withRestarts(m->mutex, m->restarts, m->cpuOnly, m->options, m->helper, stop,
                        [&] { return m->call(a, step, stop, &b, timestep); });
}

std::string HelperBackend::device() const {
    std::lock_guard lock(m->infoMutex);
    return m->device;
}

void HelperBackend::release() {
    std::lock_guard lock(m->mutex);
    m->helper.stop();
    m->loaded.clear();
    m->loadedSession = 0;
}

int HelperBackend::starts() const { return m->helper.starts(); }

} // namespace wabcut::ai::video
