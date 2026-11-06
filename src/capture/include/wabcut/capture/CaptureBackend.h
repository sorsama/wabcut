#pragma once

// The device layer behind an interface (0.7, docs/capture.md): a CaptureBackend lists the video and audio capture devices
// and opens them as sources that push pictures and sound to a sink on a thread of their own. The system backend is Media
// Foundation on Windows (capture cards, UVC webcams and whatever else the system exposes as a video capture device - a DV
// camera or deck over FireWire shows up only if its driver does) and the "not available here" backend elsewhere (V4L2 and
// AVFoundation are a later step); FakeCaptureBackend (FakeBackend.h) feeds tests and screenshots without hardware.
//
// Privacy: listing devices opens none. videoFormats() activates the device for the moment it takes to read its formats,
// so the caller asks for it when the user picks the device; openVideo() / openAudio() start the device and are called only
// on the user's action (Start preview, Record).

#include <wabcut/capture/CaptureTypes.h>

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::capture {

/// A running video source. start() begins delivering on the source's own thread; stop() ends it and returns once no sink call
/// is running or can start again. The sink must not block.
class VideoSource {
public:
    using FrameSink = std::function<void(CaptureFrame&&)>;
    virtual ~VideoSource() = default;

    /// The device's name and the format being delivered, for the panel and the log.
    virtual std::string description() const = 0;
    virtual VideoFormat format() const = 0;
    virtual std::expected<void, Error> start(FrameSink sink) = 0;
    virtual void stop() noexcept = 0;
    /// Pictures the driver or the system reported lost since start() (0 when it does not say).
    virtual std::uint64_t reportedDrops() const { return 0; }
    /// Why the source stopped delivering on its own (the device was unplugged, the driver failed); empty while it runs.
    virtual std::string failure() const { return {}; }
};

class AudioSource {
public:
    using BlockSink = std::function<void(AudioBlock&&)>;
    virtual ~AudioSource() = default;

    virtual std::string description() const = 0;
    virtual int sampleRate() const = 0;
    virtual int channels() const = 0;
    virtual std::expected<void, Error> start(BlockSink sink) = 0;
    virtual void stop() noexcept = 0;
    /// Why the source stopped delivering on its own; empty while it runs.
    virtual std::string failure() const { return {}; }
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;

    /// "Media Foundation", "none", "fake".
    virtual std::string name() const = 0;
    /// False on a platform without a capture source: the panel then says why (unsupportedReason()) instead of listing.
    virtual bool supported() const = 0;
    virtual std::string unsupportedReason() const { return {}; }

    /// The video capture devices, without formats and without opening any. Errors: DeviceError.
    virtual std::expected<std::vector<VideoDevice>, Error> videoDevices() = 0;
    /// The formats of one device, best first (largest picture, then the highest rate, then the least compressed). Opens the
    /// device for a moment. Errors: NotFound (the device is gone), DeviceError; the message says how to allow access when the
    /// system denied it.
    virtual std::expected<std::vector<VideoFormat>, Error> videoFormats(const std::string& deviceId) = 0;
    /// The sound capture devices (microphones, line inputs, capture cards' audio), the default first.
    virtual std::expected<std::vector<AudioDevice>, Error> audioDevices() = 0;

    virtual std::expected<std::unique_ptr<VideoSource>, Error> openVideo(const std::string& deviceId, const VideoFormat& format) = 0;
    /// `deviceId` empty: the system default input.
    virtual std::expected<std::unique_ptr<AudioSource>, Error> openAudio(const std::string& deviceId, int sampleRate, int channels) = 0;
};

/// The backend of this platform.
std::shared_ptr<CaptureBackend> makeSystemBackend();

enum class AccessKind : std::uint8_t { Camera, Microphone };

/// The error a backend returns when the system refuses the program access to the camera or the microphone: DeviceError with
/// words that say where to allow it (Windows: Settings > Privacy & security > Camera / Microphone, "Let desktop apps access
/// your camera"; the panel offers a button that opens that page).
Error accessDeniedError(AccessKind kind);
/// Which access `error` is the refusal of (accessDeniedError's), or nullopt.
std::optional<AccessKind> accessDeniedKind(const Error& error);

} // namespace wabcut::capture
