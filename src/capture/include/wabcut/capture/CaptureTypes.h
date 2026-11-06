#pragma once

// Capture types (0.7, card "Capture video from tape, capture card or camera", docs/capture.md): what a capture device
// delivers - pictures and sound with the host clock time they were taken at - and how a device describes itself. No Qt,
// no operating-system types: the Windows source (Media Foundation), the fake source of the tests and the recorder only
// meet here.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::capture {

/// The layouts pictures arrive in once the source has decoded or converted them (a compressed device format - MJPEG, H.264 -
/// is decoded by the system first). Always top-down.
enum class PixelFormat : std::uint8_t {
    Nv12,  ///< 4:2:0, Y plane then interleaved CbCr
    Yuy2,  ///< 4:2:2 packed, Y0 Cb Y1 Cr
    Uyvy,  ///< 4:2:2 packed, Cb Y0 Cr Y1
    I420,  ///< Y, Cb, Cr planes
    P010,  ///< 4:2:0 10-bit in 16-bit words
    Bgr24, ///< 8-bit B, G, R
    Bgra,  ///< 8-bit B, G, R, unused
};

/// "NV12", "YUY2", "UYVY", "I420", "P010", "RGB24", "RGB32".
std::string_view pixelFormatName(PixelFormat format);
/// Planes of the packed layout below.
int planeCount(PixelFormat format);
/// Row stride in bytes of plane `plane` in the packed layout (tightly packed rows).
int planeStride(PixelFormat format, int width, int plane);
/// Rows of plane `plane`.
int planeRows(PixelFormat format, int height, int plane);
/// Bytes of one picture in the packed layout (planes one after the other, no padding).
std::size_t frameBytes(PixelFormat format, int width, int height);

/// A format a device can deliver, as the format list shows it.
struct VideoFormat {
    int width = 0;
    int height = 0;
    Rate frameRate{30, 1};
    PixelFormat pixel = PixelFormat::Nv12; ///< the layout of the pictures the source delivers
    std::string nativeName;                ///< what the device sends before the system converts it: "MJPG", "YUY2", "NV12", "H264"
    bool compressed = false;               ///< nativeName is a compressed format the system decodes
    Rate pixelAspect{1, 1};                ///< shape of one pixel (DV 4:3: 10:11 at 720 x 480)
    bool interlaced = false;               ///< the device marks the pictures as two woven fields (they are kept woven)

    bool operator==(const VideoFormat&) const = default;
    /// "1920 x 1080 · 59.94 fps · MJPG".
    std::string label() const;
    double fps() const { return frameRate.isValid() ? static_cast<double>(frameRate.num) / frameRate.den : 0.0; }
};

/// A video capture device: a webcam, a capture card or a camera / deck the system exposes as one. `name` is the system's
/// text (untrusted: shown, never interpreted). `formats` is filled by CaptureBackend::videoFormats(), which may open the
/// device briefly, so it is asked for when the user picks the device.
struct VideoDevice {
    std::string id;      ///< stable within the backend (the system's symbolic link on Windows)
    std::string name;
    std::vector<VideoFormat> formats;
};

struct AudioDevice {
    std::string id;
    std::string name;
    bool isDefault = false;
};

/// One picture, packed (see planeStride), top-down. Immutable once delivered; shared between the recorder and the preview.
struct CaptureFrame {
    PixelFormat pixel = PixelFormat::Nv12;
    int width = 0;
    int height = 0;
    /// Steady-clock nanoseconds (std::chrono::steady_clock) at which the picture was taken.
    std::int64_t hostNs = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> data;

    bool valid() const { return data && data->size() >= frameBytes(pixel, width, height) && width > 0 && height > 0; }
    const std::uint8_t* plane(int index) const;
    int stride(int index) const { return planeStride(pixel, width, index); }
};

/// A frame filled by `fill(bytes)` (packed layout, frameBytes long). For sources and tests.
template <class Fill>
CaptureFrame makeFrame(PixelFormat pixel, int width, int height, std::int64_t hostNs, Fill&& fill) {
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(frameBytes(pixel, width, height));
    fill(bytes->data());
    CaptureFrame frame;
    frame.pixel = pixel;
    frame.width = width;
    frame.height = height;
    frame.hostNs = hostNs;
    frame.data = std::move(bytes);
    return frame;
}

/// A block of sound, interleaved float.
struct AudioBlock {
    std::vector<float> samples;
    int channels = 2;
    /// Index, in the device's stream, of the first frame: consecutive blocks follow each other, a jump means the device
    /// lost that much sound (the recorder fills the gap with silence).
    std::int64_t firstFrame = 0;
    /// Steady-clock nanoseconds at which the first frame was heard (the input latency already taken off).
    std::int64_t hostNs = 0;

    std::size_t frames() const { return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0; }
};

} // namespace wabcut::capture
