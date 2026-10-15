#pragma once

// AI-14 test doubles: a scripted video (a red object moving over a grey, textured background at 25 fps; at the cut a
// new shot where the object is elsewhere and bigger) and an in-process segmenter that finds the red pixels, as the
// scripted wabcut-embed (fake_embed) does.

#include <wabcut/ai/segment/ObjectMask.h>
#include <wabcut/ai/segment/Propagation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace wabcut::ai::segment::test {

inline constexpr Rate kTestRate = kRate25;
inline Ticks frameTicks(std::int64_t n) { return framesToTicks(n, kTestRate); }

/// Where the object is at frame `n` (normalised box), and the cut.
struct ScriptedScene {
    int frames = 1000;
    int cutFrame = 60; ///< the first frame of the second shot (-1: one shot)

    MatteBox objectAt(int n) const {
        if (cutFrame >= 0 && n >= cutFrame) {
            return MatteBox{0.6, 0.5, 0.25, 0.3}; // the second shot: elsewhere and bigger
        }
        return MatteBox{0.1 + 0.004 * n, 0.3, 0.15, 0.2}; // moving right
    }
};

/// The scripted video: pictureAt draws the frame containing the time (stored = display orientation, 16:9).
class ScriptedVideo final : public FrameSource {
public:
    explicit ScriptedVideo(ScriptedScene scene = {}) : m_scene(scene) {}

    std::expected<search::Image, Error> pictureAt(Ticks t, int size) override {
        ++requests;
        if (failAt && frameOf(t) == *failAt) {
            return std::unexpected(Error(ErrorCode::DecoderError, "scripted decode failure"));
        }
        const int n = frameOf(t);
        if (n < 0 || n >= m_scene.frames) {
            return std::unexpected(Error(ErrorCode::NotFound, "past the last frame"));
        }
        return draw(n, size, size);
    }
    Ticks frameDuration() const override { return frameTicks(1); }
    TimeRange frames() const override { return TimeRange{Ticks{}, frameTicks(m_scene.frames)}; }
    double aspect() const override { return 16.0 / 9.0; }

    search::Image draw(int n, int width, int height) const {
        search::Image img;
        img.width = width;
        img.height = height;
        img.rgb.resize(static_cast<std::size_t>(width) * height * 3);
        const MatteBox b = m_scene.objectAt(n);
        const bool second = m_scene.cutFrame >= 0 && n >= m_scene.cutFrame;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const double u = (x + 0.5) / width, v = (y + 0.5) / height;
                std::uint8_t* p = &img.rgb[(static_cast<std::size_t>(y) * width + x) * 3];
                const auto grey = static_cast<std::uint8_t>((second ? 60 : 110) + ((x / 16 + y / 16) % 2) * 30);
                p[0] = p[1] = p[2] = grey;
                if (u >= b.x && u < b.x + b.width && v >= b.y && v < b.y + b.height) {
                    p[0] = 220;
                    p[1] = 30;
                    p[2] = 40;
                }
            }
        }
        return img;
    }

    int frameOf(Ticks t) const { return static_cast<int>(std::floor(static_cast<double>(t.value) / frameTicks(1).value)); }

    std::atomic<int> requests{0};
    std::optional<int> failAt;

private:
    ScriptedScene m_scene;
};

/// The red pixels of each picture, at the requested size; present while there are any (objectScore +6 / -6).
class RedSegmenter final : public Segmenter {
public:
    int inputSize() const override { return size; }
    std::string modelVersion() const override { return "red-v1"; }
    std::expected<Segmentation, Error> prompt(const search::Image& picture, const Prompt& p, int, int, int width, int height,
                                               std::stop_token) override {
        ++prompts;
        lastPrompt = p;
        prompted = true;
        return segment(picture, width, height);
    }
    std::expected<Segmentation, Error> track(const search::Image& picture, int, int, int width, int height, std::stop_token stop) override {
        if (!prompted) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "track before a prompt"));
        }
        ++tracked;
        if (failOnTrack && tracked >= *failOnTrack) {
            return std::unexpected(Error(ErrorCode::DecoderError, "scripted model failure"));
        }
        if (stopAfter && tracked >= *stopAfter && stopSource) {
            stopSource->request_stop();
        }
        (void)stop;
        return segment(picture, width, height);
    }

    static Segmentation segment(const search::Image& picture, int width, int height) {
        Segmentation s;
        s.matte.width = width;
        s.matte.height = height;
        s.matte.alpha.assign(static_cast<std::size_t>(width) * height, 0);
        bool any = false;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int px = std::min(picture.width - 1, static_cast<int>((x + 0.5) * picture.width / width));
                const int py = std::min(picture.height - 1, static_cast<int>((y + 0.5) * picture.height / height));
                const std::uint8_t* p = &picture.rgb[(static_cast<std::size_t>(py) * picture.width + px) * 3];
                if (p[0] > 150 && p[1] < 100 && p[2] < 100) {
                    s.matte.alpha[static_cast<std::size_t>(y) * width + x] = 255;
                    any = true;
                }
            }
        }
        s.iou = 0.9f;
        s.objectScore = any ? 6.0f : -6.0f;
        s.present = any;
        s.device = "TestCPU";
        return s;
    }

    int size = 256;
    int prompts = 0;
    int tracked = 0;
    bool prompted = false;
    Prompt lastPrompt;
    std::optional<int> failOnTrack;
    std::optional<int> stopAfter;
    std::stop_source* stopSource = nullptr;
};

/// A fresh directory under the test output.
inline std::filesystem::path freshDir(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai_segment" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

/// Object-mask services over the scripted video and segmenter (fingerprint 0xABCDEF; the cache in `cacheRoot`).
struct ScriptedServices {
    explicit ScriptedServices(std::filesystem::path cacheRoot, ScriptedScene scene = {}) : scene(scene) {
        services = std::make_shared<SegmentServices>();
        services->model = segmenter->modelVersion();
        services->segmenter = [this](std::stop_token) -> std::expected<std::shared_ptr<Segmenter>, Error> {
            ++opened;
            return std::shared_ptr<Segmenter>(segmenter);
        };
        services->frames = [this](const MediaItem&, int) -> std::expected<std::unique_ptr<FrameSource>, Error> {
            auto video = std::make_unique<ScriptedVideo>(this->scene);
            video->failAt = failAt;
            return std::unique_ptr<FrameSource>(std::move(video));
        };
        services->fingerprint = [](const MediaItem&) -> std::expected<std::uint64_t, Error> { return 0xABCDEFull; };
        services->cacheRoot = std::move(cacheRoot);
        services->remember = [this](std::shared_ptr<const MatteTrack> track) { remembered.push_back(std::move(track)); };
        services->progress = [this](const SegmentProgress& p) {
            if (p.frame) {
                ++framesReported;
            }
        };
    }

    ScriptedScene scene;
    std::shared_ptr<RedSegmenter> segmenter = std::make_shared<RedSegmenter>();
    std::shared_ptr<SegmentServices> services;
    std::vector<std::shared_ptr<const MatteTrack>> remembered;
    std::optional<int> failAt;
    int opened = 0;
    int framesReported = 0;
};

} // namespace wabcut::ai::segment::test
