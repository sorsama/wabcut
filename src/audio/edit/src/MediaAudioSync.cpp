// The media-backed AudioSyncService (A.5): the project's files decoded with media::AudioDecoder as mono at the sync's
// rates (an angle's files back to back), the results cached in the AnalysisCache by the files' fingerprints.

#include <wabcut/audio/edit/MediaAnalysis.h>

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <tuple>

namespace wabcut::audio::edit {

namespace {

using Json = nlohmann::json;

/// Frames of `duration` at `rate`, rounded up.
std::int64_t framesOf(Ticks duration, int rate) {
    const Rate r{rate, 1};
    const std::int64_t whole = ticksToSamples(duration, r);
    return samplesToTicks(whole, r) < duration ? whole + 1 : whole;
}

/// One angle: its files back to back, mono at any rate.
class MediaSignal final : public SyncSignal {
public:
    struct File {
        std::string path;
        int streamIndex = -1;
        Ticks duration;
    };
    explicit MediaSignal(std::vector<File> files) : m_files(std::move(files)) {}

    std::int64_t frames(int rate) const override {
        std::int64_t n = 0;
        for (const File& f : m_files) {
            n += framesOf(f.duration, rate);
        }
        return n;
    }

    std::expected<void, Error> read(int rate, std::int64_t first, std::span<float> out) override {
        std::fill(out.begin(), out.end(), 0.0f);
        const auto end = first + static_cast<std::int64_t>(out.size());
        std::int64_t fileStart = 0;
        for (std::size_t i = 0; i < m_files.size(); ++i) {
            const std::int64_t length = framesOf(m_files[i].duration, rate);
            const std::int64_t from = std::max(first, fileStart);
            const std::int64_t to = std::min(end, fileStart + length);
            if (from < to) {
                auto decoder = decoderFor(i, rate);
                if (!decoder) {
                    return std::unexpected(decoder.error());
                }
                auto r = (*decoder)->read(samplesToTicks(from - fileStart, Rate{rate, 1}),
                                          out.subspan(static_cast<std::size_t>(from - first), static_cast<std::size_t>(to - from)));
                if (!r) {
                    return std::unexpected(r.error());
                }
            }
            fileStart += length;
        }
        return {};
    }

private:
    std::expected<media::AudioDecoder*, Error> decoderFor(std::size_t file, int rate) {
        auto& slot = m_decoders[{file, rate}];
        if (!slot) {
            auto opened = media::AudioDecoder::open(m_files[file].path, m_files[file].streamIndex, Rate{rate, 1}, 1);
            if (!opened) {
                return std::unexpected(opened.error());
            }
            slot = std::move(*opened);
        }
        return slot.get();
    }

    std::vector<File> m_files;
    std::map<std::pair<std::size_t, int>, std::unique_ptr<media::AudioDecoder>> m_decoders;
};

/// FNV-1a over the inputs of a sync.
struct Hash {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    void bytes(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ p[i]) * 0x100000001b3ULL;
        }
    }
    void add(std::int64_t v) { bytes(&v, sizeof v); }
    void add(std::string_view s) {
        add(static_cast<std::int64_t>(s.size()));
        bytes(s.data(), s.size());
    }
};

Json matchJson(const SyncMatch& m) {
    return Json{{"synced", m.synced},           {"offset", m.offset},
                {"offsetAtStart", m.offsetAtStart}, {"confidence", m.confidence},
                {"psr", m.peakToSidelobe},      {"overlap", m.overlapSeconds},
                {"windows", m.windows},         {"driftPpm", m.driftPpm},
                {"driftResidualMs", m.driftResidualMs}, {"driftSignificant", m.driftSignificant},
                {"speed", m.speedProposal},     {"relativeTo", m.relativeTo},
                {"problem", m.problem}};
}

SyncMatch matchFrom(const Json& j) {
    SyncMatch m;
    m.synced = j.value("synced", false);
    m.offset = j.value("offset", std::int64_t{0});
    m.offsetAtStart = j.value("offsetAtStart", std::int64_t{0});
    m.confidence = j.value("confidence", 0.0);
    m.peakToSidelobe = j.value("psr", 0.0);
    m.overlapSeconds = j.value("overlap", 0.0);
    m.windows = j.value("windows", 0);
    m.driftPpm = j.value("driftPpm", 0.0);
    m.driftResidualMs = j.value("driftResidualMs", 0.0);
    m.driftSignificant = j.value("driftSignificant", false);
    m.speedProposal = j.value("speed", 1.0);
    m.relativeTo = j.value("relativeTo", std::size_t{0});
    m.problem = j.value("problem", std::string());
    return m;
}

class MediaAudioSync final : public AudioSyncService {
public:
    explicit MediaAudioSync(std::shared_ptr<ai::AnalysisCache> cache) : m_cache(std::move(cache)) {}

