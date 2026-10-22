#include <wabcut/ai/vision/ColorFaces.h>

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/vision/HelperDetector.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace wabcut::ai::vision {

namespace {

class DetectorFaceFinder final : public color::FaceFinder {
public:
    explicit DetectorFaceFinder(std::shared_ptr<SubjectDetector> detector) : m_detector(std::move(detector)) {}

    std::expected<std::vector<color::FaceBox>, Error> faces(const color::ColorImage& image) override {
        const Image picture = detectorPicture(image);
        if (!picture.valid()) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "no picture"));
        }
        const std::array<Image, 1> one{picture};
        std::lock_guard lock(m_mutex); // one helper, one request at a time
        auto found = m_detector->detect(one, {});
        if (!found) {
            return std::unexpected(found.error());
        }
        std::vector<color::FaceBox> out;
        if (found->empty()) {
            return out;
        }
        for (const Detection& d : found->front().faces) {
            const Box b = normalized(d.box, static_cast<float>(picture.width), static_cast<float>(picture.height));
            const double x0 = std::clamp<double>(b.x, 0.0, 1.0), y0 = std::clamp<double>(b.y, 0.0, 1.0);
            const double x1 = std::clamp<double>(b.x + b.w, 0.0, 1.0), y1 = std::clamp<double>(b.y + b.h, 0.0, 1.0);
            if (x1 > x0 && y1 > y0) {
                out.push_back(color::FaceBox{x0, y0, x1 - x0, y1 - y0});
            }
        }
        return out;
    }

private:
    std::shared_ptr<SubjectDetector> m_detector;
    std::mutex m_mutex;
};

} // namespace

Image detectorPicture(const color::ColorImage& image, int longSide) {
    Image out;
    if (image.width <= 0 || image.height <= 0 || image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
        return out;
    }
    const double scale = std::min(1.0, static_cast<double>(longSide) / std::max(image.width, image.height));
    out.width = std::max(1, static_cast<int>(std::lround(image.width * scale)));
    out.height = std::max(1, static_cast<int>(std::lround(image.height * scale)));
    out.rgb.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    for (int y = 0; y < out.height; ++y) {
        const int sy = std::min(image.height - 1, static_cast<int>((y + 0.5) * image.height / out.height));
        for (int x = 0; x < out.width; ++x) {
            const int sx = std::min(image.width - 1, static_cast<int>((x + 0.5) * image.width / out.width));
            const float* p = image.rgba.data() + (static_cast<std::size_t>(sy) * image.width + sx) * 4;
            std::uint8_t* q = &out.rgb[(static_cast<std::size_t>(y) * out.width + x) * 3];
            for (int c = 0; c < 3; ++c) {
                q[c] = static_cast<std::uint8_t>(std::clamp(std::lround(std::clamp(p[c], 0.0f, 1.0f) * 255.0f), 0l, 255l));
            }
        }
    }
    return out;
}

std::shared_ptr<color::FaceFinder> detectorFaceFinder(std::shared_ptr<SubjectDetector> detector) {
    return detector ? std::make_shared<DetectorFaceFinder>(std::move(detector)) : nullptr;
}

std::shared_ptr<color::FaceFinder> installedFaceFinder(const std::filesystem::path& modelRoot, const std::filesystem::path& helper) {
    const ModelStore store(modelRoot.empty() ? ModelStore::defaultRoot() : modelRoot);
    if (!subjectModelsInstalled(store)) {
        return nullptr;
    }
    HelperDetectorOptions options = subjectDetectorOptions(store);
    if (!helper.empty()) {
        options.executable = helper;
    }
    return detectorFaceFinder(std::make_shared<HelperDetector>(std::move(options)));
}

} // namespace wabcut::ai::vision
