#include <wabcut/ai/video/MorphFaces.h>

#include <wabcut/ai/vision/HelperDetector.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>

namespace wabcut::ai::video {

namespace {

constexpr int kDetectorSide = 640; ///< what YuNet is fed (it letterboxes into 640 x 640)

class HelperFaceFinder final : public playback::FaceFinder {
public:
    HelperFaceFinder(std::filesystem::path root, std::filesystem::path helper) : m_root(std::move(root)), m_helper(std::move(helper)) {}

    std::vector<motion::FaceFeatures> find(const motion::Plane& grey) override {
        std::vector<motion::FaceFeatures> out;
        if (grey.empty()) {
            return out;
        }
        std::lock_guard lock(m_mutex); // one helper, one request at a time
        const std::shared_ptr<vision::HelperDetector> detector = detectorLocked();
        if (!detector) {
            return out;
        }
        const double scale = std::min(1.0, static_cast<double>(kDetectorSide) / std::max(grey.width, grey.height));
        search::Image picture;
        picture.width = std::max(1, static_cast<int>(std::lround(grey.width * scale)));
        picture.height = std::max(1, static_cast<int>(std::lround(grey.height * scale)));
        picture.rgb.resize(static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height) * 3);
        for (int y = 0; y < picture.height; ++y) {
            const float sy = (static_cast<float>(y) + 0.5f) / static_cast<float>(scale) - 0.5f;
            for (int x = 0; x < picture.width; ++x) {
                const float sx = (static_cast<float>(x) + 0.5f) / static_cast<float>(scale) - 0.5f;
                const long v = std::lround(std::clamp(grey.sample(sx, sy), 0.0f, 1.0f) * 255.0f);
                std::uint8_t* px = &picture.rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(picture.width) + static_cast<std::size_t>(x)) * 3];
                px[0] = px[1] = px[2] = static_cast<std::uint8_t>(v);
            }
        }
        const std::array<search::Image, 1> one{std::move(picture)};
        auto found = detector->detect(one, {});
        if (!found || found->empty()) {
            return out; // a detector that fails is the plain flow, not an error of the Morph Cut
        }
        const float back = static_cast<float>(1.0 / scale);
        for (const vision::Detection& d : found->front().faces) {
            if (d.landmarks.size() < 10 || d.box.w < 8.0f || d.box.h < 8.0f) {
                continue;
            }
            motion::FaceFeatures f;
            f.x = d.box.x * back;
            f.y = d.box.y * back;
            f.w = d.box.w * back;
            f.h = d.box.h * back;
            f.score = d.score;
            for (std::size_t i = 0; i < 5; ++i) {
                f.points[i] = {d.landmarks[2 * i] * back, d.landmarks[2 * i + 1] * back};
            }
            out.push_back(f);
        }
        return out;
    }

    std::string version() const override {
        std::lock_guard lock(m_versionMutex);
        const auto now = std::chrono::steady_clock::now();
        if (m_versionAt.time_since_epoch().count() == 0 || now - m_versionAt > std::chrono::seconds(2)) {
            // The key is asked for many times a frame: look at the disk now and then, not each time.
            m_version = "none";
            if (const ModelInfo* info = faceDetectorModel(); info != nullptr && faceDetectorInstalled(m_root)) {
                m_version = "yunet-" + info->sha256.substr(0, 8) + "-m1";
            }
            m_versionAt = now;
        }
        return m_version;
    }

private:
    std::shared_ptr<vision::HelperDetector> detectorLocked() {
        const ModelInfo* info = faceDetectorModel();
        if (info == nullptr) {
            return nullptr;
        }
        const ModelStore store(m_root.empty() ? ModelStore::defaultRoot() : m_root);
        if (!store.isInstalled(*info)) {
            m_detector.reset();
            return nullptr;
        }
        const std::filesystem::path file = store.pathFor(*info);
        if (!m_detector || m_file != file) {
            vision::HelperDetectorOptions options;
            options.executable = m_helper;
            options.faceModel = file;
            options.modelVersion = "yunet-morph";
            m_detector = std::make_shared<vision::HelperDetector>(std::move(options));
            m_file = file;
        }
        return m_detector;
    }

    std::filesystem::path m_root;
    std::filesystem::path m_helper;
    std::mutex m_mutex;
    std::shared_ptr<vision::HelperDetector> m_detector;
    std::filesystem::path m_file;
    mutable std::mutex m_versionMutex;
    mutable std::string m_version;
    mutable std::chrono::steady_clock::time_point m_versionAt{};
};

} // namespace

const ModelInfo* faceDetectorModel() { return findModel("yunet-2023mar"); }

bool faceDetectorInstalled(const std::filesystem::path& root) {
    const ModelInfo* info = faceDetectorModel();
    if (info == nullptr) {
        return false;
    }
    const ModelStore store(root.empty() ? ModelStore::defaultRoot() : root);
    return store.isInstalled(*info);
}

std::shared_ptr<playback::FaceFinder> makeFaceFinder(const std::filesystem::path& root, const std::filesystem::path& helper) {
    return std::make_shared<HelperFaceFinder>(root, helper);
}

} // namespace wabcut::ai::video
