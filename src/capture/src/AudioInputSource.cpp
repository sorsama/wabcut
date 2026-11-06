// A capture source over audio::AudioInput (miniaudio: WASAPI, Core Audio, PulseAudio / ALSA). The capture callback only fills
// AudioInput's ring; a reader thread moves what arrived to the sink every few milliseconds.
//
// Block times. AudioInput's own clock line counts frames at the nominal rate, so it could not show a device whose clock runs
// slower or faster than the computer's. The reader instead dates each block by when it collects it: the newest frame in the
// ring is taken to have been captured now (less the device's input latency), the block's first frame that many frames
// earlier. The estimate is off by up to a poll interval and a callback period; the recorder averages it (CaptureRecorder.h).

#include "SystemBackends.h"

#include <wabcut/audio/AudioInput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <thread>

namespace wabcut::capture {

namespace {

std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool mentionsDenied(const std::string& message) {
    std::string lower = message;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("denied") != std::string::npos || lower.find("permission") != std::string::npos ||
           lower.find("not authorized") != std::string::npos;
}

class InputSource final : public AudioSource {
public:
    InputSource(std::string deviceId, std::string name, int rate, int channels)
        : m_deviceId(std::move(deviceId)), m_name(std::move(name)), m_rate(rate), m_channels(channels) {}
    ~InputSource() override { shutdown(); } // non-virtual: no virtual call while destroying

    std::string description() const override { return m_name; }
    int sampleRate() const override { return m_rate; }
    int channels() const override { return m_channels; }

    std::expected<void, Error> start(BlockSink sink) override {
        if (m_thread.joinable()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "The sound source is running"});
        }
        audio::AudioInputConfig config;
        config.sampleRate = m_rate;
        config.channels = m_channels;
        config.deviceId = m_deviceId;
        config.ringSeconds = 8.0;
        if (auto started = m_input.start(config); !started) {
            if (started.error().code == ErrorCode::NotFound) {
                return std::unexpected(Error{ErrorCode::NotFound, "That sound device is gone"});
            }
            if (mentionsDenied(started.error().message)) {
                return std::unexpected(accessDeniedError(AccessKind::Microphone));
            }
            return std::unexpected(started.error());
        }
        m_input.setMonitorEnabled(false);
        if (const std::string opened = m_input.deviceName(); !opened.empty()) {
            m_name = opened;
        }
        m_sink = std::move(sink);
        m_stop = false;
        m_thread = std::thread([this] { run(); });
        return {};
    }

    void stop() noexcept override { shutdown(); }

    /// What stop() does; the destructor calls it directly.
    void shutdown() noexcept {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_input.stop();
    }

    std::string failure() const override {
        std::lock_guard lock(m_mutex);
        return m_failure;
    }

private:
    void run() {
        const auto latencyNs = static_cast<std::int64_t>(m_input.latency().total.toSeconds() * 1e9);
        std::vector<float> buffer(static_cast<std::size_t>(m_rate / 10) * static_cast<std::size_t>(m_channels));
        while (!m_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!m_input.isRunning()) {
                std::lock_guard lock(m_mutex);
                m_failure = "The sound device stopped delivering (it may have been unplugged).";
                return;
            }
            while (!m_stop.load()) {
                const std::size_t queued = m_input.readable();
                if (queued == 0) {
                    break;
                }
                const std::int64_t now = steadyNowNs();
                const std::size_t frames = m_input.read(buffer);
                if (frames == 0) {
                    break;
                }
                const std::int64_t end = m_input.readPosition();
                AudioBlock block;
                block.channels = m_channels;
                block.firstFrame = end - static_cast<std::int64_t>(frames);
                block.samples.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frames) * m_channels);
                // The newest frame of the ring was captured about now: this block's first frame lies (queued) frames before it.
                block.hostNs = now - latencyNs - static_cast<std::int64_t>(static_cast<double>(queued) * 1e9 / m_rate);
                m_sink(std::move(block));
            }
        }
    }

    std::string m_deviceId;
    std::string m_name;
    int m_rate;
    int m_channels;
    audio::AudioInput m_input;
    BlockSink m_sink;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mutex;
    std::string m_failure;
};

} // namespace

std::expected<std::vector<AudioDevice>, Error> enumerateSystemAudioDevices() {
    auto devices = audio::AudioInput::enumerateInputDevices();
    if (!devices) {
        return std::unexpected(devices.error());
    }
    std::vector<AudioDevice> out;
    for (const audio::AudioDeviceInfo& d : *devices) {
        out.push_back(AudioDevice{d.id, d.name, d.isDefault});
    }
    std::ranges::stable_sort(out, [](const AudioDevice& a, const AudioDevice& b) { return a.isDefault && !b.isDefault; });
    return out;
}

std::expected<std::unique_ptr<AudioSource>, Error> openSystemAudioSource(const std::string& deviceId, int sampleRate, int channels) {
    std::string name = "the default input";
    if (!deviceId.empty()) {
        if (auto devices = audio::AudioInput::enumerateInputDevices()) {
            const auto it = std::ranges::find(*devices, deviceId, &audio::AudioDeviceInfo::id);
            if (it == devices->end()) {
                return std::unexpected(Error{ErrorCode::NotFound, "That sound device is gone"});
            }
            name = it->name;
        }
    }
    return std::make_unique<InputSource>(deviceId, std::move(name), sampleRate, channels);
}

} // namespace wabcut::capture
