#pragma once

// Video encoder selection (Principle 4): hardware encoders first, probed at runtime; software fallback.

#include <wabcut/export/ExportSettings.h>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::exporter {

/// Hardware encoders tried, in order, for `codec` with VideoSettings::encoder == "auto". Policy: one per GPU
/// vendor present (media::gpuVendors(), primary adapter first): NVIDIA → *_nvenc, AMD → *_amf, Intel → *_qsv,
/// Apple → *_videotoolbox; on Linux AMD/Intel use *_vaapi. ProRes and DNxHR have no hardware encoder outside
/// macOS. Overridden by setHardwareEncoderOrder(), else by the WABCUT_HW_ENCODERS environment variable
/// (comma-separated FFmpeg encoder names; names for other codecs are ignored).
std::vector<std::string> hardwareEncoderOrder(VideoCodec codec);

/// Replaces the hardware candidates for the whole process with `encoders` (filtered per codec). An empty list
/// restores the policy. Thread-safe; affects exports started afterwards.
void setHardwareEncoderOrder(std::vector<std::string> encoders);

/// The software encoder of a codec: libx264, libx265, prores_ks, dnxhd, libsvtav1 (SVT-AV1, I.9); 0.2: png, tiff, dpx,
/// mjpeg, targa, exr, qtrle, cfhd.
std::string_view softwareEncoder(VideoCodec codec);

/// 0.2 (the CPU limit preference for exports): at most `threads` threads per encoder and per frame converter of the
/// exports started afterwards (and the render threads of the rendered source: one below 4); 0 = no limit (FFmpeg's
/// automatic thread count). Process-wide, thread-safe.
void setExportThreadLimit(int threads);
int exportThreadLimit();
/// The thread limit of `percent` of this computer's logical processors (at least 1); 100 or more = 0 (no limit).
int threadLimitForPercent(int percent);

/// Every encoder "auto" tries for `codec`, in order: hardwareEncoderOrder(codec) then softwareEncoder(codec).
std::vector<std::string> encoderCandidates(VideoCodec codec);

struct EncoderProbe {
    std::string encoder;                ///< the encoder an export would use, e.g. "h264_nvenc"
    std::vector<std::string> fallbacks; ///< candidates rejected first, "name: reason"
};

/// Which encoder an export with `video` at width x height and `frameRate` would use, decided exactly as the
/// export decides (each hardware candidate opened and test-encoded, then the software encoder). Opens real
/// encoders (~0.1-0.5 s with NVENC): call it off the GUI thread. Errors: Unsupported (no working encoder).
std::expected<EncoderProbe, Error> probeVideoEncoder(const VideoSettings& video, int width, int height,
                                                     Rate frameRate);

} // namespace wabcut::exporter
