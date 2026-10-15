#include <wabcut/playback/audio/VoiceOverRecorder.h>

#include <wabcut/audio/RealTime.h>
#include <wabcut/audio/WavWriter.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

namespace wabcut::playback {

std::string_view toString(VoiceOverState state) {
    switch (state) {
    case VoiceOverState::CountIn:
        return "countIn";
    case VoiceOverState::Preroll:
        return "preroll";
    case VoiceOverState::Recording:
        return "recording";
    case VoiceOverState::Postroll:
        return "postroll";
    case VoiceOverState::Finished:
        return "finished";
    case VoiceOverState::Failed:
        return "failed";
    case VoiceOverState::Idle:
        break;
    }
    return "idle";
}

namespace {

std::int64_t toNs(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

/// `t` on the sample grid of `ticksPerFrame` (nearest; exact for negative times too).
Ticks snapToFrame(Ticks t, std::int64_t ticksPerFrame) {
    const std::int64_t half = ticksPerFrame / 2;
    const std::int64_t shifted = t.value + half;
    const std::int64_t q = shifted >= 0 ? shifted / ticksPerFrame : -((-shifted + ticksPerFrame - 1) / ticksPerFrame);
    return Ticks{q * ticksPerFrame};
}

std::int64_t ticksToNs(Ticks t) {
    return static_cast<std::int64_t>(static_cast<double>(t.value) * 1e9 / static_cast<double>(kTicksPerSecond));
}

constexpr std::int64_t kSecond = kTicksPerSecond;
constexpr std::int64_t kBeepTicks = kTicksPerSecond / 10;    // 100 ms
constexpr std::int64_t kBeepFadeTicks = kTicksPerSecond / 200; // 5 ms
constexpr double kBeepHz = 880.0;
constexpr float kBeepLevel = 0.25f;  // -12 dBFS
constexpr float kClickLevel = 0.9f;
constexpr int kClickFrames = 2;

/// The output during a pass: the player's source, muted before the pre-roll (and for a loopback pass), with the
/// count-in beeps, the loopback clicks and the input monitor mixed in. RT-safe; its parameters are set before it is
/// installed and only read here.
class PassSource final : public audio::AudioSource {
public:
    PassSource(std::shared_ptr<audio::AudioSource> inner, audio::AudioInput& input, int inputChannels, int maxFrames)
        : m_inner(std::move(inner)), m_input(input), m_inputChannels(std::max(inputChannels, 1)),
          m_scratch(static_cast<std::size_t>(std::max(maxFrames, 1)) * static_cast<std::size_t>(std::max(inputChannels, 1))) {}

    Ticks countStart{};
    Ticks programStart{};
    Ticks punchIn{};
    int countIn = 0;
    bool beep = true;
    bool loopback = false;
    std::atomic<bool> monitor{false};
    float monitorGain = 1.0f;
    std::size_t keepFrames = 256;

    void render(std::span<float> out, int channels, Rate rate, Ticks start) noexcept override {
        if (m_inner) {
            m_inner->render(out, channels, rate, start);
        } else {
            std::fill(out.begin(), out.end(), 0.0f);
        }
        const auto ch = static_cast<std::size_t>(std::max(channels, 1));
        const std::size_t frames = out.size() / ch;
        const std::int64_t tpf = rate.ticksPerUnit();
        for (std::size_t i = 0; i < frames; ++i) {
            const std::int64_t t = start.value + static_cast<std::int64_t>(i) * tpf;
            float add = 0.0f;
            if (loopback || t < programStart.value) {
                for (std::size_t c = 0; c < ch; ++c) {
                    out[i * ch + c] = 0.0f; // the program is muted
                }
            }
            if (loopback && t >= punchIn.value) {
                const std::int64_t since = (t - punchIn.value) % kLoopbackClickPeriod.value;
                if (since < kClickFrames * tpf && t - punchIn.value >= kLoopbackClickPeriod.value) {
                    add += kClickLevel;
                }
            } else if (beep && countIn > 0 && t >= countStart.value && t < programStart.value) {
                const std::int64_t phase = (t - countStart.value) % kSecond;
                if (phase < kBeepTicks) {
                    const double seconds = static_cast<double>(phase) / static_cast<double>(kTicksPerSecond);
                    const double env = std::min({1.0, static_cast<double>(phase) / kBeepFadeTicks,
                                                 static_cast<double>(kBeepTicks - phase) / kBeepFadeTicks});
                    add += static_cast<float>(kBeepLevel * env * std::sin(2.0 * std::numbers::pi * kBeepHz * seconds));
                }
            }
            if (add != 0.0f) {
                for (std::size_t c = 0; c < ch; ++c) {
                    out[i * ch + c] += add;
                }
            }
        }
        if (monitor.load(std::memory_order_relaxed)) {
            const auto inCh = static_cast<std::size_t>(m_inputChannels);
            const std::size_t room = std::min(frames, m_scratch.size() / inCh);
            const std::size_t n = m_input.readMonitor(std::span<float>(m_scratch.data(), room * inCh), keepFrames);
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t c = 0; c < ch; ++c) {
                    out[i * ch + c] += monitorGain * m_scratch[i * inCh + std::min(c, inCh - 1)];
                }
            }
        }
    }

