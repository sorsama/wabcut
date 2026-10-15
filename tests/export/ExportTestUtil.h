#pragma once

// Helpers of the export tests: a real-media test timeline, ffprobe, decoding exports back, the reference
// frames and reference mix the exports are compared against. FFmpeg stays in ExportTestUtil.cpp.

#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectState.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace exporttest {

using namespace wabcut;

inline Ticks ms(std::int64_t v) {
    return Ticks{v * (kTicksPerSecond / 1000)};
}

/// build/<preset>/test-output/media (the media suite's generated clips).
std::filesystem::path mediaDir();
bool haveMedia(std::string_view name);
/// An empty build/<preset>/test-output/export/<name> directory.
std::filesystem::path freshOutputDir(std::string_view name);
/// Files in `dir` whose name contains ".wabcut-partial-" (temporaries an export must never leave behind).
std::vector<std::filesystem::path> partialFiles(const std::filesystem::path& dir);

/// A project built by hand around real test clips.
struct TestProject {
    ProjectState state;
    SequenceId sequence;

    TestProject(int width, int height, Rate rate, Ticks duration);
    MediaId addMedia(const std::string& file);
    void addVideoTrack(MediaId media, Ticks start, Ticks duration, Ticks sourceIn);
    void addAudioTrack(MediaId media, Ticks start, Ticks duration, Ticks sourceIn, double volumeDb = 0.0);
    /// A track with several clips (sorted by start; gaps in between are fine).
    void addTrack(TrackKind kind, std::vector<Clip> clips);
    /// A clip of `media`'s first video or audio stream.
    Clip clip(MediaId media, StreamKind kind, Ticks start, Ticks duration, Ticks sourceIn);
    void editSequence(const std::function<void(Sequence&)>& edit);
    void editMedia(MediaId media, const std::function<void(MediaItem&)>& edit);
    const Sequence& seq() const;
    ProjectSnapshot snapshot() const { return std::make_shared<const ProjectState>(state); }
};

/// The standard export timeline of length L: three video layers (V1 h264_1080p60_aac.mp4 at 0..L, V2
/// prores_422.mov at L/4..3L/4, V3 rotated_90.mp4 at L/2..L) and two audio tracks (A1 ramp_48k.wav from 1 s
/// at 0..L, A2 sine_48k.wav from 0.25 s at L/8..7L/8, -6 dB). L at most 2 s (rotated_90.mp4 lasts 1 s).
TestProject threeLayerTimeline(int width, int height, Rate rate, Ticks length);
bool haveTimelineMedia();

/// A request for `project` with the synthetic video source.
exporter::ExportRequest makeRequest(const TestProject& project, exporter::ExportSettings settings,
                                    const std::filesystem::path& output);

/// Wraps a video source factory: once the export has taken `frames` frames from the source, `stop` is
/// requested (a deterministic "cancel mid-export").
exporter::VideoSourceFactory cancellingAfter(exporter::VideoSourceFactory inner, std::int64_t frames,
                                             std::stop_source stop);

/// The rendered-source test timeline (canvas width x height, 2 s at `rate`):
///   V1 h264_1080p60_aac.mp4 0..1.5 s from 1 s, scaled to the canvas width;
///   V2 prores_422.mov 0.25..1.75 s, scale 0.5, offset (+200, -80), rotated 10 degrees, opacity 0.75;
///   V3 rotated_90.mp4 0.5..1.5 s (display-rotated clip), scale 0.8, offset (-300, 0), opacity 0.6, and a
///      DISABLED h264_422_10bit.mp4 clip at 0..0.5 s;
///   a gap (black) from 1.75 s to the end; audio A1 ramp_48k.wav 0..2 s, A2 sine_48k.wav 0.25..1.75 s -6 dB.
TestProject motionTimeline(int width, int height, Rate rate);
bool haveMotionTimelineMedia();

