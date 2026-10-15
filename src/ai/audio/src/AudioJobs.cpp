#include <wabcut/ai/audio/AudioJobs.h>

#include <wabcut/ai/audio/DeepFilter.h>
#include <wabcut/ai/audio/WavFile.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>

namespace wabcut::ai::audio {

namespace {

using wabcut::audio::edit::SpeechEnhancer;
using wabcut::audio::edit::StemSeparator;

/// Frames of `duration` at `rate`, rounded up (a partial last sample is kept).
std::int64_t framesOf(Ticks duration, int rate) {
    const Rate r{rate, 1};
    const std::int64_t whole = ticksToSamples(duration, r);
    return samplesToTicks(whole, r) < duration ? whole + 1 : whole;
}

std::string sanitized(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        out += plain ? c : '_';
    }
    return out;
}

/// A stream decoded forward, kept from a sliding start: what the requests read.
class Decoded {
public:
    Decoded(std::unique_ptr<media::AudioDecoder> decoder, int channels, int rate)
        : m_decoder(std::move(decoder)), m_channels(channels), m_rate(rate) {}

    /// Frames [begin, end) (decoding up to `end`, dropping what is before `begin`), interleaved.
    std::expected<std::span<const float>, Error> range(std::int64_t begin, std::int64_t end) {
        const auto ch = static_cast<std::size_t>(m_channels);
        if (begin > m_begin) {
            const auto drop = static_cast<std::size_t>(std::min(begin, m_end) - m_begin);
            m_samples.erase(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(drop * ch));
            m_begin += static_cast<std::int64_t>(drop);
        }
        while (m_end < end) {
            const std::int64_t n = std::min<std::int64_t>(m_rate, end - m_end + m_rate / 2); // about a second at a time
            const std::size_t at = m_samples.size();
            m_samples.resize(at + static_cast<std::size_t>(n) * ch);
            auto read = m_decoder->read(samplesToTicks(m_end, Rate{m_rate, 1}), std::span<float>(m_samples.data() + at, static_cast<std::size_t>(n) * ch));
            if (!read) {
                return std::unexpected(read.error());
            }
            m_end += n;
        }
        return std::span<const float>(m_samples.data() + static_cast<std::size_t>(begin - m_begin) * ch,
                                      static_cast<std::size_t>(end - begin) * ch);
    }

private:
    std::unique_ptr<media::AudioDecoder> m_decoder;
    int m_channels;
    int m_rate;
    std::int64_t m_begin = 0;
    std::int64_t m_end = 0;
    std::vector<float> m_samples;
};

std::expected<void, Error> finishFile(WavWriter& writer, const std::filesystem::path& part, const std::filesystem::path& final) {
    if (auto closed = writer.close(); !closed) {
        return closed;
    }
    std::error_code ec;
    std::filesystem::rename(part, final, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        return std::unexpected(Error(ErrorCode::IoError, "cannot move the render to " + pathToUtf8(final)));
    }
    return {};
}

/// Deletes unfinished render files however a job ends (finishFile renames the finished ones away first).
struct PartFiles {
    std::vector<std::filesystem::path> files;
    ~PartFiles() {
        for (const auto& f : files) {
            std::error_code ec;
            std::filesystem::remove(f, ec);
        }
    }
};

bool held(const std::function<bool(std::stop_token)>& waitIdle, const std::stop_token& stop) {
    return waitIdle && !waitIdle(stop);
}

/// A finished render already there: a float WAV of the expected shape.
bool complete(const std::filesystem::path& file, int channels, int rate, std::int64_t frames) {
    const auto info = readWavInfo(file);
    return info && info->channels == channels && info->rate == rate && info->frames == frames;
}

std::filesystem::path orDefault(const std::filesystem::path& p, const char* name) {
    return p.empty() ? userCacheDir() / name : p;
}

/// The helper's options for one model; its stderr goes next to the model store unless the caller chose.
AudioHelperOptions helperFor(AudioHelperOptions base) {
    if (base.log.empty()) {
        std::error_code ec;
        base.log = std::filesystem::temp_directory_path(ec) / "wabcut-audio" / "wabcut-embed-audio.log";
        std::filesystem::create_directories(base.log.parent_path(), ec);
    }
    return base;
}

/// Maps a stop requested through a progress callback back: progress(x) may throw (a command's checkpoint).
struct Progress {
    const std::function<void(double)>& report;
    double from = 0.0;
    double to = 1.0;
    void operator()(double x) const {
        if (report) {
            report(from + (to - from) * std::clamp(x, 0.0, 1.0));
        }
    }
};

class LocalSpeechEnhancer final : public SpeechEnhancer {
public:
    explicit LocalSpeechEnhancer(LocalAudioOptions options) : m_options(std::move(options)) {}