    bool renderAtSpeed(std::span<float> out, int channels, Rate rate, Ticks start, audio::TransportSpeed speed) noexcept override {
        return m_inner && m_inner->renderAtSpeed(out, channels, rate, start, speed); // a shuttle during a pass: as before
    }

    const std::shared_ptr<audio::AudioSource>& inner() const { return m_inner; }

private:
    std::shared_ptr<audio::AudioSource> m_inner;
    audio::AudioInput& m_input;
    int m_inputChannels;
    std::vector<float> m_scratch;
};

} // namespace

struct VoiceOverRecorder::Impl {
    Impl(audio::AudioEngine& e, TimelineAudioPlayer& p, audio::AudioInput& i, Options o) : engine(e), player(p), input(i), options(o) {}

    audio::AudioEngine& engine;
    TimelineAudioPlayer& player;
    audio::AudioInput& input;
    Options options;

    // The pass (control thread).
    bool running = false;
    VoiceOverOptions pass;
    std::shared_ptr<audio::AudioSource> inner;
    std::shared_ptr<PassSource> source;
    Ticks countStart{};
    Ticks programStart{};
    Ticks latency{};
    std::int64_t latencyNs = 0;
    std::string device;

    // The writer (writer thread, or writeOnce(); serialised by writeMutex).
    audio::rt::CheckedMutex writeMutex;
    std::unique_ptr<audio::WavWriter> wav;
    std::vector<float> buffer;
    std::vector<float> run;
    std::int64_t ticksPerFrame = 1;
    bool transportReady = false; ///< the transport map plays this pass (latched)
    bool started = false;
    Ticks takeStart{};
    std::vector<float> loopbackSamples; ///< channel 0 of what was written, for a loopback pass
    std::string error;

    std::atomic<std::int64_t> stopNs{std::numeric_limits<std::int64_t>::max()};
    std::atomic<std::int64_t> written{0};
    std::atomic<bool> punchedOut{false};
    std::atomic<bool> recording{false};
    std::atomic<int> state{static_cast<int>(VoiceOverState::Idle)};
    std::atomic<std::uint64_t> dropped0{0};
    std::thread writer;
    std::atomic<bool> quit{false};

    void setState(VoiceOverState s) { state.store(static_cast<int>(s), std::memory_order_release); }
    VoiceOverState currentState() const { return static_cast<VoiceOverState>(state.load(std::memory_order_acquire)); }
    std::chrono::steady_clock::time_point now() const { return options.clock ? options.clock() : std::chrono::steady_clock::now(); }
    Ticks position() const { return engine.clockAt(now()); }
    void seek(Ticks t) { options.seek ? options.seek(t) : player.seek(t); }
    void play() { options.play ? options.play() : player.play(); }
    void pause() { options.pause ? options.pause() : player.pause(); }

    /// Whether the transport map plays this pass: a clock, and a segment that runs forward from the count-in start
    /// (the seek dropped the history before it).
    bool playsThisPass(const audio::TransportMap& map) const {
        if (!map.hasClock()) {
            return false;
        }
        for (int i = 0; i < map.segmentCount(); ++i) {
            if (map.segment(i).timeline == countStart.value && map.segment(i).speedNum > 0) {
                return true;
            }
        }
        return false;
    }

