#pragma once

// Private: the pieces the platform backends are made of.

#include <wabcut/capture/CaptureBackend.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::capture {

/// The sound input devices of the audio layer (WASAPI, Core Audio, PulseAudio / ALSA), the default first.
std::expected<std::vector<AudioDevice>, Error> enumerateSystemAudioDevices();
/// A sound source over audio::AudioInput. Fails with the privacy error (accessDeniedError) when the system refuses the
/// microphone, NotFound for a device that is gone.
std::expected<std::unique_ptr<AudioSource>, Error> openSystemAudioSource(const std::string& deviceId, int sampleRate, int channels);

#ifdef _WIN32
/// Media Foundation: video capture devices (capture cards, UVC cameras), their formats, and sources over IMFSourceReader.
std::shared_ptr<CaptureBackend> makeMediaFoundationBackend();
/// A source that reads a video file through the same source reader, output type, buffer copy and clock code a camera goes
/// through (the reader decodes the file's pictures to NV12 as it does a camera's MJPEG): what the tests use to check that code
/// against pictures with known content, without opening a camera. Pictures are delivered as fast as they decode.
std::expected<std::unique_ptr<VideoSource>, Error> openMediaFoundationFile(const std::filesystem::path& file);
#endif

} // namespace wabcut::capture