    std::string modelVersion() const override { return speechModelVersion(); }

    std::expected<Take, Error> render(const MediaItem& item, int streamOrdinal, std::stop_token stop,
                                      const std::function<void(double)>& progress) override {
        std::lock_guard lock(m_mutex); // one job at a time: one helper, the GPU once
        if (streamOrdinal < 0 || static_cast<std::size_t>(streamOrdinal) >= item.info.audio.size()) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "'" + item.name + "' has no audio stream " + std::to_string(streamOrdinal)));
        }
        if (item.locator.offline || item.locator.path.empty()) {
            return std::unexpected(Error(ErrorCode::NotFound, "'" + item.name + "' is offline"));
        }
        const AudioStreamInfo& stream = item.info.audio[static_cast<std::size_t>(streamOrdinal)];
        SourceFingerprint fingerprint = item.locator.fingerprint;
        if (!fingerprint.isKnown()) {
            auto f = fingerprintFile(pathFromUtf8(item.locator.path));
            if (!f) {
                return std::unexpected(f.error());
            }
            fingerprint = *f;
        }
        const int channels = std::clamp(stream.channels, 1, search::embed::kMaxAudioChannels);
        const Ticks duration = stream.duration.value > 0 ? stream.duration : item.info.duration;
        const std::int64_t frames = framesOf(duration, dfn::kSampleRate);
        const std::filesystem::path dir = orDefault(m_options.takesDirectory, "enhanced");
        const std::filesystem::path take = takeFileFor(dir, fingerprint, streamOrdinal, modelVersion());
        Take out;
        out.path = pathToUtf8(take);
        out.model = modelVersion();
        out.source = fingerprint;
        out.seconds = static_cast<double>(frames) / dfn::kSampleRate;
        if (complete(take, channels, dfn::kSampleRate, frames)) {
            out.reused = true;
            return out;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const Progress download{progress, 0.0, 0.05};
        const Progress work{progress, 0.05, 1.0};
        ModelStore store(m_options.modelRoot.empty() ? ModelStore::defaultRoot() : m_options.modelRoot);
        auto model = installSpeechModel(store, m_options.transport.get(),
                                        m_options.extractor.empty() ? defaultExtractor() : m_options.extractor,
                                        [&](const DownloadProgress& p) {
                                            if (p.total > 0) {
                                                download(static_cast<double>(p.received) / static_cast<double>(p.total));
                                            }
                                        },
                                        stop);
        if (!model) {
            return std::unexpected(model.error());
        }
        if (!m_helper || m_helperModel != *model) {
            AudioHelperOptions o = helperFor(m_options.helper);
            o.speechModel = *model;
            m_helper = std::make_unique<AudioHelper>(std::move(o));
            m_helperModel = *model;
        }
        const auto started = std::chrono::steady_clock::now();
        auto rendered = renderEnhancedTake(item.locator.path, stream.index, channels, duration, take, *m_helper, m_options.chunks, stop,
                                           work, m_options.waitIdle);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        out.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        out.device = m_helper->device();
        return out;
    }

private:
    LocalAudioOptions m_options;
    std::mutex m_mutex;
    std::unique_ptr<AudioHelper> m_helper;
    std::filesystem::path m_helperModel;
};

class LocalStemSeparator final : public StemSeparator {
public:
    explicit LocalStemSeparator(LocalAudioOptions options) : m_options(std::move(options)) {}

    std::string modelVersion() const override { return stemModelVersion(); }