    /// Moves captured frames to the file. A frame's place is the timeline position heard when its sound reached the
    /// microphone (on the sample grid); frames placed in [punchIn, punchOut) are the take. `all`: everything
    /// readable (the end of a manual pass); otherwise frames whose microphone time is before stopNs.
    void writePass(bool all) {
        std::lock_guard lock(writeMutex);
        if (!wav) {
            return;
        }
        const audio::CaptureLine line = input.captureLine();
        const audio::TransportMap map = engine.transportMap();
        transportReady = transportReady || playsThisPass(map);
        const auto ch = static_cast<std::size_t>(wav->format().channels);
        const std::int64_t stopAt = stopNs.load(std::memory_order_acquire);
        while (line.valid) {
            const std::size_t n = input.read(std::span<float>(buffer.data(), buffer.size()));
            if (n == 0) {
                break;
            }
            if (!transportReady) {
                continue; // captured before the pass's transport started: not the take
            }
            const std::int64_t first = input.readPosition() - static_cast<std::int64_t>(n);
            run.clear();
            for (std::size_t i = 0; i < n; ++i) {
                const std::int64_t mic = line.hostTimeNs(first + static_cast<std::int64_t>(i)) - latencyNs;
                if ((!all && mic >= stopAt) || punchedOut.load(std::memory_order_relaxed)) {
                    continue;
                }
                const Ticks at = snapToFrame(map.transportTime(mic), ticksPerFrame);
                if (at < pass.punchIn) {
                    continue;
                }
                if (pass.punchOut && at >= *pass.punchOut) {
                    punchedOut.store(true, std::memory_order_release);
                    continue;
                }
                if (!started) {
                    started = true;
                    takeStart = at;
                    recording.store(true, std::memory_order_release);
                }
                run.insert(run.end(), buffer.begin() + static_cast<std::ptrdiff_t>(i * ch), buffer.begin() + static_cast<std::ptrdiff_t>((i + 1) * ch));
                if (pass.loopback) {
                    loopbackSamples.push_back(buffer[i * ch]);
                }
            }
            if (!run.empty()) {
                if (auto w = wav->write(run); !w) {
                    error = w.error().message;
                    setState(VoiceOverState::Failed);
                    return;
                }
                written.store(wav->frames(), std::memory_order_release);
            }
        }
    }

    void startWriter() {
        quit.store(false);
        writer = std::thread([this] {
            while (!quit.load(std::memory_order_acquire)) {
                writePass(false);
                std::this_thread::sleep_for(options.writerInterval);
            }
        });
    }

    void stopWriter() {
        quit.store(true, std::memory_order_release);
        if (writer.joinable()) {
            writer.join();
        }
    }

    /// Removes the pass's source from the engine (when it is still ours) and pauses the transport.
    void restoreOutput() {
        if (!source) {
            return;
        }
        pause();
        player.unwrapEngineSource(source, inner); // the player's current source again (when ours is still installed)
        input.setMonitorEnabled(false);
        source.reset();
        inner.reset();
    }
};

VoiceOverRecorder::VoiceOverRecorder(audio::AudioEngine& engine, TimelineAudioPlayer& player, audio::AudioInput& input, Options options)
    : m_impl(std::make_unique<Impl>(engine, player, input, options)) {}

VoiceOverRecorder::VoiceOverRecorder(audio::AudioEngine& engine, TimelineAudioPlayer& player, audio::AudioInput& input)
    : VoiceOverRecorder(engine, player, input, Options{}) {}

VoiceOverRecorder::~VoiceOverRecorder() {
    // A pass still running (the app quits, the project closes) keeps its take; only cancel() deletes it (A5-P1-7).
    (void)stop();
}

