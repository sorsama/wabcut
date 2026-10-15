#include <wabcut/ai/segment/HelperSegmenter.h>

#include <wabcut/ai/search/EmbedHelper.h>
#include <wabcut/ai/search/EmbedProtocol.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>

namespace wabcut::ai::segment {

namespace embed = search::embed;

std::vector<const ModelInfo*> segmentModelFiles() {
    return {findModel("sam2.1-tiny-video-encoder"),          findModel("sam2.1-tiny-video-decoder"),
            findModel("sam2.1-tiny-video-memory-attention"), findModel("sam2.1-tiny-video-memory-encoder"),
            findModel("sam2.1-tiny-video-pointer-pe"),       findModel("sam2.1-tiny-video-constants")};
}

std::string segmentModelVersion() {
    // The encoder's and decoder's digests (first 8 hex digits) and the pre/post-processing (1024 stretched input,
    // bilinear logits to the matte size): file-name safe, it is part of the cache keys.
    const auto files = segmentModelFiles();
    std::string version = "sam2.1-tiny-video";
    for (const ModelInfo* m : {files[0], files[1]}) {
        if (m != nullptr) {
            version += "-" + m->sha256.substr(0, 8);
        }
    }
    return version + "-v1";
}

std::filesystem::path segmentModelDirectory(const ModelStore& store) {
    const ModelInfo* encoder = segmentModelFiles().front();
    return encoder != nullptr ? store.pathFor(*encoder).parent_path() : store.root() / "sam2.1-tiny-video";
}

bool segmentModelInstalled(const ModelStore& store) {
    return std::ranges::all_of(segmentModelFiles(), [&](const ModelInfo* m) { return m != nullptr && store.isInstalled(*m); });
}

HelperSegmenterOptions segmenterOptions(const ModelStore& store) {
    HelperSegmenterOptions options;
    options.modelDirectory = segmentModelDirectory(store);
    options.modelVersion = segmentModelVersion();
    return options;
}

namespace {

search::EmbedHelperOptions helperOptions(const HelperSegmenterOptions& o) {
    search::EmbedHelperOptions h;
    h.executable = o.executable;
    h.arguments = o.arguments;
    h.log = o.log;
    h.startTimeout = o.startTimeout;
    h.lowPriority = false; // the user waits for it
    return h;
}

std::filesystem::path workDirOf(const HelperSegmenterOptions& o) {
    if (!o.workDir.empty()) {
        return o.workDir;
    }
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "wabcut-embed";
}

/// The prompt's points in the model's input pixels: clicks (label 1 / 0), a box as its corners (labels 2 and 3).
std::vector<embed::SegmentPoint> pointsOf(const Prompt& p, int size) {
    std::vector<embed::SegmentPoint> out;
    const auto px = [&](double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0) * size); };
    if (p.box) {
        out.push_back({px(p.box->x), px(p.box->y), 2});
        out.push_back({px(p.box->x + p.box->width), px(p.box->y + p.box->height), 3});
    }
    for (const PromptPoint& point : p.points) {
        out.push_back({px(point.x), px(point.y), point.positive ? 1 : 0});
    }
    return out;
}

} // namespace

struct HelperSegmenter::Impl {
    explicit Impl(HelperSegmenterOptions o) : options(std::move(o)), helper(helperOptions(options)), dir(workDirOf(options)) {}

    HelperSegmenterOptions options;
    search::EmbedHelper helper;
    std::filesystem::path dir;
    std::mutex mutex;            ///< one request at a time
    mutable std::mutex infoMutex;
    std::string device;
    int loadedSession = 0;
    int promptedSession = 0;     ///< the helper session holding the object's memory
    bool cpuOnly = false;
    int restarts = 0;            ///< since the last prompt
    /// The last prompt, replayed after a crash.
    struct Replay {
        search::Image picture;
        std::vector<embed::SegmentPoint> points;
        int frame = 0;
        int frames = 1;
    };
    std::optional<Replay> replay;
    std::atomic<int> counter{0};

    std::expected<void, Error> load(const std::stop_token& stop) {
        if (auto started = helper.start(stop); !started) {
            return started;
        }
        if (loadedSession == helper.session()) {
            return {};
        }
        const auto send = [&](const std::string& on) {
            embed::Load request;
            request.id = helper.nextId();
            request.segmentation = options.modelDirectory;
            request.device = on;
            request.threads = options.threads;
            return helper.call<embed::Loaded>(embed::Request{request}, request.id, "loading the segmentation model", options.loadTimeout, stop);
        };
        auto loaded = send(cpuOnly ? std::string("cpu") : options.device);
        if (!loaded && !cpuOnly && options.device != "cpu" && !helper.running() && !stop.stop_requested()) {
            // The GPU provider took the helper down while creating its sessions: once more on the CPU.
            cpuOnly = true;
            if (auto restarted = helper.start(stop); !restarted) {
                return restarted;
            }
            loaded = send("cpu");
        }
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        loadedSession = helper.session();
        std::lock_guard lock(infoMutex);
        device = loaded->device;
        return {};
    }

