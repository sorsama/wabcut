// Export settings: presets, validation, and what resolve() makes of a sequence (range, sizes, frame and
// sample counts). Pure: no media, no encoders.

#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>

using namespace wabcut;
using namespace wabcut::exporter;

namespace {

Sequence makeSequence(int width, int height, Rate rate, Ticks duration) {
    Sequence s;
    s.width = width;
    s.height = height;
    s.frameRate = rate;
    s.duration = duration;
    return s;
}

Ticks ms(std::int64_t v) {
    return Ticks{v * (kTicksPerSecond / 1000)};
}

ExportSettings preset(std::string_view id) {
    const auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

} // namespace

TEST_CASE("Built-in presets exist, validate and say what they promise", "[export][settings]") {
    const auto presets = builtInPresets();
    REQUIRE(presets.size() == 18); // six, and twelve for gaming videos (I.9)
    for (const Preset& p : presets) {
        INFO(p.id);
        CHECK(!p.name.empty());
        CHECK(p.settings.presetId == p.id);
        CHECK(validate(p.settings).has_value());
        CHECK(findPreset(p.id).has_value());
    }
    CHECK_FALSE(findPreset("no-such-preset").has_value());

    const ExportSettings match = preset("match-source");
    CHECK(match.container == Container::Mp4);
    CHECK(match.video.codec == VideoCodec::H264);
    CHECK(match.video.rateControl == RateControl::Quality);
    CHECK(match.video.encoder == "auto");
    CHECK_FALSE(match.video.width.has_value()); // the sequence size

    const ExportSettings yt4k = preset("youtube-4k");
    CHECK(yt4k.video.width == 3840);
    CHECK(yt4k.video.height == 2160);
    CHECK(yt4k.video.rateControl == RateControl::Bitrate);

    const ExportSettings vertical = preset("vertical-1080x1920");
    CHECK(vertical.video.width == 1080);
    CHECK(vertical.video.height == 1920);
    CHECK(vertical.video.scaleMode == ScaleMode::Fill);

    const ExportSettings prores = preset("prores-422-hq");
    CHECK(prores.container == Container::Mov);
    CHECK(prores.video.codec == VideoCodec::ProRes);
    CHECK(prores.video.proresProfile == ProResProfile::Hq);
    CHECK(prores.video.pixelFormat == OutputPixelFormat::Yuv422p10);
    CHECK(prores.audio.codec == AudioCodec::Pcm24);

    const ExportSettings wav = preset("audio-wav");
    CHECK(wav.container == Container::Wav);
    CHECK_FALSE(wav.video.enabled);
    CHECK(wav.audio.codec == AudioCodec::Pcm24);
}

TEST_CASE("validate() rejects impossible combinations with a message", "[export][settings]") {
    auto rejects = [](const ExportSettings& s) {
        const auto r = validate(s);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::InvalidArgument);
        CHECK(!r.error().message.empty());
    };
    ExportSettings s = preset("match-source");
    s.video.codec = VideoCodec::ProRes; // ProRes in MP4
    s.video.pixelFormat = OutputPixelFormat::Yuv422p10;
    rejects(s);

    s = preset("match-source");
    s.audio.codec = AudioCodec::Pcm16; // PCM in MP4
    rejects(s);

    s = preset("match-source");
    s.video.width = 1919; // odd
    s.video.height = 1080;
    rejects(s);

    s = preset("match-source");
    s.video.width = 1920; // height missing
    rejects(s);

    s = preset("audio-wav");
    s.video.enabled = true; // video in WAV
    rejects(s);

    s = preset("match-source");
    s.video.enabled = false;
    s.audio.enabled = false;
    rejects(s);

    s = preset("prores-422-hq");
    s.video.pixelFormat = OutputPixelFormat::Yuv420p; // ProRes 422 is 4:2:2 10-bit
    rejects(s);

    s = preset("match-source");
    s.range = RangeMode::Custom; // no custom range
    rejects(s);

    s = preset("match-source");
    s.audio.sampleRate = 12345;
    rejects(s);

    s = preset("match-source");
    s.video.quality = 60;
    rejects(s);

    s = preset("prores-422-hq");
    s.container = Container::Mkv; // ProRes in MKV is fine
    CHECK(validate(s).has_value());
}

