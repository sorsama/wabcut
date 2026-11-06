#pragma once

// CaptureWriter (0.7, card "Capture video from tape, capture card or camera", docs/capture.md): the file side of a live
// capture. Pictures and sound arrive in capture order from one thread; they are converted to the encoder's layout
// (libswscale), encoded with the export's own encoders (hardware first, probed, then software; the fast software presets
// because the source is live) and muxed into a file that stays readable if the program stops before finish():
//   * Matroska writes a cluster about every second (cluster_time_limit) and each packet is flushed, so a crash loses at
//     most the last second; the missing index and duration are put back by recoverCapture() in the capture library;
//   * MP4 / MOV are written as fragments (frag_keyframe + empty_moov) for the same reason.
// The video is constant frame rate: picture k is shown at k / frameRate (the recorder repeats or drops pictures to keep the
// device's irregular arrival on that grid). The sound is written from sample 0 as given; the recorder pads or trims it to
// line up with the first picture.
//
// Threading: one thread calls writeVideo / writeAudio / finish. Nothing here is thread-safe on its own.

#include <wabcut/export/ExportSettings.h>

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wabcut::exporter {

/// The layouts a capture device's pictures arrive in, top-down (a bottom-up device picture is turned over by the source).
enum class CapturePixel : std::uint8_t {
    Nv12,    ///< 4:2:0, Y plane then interleaved CbCr
    Yuy2,    ///< 4:2:2, Y0 Cb Y1 Cr
    Uyvy,    ///< 4:2:2, Cb Y0 Cr Y1
    Yuv420p, ///< Y, Cb, Cr planes
    P010,    ///< 4:2:0 10-bit in 16-bit words, Y plane then interleaved CbCr
    Bgr24,   ///< 8-bit B, G, R
    Bgra,    ///< 8-bit B, G, R, unused
};

/// One picture: up to three planes with their row strides in bytes (a packed layout uses plane 0 only).
struct CapturePicture {
    const std::uint8_t* planes[3] = {nullptr, nullptr, nullptr};
    int strides[3] = {0, 0, 0};
};

struct CaptureWriterSettings {
    std::filesystem::path file;          ///< created (its folder must exist); an existing file is replaced
    Container container = Container::Mkv;
    VideoSettings video;                 ///< codec, encoder, rate control, output pixel format (size and rate come from below)
    int width = 0;                       ///< even
    int height = 0;                      ///< even
    Rate frameRate{30, 1};
    Rate pixelAspect{1, 1};              ///< shape of one pixel (DV: 10:11 or 40:33), written into the stream
    CapturePixel pixel = CapturePixel::Nv12;
    bool hasAudio = false;
    AudioSettings audio;                 ///< sampleRate and channels are those of the samples given to writeAudio
    std::string title;                   ///< the file's title tag (the clip name)
};

struct CaptureWriteResult {
    std::filesystem::path file;
    std::string encoder;                 ///< e.g. "h264_nvenc", "libx264"
    std::vector<std::string> fallbacks;  ///< encoders tried first and rejected, "name: reason"
    std::int64_t videoFrames = 0;
    std::int64_t audioFrames = 0;        ///< samples per channel
};

class CaptureWriter {
public:
    /// Opens the encoders and the file and writes its header. Errors: InvalidArgument (odd or empty size, a codec the
    /// container cannot hold), Unsupported (no working encoder), IoError (the file cannot be created).
    static std::expected<std::unique_ptr<CaptureWriter>, Error> open(const CaptureWriterSettings& settings);
    /// A writer that was not finished closes the file as it is (no trailer, no index): it stays readable up to the last
    /// complete cluster, and recoverCapture() makes it a clean file.
    ~CaptureWriter();

    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    /// Encodes the picture shown at `frameIndex` / frameRate. Indices must increase.
    std::expected<void, Error> writeVideo(std::int64_t frameIndex, const CapturePicture& picture);
    /// Encodes interleaved samples that follow the ones written before (settings.audio.channels per frame).
    std::expected<void, Error> writeAudio(std::span<const float> interleaved);
    /// Flushes the encoders, writes the index and closes the file.
    std::expected<CaptureWriteResult, Error> finish();

    const std::string& encoderName() const;
    std::int64_t videoFrames() const;
    std::int64_t audioFrames() const;

private:
    CaptureWriter();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::exporter
