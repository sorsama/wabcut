#pragma once

// The first-run performance check (I.7; docs/release.md "First-run performance check"): about ten seconds that measure
// what this computer can play and export, and the media-readiness settings that follow (proxy policy, format and size,
// preview quality). Optional and skippable; the app keeps the report and applies the recommendation only when the user
// accepts it (MediaReadinessBridge, Settings > Media & cache).
//
// What it measures, within one time budget (each step is skipped, and says so, once the budget is spent):
//   * decoding: a short 1080p sample of each of H.264, HEVC and ProRes 422 Proxy (made here with the export path's
//     encoders: hardware first for H.264 / HEVC, prores_ks for ProRes) decoded with the hardware backend
//     media::VideoDecoder picks and in software - frames per second, and which backend took it;
//   * export encoders: what an H.264 and an HEVC export would use (exporter::probeVideoEncoder: hardware or software);
//   * the GPUs (media::gpuAdapters), which one Wabcut uses and why (media::selectedGpu, issue #10), and the CPU threads;
//   * the audio output: whether a device is there, and the default one's name (audio::AudioEngine).
// The app adds its graphics API and adapter (RendererInfo) to the report it shows.
//
// Threading: runPerformanceProbe() blocks (seconds); call it off the GUI thread. recommend() and the JSON helpers are
// pure.

#include <wabcut/model/Time.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::editor {

struct DecodeMeasurement {
    std::string codec;           ///< "H.264", "HEVC", "ProRes"
    bool available = false;      ///< a sample could be made and opened
    std::string hardwareBackend; ///< "d3d11va", "cuda", "videotoolbox", ...; empty when no hardware decoder took it
    double hardwareFps = 0.0;    ///< frames per second at the sample's size through that backend; 0 when none
    double softwareFps = 0.0;    ///< in software; 0 when not measured
    std::string note;            ///< why it is unavailable or a part was skipped

    /// The faster of the two (what playback gets).
    double bestFps() const { return hardwareFps > softwareFps ? hardwareFps : softwareFps; }
    bool operator==(const DecodeMeasurement&) const = default;
};

struct EncoderMeasurement {
    std::string codec;   ///< "H.264", "HEVC"
    std::string encoder; ///< the FFmpeg encoder an export would use ("h264_nvenc", "libx264"); empty when none works
    bool hardware = false;
    std::string note;
    bool operator==(const EncoderMeasurement&) const = default;
};

/// What the check recommends for the media-readiness settings (MediaReadinessBridge's values).
struct ReadinessRecommendation {
    std::string proxyPolicy = "auto";    ///< "off", "auto", "always"
    std::string proxyFormat = "prores";  ///< "prores", "dnxhr", "h264"
    int proxyDivisor = 2;                ///< 2 or 4
    std::string previewQuality = "auto"; ///< "auto", "full", "half", "quarter"
    std::vector<std::string> reasons;    ///< one sentence per decision, for the summary
    bool operator==(const ReadinessRecommendation&) const = default;
};

struct PerformanceReport {
    std::vector<DecodeMeasurement> decode;
    std::vector<EncoderMeasurement> encoders;
    std::vector<std::string> gpus; ///< vendors, the GPU Wabcut uses first
    std::string graphics;          ///< set by the app: "D3D11 · RTX 3060"
    /// Issue #10: the GPU export encoders use (media::selectedGpu()) and why (media::gpuReasonKey: "highPerformance",
    /// "userChoice", "windowsPowerSaving", ...); empty without one.
    std::string gpuName;
    std::string gpuReason;
    /// The preview's and decoding's GPU in this run (media::previewGpu()) when it is another one: a new Graphics card
    /// choice waits for the next start there.
    std::string previewGpuName;
    std::vector<std::string> gpuNames; ///< every hardware GPU, high performance first
    bool audioDevice = false;      ///< an output device is present
    std::string audioName;         ///< the default output's name
    std::string audioNote;
    int cpuThreads = 0;
    int width = 0;                 ///< the decode samples' size
    int height = 0;
    double frameRate = 0.0;        ///< and rate (real time = this many frames per second)
    double seconds = 0.0;          ///< how long the check took
    bool cancelled = false;
    bool outOfTime = false;        ///< the budget ran out: some steps were skipped (see the notes)
    ReadinessRecommendation recommendation;
    bool operator==(const PerformanceReport&) const = default;
};

struct ProbeOptions {
    /// Where the samples are written (deleted afterwards). Default: a fresh folder under the system temp folder.
    std::filesystem::path workDir;
    /// The whole check's time budget.
    double budgetSeconds = 10.0;
    int width = 1920;
    int height = 1080;
    Rate frameRate{30, 1};
    /// Frames per sample.
    int frames = 24;
    bool audio = true;
};

/// Runs the check (blocking). `onProgress(fraction, step)` reports each step ("Decoding H.264", ...). A stop request
/// ends it early with what was measured (`cancelled`).
PerformanceReport runPerformanceProbe(const ProbeOptions& options = {}, std::stop_token stop = {},
                                      const std::function<void(double, std::string_view)>& onProgress = {});

/// The media-readiness settings for `report` (its recommendation is not read):
///   * how much faster than real time this computer decodes 1080p H.264 and HEVC (the slower of the two, at its best
///     backend) decides the preview quality and whether proxies are made for everything ("always") or only for media
///     that needs them ("auto"): at least 2x: auto, preview Auto (full quality when the monitor needs it), 1/2-size
///     proxies; at least 1x: auto, preview at 1/2, 1/4-size proxies; below real time: always, preview at 1/4, 1/4-size;
///     nothing measured: the defaults;
///   * the proxy format: H.264 (all-intra, made by the GPU's encoder) when a hardware H.264 encoder and hardware H.264
///     decoding both work, else ProRes Proxy (fast to decode on any CPU).
ReadinessRecommendation recommend(const PerformanceReport& report);

/// JSON for the app's settings and back (nullopt for text that is not a report).
std::string toJson(const PerformanceReport& report);
std::optional<PerformanceReport> reportFromJson(std::string_view json);

} // namespace wabcut::editor