    std::expected<Result, Error> sync(const ProjectState& state, std::span<const Angle> angles, std::size_t reference,
                                      const SyncOptions& options, std::stop_token stop,
                                      const std::function<void(double)>& progress) override {
        if (angles.size() < 2 || reference >= angles.size()) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "audio sync needs two angles or more and a reference among them"));
        }
        Result result;
        std::vector<std::unique_ptr<MediaSignal>> signals;
        std::vector<std::vector<Ticks>> durations;
        Hash key;
        key.add(static_cast<std::int64_t>(reference));
        for (const Angle& angle : angles) {
            if (angle.media.empty()) {
                return std::unexpected(Error(ErrorCode::InvalidArgument, "an angle without media"));
            }
            std::vector<const MediaItem*> items;
            for (const MediaId id : angle.media) {
                const MediaItem* item = state.findMedia(id);
                if (item == nullptr) {
                    return std::unexpected(Error(ErrorCode::NotFound, "media " + std::to_string(id.value) + " does not exist"));
                }
                if (item->locator.offline || item->locator.path.empty()) {
                    return std::unexpected(Error(ErrorCode::NotFound, "'" + item->name + "' is offline"));
                }
                if (angle.stream < 0 || static_cast<std::size_t>(angle.stream) >= item->info.audio.size()) {
                    return std::unexpected(Error(ErrorCode::NotFound, "'" + item->name + "' has no audio stream " + std::to_string(angle.stream)));
                }
                items.push_back(item);
            }
            // A camera's split recording plays in the order it was recorded (the files' times, then their names).
            if (!angle.keepOrder) {
                std::ranges::stable_sort(items, [](const MediaItem* a, const MediaItem* b) {
                    return std::tie(a->info.modifiedTime, a->name) < std::tie(b->info.modifiedTime, b->name);
                });
            }
            std::vector<MediaSignal::File> files;
            std::vector<MediaId> order;
            std::vector<Ticks> lengths;
            key.add(static_cast<std::int64_t>(items.size()));
            for (const MediaItem* item : items) {
                const AudioStreamInfo& s = item->info.audio[static_cast<std::size_t>(angle.stream)];
                const Ticks duration = s.duration.value > 0 ? s.duration : item->info.duration;
                files.push_back({item->locator.path, s.index, duration});
                order.push_back(item->id);
                lengths.push_back(duration);
                if (item->locator.fingerprint.isKnown()) {
                    key.add(item->locator.fingerprint.size);
                    key.add(item->locator.fingerprint.sampleHash);
                } else {
                    auto fingerprint = media::sourceFingerprint(item->locator.path);
                    if (!fingerprint) {
                        return std::unexpected(fingerprint.error());
                    }
                    key.add(static_cast<std::int64_t>(*fingerprint));
                }
                key.add(static_cast<std::int64_t>(s.index));
                key.add(duration.value);
            }
            signals.push_back(std::make_unique<MediaSignal>(std::move(files)));
            result.order.push_back(std::move(order));
            durations.push_back(std::move(lengths));
        }
        const ai::AnalysisKey cacheKey{key.h == 0 ? 1 : key.h, 0, "audio.sync", "sync1",
                                       ai::hashParams({{"analysisRate", std::to_string(options.analysisRate)},
                                                       {"refineRate", std::to_string(options.refineRate)},
                                                       {"minOverlap", std::to_string(options.minOverlapSeconds)},
                                                       {"minConfidence", std::to_string(options.minConfidence)},
                                                       {"windows", std::to_string(options.refineWindows)},
                                                       {"window", std::to_string(options.windowSeconds)},
                                                       {"search", std::to_string(options.searchSeconds)},
                                                       {"chain", options.chain ? "1" : "0"}})};
        bool cached = false;
        if (m_cache) {
            if (auto hit = m_cache->get(cacheKey); hit && *hit) {
                const Json j = Json::parse(**hit, nullptr, false);
                if (j.is_object() && j.contains("matches") && j["matches"].is_array() && j["matches"].size() == angles.size()) {
                    result.report.reference = reference;
                    for (const Json& m : j["matches"]) {
                        result.report.matches.push_back(matchFrom(m));
                    }
                    cached = true;
                }
            }
        }
        if (!cached) {
            std::vector<SyncSignal*> raw;
            for (auto& s : signals) {
                raw.push_back(s.get());
            }
            auto report = syncSignals(raw, reference, options, stop, progress);
            if (!report) {
                return std::unexpected(report.error());
            }
            result.report = std::move(*report);
            if (m_cache) {
                Json matches = Json::array();
                for (const SyncMatch& m : result.report.matches) {
                    matches.push_back(matchJson(m));
                }
                (void)m_cache->put(cacheKey, Json{{"matches", std::move(matches)}}.dump());
            }
        }
        result.cached = cached;
        // Where each file starts on the reference's clock: the angle's offset plus the files before it.
        const Rate rate{options.refineRate, 1};
        for (std::size_t a = 0; a < angles.size(); ++a) {
            std::vector<Ticks> starts;
            std::int64_t before = 0;
            for (const Ticks d : durations[a]) {
                starts.push_back(samplesToTicks(result.report.matches[a].offset + before, rate));
                before += framesOf(d, options.refineRate);
            }
            result.starts.push_back(std::move(starts));
        }
        return result;
    }

private:
    std::shared_ptr<ai::AnalysisCache> m_cache;
};

} // namespace

std::shared_ptr<AudioSyncService> makeMediaAudioSync(std::shared_ptr<ai::AnalysisCache> cache) {
    return std::make_shared<MediaAudioSync>(std::move(cache));
}

} // namespace wabcut::audio::edit
