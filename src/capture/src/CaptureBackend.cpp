#include <wabcut/capture/CaptureBackend.h>

#include "SystemBackends.h"

#include <string_view>

namespace wabcut::capture {

namespace {

constexpr std::string_view kCameraDenied = "Windows has blocked the camera for this app. Open Settings > Privacy & security > Camera, turn on "
                                           "\"Let desktop apps access your camera\" (and Camera access), then try again.";
constexpr std::string_view kMicrophoneDenied = "Windows has blocked the microphone for this app. Open Settings > Privacy & security > Microphone, "
                                               "turn on \"Let desktop apps access your microphone\" (and Microphone access), then try again.";

/// The backend of a platform that has no capture source yet.
class UnavailableBackend final : public CaptureBackend {
public:
    std::string name() const override { return "none"; }
    bool supported() const override { return false; }
    std::string unsupportedReason() const override {
        return "Capturing from a camera or capture card works on Windows in this version. Support for Linux (V4L2) and macOS "
               "(AVFoundation) is planned.";
    }
    std::expected<std::vector<VideoDevice>, Error> videoDevices() override { return {}; }
    std::expected<std::vector<VideoFormat>, Error> videoFormats(const std::string&) override { return {}; }
    std::expected<std::vector<AudioDevice>, Error> audioDevices() override { return {}; }
    std::expected<std::unique_ptr<VideoSource>, Error> openVideo(const std::string&, const VideoFormat&) override {
        return std::unexpected(Error{ErrorCode::Unsupported, unsupportedReason()});
    }
    std::expected<std::unique_ptr<AudioSource>, Error> openAudio(const std::string&, int, int) override {
        return std::unexpected(Error{ErrorCode::Unsupported, unsupportedReason()});
    }
};

} // namespace

Error accessDeniedError(AccessKind kind) {
    return Error{ErrorCode::DeviceError, std::string(kind == AccessKind::Camera ? kCameraDenied : kMicrophoneDenied)};
}

std::optional<AccessKind> accessDeniedKind(const Error& error) {
    if (error.code != ErrorCode::DeviceError) {
        return std::nullopt;
    }
    if (error.message == kCameraDenied) {
        return AccessKind::Camera;
    }
    if (error.message == kMicrophoneDenied) {
        return AccessKind::Microphone;
    }
    return std::nullopt;
}

std::shared_ptr<CaptureBackend> makeSystemBackend() {
#ifdef _WIN32
    return makeMediaFoundationBackend();
#else
    return std::make_shared<UnavailableBackend>();
#endif
}

} // namespace wabcut::capture
