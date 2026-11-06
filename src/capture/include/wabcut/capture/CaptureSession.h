#pragma once

// CaptureSession (0.7, docs/capture.md): what the Capture panel drives. It opens a video device (and a sound device) on the
// user's action, keeps the newest picture for the live view, and records between Record and Stop:
//
//   startPreview()      opens the devices and starts them; the pictures only feed the live view
//   startRecording()    from then on every picture and block of sound also goes to a CaptureRecorder (a file)
//   stopRecording()     finishes the file; the live view carries on
//   stopPreview()       closes the devices (finishing a recording first)
//
// The sources deliver on their own threads; the session only stores the newest picture (a shared pointer under a mutex),
// measures the rate and the sound level, and hands everything to the recorder. The control calls belong to one thread; the
// readers (status(), latestFrame()) may be used from any thread.

#include <wabcut/capture/CaptureBackend.h>
#include <wabcut/capture/CaptureRecorder.h>

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace wabcut::capture {

struct SessionConfig {
    std::string videoDeviceId;
    VideoFormat format;           ///< one of videoFormats(videoDeviceId)
    bool audio = true;
    std::string audioDeviceId;    ///< empty: the system default input
    int audioRate = 48000;
    int audioChannels = 2;
};

struct RecordingRequest {
    std::filesystem::path file;
    std::string presetId = "high";
    std::string encoder; ///< see RecorderConfig::encoder
    std::string title;
    std::string provenance;
};

struct SessionStatus {
    bool previewing = false;
    bool recording = false;
    std::string videoDescription; ///< the device's name
    std::string audioDescription;
    VideoFormat format;
    std::uint64_t pictures = 0;    ///< pictures received since the preview started
    double measuredFps = 0.0;      ///< over the last second
    std::uint64_t deviceDrops = 0; ///< what the driver reported lost
    float audioPeak = 0.0f;        ///< 0..1, the largest sample since the previous status() (the meter)
    std::uint64_t previewSerial = 0; ///< changes with every new picture
    RecorderStatus recorder;
    /// A source stopped on its own (unplugged, driver failed): the text; empty while all runs.
    std::string failure;
};

class CaptureSession {
public:
    explicit CaptureSession(std::shared_ptr<CaptureBackend> backend);
    /// A recording in progress is finished like stopRecording() does (the file is complete).
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    CaptureBackend& backend() { return *m_backend; }

    /// Opens and starts the devices. Errors are the backend's (accessDeniedError for a refused camera or microphone, NotFound,
    /// DeviceError); nothing stays open when it fails.
    std::expected<void, Error> startPreview(const SessionConfig& config);
    void stopPreview();
    bool previewing() const;

    /// Requires a running preview. Errors: InvalidArgument (no preview, recording already), the recorder's.
    std::expected<void, Error> startRecording(const RecordingRequest& request);
    /// Finishes the file; see CaptureRecorder::stop(). Errors: InvalidArgument (not recording) and the recorder's.
    std::expected<RecorderResult, Error> stopRecording();
    /// Ends the recording as a crash would (the program is quitting and cannot finish the file): file and journal stay.
    void abandonRecording() noexcept;
    /// Ends the recording and deletes the file.
    void cancelRecording() noexcept;
    bool recording() const;
    /// The file being recorded, empty when not recording.
    std::filesystem::path recordingFile() const;

    /// The result of a recording stopPreview() had to finish itself (the devices were closed while it ran), once.
    std::optional<RecorderResult> takeOrphanResult();

    SessionStatus status();
    /// The newest picture, or an empty frame (valid() false) before the first.
    CaptureFrame latestFrame() const;
    /// The format the open source delivers (the preview's), for the pixel aspect of the live view.
    VideoFormat format() const;

private:
    void onFrame(CaptureFrame&& frame);
    void onAudio(AudioBlock&& block);
    std::shared_ptr<CaptureRecorder> recorder() const;

    std::shared_ptr<CaptureBackend> m_backend;
    mutable std::mutex m_control; ///< the lifetime of the sources against status()
    std::unique_ptr<VideoSource> m_video;
    std::unique_ptr<AudioSource> m_audio;
    SessionConfig m_config;
    VideoFormat m_format;
    bool m_previewing = false;
    std::string m_videoName;
    std::string m_audioName;

    mutable std::mutex m_frameMutex;
    CaptureFrame m_latest;
    std::uint64_t m_pictures = 0;
    std::uint64_t m_serial = 0;
    std::int64_t m_windowStartNs = 0;
    std::uint64_t m_windowPictures = 0;
    double m_fps = 0.0;
    float m_peak = 0.0f;

    mutable std::mutex m_recorderMutex;
    std::shared_ptr<CaptureRecorder> m_recorder;
    std::optional<RecorderResult> m_orphan; ///< a recording stopPreview() had to finish itself
};

} // namespace wabcut::capture
