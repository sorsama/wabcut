#include <wabcut/ai/AudioEnergy.h>

#include <wabcut/ai/Timestamps.h>

#include "TempWorkspace.h"

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace wabcut::ai {

namespace {

/// Bump when a change would measure the same audio differently (part of the analysis-cache key).
constexpr std::string_view kEnergyVersion = "energy-1";
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
/// Silence between ranges in the VAD's input, so one range's speech does not run into the next.
constexpr std::int64_t kVadSeparator = kTranscriptionSampleRate / 4;

std::unexpected<Error> fail(ErrorCode code, std::string message) { return std::unexpected(Error{code, std::move(message)}); }

double toDb(double amplitudeSquaredOrPeak, bool squared) {
    if (amplitudeSquaredOrPeak <= 0.0) {
        return kNegInf;
    }
    return squared ? 10.0 * std::log10(amplitudeSquaredOrPeak) : 20.0 * std::log10(amplitudeSquaredOrPeak);
}

nlohmann::json dbJson(double db) { return std::isfinite(db) ? nlohmann::json(db) : nlohmann::json(nullptr); }
double dbFrom(const nlohmann::json& j) { return j.is_number() ? j.get<double>() : kNegInf; }

std::string rangesParam(const std::vector<TimeRange>& ranges) {
    std::string out;
    for (const TimeRange& r : ranges) {
        out += std::to_string(r.start.value) + "+" + std::to_string(r.duration.value) + ";";
    }
    return out;
}

using RangeKey = std::tuple<std::uint64_t, int, std::int64_t, std::int64_t>;

/// What a stat says about a file: enough to notice it was replaced.
struct FileStamp {
    bool exists = false;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
    bool operator==(const FileStamp&) const = default;
};

FileStamp stampOf(const std::string& utf8Path) {
    FileStamp s;
    try {
        const std::filesystem::path p = pathFromUtf8(utf8Path);
        std::error_code ec;
        s.size = std::filesystem::file_size(p, ec);
        s.exists = !ec;
        s.modified = std::filesystem::last_write_time(p, ec);
        s.exists = s.exists && !ec;
    } catch (const std::exception&) {
    }
    return s;
}

} // namespace

EnergyMeasure measureSamples(std::span<const float> samples) {
    double sum = 0.0;
    double peak = 0.0;
    for (const float s : samples) {
        sum += static_cast<double>(s) * static_cast<double>(s);
        peak = std::max(peak, std::abs(static_cast<double>(s)));
    }
    EnergyMeasure m;
    m.rmsDb = samples.empty() ? kNegInf : toDb(sum / static_cast<double>(samples.size()), true);
    m.peakDb = toDb(peak, false);
    return m;
}

struct AudioEnergyService::Impl {
    AudioEnergyServiceConfig config;
    std::unique_ptr<TranscriptionService> local;
    mutable std::mutex mutex;
    std::map<RangeKey, EnergyMeasure> memory;
    /// Path and container stream -> fingerprint and resolved stream (cached() needs no file access).
    /// Path and container stream -> what run() last found there, with the file's size and modification time:
    /// cached() trusts it only while the file still looks the same (review P2-17: a replaced file is new audio).
    struct Known {
        std::uint64_t fingerprint = 0;
        int stream = -1;
        FileStamp stamp;
    };
    std::map<std::pair<std::string, int>, Known> identities;
    struct Running {
        std::shared_ptr<media::detail::JobState<EnergyReport>> state;
        std::thread thread;
    };
    std::list<Running> jobs;

    bool vadAvailable() const {
        std::error_code ec;
        return std::filesystem::exists(config.local.inferExecutable, ec);
    }

    AnalysisKey diskKey(const AudioIdentity& id, const EnergyRequest& request) const {
        return AnalysisKey{id.fingerprint, id.streamIndex, "energy", std::string(kEnergyVersion),
                           hashParams({{"ranges", rangesParam(request.ranges)}, {"speech", request.speech ? "1" : "0"}})};
    }

    std::optional<std::vector<EnergyMeasure>> fromDisk(const AudioIdentity& id, const EnergyRequest& request) const {
        if (config.cacheRoot.empty()) {
            return std::nullopt;
        }
        const AnalysisCache cache(config.cacheRoot);
        auto payload = cache.get(diskKey(id, request));
        if (!payload || !*payload) {
            return std::nullopt;
        }
        const auto j = nlohmann::json::parse(**payload, nullptr, false);
        if (!j.is_object() || !j.contains("ranges") || !j["ranges"].is_array() || j["ranges"].size() != request.ranges.size()) {
            return std::nullopt;
        }
        std::vector<EnergyMeasure> out;
        for (const auto& r : j["ranges"]) {
            if (!r.is_object() || !r.contains("rms") || !r.contains("peak") ||
                !(r["rms"].is_number() || r["rms"].is_null()) ||
                !(r["peak"].is_number() || r["peak"].is_null())) {
                return std::nullopt;
            }
            EnergyMeasure m;
            m.rmsDb = dbFrom(r.value("rms", nlohmann::json()));
            m.peakDb = dbFrom(r.value("peak", nlohmann::json()));
            if (r.contains("speech") && r["speech"].is_number()) {
                m.speech = r["speech"].get<float>();
                if (!std::isfinite(*m.speech) || *m.speech < 0.0f || *m.speech > 1.0f) {
                    return std::nullopt;
                }
            }
            out.push_back(m);
        }
        return out;
    }

    void toDisk(const AudioIdentity& id, const EnergyRequest& request, const std::vector<EnergyMeasure>& measures) const {
        if (config.cacheRoot.empty()) {
            return;
        }
        nlohmann::json ranges = nlohmann::json::array();
        for (const EnergyMeasure& m : measures) {
            ranges.push_back({{"rms", dbJson(m.rmsDb)},
                              {"peak", dbJson(m.peakDb)},
                              {"speech", m.speech ? nlohmann::json(*m.speech) : nlohmann::json(nullptr)}});
        }
        AnalysisCache cache(config.cacheRoot);
        [[maybe_unused]] const auto stored = cache.put(diskKey(id, request), nlohmann::json{{"ranges", ranges}}.dump());
    }

    std::expected<EnergyReport, Error> run(const EnergyRequest& request, std::stop_token stop,
                                           const std::function<void(float)>& progress) {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "energy measurement cancelled");
        }
        TranscriptionRequest media;
        media.mediaPath = request.mediaPath;
        media.streamIndex = request.containerStream;
        auto id = identifyMediaAudio(media);
        if (!id) {
            return std::unexpected(id.error());
        }
        EnergyReport report;
        report.fingerprint = id->fingerprint;
        report.streamIndex = id->streamIndex;
        const bool wantSpeech = request.speech && vadAvailable();

        // Memory first: every range already measured (with speech, when that is wanted) needs no decode.
        std::vector<std::optional<EnergyMeasure>> found(request.ranges.size());
        bool all = true;
        {
            std::lock_guard lock(mutex);
            identities[{request.mediaPath, request.containerStream}] = {id->fingerprint, id->streamIndex,
                                                                        stampOf(request.mediaPath)};
            for (std::size_t i = 0; i < request.ranges.size(); ++i) {
                const TimeRange& r = request.ranges[i];
                const auto it = memory.find({id->fingerprint, id->streamIndex, r.start.value, r.duration.value});
                if (it != memory.end() && (!wantSpeech || it->second.speech)) {
                    found[i] = it->second;
                } else {
                    all = false;
                }
            }
        }
        if (!all) {
            if (auto disk = fromDisk(*id, request); disk && (!wantSpeech || std::ranges::all_of(*disk, [](const EnergyMeasure& m) {
                                                                 return m.speech.has_value();
                                                             }))) {
                for (std::size_t i = 0; i < found.size(); ++i) {
                    found[i] = (*disk)[i];
                }
                all = true;
            }
        }
        if (all) {
            report.fromCache = true;
            for (const auto& m : found) {
                report.ranges.push_back(*m);
            }
            remember(*id, request.ranges, report.ranges);
            if (progress) {
                progress(1.0f);
            }
            return report;
        }

        auto decoder = media::AudioDecoder::open(request.mediaPath, id->streamIndex, kTranscriptionRate,
                                                 media::ChannelLayout::mono());
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        // Decode the missing ranges in time order (the decoder seeks forward cheaply).
        std::vector<std::size_t> order;
        std::int64_t total = 0;
        for (std::size_t i = 0; i < request.ranges.size(); ++i) {
            if (!found[i]) {
                order.push_back(i);
                total += std::max<std::int64_t>(ticksToSamples(request.ranges[i].duration, kTranscriptionRate), 0);
            }
        }
        std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return request.ranges[a].start < request.ranges[b].start; });

        std::unique_ptr<detail::TempWorkspace> workspace; // destroyed after the PCM stream closes
        std::filesystem::path pcmPath;
        std::ofstream pcm;
        std::vector<std::pair<std::int64_t, std::int64_t>> vadSpan(request.ranges.size(), {0, 0}); ///< offset, length
        std::int64_t vadSamples = 0;
        if (wantSpeech) {
            auto created = detail::TempWorkspace::create(config.workDir);
            if (!created) {
                return std::unexpected(created.error());
            }
            workspace = std::move(*created);
            pcmPath = workspace->path() / "audio.f32";
            pcm.open(pcmPath, std::ios::binary | std::ios::trunc);
            if (!pcm) {
                return fail(ErrorCode::IoError, "cannot write " + pathToUtf8(pcmPath));
            }
        }
        const std::vector<float> gap(static_cast<std::size_t>(kVadSeparator), 0.0f);

        std::vector<float> block(static_cast<std::size_t>(kTranscriptionSampleRate));
        std::int64_t done = 0;
        for (const std::size_t i : order) {
            const TimeRange& r = request.ranges[i];
            const std::int64_t length = std::max<std::int64_t>(ticksToSamples(r.duration, kTranscriptionRate), 0);
            double sum = 0.0;
            double peak = 0.0;
            if (wantSpeech) {
                vadSpan[i] = {vadSamples, length};
            }
            for (std::int64_t at = 0; at < length;) {
                if (stop.stop_requested()) {
                    return fail(ErrorCode::Cancelled, "energy measurement cancelled");
                }
                const auto n = static_cast<std::size_t>(std::min<std::int64_t>(length - at, static_cast<std::int64_t>(block.size())));
                auto read = (*decoder)->read(r.start + samplesToTicks(at, kTranscriptionRate), std::span<float>(block.data(), n));
                if (!read) {
                    return std::unexpected(read.error());
                }
                for (std::size_t sample = 0; sample < n; ++sample) {
                    const double value = block[sample];
                    sum += value * value;
                    peak = std::max(peak, std::abs(value));
                }
                if (wantSpeech) {
                    pcm.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(n * sizeof(float)));
                }
                at += static_cast<std::int64_t>(n);
                done += static_cast<std::int64_t>(n);
                if (progress && total > 0) {
                    progress((wantSpeech ? 0.7f : 1.0f) * static_cast<float>(done) / static_cast<float>(total));
                }
            }
            found[i] = EnergyMeasure{length > 0 ? toDb(sum / static_cast<double>(length), true) : kNegInf,
                                     toDb(peak, false), std::nullopt};
            if (wantSpeech) {
                pcm.write(reinterpret_cast<const char*>(gap.data()), static_cast<std::streamsize>(gap.size() * sizeof(float)));
                vadSamples += length + kVadSeparator;
            }
        }
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "energy measurement cancelled");
        }
        if (wantSpeech) {
            pcm.close();
            if (!pcm) {
                return fail(ErrorCode::IoError, "cannot write " + pathToUtf8(pcmPath));
            }
            // Speech fraction is optional: a VAD failure (model not downloaded, helper crash) leaves it unmeasured.
            if (auto speech = local->detectSpeech(pcmPath, vadSamples, stop, config.vadModel); speech) {
                for (const std::size_t i : order) {
                    const auto [offset, length] = vadSpan[i];
                    std::int64_t overlap = 0;
                    for (const SampleRange& s : *speech) {
                        overlap += std::max<std::int64_t>(0, std::min(s.end, offset + length) - std::max(s.begin, offset));
                    }
                    found[i]->speech = length > 0 ? static_cast<float>(static_cast<double>(overlap) / static_cast<double>(length)) : 0.0f;
                }
            } else if (speech.error().code == ErrorCode::Cancelled) {
                return std::unexpected(speech.error());
            }
        }
        for (const auto& m : found) {
            report.ranges.push_back(*m);
        }
        remember(*id, request.ranges, report.ranges);
        toDisk(*id, request, report.ranges);
        if (progress) {
            progress(1.0f);
        }
        return report;
    }

    void remember(const AudioIdentity& id, const std::vector<TimeRange>& ranges, const std::vector<EnergyMeasure>& measures) {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            auto& slot = memory[{id.fingerprint, id.streamIndex, ranges[i].start.value, ranges[i].duration.value}];
            const auto speech = measures[i].speech ? measures[i].speech : slot.speech;
            slot = measures[i];
            slot.speech = speech;
        }
    }
};