    std::expected<Result, Error> separate(const MediaItem& item, int streamOrdinal, std::stop_token stop,
                                          const std::function<void(double)>& progress) override {
        std::lock_guard lock(m_mutex);
        if (streamOrdinal < 0 || static_cast<std::size_t>(streamOrdinal) >= item.info.audio.size()) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "'" + item.name + "' has no audio stream " + std::to_string(streamOrdinal)));
        }
        if (item.locator.offline || item.locator.path.empty()) {
            return std::unexpected(Error(ErrorCode::NotFound, "'" + item.name + "' is offline"));
        }
        const StemModelSpec& spec = stemModelSpec();
        const AudioStreamInfo& stream = item.info.audio[static_cast<std::size_t>(streamOrdinal)];
        SourceFingerprint fingerprint = item.locator.fingerprint;
        if (!fingerprint.isKnown()) {
            auto f = fingerprintFile(pathFromUtf8(item.locator.path));
            if (!f) {
                return std::unexpected(f.error());
            }
            fingerprint = *f;
        }
        const Ticks duration = stream.duration.value > 0 ? stream.duration : item.info.duration;
        const std::int64_t frames = framesOf(duration, spec.sampleRate);
        const std::filesystem::path dir = orDefault(m_options.stemsDirectory, "stems") /
                                          (std::to_string(fingerprint.size) + "-" + fingerprint.sampleHash + "-a" +
                                           std::to_string(streamOrdinal) + "-" + sanitized(modelVersion()));
        Result result;
        result.model = modelVersion();
        result.seconds = static_cast<double>(frames) / spec.sampleRate;
        const bool done = std::ranges::all_of(spec.stems, [&](const std::string& s) {
            return complete(dir / (s + ".wav"), 2, spec.sampleRate, frames);
        });
        if (!done) {
            // The memory budget, before anything is downloaded or started (refuse gracefully on small machines).
            const llm::SystemProbe probe = m_options.probe ? m_options.probe() : llm::probeSystem();
            auto device = stemDevice(probe, m_options.helper.device);
            if (!device) {
                return std::unexpected(device.error());
            }
            const Progress download{progress, 0.0, 0.1};
            const Progress work{progress, 0.1, 1.0};
            ModelStore store(m_options.modelRoot.empty() ? ModelStore::defaultRoot() : m_options.modelRoot);
            auto model = store.ensure(stemModel(), m_options.transport.get(),
                                      [&](const DownloadProgress& p) {
                                          if (p.total > 0) {
                                              download(static_cast<double>(p.received) / static_cast<double>(p.total));
                                          }
                                      },
                                      stop);
            if (!model) {
                return std::unexpected(model.error());
            }
            if (!m_helper || m_helperModel != *model || m_device != *device) {
                AudioHelperOptions o = helperFor(m_options.helper);
                o.stemModel = *model;
                o.device = *device;
                m_helper = std::make_unique<AudioHelper>(std::move(o));
                m_helperModel = *model;
                m_device = *device;
            }
            // A separation model holds a lot of memory (about 2.5 GiB): give it back after every job, however it ends
            // (a failure or a cancel between requests leaves the helper running otherwise).
            struct ShutDownOnExit {
                AudioHelper& helper;
                ~ShutDownOnExit() { helper.shutdown(); }
            } shutDown{*m_helper};
            const auto started = std::chrono::steady_clock::now();
            auto written = separateStemsOf(item.locator.path, stream.index, duration, dir, *m_helper, spec, stop, work, m_options.waitIdle);
            if (!written) {
                return std::unexpected(written.error());
            }
            result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            result.device = m_helper->device();
        }
        for (const std::string& s : spec.stems) {
            const std::filesystem::path file = dir / (s + ".wav");
            auto info = media::probe(pathToUtf8(file));
            if (!info) {
                return std::unexpected(info.error());
            }
            auto f = fingerprintFile(file);
            if (!f) {
                return std::unexpected(f.error());
            }
            result.stems.push_back(Stem{s, pathToUtf8(file), std::move(*info), std::move(*f)});
        }
        return result;
    }

private:
    LocalAudioOptions m_options;
    std::mutex m_mutex;
    std::unique_ptr<AudioHelper> m_helper;
    std::filesystem::path m_helperModel;
    std::string m_device;
};

} // namespace

std::filesystem::path takeFileFor(const std::filesystem::path& directory, const SourceFingerprint& source, int streamOrdinal,
                                  std::string_view model) {
    return directory / (std::to_string(source.size) + "-" + sanitized(source.sampleHash) + "-a" + std::to_string(streamOrdinal) +
                        "-" + sanitized(model) + ".wav");
}

