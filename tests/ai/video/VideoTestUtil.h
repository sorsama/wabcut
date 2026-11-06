#pragma once

// Test doubles for the AI video-effect runner (0.7): synthetic pictures, a backend that "runs" the models by marking the
// values (denoise +1000, deblur +2000, upscalers a nearest-neighbour enlargement), and a frame source of N frames at 25 fps.
// No model, no media, no helper.

#include "FakeGaze.h"

#include <wabcut/ai/video/Backend.h>
#include <wabcut/ai/video/Picture.h>
#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/model/Time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace wabcut::ai::video::test {

inline constexpr Rate kVideoTestRate{25, 1};
inline Ticks frameTicks(std::int64_t n) { return framesToTicks(n, kVideoTestRate); }

/// An empty directory of its own under the test output.
inline std::filesystem::path freshDir(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-video" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

/// A deterministic picture: horizontal and vertical ramps with a diagonal and a checker, different for every seed.
inline Picture pattern(int width, int height, int seed = 0, bool alpha = false) {
    Picture p;
    p.width = width;
    p.height = height;
    p.rgb.resize(p.pixels() * 3);
    if (alpha) {
        p.alpha.resize(p.pixels());
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            const int checker = ((x / 4 + y / 4) % 2) * 6000;
            p.rgb[3 * i] = static_cast<std::uint16_t>(std::min(65535, 2000 + x * 40000 / std::max(1, width) + seed * 300 + checker));
            p.rgb[3 * i + 1] = static_cast<std::uint16_t>(std::min(65535, 2000 + y * 40000 / std::max(1, height) + seed * 100));
            p.rgb[3 * i + 2] = static_cast<std::uint16_t>(std::min(65535, 3000 + ((x + y) * 20000) / std::max(1, width + height) + seed * 50));
            if (alpha) {
                p.alpha[i] = static_cast<std::uint16_t>(65535 - x * 30000 / std::max(1, width));
            }
        }
    }
    return p;
}

/// The face map the fake face parser answers (and the scripted helper's, tests/ai/search/fake_embed.cpp): R skin, G mouth, B eyes, 16 bits,
/// all or nothing: skin is an ellipse over the middle half of the picture, the mouth a bar low in it, the eyes two bars high in it.
inline Picture fakeFaceMap(int width, int height) {
    Picture p;
    p.width = width;
    p.height = height;
    p.rgb.assign(p.pixels() * 3, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double u = (x + 0.5) / width;
            const double v = (y + 0.5) / height;
            const double ex = (u - 0.5) / 0.25;
            const double ey = (v - 0.5) / 0.35;
            const bool mouth = u > 0.42 && u < 0.58 && v > 0.62 && v < 0.68;
            const bool eyes = v > 0.38 && v < 0.42 && ((u > 0.35 && u < 0.43) || (u > 0.57 && u < 0.65));
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3;
            p.rgb[i] = ex * ex + ey * ey < 1.0 && !mouth && !eyes ? 65535 : 0;
            p.rgb[i + 1] = mouth ? 65535 : 0;
            p.rgb[i + 2] = eyes ? 65535 : 0;
        }
    }
    return p;
}

/// "Runs" the models by marking the values (clamped): denoise +1000, deblur +2000, an upscaler enlarges by nearest
/// neighbour. Records every call; can fail at a given call or report a missing model.
class FakeBackend final : public IVideoBackend {
public:
    struct Call {
        Model model;
        int width;
        int height;
        double sigma;
        int tile;
    };
    std::vector<Call> calls;
    int failAtCall = -1;                      ///< the call (0-based) that fails
    ErrorCode failure = ErrorCode::DecoderError;
    std::function<void(int)> onCall;          ///< after a call succeeded: its 0-based index
    std::string deviceName = "FakeGPU";
    int wrongSizeAtCall = -1;                 ///< the call that returns a picture of the wrong size
    /// Frame interpolation calls (0.7): the weight of each, and the sizes.
    struct Interpolation {
        double weight;
        int width;
        int height;
    };
    std::vector<Interpolation> interpolations;
    int failInterpolationAt = -1;             ///< the interpolation (0-based) that fails
    /// 0.7 eye measurements: the face the fake landmark model finds (`gazeFaces` copies of it; 0 = a picture without a face).
    FakeFaceSpec gaze;
    int gazeFaces = 1;