AudioEnergyService::AudioEnergyService(AudioEnergyServiceConfig config) : m_impl(std::make_unique<Impl>()) {
    if (config.local.inferExecutable.empty()) {
        config.local.inferExecutable = defaultInferExecutable();
    }
    if (config.workDir.empty()) {
        std::error_code ec;
        config.workDir = std::filesystem::temp_directory_path(ec) / "wabcut-energy";
    }
    m_impl->local = std::make_unique<TranscriptionService>(config.local);
    m_impl->config = std::move(config);
}

AudioEnergyService::~AudioEnergyService() {
    std::list<Impl::Running> jobs;
    {
        std::lock_guard lock(m_impl->mutex);
        jobs.swap(m_impl->jobs);
    }
    for (Impl::Running& job : jobs) {
        job.state->stop.request_stop();
    }
    for (Impl::Running& job : jobs) {
        if (job.thread.joinable()) {
            job.thread.join();
        }
    }
}

media::Job<EnergyReport> AudioEnergyService::measure(EnergyRequest request) {
    auto state = std::make_shared<media::detail::JobState<EnergyReport>>();
    std::list<Impl::Running> finished; // joined below, outside the lock
    {
        std::lock_guard lock(m_impl->mutex);
        for (auto it = m_impl->jobs.begin(); it != m_impl->jobs.end();) {
            bool done = false;
            {
                std::lock_guard l(it->state->mutex);
                done = it->state->result.has_value();
            }
            const auto next = std::next(it);
            if (done) {
                finished.splice(finished.end(), m_impl->jobs, it);
            }
            it = next;
        }
        Impl* impl = m_impl.get();
        m_impl->jobs.push_back({state, std::thread([impl, state, request = std::move(request)] {
                                    std::expected<EnergyReport, Error> result = std::unexpected(Error{ErrorCode::Unknown, "energy measurement failed"});
                                    try {
                                        result = impl->run(request, state->stop.get_token(), [&](float p) {
                                            state->progress.store(std::min(p, 0.999f), std::memory_order_relaxed);
                                        });
                                    } catch (const std::exception& e) { // review P2-13: an exception must not terminate the app
                                        result = std::unexpected(Error{ErrorCode::Unknown, std::string("energy measurement failed: ") + e.what()});
                                    } catch (...) {
                                    }
                                    state->complete(std::move(result));
                                })});
    }
    for (Impl::Running& job : finished) {
        job.thread.join();
    }
    return media::Job<EnergyReport>(state);
}

std::expected<EnergyReport, Error> AudioEnergyService::run(const EnergyRequest& request, std::stop_token stop,
                                                           const std::function<void(float)>& progress) {
    return m_impl->run(request, std::move(stop), progress);
}

std::optional<EnergyMeasure> AudioEnergyService::cached(const std::string& mediaPath, int containerStream,
                                                        TimeRange range) const {
    const FileStamp now = stampOf(mediaPath); // a stat, outside the lock
    std::lock_guard lock(m_impl->mutex);
    const auto id = m_impl->identities.find({mediaPath, containerStream});
    if (id == m_impl->identities.end() || !(id->second.stamp == now) || !now.exists) {
        return std::nullopt;
    }
    const auto it = m_impl->memory.find({id->second.fingerprint, id->second.stream, range.start.value, range.duration.value});
    if (it == m_impl->memory.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace wabcut::ai
