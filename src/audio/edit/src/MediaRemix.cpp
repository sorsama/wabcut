// The media-backed MusicAnalyzer (A.7, Music Remix): a stream decoded with media::AudioDecoder as mono at the analysis
// rate, its beats tracked with ai/analysis's DspBeatTracker and its beat features computed (Remix.h); the result cached
// in the AnalysisCache by the file's fingerprint. Remix takes are rendered from the stream at its own rate and channel
// count, piece by piece in one-second chunks (never the whole file in memory), into a float WAV.

#include <wabcut/audio/edit/MediaAnalysis.h>

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/analysis/BeatDetection.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace wabcut::audio::edit {

namespace {

using Json = nlohmann::json;

constexpr int kChunkSeconds = 1;

Json analysisJson(const MusicAnalysis& a) {
    Json beats = Json::array();
    for (const Ticks t : a.beats) {
        beats.push_back(t.value);
    }
    Json downbeats = Json::array();
    for (const Ticks t : a.downbeats) {
        downbeats.push_back(t.value);
    }
    Json features = Json::array();
    for (const BeatFeatures& f : a.features) {
        Json row = Json::array();
        for (const float v : f.chroma) {
            row.push_back(v);
        }
        for (const float v : f.mfcc) {
            row.push_back(v);
        }
        row.push_back(f.rms);
        features.push_back(std::move(row));
    }
    return Json{{"bpm", a.bpm},     {"confidence", a.beatConfidence}, {"beatsPerBar", a.beatsPerBar}, {"duration", a.duration.value},
                {"beats", beats}, {"downbeats", downbeats},          {"features", features}};
}

std::optional<MusicAnalysis> analysisFrom(const Json& j) {
    if (!j.is_object() || !j.contains("beats") || !j.contains("features")) {
        return std::nullopt;
    }
    MusicAnalysis a;
    a.bpm = j.value("bpm", 0.0);
    a.beatConfidence = j.value("confidence", 0.0f);
    a.beatsPerBar = j.value("beatsPerBar", 4);
    a.duration = Ticks{j.value("duration", std::int64_t{0})};
    for (const Json& t : j["beats"]) {
        a.beats.push_back(Ticks{t.get<std::int64_t>()});
    }
    for (const Json& t : j["downbeats"]) {
        a.downbeats.push_back(Ticks{t.get<std::int64_t>()});
    }
    for (const Json& row : j["features"]) {
        if (!row.is_array() || row.size() != 26) {
            return std::nullopt;
        }
        BeatFeatures f;
        for (std::size_t k = 0; k < 12; ++k) {
            f.chroma[k] = row[k].get<float>();
        }
        for (std::size_t k = 0; k < 13; ++k) {
            f.mfcc[k] = row[12 + k].get<float>();
        }
        f.rms = row[25].get<float>();
        a.features.push_back(f);
    }
    if (a.beats.size() >= 2 && a.features.size() + 1 != a.beats.size()) {
        return std::nullopt;
    }
    return a;
}

/// A float WAV written as it goes (sizes patched when it is closed).
class WavWriter {
public:
    WavWriter(const std::filesystem::path& path, int channels, int rate) : m_file(path, std::ios::binary), m_channels(channels), m_rate(rate) {
        header(0);
    }
    bool ok() const { return m_file.good(); }
    void write(const float* interleaved, std::size_t frames) {
        m_file.write(reinterpret_cast<const char*>(interleaved), static_cast<std::streamsize>(frames * static_cast<std::size_t>(m_channels) * sizeof(float)));
        m_frames += frames;
    }
    bool close() {
        m_file.seekp(0);
        header(m_frames);
        m_file.close();
        return !m_file.fail();
    }

private:
    void u32(std::uint32_t v) { m_file.write(reinterpret_cast<const char*>(&v), 4); }
    void u16(std::uint16_t v) { m_file.write(reinterpret_cast<const char*>(&v), 2); }
    void header(std::uint64_t frames) {
        const auto bytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames * static_cast<std::uint64_t>(m_channels) * 4u, 0xFFFFFF00u));
        m_file.write("RIFF", 4);
        u32(36 + bytes);
        m_file.write("WAVEfmt ", 8);
        u32(16);
        u16(3); // IEEE float
        u16(static_cast<std::uint16_t>(m_channels));
        u32(static_cast<std::uint32_t>(m_rate));
        u32(static_cast<std::uint32_t>(m_rate * m_channels * 4));
        u16(static_cast<std::uint16_t>(m_channels * 4));
        u16(32);
        m_file.write("data", 4);
        u32(bytes);
    }
    std::ofstream m_file;
    int m_channels;
    int m_rate;
    std::uint64_t m_frames = 0;
};

