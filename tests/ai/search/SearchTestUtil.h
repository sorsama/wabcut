#pragma once

// Fixtures of the smart-search tests: no model, no decoder, no download. Pictures are solid colours and the fixture
// embedder maps a picture to its mean colour and a text to its colour words ("red" -> the red axis), so "which shot is
// red" has a known answer; media are hand-made items with shots, colours, dates and transcripts in memory.

#include <wabcut/ai/search/FootageSearch.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace wabcut::ai::search::test {

inline constexpr int kFixtureDim = 8;

/// A scratch directory, removed when the test ends.
struct TempDir {
    explicit TempDir(const std::string& name) {
        std::random_device device;
        path = std::filesystem::temp_directory_path() / ("wabcut-search-" + name + "-" + std::to_string(device()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::filesystem::path path;
};

inline Vector unit(std::vector<float> values, int dim = kFixtureDim) {
    values.resize(static_cast<std::size_t>(dim), 0.0f);
    normalize(values);
    return values;
}

inline Image solid(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    for (std::size_t i = 0; i < image.rgb.size(); i += 3) {
        image.rgb[i] = r;
        image.rgb[i + 1] = g;
        image.rgb[i + 2] = b;
    }
    return image;
}

/// Mean colour of a picture as a vector; colour words of a text.
class FixtureEmbedder : public Embedder {
public:
    std::expected<std::vector<Vector>, Error> embedImages(std::span<const Image> images, std::stop_token stop) override {
        if (stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        ++imageCalls;
        std::vector<Vector> out;
        for (const Image& image : images) {
            double sum[3] = {0, 0, 0};
            for (std::size_t i = 0; i + 2 < image.rgb.size(); i += 3) {
                for (int c = 0; c < 3; ++c) {
                    sum[c] += image.rgb[i + static_cast<std::size_t>(c)];
                }
            }
            const double n = std::max<double>(1.0, static_cast<double>(image.rgb.size() / 3)) * 255.0;
            out.push_back(unit({static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n), 0, 0, 0, 0,
                                0.05f}));
            imagesEmbedded += 1;
        }
        return out;
    }
    std::expected<Vector, Error> embedText(std::string_view text, std::stop_token) override {
        ++textCalls;
        const auto has = [&](std::string_view word) { return text.find(word) != std::string_view::npos; };
        const float r = has("red") || has("yellow") ? 1.0f : 0.0f;
        const float g = has("green") || has("yellow") ? 1.0f : 0.0f;
        const float b = has("blue") ? 1.0f : 0.0f;
        if (r + g + b == 0.0f) {
            return unit({0, 0, 0, 0, 0, 0, 1, 0}); // nothing any picture shows
        }
        return unit({r, g, b});
    }
    std::string modelVersion() const override { return version; }
    int dimension() const override { return kFixtureDim; }

    std::string version = "fixture-colours/1";
    std::atomic<int> imageCalls{0};
    std::atomic<int> imagesEmbedded{0};
    std::atomic<int> textCalls{0};
};

using Rgb = std::array<std::uint8_t, 3>;
inline constexpr Rgb kRed{220, 20, 20};
inline constexpr Rgb kGreen{20, 200, 30};
inline constexpr Rgb kBlue{20, 30, 220};
inline constexpr Rgb kGrey{120, 120, 120};

inline Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

/// Shots of [0, length) seconds cut at `cuts` seconds.
inline analysis::ShotAnalysis shotsAt(double length, std::vector<double> cuts) {
    analysis::ShotAnalysis shots;
    shots.start = Ticks{0};
    shots.end = seconds(length);
    for (const double c : cuts) {
        shots.cuts.push_back(analysis::ShotCut{seconds(c), static_cast<std::int64_t>(c * 25), analysis::CutKind::Cut, 0.9f});
    }
    return shots;
}

/// A transcript of (word, start s, end s).
inline std::shared_ptr<const Transcript> spoken(std::string language, std::vector<std::tuple<std::string, double, double>> words) {
    auto t = std::make_shared<Transcript>();
    t->language = std::move(language);
    for (const auto& [text, from, to] : words) {
        TranscriptWord w;
        w.text = text;
        w.t0 = seconds(from);
        w.t1 = seconds(to);
        w.confidence = 0.9f;
        t->words.push_back(std::move(w));
    }
    return t;
}

/// Media of the fixtures, by media id.
class FixtureAccess : public SearchMediaAccess {
public:
    struct Item {
        std::uint64_t fingerprint = 0;
        std::string date;
        analysis::ShotAnalysis shots;
        std::vector<Rgb> shotColours; ///< one per shot
        std::shared_ptr<const Transcript> transcript;
        bool offline = false;
    };

    std::optional<std::uint64_t> fingerprint(const MediaItem& media) override {
        std::lock_guard lock(mutex);
        const auto it = items.find(media.id.value);
        return it == items.end() || it->second.offline ? std::nullopt : std::optional<std::uint64_t>(it->second.fingerprint);
    }
    std::string date(const MediaItem& media) override {
        std::lock_guard lock(mutex);
        const auto it = items.find(media.id.value);
        return it == items.end() ? std::string() : it->second.date;
    }
    std::expected<analysis::ShotAnalysis, Error> shots(const MediaItem& media, const analysis::AnalysisProgress& progress) override {
        if (progress && !progress(1.0)) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        std::lock_guard lock(mutex);
        ++shotCalls;
        return items.at(media.id.value).shots;
    }
    std::expected<std::vector<Image>, Error> frames(const MediaItem& media, std::span<const Ticks> times, int width,
                                                    std::stop_token) override {
        std::lock_guard lock(mutex);
        ++frameCalls;
        const Item& item = items.at(media.id.value);
        std::vector<Image> out;
        for (const Ticks t : times) {
            std::size_t shot = 0;
            for (const auto& cut : item.shots.cuts) {
                shot += cut.time <= t ? 1 : 0;
            }
            const Rgb c = item.shotColours.at(std::min(shot, item.shotColours.size() - 1));
            out.push_back(solid(width, width * 9 / 16, c[0], c[1], c[2]));
        }
        return out;
    }
    std::shared_ptr<const Transcript> transcript(const MediaItem& media) override {
        std::lock_guard lock(mutex);
        const auto it = items.find(media.id.value);
        return it == items.end() ? nullptr : it->second.transcript;
    }

    std::mutex mutex;
    std::map<std::uint64_t, Item> items;
    int shotCalls = 0;
    int frameCalls = 0;
};

inline MediaPtr mediaItem(std::uint64_t id, std::string name, double length, bool video = true, bool audio = true) {
    auto m = std::make_shared<MediaItem>();
    m->id = MediaId{id};
    m->name = std::move(name);
    m->info.duration = seconds(length);
    if (video) {
        VideoStreamInfo v;
        v.index = 0;
        v.duration = seconds(length);
        m->info.video.push_back(v);
    }
    if (audio) {
        AudioStreamInfo a;
        a.index = video ? 1 : 0;
        a.duration = seconds(length);
        m->info.audio.push_back(a);
    }
    return m;
}

inline ProjectState projectWith(std::vector<MediaPtr> media) {
    ProjectState state;
    state.media = std::move(media);
    return state;
}

} // namespace wabcut::ai::search::test