/// The program monitor's picture of `project` at sequence `time`, rendered the way the preview does it: the
/// playback evaluator, decoders with the preview's default options (hardware allowed) and
/// render::OffscreenRenderer in the Display role with the preview texture pool. Decoders persist across calls
/// (keyed by clip) so a forward walk decodes sequentially. RGBA8, canvas size.
class PreviewRenderer {
public:
    explicit PreviewRenderer(const TestProject& project);
    ~PreviewRenderer();
    /// RGBA8 at `size` (default: the canvas).
    std::vector<std::uint8_t> render(Ticks time, Ticks frameDuration, int width = 0, int height = 0);
    /// Captions drawn as the program monitor draws them (playback::Scheduler::setCaptions); none by default.
    void setCaptions(std::optional<playback::CaptionSettings> captions);
    int width() const;
    int height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---- ffprobe ----------------------------------------------------------------------------------------------

struct ProbeResult {
    std::vector<std::map<std::string, std::string>> streams;
    std::map<std::string, std::string> format;
    /// The first stream of codec_type `type` ("video", "audio"), or nullptr.
    const std::map<std::string, std::string>* stream(std::string_view type) const;
};

/// Runs the vcpkg ffprobe on `file`. nullopt (and `error`) when it cannot run.
std::optional<ProbeResult> ffprobe(const std::filesystem::path& file, std::string* error = nullptr);

// ---- Decoding ---------------------------------------------------------------------------------------------

/// A planar YUV frame, samples widened to 16 bits.
struct YuvFrame {
    int width = 0;
    int height = 0;
    int chromaWidth = 0;
    int chromaHeight = 0;
    int bits = 8;
    std::string pixelFormat; ///< FFmpeg name, e.g. "yuv420p"
    double seconds = 0.0;    ///< presentation time
    std::array<std::vector<std::uint16_t>, 3> planes;

    std::uint16_t y(int x, int yy) const { return planes[0][static_cast<std::size_t>(yy) * width + x]; }
};

/// The video packets of a file (= frames for intra-only formats such as GIF); -1 when it cannot be opened.
int countVideoPackets(const std::filesystem::path& file);

/// Decodes every frame of the first video stream (software decoding) and calls `onFrame` in display order.
bool decodeVideo(const std::filesystem::path& file, const std::function<void(const YuvFrame&)>& onFrame,
                 std::string* error = nullptr);

/// Frame number shown by a synthetic-pattern frame (its barcode), or -1 when the code is not readable.
int barcodeOf(const YuvFrame& frame);

/// What the export should have encoded for sequence frame `number`: the synthetic pattern rendered at
/// renderWidth x renderHeight, centre-cropped / padded to the frame size of `like` as the export does,
/// converted to `like`'s layout (BT.709 limited, the export's swscale flags).
YuvFrame referenceFrame(int renderWidth, int renderHeight, std::int64_t number, const YuvFrame& like);

/// `rgba` (width x height, RGBA8) converted the way the export converts rendered frames, to `like`'s layout.
YuvFrame yuvFromRgba(const std::vector<std::uint8_t>& rgba, int width, int height, const YuvFrame& like);

/// PSNR over all three planes (peak 2^bits - 1); +infinity for identical frames.
double psnr(const YuvFrame& a, const YuvFrame& b);

struct DecodedAudio {
    std::string codec;
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;    ///< interleaved, every format
    std::vector<std::int32_t> pcm; ///< interleaved integer samples (PCM codecs only): s16 or 24-bit values
};

/// Decodes the first audio stream completely (decoder-side priming/padding trimming applied as FFmpeg does).
std::optional<DecodedAudio> decodeAudio(const std::filesystem::path& file, std::string* error = nullptr);

/// The reference mix: the sequence's audio tracks through the playback path (MediaAudioFeed + renderTrack +
/// the player's mixer rules) from `start`, in blocks of 1024 frames, `frames` sample frames, interleaved.
std::vector<float> referenceMix(const TestProject& project, Ticks start, std::int64_t frames,
                                int channels = 2, int sampleRate = 48000);

/// Appends a measurement line to build/<preset>/test-output/export/metrics.txt (and shows it on failure).
void metric(const std::string& line);

/// Restores the process-wide hardware encoder order when it goes out of scope.
struct EncoderOrderGuard {
    ~EncoderOrderGuard();
};

} // namespace exporttest