std::expected<std::int64_t, Error> renderEnhancedTake(const std::string& sourcePath, int streamIndex, int channels, Ticks duration,
                                                      const std::filesystem::path& take, AudioHelper& helper,
                                                      const ChunkOptions& chunks, std::stop_token stop,
                                                      const std::function<void(double)>& progress,
                                                      const std::function<bool(std::stop_token)>& waitIdle) {
    constexpr int rate = dfn::kSampleRate;
    auto decoder = media::AudioDecoder::open(sourcePath, streamIndex, Rate{rate, 1}, channels);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    Decoded source(std::move(*decoder), channels, rate);
    const std::int64_t total = framesOf(duration, rate);
    const std::vector<Segment> plan = planChunks(total, chunks);
    // Every request the same length (DirectML compiles a graph per shape): the longest, the others padded with silence.
    std::int64_t fixed = 0;
    for (const Segment& s : plan) {
        fixed = std::max(fixed, s.readEnd - s.readBegin);
    }
    std::filesystem::path part = take;
    part += ".part";
    PartFiles cleanup{{part}};
    auto writer = WavWriter::create(part, channels, rate);
    if (!writer) {
        return std::unexpected(writer.error());
    }
    OverlapAssembler assembler(channels);
    const auto ch = static_cast<std::size_t>(channels);
    std::vector<float> block;
    for (const Segment& s : plan) {
        if (stop.stop_requested() || held(waitIdle, stop)) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        auto input = source.range(s.readBegin, s.readEnd);
        if (!input) {
            return std::unexpected(input.error());
        }
        block.assign(static_cast<std::size_t>(fixed) * ch, 0.0f);
        std::ranges::copy(*input, block.begin());
        auto enhanced = helper.enhance(block, channels, stop);
        if (!enhanced) {
            return std::unexpected(enhanced.error());
        }
        enhanced->resize(input->size()); // the padding's output is dropped
        if (auto w = writer->write(assembler.push(s, *enhanced)); !w) {
            return std::unexpected(w.error());
        }
        if (progress) {
            progress(total > 0 ? static_cast<double>(assembler.finishedFrames()) / static_cast<double>(total) : 1.0);
        }
    }
    if (auto done = finishFile(*writer, part, take); !done) {
        return std::unexpected(done.error());
    }
    return total;
}

std::expected<std::vector<std::filesystem::path>, Error> separateStemsOf(const std::string& sourcePath, int streamIndex, Ticks duration,
                                                                         const std::filesystem::path& directory, AudioHelper& helper,
                                                                         const StemModelSpec& spec, std::stop_token stop,
                                                                         const std::function<void(double)>& progress,
                                                                         const std::function<bool(std::stop_token)>& waitIdle) {
    auto decoder = media::AudioDecoder::open(sourcePath, streamIndex, Rate{spec.sampleRate, 1}, 2);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    Decoded source(std::move(*decoder), 2, spec.sampleRate);
    const std::int64_t total = framesOf(duration, spec.sampleRate);
    const std::vector<Segment> plan = planSegments(total, spec.segment, spec.overlap);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const auto stems = spec.stems.size();
    std::vector<WavWriter> writers;
    std::vector<std::filesystem::path> parts;
    PartFiles cleanup;
    std::vector<OverlapAssembler> assemblers(stems, OverlapAssembler(2));
    for (const std::string& s : spec.stems) {
        std::filesystem::path part = directory / (s + ".wav.part");
        auto writer = WavWriter::create(part, 2, spec.sampleRate);
        if (!writer) {
            return std::unexpected(writer.error());
        }
        writers.push_back(std::move(*writer));
        cleanup.files.push_back(part);
        parts.push_back(std::move(part));
    }
    for (const Segment& s : plan) {
        if (stop.stop_requested() || held(waitIdle, stop)) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        auto input = source.range(s.readBegin, s.readEnd);
        if (!input) {
            return std::unexpected(input.error());
        }
        auto separated = helper.separate(*input, spec.sampleRate, static_cast<int>(stems), stop);
        if (!separated) {
            return std::unexpected(separated.error());
        }
        const std::size_t perStem = input->size();
        for (std::size_t k = 0; k < stems; ++k) {
            const std::span<const float> stem(separated->data() + k * perStem, perStem);
            if (auto w = writers[k].write(assemblers[k].push(s, stem)); !w) {
                return std::unexpected(w.error());
            }
        }
        if (progress) {
            progress(total > 0 ? static_cast<double>(assemblers.front().finishedFrames()) / static_cast<double>(total) : 1.0);
        }
    }
    std::vector<std::filesystem::path> files;
    for (std::size_t k = 0; k < stems; ++k) {
        const std::filesystem::path final = directory / (spec.stems[k] + ".wav");
        if (auto done = finishFile(writers[k], parts[k], final); !done) {
            return std::unexpected(done.error());
        }
        files.push_back(final);
    }
    return files;
}

std::shared_ptr<wabcut::audio::edit::SpeechEnhancer> makeSpeechEnhancer(LocalAudioOptions options) {
    return std::make_shared<LocalSpeechEnhancer>(std::move(options));
}

std::shared_ptr<wabcut::audio::edit::StemSeparator> makeStemSeparator(LocalAudioOptions options) {
    return std::make_shared<LocalStemSeparator>(std::move(options));
}

} // namespace wabcut::ai::audio