TEST_CASE("resolve(): ranges, exact frame and sample counts", "[export][settings]") {
    Sequence seq = makeSequence(1920, 1080, kRate30, Ticks::fromSeconds(10));
    ExportSettings s = preset("match-source");

    auto r = resolve(s, seq);
    REQUIRE(r.has_value());
    CHECK(r->range == TimeRange{Ticks{}, Ticks::fromSeconds(10)});
    CHECK(r->frameCount == 300);
    CHECK(r->duration == Ticks::fromSeconds(10));
    CHECK(r->audioSamples == 480'000);
    CHECK(r->width == 1920);
    CHECK(r->height == 1080);
    CHECK(r->renderWidth == 1920);
    CHECK(r->renderHeight == 1080);
    CHECK(r->frameRate == kRate30);

    // NTSC: ceil(10 s * 30000/1001) = 300 frames; audio covers exactly those 300 frames.
    seq.frameRate = kRate29_97;
    r = resolve(s, seq);
    REQUIRE(r.has_value());
    CHECK(r->frameCount == 300);
    CHECK(r->audioSamples == 480'480); // 300 * 1001/30000 s * 48 kHz
    CHECK(r->duration == framesToTicks(300, kRate29_97));

    // In/Out marks.
    seq.frameRate = kRate30;
    s.range = RangeMode::InOut;
    CHECK_FALSE(resolve(s, seq).has_value()); // no marks
    seq.markIn = ms(500);
    seq.markOut = ms(1500);
    r = resolve(s, seq);
    REQUIRE(r.has_value());
    CHECK(r->range == TimeRange{ms(500), ms(1000)});
    CHECK(r->frameCount == 30);
    CHECK(r->audioSamples == 48'000);

    // Work area.
    s.range = RangeMode::WorkArea;
    CHECK_FALSE(resolve(s, seq).has_value());
    seq.workArea = TimeRange{Ticks::fromSeconds(2), Ticks::fromSeconds(3)};
    r = resolve(s, seq);
    REQUIRE(r.has_value());
    CHECK(r->range.start == Ticks::fromSeconds(2));
    CHECK(r->frameCount == 90);

    // A range that is not a whole number of frames gets one more (partial) frame.
    s.range = RangeMode::Custom;
    s.customRange = TimeRange{Ticks{}, ms(1010)};
    r = resolve(s, seq);
    REQUIRE(r.has_value());
    CHECK(r->frameCount == 31);
    CHECK(r->audioSamples == 49'600); // 31 frames at 30 fps

    // An output frame rate other than the sequence's.
    s.range = RangeMode::WholeSequence;
    s.video.frameRate = kRate60;
    r = resolve(s, seq);
    REQUIRE(r.has_value());
    CHECK(r->frameCount == 600);

    // Audio only: the range length exactly.
    ExportSettings wav = preset("audio-wav");
    wav.range = RangeMode::Custom;
    wav.customRange = TimeRange{Ticks{}, ms(1010)};
    r = resolve(wav, seq);
    REQUIRE(r.has_value());
    CHECK(r->frameCount == 0);
    CHECK(r->audioSamples == 48'480);

    // The whole sequence also covers clips past an explicit duration that is too short.
    Sequence empty = makeSequence(1920, 1080, kRate30, Ticks{});
    CHECK_FALSE(resolve(preset("match-source"), empty).has_value()); // empty range
}

TEST_CASE("resolve(): the whole sequence ends at its last clip after a ripple delete (RC.1)", "[export][settings]") {
    // Ripple removals never shrink a sequence's duration (SequenceEditor), so a sequence cut from 24 s to 22.5 s still
    // says 24 s; exporting it used to add 1.5 s of black and silence at the end. Premiere's Entire Sequence ends at the
    // last clip.
    Sequence seq = makeSequence(1920, 1080, kRate30, Ticks::fromSeconds(24));
    auto track = std::make_shared<Track>();
    track->kind = TrackKind::Video;
    Clip clip;
    clip.timelineRange = TimeRange{Ticks{}, ms(22'500)};
    track->clips.push_back(clip);
    seq.videoTracks.push_back(track);
    auto r = resolve(preset("match-source"), seq);
    REQUIRE(r.has_value());
    CHECK(r->range == TimeRange{Ticks{}, ms(22'500)});
    CHECK(r->frameCount == 675);
    // Without clips the duration still says how long the sequence is (a generator-only or empty timeline).
    const Sequence bare = makeSequence(1920, 1080, kRate30, Ticks::fromSeconds(10));
    r = resolve(preset("match-source"), bare);
    REQUIRE(r.has_value());
    CHECK(r->range == TimeRange{Ticks{}, Ticks::fromSeconds(10)});
}
TEST_CASE("resolve(): Fit pads, Fill centre-crops, Stretch distorts", "[export][settings]") {
    const Sequence seq = makeSequence(1920, 1080, kRate30, Ticks::fromSeconds(1));

    ExportSettings vertical = preset("vertical-1080x1920"); // Fill
    auto r = resolve(vertical, seq);
    REQUIRE(r.has_value());
    CHECK(r->width == 1080);
    CHECK(r->height == 1920);
    CHECK(r->renderHeight == 1920);
    CHECK(r->renderWidth == 3414); // 1920 * 16/9 = 3413.3 -> nearest even

    ExportSettings fit = vertical;
    fit.video.scaleMode = ScaleMode::Fit;
    r = resolve(fit, seq);
    REQUIRE(r.has_value());
    CHECK(r->renderWidth == 1080);
    CHECK(r->renderHeight == 608); // 1080 * 9/16 = 607.5 -> nearest even

    ExportSettings fourThree = preset("match-source");
    fourThree.video.width = 1440;
    fourThree.video.height = 1080;
    r = resolve(fourThree, seq); // Fit by default; the canvas is wider than 4:3: letterbox
    REQUIRE(r.has_value());
    CHECK(r->renderWidth == 1440);
    CHECK(r->renderHeight == 810);

    fourThree.video.scaleMode = ScaleMode::Stretch;
    r = resolve(fourThree, seq);
    REQUIRE(r.has_value());
    CHECK(r->renderWidth == 1440);
    CHECK(r->renderHeight == 1080);

    ExportSettings yt1080 = preset("youtube-1080p"); // same aspect: no pad, no crop
    r = resolve(yt1080, makeSequence(3840, 2160, kRate30, Ticks::fromSeconds(1)));
    REQUIRE(r.has_value());
    CHECK(r->renderWidth == 1920);
    CHECK(r->renderHeight == 1080);
}

TEST_CASE("PCM quantization follows FFmpeg's convention", "[export][settings]") {
    CHECK(quantizePcmSample(0.0f, 16) == 0);
    CHECK(quantizePcmSample(1.0f, 16) == 32767);
    CHECK(quantizePcmSample(-1.0f, 16) == -32768);
    CHECK(quantizePcmSample(2.0f, 16) == 32767);
    CHECK(quantizePcmSample(0.5f, 16) == 16384);
    CHECK(quantizePcmSample(1.0f, 24) == 8'388'607);
    CHECK(quantizePcmSample(-1.0f, 24) == -8'388'608);
    CHECK(quantizePcmSample(0.25f, 24) == 2'097'152);
    CHECK(quantizePcmSample(std::numeric_limits<float>::quiet_NaN(), 24) == 0);
}

TEST_CASE("Hardware encoder policy: one candidate per GPU vendor, overridable", "[export][encoders]") {
    struct Restore {
        ~Restore() { setHardwareEncoderOrder({}); }
    } restore;

    CHECK(softwareEncoder(VideoCodec::H264) == "libx264");
    CHECK(softwareEncoder(VideoCodec::Hevc) == "libx265");
    CHECK(softwareEncoder(VideoCodec::ProRes) == "prores_ks");
    CHECK(softwareEncoder(VideoCodec::DnxHR) == "dnxhd");

    const auto vendors = media::gpuVendors();
    const bool nvidia = std::find(vendors.begin(), vendors.end(), "NVIDIA") != vendors.end();
    const auto h264 = hardwareEncoderOrder(VideoCodec::H264);
    if (nvidia) {
        REQUIRE_FALSE(h264.empty());
        CHECK(h264.front() == "h264_nvenc"); // the RTX 3060 dev machine: primary adapter first
    }
    for (const std::string& name : h264) {
        CHECK(name.starts_with("h264_"));
    }
    const auto candidates = encoderCandidates(VideoCodec::H264);
    REQUIRE_FALSE(candidates.empty());
    CHECK(candidates.back() == "libx264"); // software always last
    CHECK(candidates.size() == h264.size() + 1);
#if !defined(__APPLE__)
    CHECK(hardwareEncoderOrder(VideoCodec::ProRes).empty()); // no ProRes hardware outside macOS
#endif
    CHECK(hardwareEncoderOrder(VideoCodec::DnxHR).empty());

    // Override: filtered per codec, software names dropped; empty restores the policy.
    setHardwareEncoderOrder({"h264_amf", "hevc_nvenc", "libx264", "h264_qsv"});
    CHECK(hardwareEncoderOrder(VideoCodec::H264) == std::vector<std::string>{"h264_amf", "h264_qsv"});
    CHECK(hardwareEncoderOrder(VideoCodec::Hevc) == std::vector<std::string>{"hevc_nvenc"});
    CHECK(encoderCandidates(VideoCodec::Hevc) == std::vector<std::string>{"hevc_nvenc", "libx265"});
    setHardwareEncoderOrder({});
    CHECK(hardwareEncoderOrder(VideoCodec::H264) == h264);
}

TEST_CASE("Audio channels 0 follow the master; AC-3 is up to 5.1 at 32 to 48 kHz (A.9)", "[export][settings][channels]") {
    ExportSettings s = preset("match-source");
    s.audio.channels = 0;
    CHECK(validate(s).has_value());
    CHECK(exportAudioChannels(s.audio, ChannelFormat::Surround51) == 6);
    CHECK(exportAudioChannels(s.audio, ChannelFormat::Stereo) == 2);
    s.audio.channels = 2;
    CHECK(exportAudioChannels(s.audio, ChannelFormat::Surround51) == 2); // asked for: the downmix
    s.audio.codec = AudioCodec::Ac3;
    CHECK(validate(s).has_value()); // AC-3 in MP4
    CHECK(audioCodecName(AudioCodec::Ac3) == "ac3");
    s.audio.channels = 8;
    CHECK_FALSE(validate(s).has_value());
    s.audio.channels = 6;
    s.audio.sampleRate = 96000;
    CHECK_FALSE(validate(s).has_value());
    ExportSettings wav = preset("audio-wav");
    wav.audio.codec = AudioCodec::Ac3;
    CHECK_FALSE(validate(wav).has_value());
    s = preset("match-source");
    s.audio.channels = -1;
    CHECK_FALSE(validate(s).has_value());
}
