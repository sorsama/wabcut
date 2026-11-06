#pragma once

// CaptureRecorder (0.7, docs/capture.md): one recording. Pictures and sound arrive from the sources' threads
// (pushVideo / pushAudio, which only queue), a worker thread turns them into a file with the export's encoders
// (exporter::CaptureWriter):
//
//   * Constant frame rate. The first picture is time 0. A picture taken at host time t belongs to slot round((t - t0) x rate);
//     a slot the device did not fill (it skipped a picture, or the computer was too slow to deliver it) is filled by
//     repeating the last picture and counted as a dropped frame; a picture that arrives for a slot already filled is not
//     written. A device that stops delivering for more than 30 seconds does not make the file longer by more than that.
//   * Sound lines up with the picture. Each block says when its first frame was heard; the sound of frame f of the file is
//     what was heard at t0 + f / sampleRate. The first block is padded with silence or trimmed so that holds; afterwards the
//     file's sound follows the sound device's own clock, and a drift between the two clocks beyond 50 ms is corrected
//     in one step (silence inserted, or the excess dropped). A jump in the device's frame count (sound it lost) is filled
//     with silence.
//   * Bounded memory. The queue holds about 1.5 seconds of pictures (at most 128 and 512 MB); when the encoder cannot keep up,
//     pictures that do not fit are dropped and counted, never waited for.
//   * Survives the program stopping. The file is written as a Matroska file with a cluster each second and a journal next to
//     it (CaptureFiles.h); abandon() and a crash leave both, stop() finishes the file and removes the journal.
//
// Threading: start(), stop(), abandon(), cancel() belong to one control thread; pushVideo / pushAudio may be called from any
// number of source threads; status() from any thread.

#include <wabcut/capture/CaptureFiles.h>
#include <wabcut/capture/CapturePresets.h>
#include <wabcut/capture/CaptureTypes.h>

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace wabcut::capture {

struct RecorderConfig {
    std::filesystem::path file;  ///< created in its folder (which must exist); an existing file is replaced
    VideoFormat video;           ///< the pictures the sources deliver (size, layout, rate, pixel aspect)
    std::string presetId = "high";
    /// "": the preset's encoder (hardware first, then software). Tests name "libx264" to stay off the graphics card.
    std::string encoder;
    bool audio = false;
    int audioRate = 48000;
    int audioChannels = 2;
    std::string title;           ///< the file's title tag
    std::string provenance;      ///< kept in the journal verbatim: what the capture is for (project, bin)
    /// Pictures the queue may hold; 0: about 1.5 seconds of them within 128 pictures and 512 MB.
    int maxQueuedFrames = 0;
    /// Tests: called on the worker before each picture is encoded (to hold it back and fill the queue).
    std::function<void()> encodeHook;
};

enum class RecorderState : std::uint8_t {
    Idle,      ///< not started, or ended
    Waiting,   ///< started, no picture yet
    Recording, ///< pictures are being written
    Failed,    ///< the file could not be written (status().error says why); stop() reports it
};

struct RecorderStatus {
    RecorderState state = RecorderState::Idle;
    std::int64_t framesWritten = 0;     ///< slots written, repeated ones included
    std::uint64_t framesDropped = 0;    ///< slots written without a picture of their own: the last picture repeated (the device's
                                        ///< skips and the pictures the encoder could not take, together)
    std::uint64_t framesRepeated = 0;   ///< the same count, as the recorder counts it
    std::uint64_t framesLost = 0;       ///< of those, pictures the full queue refused because the encoder fell behind
    std::uint64_t framesRejected = 0;   ///< pictures of another size or layout than the capture's
    std::int64_t audioFrames = 0;       ///< samples per channel written (silence included)
    std::int64_t audioPadded = 0;       ///< samples of silence the alignment and the gaps added
    std::int64_t audioTrimmed = 0;      ///< samples it dropped
    double seconds = 0.0;               ///< framesWritten / rate
    std::int64_t bytes = 0;             ///< the file's size so far (refreshed about twice a second)
    float audioPeak = 0.0f;             ///< largest sample written since the previous status()
    std::string encoder;                ///< e.g. "h264_nvenc"
    std::string error;
};

struct RecorderResult {
    std::filesystem::path file;
    double seconds = 0.0;
    std::int64_t videoFrames = 0;
    std::int64_t audioFrames = 0;
    std::uint64_t framesDropped = 0;
    std::int64_t bytes = 0;
    std::string encoder;
    VideoFormat video;
    bool audio = false;
};

class CaptureRecorder {
public:
    CaptureRecorder();
    /// A recording that was not stopped is abandoned: the file and its journal stay for recoverCapture().
    ~CaptureRecorder();

    CaptureRecorder(const CaptureRecorder&) = delete;
    CaptureRecorder& operator=(const CaptureRecorder&) = delete;

    /// Creates the file and its journal and starts the worker. Errors: InvalidArgument (running already, a bad format),
    /// Unsupported (no working encoder), IoError.
    std::expected<void, Error> start(const RecorderConfig& config);
    /// Queue a picture / a block of sound for the file. Cheap; ignored unless started.
    void pushVideo(CaptureFrame frame) noexcept;
    void pushAudio(AudioBlock block) noexcept;
    /// Writes what is queued, finishes the file (index, duration) and removes the journal. Errors: NotFound (no picture ever
    /// arrived: the empty file and the journal are removed), IoError / EncoderError (the writer failed: the file stays,
    /// with its journal, for recovery).
    std::expected<RecorderResult, Error> stop();
    /// Ends the recording as a crash would: the queue is dropped, the file is closed without its index, the journal stays.
    void abandon() noexcept;
    /// Ends the recording and deletes the file and its journal.
    void cancel() noexcept;

    RecorderStatus status();
    bool active() const;
    const std::filesystem::path& file() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::capture
