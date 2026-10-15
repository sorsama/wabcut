#pragma once

// Proxy media (docs/architecture.md, "Progressive media readiness"): an editing-friendly intra-frame copy of a
// source file at half or quarter resolution. The transcode reuses the export encoder layer (muxer, encoder
// selection with hardware probing and fallback) and keeps the source's timestamps: proxy frame n covers exactly
// the source time of source frame n (VFR included), relative to the same media origin, so a proxy can stand in
// for its original frame for frame. Audio streams are copied, not re-encoded. Exports never use proxies.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::exporter {

enum class ProxyCodec : std::uint8_t {
    ProResProxy, ///< prores_ks profile 0 ("proxy"), 10-bit 4:2:2: the default
    DnxhrLb,     ///< dnxhd with the DNxHR LB profile, 8-bit 4:2:2
    H264Intra,   ///< all-intra H.264 8-bit 4:2:0: h264_nvenc (fast) when it works, else libx264
};

struct ProxySettings {
    ProxyCodec codec = ProxyCodec::ProResProxy;
    /// 2 = half resolution, 4 = quarter (each dimension).
    int divisor = 2;
    /// Copy the source's audio streams into the proxy (a self-contained file; the timeline reads originals).
    bool copyAudio = true;
};

/// ProxyProvenance::generator of the proxies this build writes.
inline constexpr std::string_view kProxyGenerator = "wabcut-proxy 1";

/// Stable description stored in ProxyProvenance::settings, e.g. "prores_ks proxy 1/2 audio=copy".
std::string describeProxySettings(const ProxySettings& settings);
/// Parses describeProxySettings() output (nullopt-like failure: returns false).
bool parseProxySettings(std::string_view text, ProxySettings& out);
/// Frame size of a proxy of a `width` x `height` source: each side divided and rounded to even, at least 2.
std::pair<int, int> proxyFrameSize(int width, int height, int divisor);

struct ProxyControl {
    std::stop_token stop;
    /// Called between frames; may block while foreground work runs (the resource governor). Returning false
    /// cancels the transcode.
    std::function<bool()> yield;
    /// After each frame: frames written, frames expected (0 when the source does not say).
    std::function<void(std::int64_t done, std::int64_t total)> onProgress;
};

struct ProxyResult {
    std::filesystem::path file;
    std::string encoder;                ///< e.g. "prores_ks", "h264_nvenc", "libx264"
    std::vector<std::string> fallbacks; ///< encoders tried and rejected first ("h264_nvenc: <reason>")
    int width = 0;
    int height = 0;
    std::int64_t frames = 0;
    int audioStreams = 0;
    bool timecode = false; ///< the source's start timecode was carried over
    Ticks firstFrame{};    ///< source time of the first frame (equal in source and proxy)
};

/// Transcodes the best video stream of `source` (UTF-8 path) into `output` (a .mov). Writes "<output>.partial"
/// and renames it on success; on any error or cancellation nothing is left behind. The written file is checked
/// before the rename: same frame count and first-frame time as the source. Errors: NotFound, Unsupported (no
/// video, sources with alpha, no working encoder), Cancelled, DecoderError, EncoderError, IoError, and
/// InvalidArgument when the check fails.
std::expected<ProxyResult, Error> transcodeProxy(const std::string& source, const std::filesystem::path& output,
                                                 const ProxySettings& settings, const ProxyControl& control = {});

} // namespace wabcut::exporter