std::expected<void, Error> VoiceOverRecorder::start(const VoiceOverOptions& options) {
    Impl& d = *m_impl;
    if (d.running) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "a recording pass is already running"));
    }
    if (!d.input.isRunning()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the audio input is not running"));
    }
    if (options.punchOut && *options.punchOut <= options.punchIn) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the punch-out must come after the punch-in"));
    }
    if (options.preroll.value < 0 || options.postroll.value < 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the pre-roll and post-roll cannot be negative"));
    }
    const audio::AudioInputConfig inputConfig = d.input.config();
    audio::WavFormat format{inputConfig.sampleRate, inputConfig.channels, options.bitsPerSample};
    auto wav = audio::WavWriter::create(options.file, format, inputConfig.sampleRate / 2, options.provenance);
    if (!wav) {
        return std::unexpected(wav.error());
    }
    d.pass = options;
    d.pass.countIn = std::clamp(options.countIn, 0, 10);
    d.wav = std::move(*wav);
    d.buffer.assign(static_cast<std::size_t>(4096 * inputConfig.channels), 0.0f);
    d.run.clear();
    d.run.reserve(d.buffer.size());
    d.ticksPerFrame = samplesToTicks(1, Rate{inputConfig.sampleRate, 1}).value;
    d.transportReady = false;
    d.started = false;
    d.takeStart = Ticks{};
    d.loopbackSamples.clear();
    d.error.clear();
    d.stopNs.store(std::numeric_limits<std::int64_t>::max());
    d.written.store(0);
    d.punchedOut.store(false);
    d.recording.store(false);
    d.dropped0.store(d.input.stats().framesDropped);
    d.latency = d.input.latency().total + options.calibration;
    d.latencyNs = ticksToNs(d.latency);
    d.device = d.input.deviceName();
    d.programStart = options.punchIn - options.preroll;
    d.countStart = d.programStart - Ticks{static_cast<std::int64_t>(d.pass.countIn) * kSecond};
    // Frames captured before this pass are not its take.
    std::vector<float> discard(static_cast<std::size_t>(4096 * inputConfig.channels));
    while (d.input.read(discard) > 0) {
    }

    const audio::AudioDeviceConfig engineConfig = d.engine.config();
    d.input.setMonitorEnabled(options.monitor && !options.loopback);
    // Review A5-P2-3: installed through the player, so a change of the master's format (a new program source) waits
    // for the end of the pass instead of leaving this one playing a source nobody feeds.
    d.inner = d.player.wrapEngineSource([&](std::shared_ptr<audio::AudioSource> program) {
        auto wrapper = std::make_shared<PassSource>(std::move(program), d.input, inputConfig.channels,
                                                    engineConfig.maxBlockFrames);
        wrapper->countStart = d.countStart;
        wrapper->programStart = d.programStart;
        wrapper->punchIn = options.punchIn;
        wrapper->countIn = d.pass.countIn;
        wrapper->beep = options.countInBeep;
        wrapper->loopback = options.loopback;
        wrapper->monitorGain = static_cast<float>(std::pow(10.0, options.monitorGainDb / 20.0));
        wrapper->keepFrames = static_cast<std::size_t>(std::max(d.input.latency().periodFrames, std::int64_t{64}));
        wrapper->monitor.store(options.monitor && !options.loopback);
        d.source = wrapper;
        return std::shared_ptr<audio::AudioSource>(std::move(wrapper));
    });
    d.pause(); // a known transport: held at the count-in start, then playing from it
    d.seek(d.countStart);
    d.play();
    d.running = true;
    d.setState(d.pass.countIn > 0 ? VoiceOverState::CountIn : VoiceOverState::Preroll);
    if (d.options.writerThread) {
        d.startWriter();
    }
    return {};
}

void VoiceOverRecorder::tick() {
    Impl& d = *m_impl;
    if (!d.running) {
        return;
    }
    const VoiceOverState s = d.currentState();
    if (s == VoiceOverState::Failed || s == VoiceOverState::Finished) {
        return;
    }
    const Ticks now = d.position();
    if (d.pass.punchOut && now >= *d.pass.punchOut) {
        d.setState(VoiceOverState::Postroll);
        // The end of the post-roll, once the input past the punch-out has come in (the take is complete), or a
        // second later at most (an input that stalled).
        const Ticks end = *d.pass.punchOut + d.pass.postroll;
        if ((now >= end && d.punchedOut.load(std::memory_order_acquire)) || now >= end + Ticks{kSecond}) {
            d.pause();
            d.setState(VoiceOverState::Finished);
        }
        return;
    }
    if (d.recording.load(std::memory_order_acquire) || now >= d.pass.punchIn) {
        d.setState(VoiceOverState::Recording);
    } else if (now >= d.programStart) {
        d.setState(VoiceOverState::Preroll);
    }
}