    std::expected<Segmentation, Error> call(const search::Image& picture, const std::vector<embed::SegmentPoint>& points, bool prompt,
                                            int frame, int frames, int width, int height, const std::stop_token& stop) {
        if (picture.width != 1024 || picture.height != 1024 || picture.rgb.size() != std::size_t{1024} * 1024 * 3) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the segmentation model takes 1024 x 1024 pictures"));
        }
        if (width <= 0 || height <= 0 || width > embed::kMaxMatteSide || height > embed::kMaxMatteSide) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "matte size out of bounds"));
        }
        if (auto loaded = load(stop); !loaded) {
            return std::unexpected(loaded.error());
        }
        if (!prompt && replay && promptedSession != helper.session()) {
            // A new helper (after a crash or release()) has no memory of the object: the prompt again (a tiny matte).
            if (auto again = call(replay->picture, replay->points, true, replay->frame, replay->frames, 16, 16, stop); !again) {
                return std::unexpected(again.error());
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string stem = "segment-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" + std::to_string(counter++);
        const std::filesystem::path pictureFile = dir / (stem + ".rgb");
        const std::filesystem::path matteFile = dir / (stem + ".a8");
        struct Cleanup {
            std::filesystem::path a, b;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove(a, e);
                std::filesystem::remove(b, e);
            }
        } cleanup{pictureFile, matteFile};
        auto written = embed::writeImageBatch(pictureFile, std::span<const search::Image>(&picture, 1));
        if (!written) {
            return std::unexpected(written.error());
        }
        embed::SegmentFrame request;
        request.id = helper.nextId();
        request.mode = prompt ? "prompt" : "track";
        request.images = *written;
        request.frame = frame;
        request.frames = std::max(frames, frame + 1);
        request.points = points;
        request.output = matteFile;
        request.width = width;
        request.height = height;
        auto answer = helper.call<embed::Segmented>(embed::Request{request}, request.id, prompt ? "segmenting the object" : "tracking the object",
                                                    options.requestTimeout, stop);
        if (!answer) {
            return std::unexpected(answer.error());
        }
        if (answer->width != width || answer->height != height) {
            return std::unexpected(helper.abandon(ErrorCode::DecoderError, "wabcut-embed returned a matte of the wrong size"));
        }
        Segmentation s;
        s.matte.width = width;
        s.matte.height = height;
        s.matte.alpha.resize(static_cast<std::size_t>(width) * height);
        std::ifstream in(matteFile, std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(s.matte.alpha.data()), static_cast<std::streamsize>(s.matte.alpha.size()))) {
            return std::unexpected(Error(ErrorCode::IoError, "the segmentation matte could not be read"));
        }
        s.iou = answer->iou;
        s.objectScore = answer->objectScore;
        s.present = answer->present;
        s.milliseconds = answer->milliseconds;
        s.device = answer->device;
        if (prompt) {
            promptedSession = helper.session();
        }
        return s;
    }

    /// `run`; after a crash (the helper is gone) again, on the CPU (call() replays the prompt into the new helper).
    template <class Fn>
    std::expected<Segmentation, Error> withRestart(Fn&& run, const std::stop_token& stop) {
        while (true) {
            auto r = run();
            if (r || helper.running() || stop.stop_requested() || r.error().code == ErrorCode::InvalidArgument ||
                r.error().code == ErrorCode::Cancelled) {
                return r;
            }
            if (++restarts > options.maxRestarts) {
                return std::unexpected(Error(r.error().code, "the segmentation helper keeps crashing: " + r.error().message));
            }
            cpuOnly = cpuOnly || options.device != "cpu"; // the GPU provider is the likely culprit
        }
    }
};

HelperSegmenter::HelperSegmenter(HelperSegmenterOptions options) : m(std::make_unique<Impl>(std::move(options))) {}

HelperSegmenter::~HelperSegmenter() = default;

std::string HelperSegmenter::modelVersion() const { return m->options.modelVersion.empty() ? segmentModelVersion() : m->options.modelVersion; }

std::expected<Segmentation, Error> HelperSegmenter::prompt(const search::Image& picture, const Prompt& p, int frame, int frames, int width,
                                                           int height, std::stop_token stop) {
    std::lock_guard lock(m->mutex);
    const std::vector<embed::SegmentPoint> points = pointsOf(p, inputSize());
    if (points.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "point at the object or draw a box around it"));
    }
    m->restarts = 0;
    m->replay = Impl::Replay{picture, points, frame, frames};
    return m->withRestart([&] { return m->call(picture, points, true, frame, frames, width, height, stop); }, stop);
}

std::expected<Segmentation, Error> HelperSegmenter::track(const search::Image& picture, int frame, int frames, int width, int height,
                                                          std::stop_token stop) {
    std::lock_guard lock(m->mutex);
    if (!m->replay) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no object to track: prompt first"));
    }
    return m->withRestart([&] { return m->call(picture, {}, false, frame, frames, width, height, stop); }, stop);
}

std::string HelperSegmenter::device() const {
    std::lock_guard lock(m->infoMutex);
    return m->device;
}

int HelperSegmenter::starts() const { return m->helper.starts(); }

void HelperSegmenter::release() {
    std::lock_guard lock(m->mutex);
    m->helper.stop();
    m->loadedSession = 0;
}

} // namespace wabcut::ai::segment