struct StreamSource {
    std::string path;
    int containerIndex = -1;
    int rate = 48000;
    int channels = 2;
    Ticks duration;
    std::uint64_t fingerprint = 0;
};

std::expected<StreamSource, Error> sourceOf(const MediaItem& item, int stream) {
    if (item.locator.offline || item.locator.path.empty()) {
        return std::unexpected(Error(ErrorCode::NotFound, "'" + item.name + "' is offline"));
    }
    if (stream < 0 || static_cast<std::size_t>(stream) >= item.info.audio.size()) {
        return std::unexpected(Error(ErrorCode::NotFound, "'" + item.name + "' has no audio stream " + std::to_string(stream)));
    }
    const AudioStreamInfo& s = item.info.audio[static_cast<std::size_t>(stream)];
    StreamSource out;
    out.path = item.locator.path;
    out.containerIndex = s.index;
    out.rate = s.sampleRate.den == 1 && s.sampleRate.num >= 8000 ? static_cast<int>(s.sampleRate.num) : 48000;
    out.channels = std::clamp(s.channels, 1, 2);
    out.duration = s.duration.value > 0 ? s.duration : item.info.duration;
    auto fingerprint = media::sourceFingerprint(out.path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    out.fingerprint = *fingerprint;
    return out;
}

class MediaMusicAnalyzer final : public MusicAnalyzer {
public:
    MediaMusicAnalyzer(std::shared_ptr<ai::AnalysisCache> cache, std::filesystem::path takes)
        : m_cache(std::move(cache)), m_takes(takes.empty() ? ai::userCacheDir() / "remix" : std::move(takes)) {}

    std::expected<MusicAnalysis, Error> analyze(const MediaItem& item, int streamOrdinal, std::stop_token stop,
                                                const std::function<void(double)>& progress) override {
        auto source = sourceOf(item, streamOrdinal);
        if (!source) {
            return std::unexpected(source.error());
        }
        const ai::AnalysisKey key{source->fingerprint, source->containerIndex, "audio.remix", std::string(MusicAnalysis::kAnalyzer),
                                  ai::hashParams({{"rate", std::to_string(kRemixAnalysisRate)}, {"tracker", "dsp-1"}})};
        if (m_cache) {
            if (auto hit = m_cache->get(key); hit && *hit) {
                if (auto cached = analysisFrom(Json::parse(**hit, nullptr, false))) {
                    return *cached;
                }
            }
        }
        // Decode the whole stream as mono at the analysis rate.
        auto decoder = media::AudioDecoder::open(source->path, source->containerIndex, Rate{kRemixAnalysisRate, 1}, 1);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        std::vector<float> mono;
        const auto total = static_cast<std::size_t>(std::max<std::int64_t>(ticksToSamples(source->duration, Rate{kRemixAnalysisRate, 1}), 0));
        mono.reserve(total);
        std::vector<float> chunk(static_cast<std::size_t>(kRemixAnalysisRate * kChunkSeconds));
        for (std::size_t at = 0; at < total; at += chunk.size()) {
            if (stop.stop_requested()) {
                return std::unexpected(Error(ErrorCode::Cancelled, "the remix analysis was stopped"));
            }
            const std::size_t want = std::min(chunk.size(), total - at);
            auto read = (*decoder)->read(samplesToTicks(static_cast<std::int64_t>(at), Rate{kRemixAnalysisRate, 1}),
                                         std::span<float>(chunk.data(), want));
            if (!read) {
                return std::unexpected(read.error());
            }
            mono.insert(mono.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(want));
            if (progress) {
                progress(0.4 * static_cast<double>(at + want) / static_cast<double>(std::max<std::size_t>(total, 1)));
            }
            if (read->endOfStream) {
                break;
            }
        }
        ai::analysis::MonoAudio audio{mono, kRemixAnalysisRate, Ticks{}};
        auto beats = ai::analysis::detectBeats(audio, ai::analysis::BeatOptions{}, [&](double f) {
            if (progress) {
                progress(0.4 + 0.4 * f);
            }
            return !stop.stop_requested();
        });
        if (!beats) {
            return std::unexpected(beats.error());
        }
        MusicAnalysis out;
        out.bpm = beats->bpm;
        out.beatConfidence = beats->confidence;
        out.beatsPerBar = beats->beatsPerBar;
        out.beats = beats->beats;
        out.downbeats = beats->downbeats;
        out.duration = samplesToTicks(static_cast<std::int64_t>(mono.size()), Rate{kRemixAnalysisRate, 1});
        if (out.beats.size() < 2) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "no beats found in '" + item.name + "'"));
        }
        out.features = beatFeatures(mono, kRemixAnalysisRate, out.beats);
        if (progress) {
            progress(1.0);
        }
        if (m_cache) {
            (void)m_cache->put(key, analysisJson(out).dump());
        }
        return out;
    }

    std::expected<Take, Error> render(const MediaItem& item, int streamOrdinal, const RemixPlan& plan, std::stop_token stop,
                                      const std::function<void(double)>& progress) override {
        const auto started = std::chrono::steady_clock::now();
        auto source = sourceOf(item, streamOrdinal);
        if (!source) {
            return std::unexpected(source.error());
        }
        const Rate rate{source->rate, 1};
        const int ch = source->channels;
        auto decoder = media::AudioDecoder::open(source->path, source->containerIndex, rate, ch);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        // The take's name: the file, the stream and the plan (its pieces and crossfade).
        std::uint64_t h = 0xcbf29ce484222325ULL ^ source->fingerprint;
        const auto mix = [&h](std::int64_t v) {
            for (int i = 0; i < 8; ++i) {
                h = (h ^ static_cast<std::uint64_t>((v >> (8 * i)) & 0xff)) * 0x100000001b3ULL;
            }
        };
        for (const RemixSegment& s : plan.segments) {
            mix(s.sourceStart.value);
            mix(s.sourceEnd.value);
        }
        mix(plan.crossfade.value);
        mix(source->containerIndex);
        std::error_code ec;
        std::filesystem::create_directories(m_takes, ec);
        char name[64];
        std::snprintf(name, sizeof name, "remix-%016llx.wav", static_cast<unsigned long long>(h));
        const std::filesystem::path file = m_takes / name;
        if (!std::filesystem::is_regular_file(file, ec)) {
            const std::filesystem::path part = std::filesystem::path(file).concat(".part");
            {
                WavWriter wav(part, ch, source->rate);
                if (!wav.ok()) {
                    return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(part)));
                }
                const std::int64_t half = ticksToSamples(Ticks{plan.crossfade.value / 2}, rate);
                const std::int64_t totalFrames = ticksToSamples(plan.duration, rate);
                std::vector<float> carry; // the previous piece's fade-out over the next cut, already weighted
                std::vector<float> buffer;
                std::int64_t written = 0;
                for (std::size_t k = 0; k < plan.segments.size(); ++k) {
                    const RemixSegment& s = plan.segments[k];
                    const std::int64_t first = ticksToSamples(s.sourceStart, rate);
                    const std::int64_t length = ticksToSamples(s.sourceEnd, rate) - first;
                    const bool joinBefore = k > 0;
                    const bool joinAfter = k + 1 < plan.segments.size();
                    const std::int64_t from = joinBefore ? -half : 0;
                    const std::int64_t to = joinAfter ? length + half : length;
                    const std::int64_t holdFrom = joinAfter ? length - half : to; // kept back for the next piece
                    for (std::int64_t i = from; i < to;) {
                        if (stop.stop_requested()) {
                            std::filesystem::remove(part, ec);
                            return std::unexpected(Error(ErrorCode::Cancelled, "the remix render was stopped"));
                        }
                        const std::int64_t n = std::min<std::int64_t>(to - i, source->rate * kChunkSeconds);
                        buffer.assign(static_cast<std::size_t>(n * ch), 0.0f);
                        auto read = (*decoder)->read(samplesToTicks(first + i, rate), buffer);
                        if (!read) {
                            std::filesystem::remove(part, ec);
                            return std::unexpected(read.error());
                        }
                        for (std::int64_t f = 0; f < n; ++f) {
                            const std::int64_t at = i + f;
                            double g = 1.0;
                            if (joinBefore && at < half) {
                                g = (static_cast<double>(at + half) + 0.5) / static_cast<double>(2 * half);
                            }
                            if (joinAfter && at >= length - half) {
                                g *= 1.0 - (static_cast<double>(at - (length - half)) + 0.5) / static_cast<double>(2 * half);
                            }
                            for (int c = 0; c < ch; ++c) {
                                float& v = buffer[static_cast<std::size_t>(f * ch + c)];
                                v = static_cast<float>(v * g);
                                if (joinBefore && at < half) {
                                    const auto ci = static_cast<std::size_t>((at + half) * ch + c);
                                    v += ci < carry.size() ? carry[ci] : 0.0f;
                                }
                            }
                        }
                        // Frames before holdFrom go out now; the rest is the next cut's fade-out.
                        const std::int64_t outCount = std::clamp<std::int64_t>(holdFrom - i, 0, n);
                        const std::int64_t room = std::max<std::int64_t>(totalFrames - written, 0);
                        const std::int64_t emit = std::min(outCount, room);
                        if (emit > 0) {
                            wav.write(buffer.data(), static_cast<std::size_t>(emit));
                            written += emit;
                        }
                        if (outCount < n) {
                            if (i + outCount == holdFrom) {
                                carry.clear();
                            }
                            carry.insert(carry.end(), buffer.begin() + static_cast<std::ptrdiff_t>(outCount * ch), buffer.end());
                        }
                        i += n;
                    }
                    if (progress) {
                        progress(static_cast<double>(k + 1) / static_cast<double>(plan.segments.size()));
                    }
                }
                if (!wav.close()) {
                    std::filesystem::remove(part, ec);
                    return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(part)));
                }
            }
            std::filesystem::rename(part, file, ec);
            if (ec) {
                return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(file) + ": " + ec.message()));
            }
        }
        Take take;
        take.path = pathToUtf8(std::filesystem::absolute(file, ec));
        auto info = media::probe(take.path);
        if (!info) {
            return std::unexpected(info.error());
        }
        take.info = *info;
        if (auto fp = fingerprintFile(file)) {
            take.fingerprint = *fp;
        }
        take.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return take;
    }

private:
    std::shared_ptr<ai::AnalysisCache> m_cache;
    std::filesystem::path m_takes;
};

} // namespace

std::shared_ptr<MusicAnalyzer> makeMediaMusicAnalyzer(std::shared_ptr<ai::AnalysisCache> cache, std::filesystem::path takesDirectory) {
    return std::make_shared<MediaMusicAnalyzer>(std::move(cache), std::move(takesDirectory));
}

} // namespace wabcut::audio::edit
