#include <wabcut/ai/vision/HelperDetector.h>

#include <wabcut/ai/search/EmbedProtocol.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>

namespace wabcut::ai::vision {

namespace {

std::string randomToken() {
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32) ^ device());
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(rng()));
    return text;
}

search::EmbedHelperOptions helperOptions(const HelperDetectorOptions& o) {
    search::EmbedHelperOptions h;
    h.executable = o.executable;
    h.arguments = o.arguments;
    h.log = o.log;
    h.startTimeout = o.startTimeout;
    return h;
}

bool finiteRow(const std::vector<float>& row) {
    return std::ranges::all_of(row, [](float v) { return std::isfinite(v); });
}

/// A helper row (x, y, w, h, score, ...) in the batch picture's pixels as a Detection in the caller's pixels.
Detection detectionOf(const std::vector<float>& row, float scale) {
    Detection d;
    d.box = Box{row[0] * scale, row[1] * scale, row[2] * scale, row[3] * scale};
    d.score = row[4];
    return d;
}

} // namespace

std::vector<const ModelInfo*> subjectModelFiles() { return {findModel("yunet-2023mar"), findModel("yolox-tiny")}; }

std::string subjectModelVersion() {
    // The weights' digests (first 8 hex digits) and the decoding (Detection.cpp: input sizes, letterboxes, NMS).
    std::string version;
    for (const ModelInfo* m : subjectModelFiles()) {
        if (m != nullptr) {
            version += (version.empty() ? "" : "+") + m->id + '-' + m->sha256.substr(0, 8);
        }
    }
    return version + "/v1";
}

HelperDetectorOptions subjectDetectorOptions(const ModelStore& store) {
    HelperDetectorOptions options;
    const auto files = subjectModelFiles();
    if (files[0] != nullptr) {
        options.faceModel = store.pathFor(*files[0]);
    }
    if (files[1] != nullptr) {
        options.objectModel = store.pathFor(*files[1]);
    }
    options.modelVersion = subjectModelVersion();
    return options;
}

bool subjectModelsInstalled(const ModelStore& store) {
    return std::ranges::all_of(subjectModelFiles(), [&](const ModelInfo* m) { return m != nullptr && store.isInstalled(*m); });
}

// --- HelperDetector -------------------------------------------------------------------------------------------------

struct HelperDetector::Impl {
    explicit Impl(HelperDetectorOptions o) : options(std::move(o)), helper(helperOptions(options)) {}

    HelperDetectorOptions options;
    search::EmbedHelper helper;
    mutable std::mutex mutex;      ///< one request at a time
    mutable std::mutex infoMutex;  ///< `device` and `milliseconds`, so status reads never wait for a request
    int loadedSession = 0;         ///< the helper session the detectors were loaded in (0 = not loaded)
    std::string device;
    double milliseconds = 0.0;
    std::string token = randomToken();

    std::expected<void, Error> load(const std::stop_token& stop) {
        if (auto started = helper.start(stop); !started) {
            return started;
        }
        const int session = helper.session();
        if (loadedSession == session) {
            return {};
        }
        search::embed::Load request;
        request.id = helper.nextId();
        request.device = options.device;
        request.threads = options.threads;
        request.faces = options.faceModel;
        request.objects = options.objectModel;
        auto loaded = helper.call<search::embed::Loaded>(search::embed::Request{request}, request.id, "loading the detectors",
                                                         options.loadTimeout, stop);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        {
            std::lock_guard infoLock(infoMutex);
            device = loaded->device;
        }
        loadedSession = session;
        return {};
    }

