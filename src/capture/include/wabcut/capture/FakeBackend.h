#pragma once

// FakeCaptureBackend (0.7, docs/capture.md): capture devices without hardware, for the tests and the panel's screenshots.
// Its video devices deliver a moving test picture in real time at the chosen format's rate - a gradient that drifts, with the
// picture's number as a 16-bit barcode in the top-left corner (cells of 16 x 16 pixels, bit b at x = 16 b, light = 1; NV12,
// YUY2 and RGB32 only) - and its audio devices a steady 440 Hz tone. Pictures carry the time they are due at, so the slot a
// picture lands in does not depend on how late the thread ran. It counts how often a device was opened or queried, so a test
// can show that nothing opens until the user asks.

#include <wabcut/capture/CaptureBackend.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::capture {

struct FakeBackendOptions {
    bool supported = true;
    std::string unsupportedReason;
    std::vector<VideoDevice> video;
    std::vector<AudioDevice> audio;
    /// The system refuses access (the Windows privacy settings): opening the device fails with accessDeniedError().
    bool denyCamera = false;
    bool denyMicrophone = false;
    /// The first picture comes this long after start() (a webcam's start-up), the first sound after `audioDelayMs`.
    int videoStartDelayMs = 0;
    int audioDelayMs = 0;
    /// The video source stops delivering after this many pictures and reports a failure (an unplugged device); -1: never.
    int videoFailAfter = -1;
    double toneHz = 440.0;
    float toneLevel = 0.25f;
};

class FakeCaptureBackend final : public CaptureBackend {
public:
    /// Two video devices - "Fake HDMI Capture" (1080p30 compressed, 720p60, 720p30, 360p30) and "Fake DV Deck" (720 x 480 at
    /// 29.97 with 10:11 pixels) - and two audio devices.
    static FakeBackendOptions defaults();
    explicit FakeCaptureBackend(FakeBackendOptions options = defaults());

    std::string name() const override { return "fake"; }
    bool supported() const override { return m_options.supported; }
    std::string unsupportedReason() const override { return m_options.unsupportedReason; }
    std::expected<std::vector<VideoDevice>, Error> videoDevices() override;
    std::expected<std::vector<VideoFormat>, Error> videoFormats(const std::string& deviceId) override;
    std::expected<std::vector<AudioDevice>, Error> audioDevices() override;
    std::expected<std::unique_ptr<VideoSource>, Error> openVideo(const std::string& deviceId, const VideoFormat& format) override;
    std::expected<std::unique_ptr<AudioSource>, Error> openAudio(const std::string& deviceId, int sampleRate, int channels) override;

    int videoOpens() const { return m_videoOpens.load(); }
    int audioOpens() const { return m_audioOpens.load(); }
    int formatQueries() const { return m_formatQueries.load(); }
    FakeBackendOptions& options() { return m_options; }

private:
    FakeBackendOptions m_options;
    std::atomic<int> m_videoOpens{0};
    std::atomic<int> m_audioOpens{0};
    std::atomic<int> m_formatQueries{0};
};

/// Draws test picture `index` into `bytes` (packed layout of `pixel`, frameBytes long). NV12, YUY2 and RGB32 only; other layouts
/// are left grey.
void drawFakePicture(PixelFormat pixel, int width, int height, std::int64_t index, std::uint8_t* bytes);
/// The picture number encoded in the barcode of a packed luma plane / RGB32 picture, or -1 when it cannot be read.
std::int64_t fakePictureNumber(PixelFormat pixel, int width, int height, const std::uint8_t* bytes);

} // namespace wabcut::capture