std::expected<VoiceOverResult, Error> VoiceOverRecorder::stop() {
    Impl& d = *m_impl;
    if (!d.running) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no recording pass is running"));
    }
    // Everything the microphone heard until now; with a manual writer, everything delivered.
    d.stopNs.store(toNs(d.now()), std::memory_order_release);
    d.stopWriter();
    d.writePass(!d.options.writerThread);
    d.restoreOutput();
    d.running = false;
    VoiceOverResult result;
    result.file = d.pass.file;
    result.frames = d.wav ? d.wav->frames() : 0;
    const audio::WavFormat format = d.wav ? d.wav->format() : audio::WavFormat{};
    auto finished = d.wav ? d.wav->finish() : std::expected<void, Error>{};
    d.wav.reset();
    d.setState(VoiceOverState::Idle);
    if (!d.error.empty()) {
        return std::unexpected(Error(ErrorCode::IoError, "the take could not be written: " + d.error));
    }
    if (!finished) {
        return std::unexpected(finished.error());
    }
    if (result.frames == 0) {
        std::error_code ec;
        std::filesystem::remove(d.pass.file, ec);
        audio::removeWavJournal(d.pass.file);
        return std::unexpected(Error(ErrorCode::NotFound, "nothing was recorded (stopped before the punch-in)"));
    }
    result.sampleRate = format.sampleRate;
    result.channels = format.channels;
    result.start = d.takeStart;
    result.duration = samplesToTicks(result.frames, Rate{format.sampleRate, 1});
    result.latencyCompensation = d.latency;
    result.droppedFrames = d.input.stats().framesDropped - d.dropped0.load();
    result.punchedOut = d.punchedOut.load();
    result.device = d.device;
    if (d.pass.loopback) {
        // Where the clicks came back on the timeline, against where they were played (every period from the punch-in).
        LoopbackResult lb;
        lb.reportedOutput = d.engine.latency().total;
        lb.reportedInput = d.input.latency().total;
        std::vector<std::int64_t> residuals;
        const Rate rate{format.sampleRate, 1};
        for (const std::int64_t at : findClicks(d.loopbackSamples, 1, format.sampleRate)) {
            const Ticks placed = result.start + samplesToTicks(at, rate);
            const std::int64_t k = std::llround(static_cast<double>((placed - d.pass.punchIn).value) / static_cast<double>(kLoopbackClickPeriod.value));
            if (k < 1) {
                continue;
            }
            residuals.push_back((placed - (d.pass.punchIn + Ticks{k * kLoopbackClickPeriod.value})).value);
        }
        if (!residuals.empty()) {
            std::nth_element(residuals.begin(), residuals.begin() + static_cast<std::ptrdiff_t>(residuals.size() / 2), residuals.end());
            lb.residual = Ticks{residuals[residuals.size() / 2]};
        }
        lb.clicksFound = static_cast<int>(residuals.size());
        lb.roundTrip = lb.reportedOutput + lb.reportedInput + d.pass.calibration + lb.residual;
        result.loopback = lb;
    }
    return result;
}

void VoiceOverRecorder::cancel() {
    Impl& d = *m_impl;
    if (!d.running) {
        return;
    }
    d.stopWriter();
    d.restoreOutput();
    d.running = false;
    if (d.wav) {
        (void)d.wav->finish();
        d.wav.reset();
    }
    std::error_code ec;
    std::filesystem::remove(d.pass.file, ec);
    audio::removeWavJournal(d.pass.file);
    d.setState(VoiceOverState::Idle);
}

void VoiceOverRecorder::abandon() {
    Impl& d = *m_impl;
    if (!d.running) {
        return;
    }
    d.stopWriter();
    d.running = false;
    const std::int64_t frames = d.wav ? d.wav->frames() : 0;
    if (d.wav) {
        (void)d.wav->finish();
        d.wav.reset();
    }
    if (frames == 0) { // nothing to recover
        std::error_code ec;
        std::filesystem::remove(d.pass.file, ec);
        audio::removeWavJournal(d.pass.file);
    }
    d.input.setMonitorEnabled(false);
    d.setState(VoiceOverState::Idle);
}

VoiceOverStatus VoiceOverRecorder::status() const {
    const Impl& d = *m_impl;
    VoiceOverStatus s;
    s.state = d.currentState();
    const audio::AudioInputConfig config = d.input.config();
    s.recorded = samplesToTicks(d.written.load(std::memory_order_acquire), Rate{config.sampleRate, 1});
    s.droppedFrames = d.input.stats().framesDropped - d.dropped0.load();
    if (s.state == VoiceOverState::CountIn) {
        const Ticks now = d.position();
        const std::int64_t left = d.programStart.value - now.value;
        s.countInRemaining = static_cast<int>(std::clamp<std::int64_t>((left + kSecond - 1) / kSecond, 1, d.pass.countIn));
    }
    const audio::InputLatency in = d.input.latency();
    s.monitorLatency = in.total + samplesToTicks(std::max(in.periodFrames, std::int64_t{64}), Rate{config.sampleRate, 1}) +
                       d.engine.latency().total;
    return s;
}

void VoiceOverRecorder::writeOnce() { m_impl->writePass(false); }

std::vector<std::int64_t> findClicks(std::span<const float> samples, int channels, int sampleRate, float threshold) {
    std::vector<std::int64_t> out;
    const auto ch = static_cast<std::size_t>(std::max(channels, 1));
    const std::size_t frames = samples.size() / ch;
    const auto refractory = static_cast<std::int64_t>(sampleRate / 20); // 50 ms between clicks at least
    std::int64_t last = -refractory;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto at = static_cast<std::int64_t>(i);
        if (std::fabs(samples[i * ch]) >= threshold && at - last >= refractory) {
            out.push_back(at);
            last = at;
        }
    }
    return out;
}

} // namespace wabcut::playback