    /// Detects in `pictures` (one size, within the protocol's bounds); `scale` maps their pixels to the caller's.
    std::expected<void, Error> run(std::vector<Image>& pictures, float scale, const std::filesystem::path& file,
                                   std::vector<FrameDetections>& out, const std::stop_token& stop) {
        auto written = search::embed::writeImageBatch(file, pictures);
        if (!written) {
            return std::unexpected(written.error());
        }
        search::embed::Detect request;
        request.id = helper.nextId();
        request.images = *written;
        request.faces = !options.faceModel.empty();
        request.objects = !options.objectModel.empty();
        request.faceThreshold = options.faceThreshold;
        request.objectThreshold = options.objectThreshold;
        request.allClasses = options.allClasses;
        auto detections = helper.call<search::embed::Detections>(search::embed::Request{request}, request.id, "detecting subjects",
                                                                 options.requestTimeout, stop);
        if (!detections) {
            return std::unexpected(detections.error());
        }
        if (detections->frames.size() != pictures.size()) {
            return std::unexpected(helper.abandon(ErrorCode::DecoderError, "wabcut-embed returned detections of " +
                                                                               std::to_string(detections->frames.size()) + " pictures for " +
                                                                               std::to_string(pictures.size())));
        }
        {
            std::lock_guard infoLock(infoMutex);
            milliseconds += detections->milliseconds;
        }
        for (const search::embed::DetectedFrame& frame : detections->frames) {
            FrameDetections found;
            for (const std::vector<float>& row : frame.faces) {
                if (!finiteRow(row)) {
                    continue;
                }
                Detection d = detectionOf(row, scale);
                d.classId = 0;
                for (std::size_t i = 5; i < search::embed::kFaceRow; ++i) {
                    d.landmarks.push_back(row[i] * scale);
                }
                found.faces.push_back(std::move(d));
            }
            for (const std::vector<float>& row : frame.objects) {
                if (!finiteRow(row) || row[5] < 0.0f || row[5] >= static_cast<float>(kYoloxClasses)) {
                    continue;
                }
                Detection d = detectionOf(row, scale);
                d.classId = static_cast<int>(row[5]);
                if (options.allClasses) {
                    found.objects.push_back(d);
                }
                if (d.classId == kCocoPerson) {
                    found.people.push_back(std::move(d));
                }
            }
            out.push_back(std::move(found));
        }
        return {};
    }
};

HelperDetector::HelperDetector(HelperDetectorOptions options) {
    if (options.workDir.empty()) {
        std::error_code ec;
        options.workDir = std::filesystem::temp_directory_path(ec) / "wabcut-embed";
    }
    options.batchSize = std::clamp(options.batchSize, 1, search::embed::kMaxImagesPerRequest);
    m_impl = std::make_unique<Impl>(std::move(options));
}

HelperDetector::~HelperDetector() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->helper.stop();
}

std::expected<std::vector<FrameDetections>, Error> HelperDetector::detect(std::span<const Image> frames, std::stop_token stop) {
    std::lock_guard lock(m_impl->mutex);
    Impl& d = *m_impl;
    std::vector<FrameDetections> out;
    if (frames.empty()) {
        return out;
    }
    if (d.options.faceModel.empty() && d.options.objectModel.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "subject detection needs a face or an object model"));
    }
    if (auto loaded = d.load(stop); !loaded) {
        return std::unexpected(loaded.error());
    }
    std::error_code ec;
    std::filesystem::create_directories(d.options.workDir, ec);
    const std::filesystem::path file = d.options.workDir / ("detect-" + d.token + ".rgb");
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code e;
            std::filesystem::remove(path, e);
        }
    } removeFile{file};

    out.reserve(frames.size());
    std::vector<Image> batch;
    float batchScale = 1.0f;
    int batchWidth = 0;
    int batchHeight = 0;
    const auto flush = [&]() -> std::expected<void, Error> {
        if (batch.empty()) {
            return {};
        }
        auto ran = d.run(batch, batchScale, file, out, stop);
        batch.clear();
        return ran;
    };
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const Image& frame = frames[i];
        if (!frame.valid()) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "subject detection: picture " + std::to_string(i) + " is empty"));
        }
        // Within the protocol's bounds: a larger picture is shrunk and its detections scaled back.
        const int side = std::max(frame.width, frame.height);
        const float scale = side > search::embed::kMaxImageSide ? static_cast<float>(side) / search::embed::kMaxImageSide : 1.0f;
        const int width = std::max(1, static_cast<int>(std::lround(static_cast<float>(frame.width) / scale)));
        const int height = std::max(1, static_cast<int>(std::lround(static_cast<float>(frame.height) / scale)));
        if (!batch.empty() && (width != batchWidth || height != batchHeight || static_cast<int>(batch.size()) >= d.options.batchSize)) {
            if (auto ok = flush(); !ok) {
                return std::unexpected(ok.error());
            }
        }
        batchWidth = width;
        batchHeight = height;
        batchScale = scale;
        batch.push_back(scale == 1.0f ? frame : search::resizeBilinear(frame, width, height));
    }
    if (auto ok = flush(); !ok) {
        return std::unexpected(ok.error());
    }
    return out;
}

std::string HelperDetector::modelVersion() const { return m_impl->options.modelVersion; }

std::string HelperDetector::device() const {
    std::lock_guard lock(m_impl->infoMutex);
    return m_impl->device;
}

void HelperDetector::shutdown() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->helper.stop();
}

int HelperDetector::starts() const { return m_impl->helper.starts(); }

double HelperDetector::inferenceMilliseconds() const {
    std::lock_guard lock(m_impl->infoMutex);
    return m_impl->milliseconds;
}

} // namespace wabcut::ai::vision