    std::expected<StepResult, Error> run(const Picture& in, const Step& step, std::stop_token stop) override {
        if (stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        const int index = static_cast<int>(calls.size());
        calls.push_back({step.model, in.width, in.height, step.sigma, step.tile});
        if (index == failAtCall) {
            return std::unexpected(Error(failure, failure == ErrorCode::NotFound ? "the model is not downloaded" : "the helper crashed"));
        }
        if (step.model == Model::FaceParse) {
            // 0.7 face map: skin, mouth and eyes of one fixed face, the picture's size.
            StepResult f;
            f.device = deviceName;
            f.milliseconds = 10.0;
            f.picture = fakeFaceMap(in.width, in.height);
            if (onCall) {
                onCall(index);
            }
            return f;
        }
        if (step.model == Model::FaceLandmarks) {
            // 0.7 eye measurements: the landmark table of the fake face (a picture kGazeSlotCount wide, a row per face), not a picture of the input's size.
            StepResult g;
            g.device = deviceName;
            g.milliseconds = 10.0;
            g.picture = fakeGazeTable(in.width, in.height, gaze, gazeFaces);
            if (onCall) {
                onCall(index);
            }
            return g;
        }
        if (step.model == Model::Depth) {
            // 0.7 depth: the nearness ramps from 0 (left, far) to 1 (right, near), the same size, grey.
            StepResult d;
            d.device = deviceName;
            d.milliseconds = 10.0;
            d.picture.width = in.width;
            d.picture.height = in.height;
            d.picture.rgb.resize(d.picture.pixels() * 3);
            for (int y = 0; y < in.height; ++y) {
                for (int x = 0; x < in.width; ++x) {
                    const auto v = static_cast<std::uint16_t>(65535.0 * x / std::max(1, in.width - 1));
                    for (int c = 0; c < 3; ++c) {
                        d.picture.rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(in.width) + static_cast<std::size_t>(x)) * 3 +
                                      static_cast<std::size_t>(c)] = v;
                    }
                }
            }
            if (onCall) {
                onCall(index);
            }
            return d;
        }
        const int scale = describe(step.model).scale;
        const int mark = step.model == Model::Denoise ? 1000 : step.model == Model::Deblur ? 2000 : 0;
        StepResult r;
        r.device = deviceName;
        r.milliseconds = 10.0;
        r.picture.width = in.width * scale;
        r.picture.height = in.height * scale;
        r.picture.rgb.resize(r.picture.pixels() * 3);
        for (int y = 0; y < r.picture.height; ++y) {
            for (int x = 0; x < r.picture.width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const int v = in.rgb[(static_cast<std::size_t>(y / scale) * static_cast<std::size_t>(in.width) + static_cast<std::size_t>(x / scale)) * 3 +
                                         static_cast<std::size_t>(c)] + mark;
                    r.picture.rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(r.picture.width) + static_cast<std::size_t>(x)) * 3 +
                                  static_cast<std::size_t>(c)] = static_cast<std::uint16_t>(std::min(v, 65535));
                }
            }
        }
        if (index == wrongSizeAtCall) {
            r.picture = in; // not enlarged
        }
        if (onCall) {
            onCall(index);
        }
        return r;
    }
    std::expected<StepResult, Error> interpolate(const Picture& a, const Picture& b, double timestep, std::stop_token stop) override {
        if (stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        const int index = static_cast<int>(interpolations.size());
        interpolations.push_back({timestep, a.width, a.height});
        if (index == failInterpolationAt) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the helper crashed"));
        }
        StepResult r;
        r.device = deviceName;
        r.milliseconds = 10.0;
        r.picture = a;
        r.picture.alpha.clear();
        for (std::size_t i = 0; i < r.picture.rgb.size(); ++i) { // a straight cross-fade: a + (b - a) x timestep
            r.picture.rgb[i] = static_cast<std::uint16_t>(std::lround(a.rgb[i] + (static_cast<double>(b.rgb[i]) - a.rgb[i]) * timestep));
        }
        return r;
    }
    std::string device() const override { return deviceName; }
};

/// A packed RGBA8 stored frame of a picture (the tests' stand-in for the media layer's encoder).
inline StoredFrame packRgba8(const Picture& p) {
    StoredFrame s;
    s.width = p.width;
    s.height = p.height;
    s.format = 8; // media::PixelFormat::RGBA8
    s.planes.resize(1);
    s.planes[0].width = p.width;
    s.planes[0].height = p.height;
    s.planes[0].bytesPerTexel = 4;
    s.planes[0].data.resize(p.pixels() * 4);
    for (std::size_t i = 0; i < p.pixels(); ++i) {
        for (int c = 0; c < 3; ++c) {
            s.planes[0].data[4 * i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(p.rgb[3 * i + static_cast<std::size_t>(c)] >> 8);
        }
        s.planes[0].data[4 * i + 3] = p.hasAlpha() ? static_cast<std::uint8_t>(p.alpha[i] >> 8) : 255;
    }
    return s;
}

/// `frames` frames at 25 fps of pattern(width, height, frame number).
class FakeSource final : public IFrameSource {
public:
    FakeSource(int frames, int width, int height, bool alpha = false) : m_frames(frames), m_width(width), m_height(height), m_alpha(alpha) {}

    std::atomic<int> pictureCalls{0}; ///< frames converted to pictures (skipped frames cost none)
    std::atomic<int> frameCalls{0};
    int failFrame = -1;               ///< the frame whose lookup fails

    std::expected<std::optional<SourceFrame>, Error> frameAt(Ticks time, const std::stop_token&) override {
        ++frameCalls;
        const std::int64_t n = time.value / frameTicks(1).value;
        if (n >= m_frames) {
            return std::optional<SourceFrame>{};
        }
        if (static_cast<int>(n) == failFrame) {
            return std::unexpected(Error(ErrorCode::DecoderError, "scripted decode failure"));
        }
        SourceFrame f;
        f.pts = frameTicks(n);
        f.duration = frameTicks(1);
        const int seed = static_cast<int>(n);
        f.picture = [this, seed]() -> std::expected<Picture, Error> {
            ++pictureCalls;
            return pattern(m_width, m_height, seed, m_alpha);
        };
        f.encode = [](const Picture& p) -> std::expected<StoredFrame, Error> { return packRgba8(p); };
        return std::optional<SourceFrame>{std::move(f)};
    }

private:
    int m_frames;
    int m_width;
    int m_height;
    bool m_alpha;
};

/// A cache identity for the tests.
inline CacheIdentity identity(std::uint64_t chain = 7) {
    CacheIdentity id;
    id.media = 0x1234;
    id.stream = 0;
    id.chain = chain;
    id.modelVersion = "fake-model-p1";
    return id;
}

} // namespace wabcut::ai::video::test
