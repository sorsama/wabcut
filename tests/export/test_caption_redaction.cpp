// Codex wave-4 P1-3: a word the soundtrack bleeps or mutes (a source-anchored redaction with audio) must not survive as
// text in an export. Through ExportJob (synthetic picture, so no GPU): the SRT and VTT sidecars carry "[bleep]" (or the
// chosen token) instead of the word, exactly for timed words and for the whole cue without word timings; cues outside
// the bleeped range, disabled redactions and excepted clips are unchanged. Then the mapping itself: clip speed,
// reverse, freeze frames and remapped clips (exact in 100 % segments, whole elsewhere).
// The sidecar cases failed before the fix: the sidecar showed "Alice".

#include "ExportTestUtil.h"

#include <wabcut/playback/video/CaptionRedaction.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;
using wabcut::playback::redactCaptionTrack;
using wabcut::playback::redactedSpeechRanges;

namespace {

ExportSettings presetSettings(std::string_view id) {
    const auto preset = findPreset(id);
    REQUIRE(preset.has_value());
    return preset->settings;
}

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The export timeline (A1 = ramp_48k.wav from 1 s at 0..1 s) with captions and a bleep of media 1.4..1.6 s of A1's
/// file, which plays at timeline 0.4..0.6 s.
struct BleepedProject {
    TestProject project{640, 360, Rate{30, 1}, Ticks::fromSeconds(1)};
    MediaId speech;
    CaptionTrack track;
    std::shared_ptr<playback::CaptionWordTimings> words = std::make_shared<playback::CaptionWordTimings>();

    BleepedProject() {
        project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
        speech = project.seq().audioTracks.front()->clips.begin()->media;
        track.id = project.state.ids.allocate<CaptionTrackId>();
        track.name = "English";
        track.enabled = true;
        track.cues = {{ms(0), ms(250), "Hello there"},
                      {ms(300), ms(400), "call Alice now"},  // timed words: only "Alice" is bleeped
                      {ms(750), ms(200), "back to normal"}}; // after the bleep
        words->cues[{track.id.value, ms(300).value}] = {{ms(300), ms(400)}, {ms(400), ms(600)}, {ms(600), ms(700)}};
        project.editSequence([&](Sequence& s) { s.captionTracks = {track}; });
        Redaction r;
        r.id = project.state.ids.allocate<RedactionId>();
        r.media = speech;
        r.name = "Name";
        r.source = TimeRange::fromStartEnd(Ticks::fromSeconds(1), Ticks::fromSeconds(2));
        r.audio = RedactionAudio::Bleep;
        r.audioRanges = {TimeRange::fromStartEnd(ms(1400), ms(1600))};
        project.state.redactions = {r};
    }

    std::string sidecar(const std::filesystem::path& dir, CaptionExport mode, bool timed, const char* name,
                        std::string token = {}) const {
        ExportSettings settings = presetSettings("youtube-1080p");
        settings.audio.enabled = false;
        settings.captions.mode = mode;
        if (timed) {
            settings.captions.words = words;
        }
        if (!token.empty()) {
            settings.captions.redactedWord = std::move(token);
        }
        const auto out = dir / name;
        const auto result = ExportJob(makeRequest(project, settings, out)).run();
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->captions.empty());
        return readFile(result->captions);
    }
};

} // namespace

TEST_CASE("Caption redaction: sidecars never show a bleeped word", "[export][captions][redaction]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("caption-redaction");
    BleepedProject p;

    SECTION("timed words: exactly the bleeped word, in SRT and VTT") {
        const std::string srt = p.sidecar(dir, CaptionExport::Srt, true, "timed.mp4");
        CHECK(srt.find("Alice") == std::string::npos);
        CHECK(srt.find("call [bleep] now") != std::string::npos);
        CHECK(srt.find("Hello there") != std::string::npos);
        CHECK(srt.find("back to normal") != std::string::npos);
        const std::string vtt = p.sidecar(dir, CaptionExport::Vtt, true, "timed-vtt.mp4");
        CHECK(vtt.find("Alice") == std::string::npos);
        CHECK(vtt.find("call [bleep] now") != std::string::npos);
    }
    SECTION("without word timings the whole cue is masked") {
        const std::string srt = p.sidecar(dir, CaptionExport::Srt, false, "untimed.mp4");
        CHECK(srt.find("Alice") == std::string::npos);
        CHECK(srt.find("call") == std::string::npos);
        CHECK(srt.find("[bleep]") != std::string::npos);
        CHECK(srt.find("Hello there") != std::string::npos);
    }
    SECTION("a chosen token") {
        const std::string srt = p.sidecar(dir, CaptionExport::Srt, true, "token.mp4", "\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F");
        CHECK(srt.find("call \xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F now") != std::string::npos);
    }
    SECTION("a disabled redaction, a picture-only redaction and an excepted clip leave the text") {
        BleepedProject q;
        SECTION("disabled") {
            q.project.state.redactions.front().enabled = false;
        }
        SECTION("picture only") {
            q.project.state.redactions.front().audio = RedactionAudio::None;
        }
        SECTION("excepted clip") {
            q.project.state.redactions.front().exceptions = {q.project.seq().audioTracks.front()->clips.begin()->id};
        }
        const std::string srt = q.sidecar(dir, CaptionExport::Srt, true, "kept.mp4");
        CHECK(srt.find("call Alice now") != std::string::npos);
    }
}

namespace {

Clip clipOf(MediaId media, Ticks start, Ticks duration, Ticks sourceIn) {
    Clip c;
    c.id = ClipId{media.value * 100 + static_cast<std::uint64_t>(start.value % 97)};
    c.media = media;
    c.stream = StreamRef{StreamKind::Audio, 0};
    c.timelineRange = TimeRange{start, duration};
    c.sourceIn = sourceIn;
    return c;
}

Sequence sequenceWith(std::vector<Clip> clips) {
    Sequence s;
    auto track = std::make_shared<Track>();
    track->clips = ClipList(std::move(clips));
    s.audioTracks = {track};
    return s;
}

ProjectState bleep(MediaId media, TimeRange source) {
    ProjectState state;
    Redaction r;
    r.id = RedactionId{900};
    r.media = media;
    r.source = source;
    r.audio = RedactionAudio::Mute;
    state.redactions = {r};
    return state;
}

} // namespace

TEST_CASE("Caption redaction: bleeped media time maps onto the timeline through every kind of clip",
          "[export][captions][redaction]") {
    const MediaId m{7};
    const ProjectState state = bleep(m, TimeRange::fromStartEnd(Ticks::fromSeconds(2), Ticks::fromSeconds(3)));
    const auto near = [](const std::vector<TimeRange>& r, double a, double b) {
        return r.size() == 1 && std::abs(r[0].start.toSeconds() - a) < 1e-6 && std::abs(r[0].end().toSeconds() - b) < 1e-6;
    };

    SECTION("100 %: shifted") {
        const auto r = redactedSpeechRanges(state, sequenceWith({clipOf(m, Ticks::fromSeconds(10), Ticks::fromSeconds(5), {})}));
        CHECK(near(r, 12.0, 13.0));
    }
    SECTION("200 %: half as long") {
        Clip c = clipOf(m, Ticks::fromSeconds(10), Ticks::fromSeconds(5), {});
        c.speed = Speed{2, 1};
        const auto r = redactedSpeechRanges(state, sequenceWith({c}));
        REQUIRE(r.size() == 1);
        CHECK(r[0].start.toSeconds() <= 11.0);
        CHECK(r[0].end().toSeconds() >= 11.5);
        CHECK(r[0].duration.toSeconds() < 0.51);
    }
    SECTION("reversed: from the clip's end") {
        Clip c = clipOf(m, Ticks::fromSeconds(10), Ticks::fromSeconds(4), {}); // source 0..4 s played backwards
        c.reversed = true;
        const auto r = redactedSpeechRanges(state, sequenceWith({c}));
        REQUIRE(r.size() == 1);
        CHECK(r[0].start.toSeconds() <= 11.0);
        CHECK(r[0].end().toSeconds() >= 12.0);
        CHECK(r[0].duration.toSeconds() < 1.01);
    }
    SECTION("outside the clip's source: nothing") {
        CHECK(redactedSpeechRanges(state, sequenceWith({clipOf(m, Ticks{}, Ticks::fromSeconds(1), Ticks::fromSeconds(5))})).empty());
        CHECK(redactedSpeechRanges(state, sequenceWith({clipOf(MediaId{8}, Ticks{}, Ticks::fromSeconds(5), {})})).empty());
    }
    SECTION("remapped: exact at 100 %, a whole ramp segment otherwise") {
        Clip c = clipOf(m, Ticks::fromSeconds(10), Ticks::fromSeconds(4), Ticks{});
        ClipContent content = c.content();
        Curve remap;
        // Curve 0..1.5 s plays media 0..1.5 s at 100 %, 1.5..2.5 s ramps to 3.5 s (200 %), then 100 % again.
        remap.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{ms(1500), 1.5 * kTicksPerSecond},
                      Keyframe{ms(2500), 3.5 * kTicksPerSecond},
                      Keyframe{Ticks::fromSeconds(4), 5.0 * kTicksPerSecond}};
        content.timeRemap = remap;
        c.setContent(std::move(content));
        const auto r = redactedSpeechRanges(state, sequenceWith({c}));
        CHECK(near(r, 11.5, 12.5)); // the 200 % segment shows 2..3 s: covered whole
        const ProjectState late = bleep(m, TimeRange::fromStartEnd(Ticks::fromSeconds(4), ms(4500)));
        // The last segment plays media 3.5..5 s at 100 % from 12.5 s: 4..4.5 s is heard at 13..13.5 s exactly.
        CHECK(near(redactedSpeechRanges(late, sequenceWith({c})), 13.0, 13.5));
    }
}

namespace {

Clip nestedClipOf(ClipId id, SequenceId sequence, Ticks start, Ticks duration, Ticks sourceIn, AngleSet angles = {}) {
    Clip c;
    c.id = id;
    c.sequence = sequence;
    c.stream = StreamRef{StreamKind::Audio, 0};
    c.timelineRange = TimeRange{start, duration};
    c.sourceIn = sourceIn;
    c.angles = angles;
    return c;
}

SequencePtr sequenceOf(SequenceId id, std::vector<std::vector<Clip>> tracks, std::optional<MulticamInfo> multicam = {}) {
    auto s = std::make_shared<Sequence>();
    s->id = id;
    s->width = 1920;
    s->height = 1080;
    s->duration = Ticks::fromSeconds(30);
    std::uint64_t trackId = id.value * 10;
    for (std::vector<Clip>& clips : tracks) {
        auto track = std::make_shared<Track>();
        track->id = TrackId{++trackId};
        track->kind = TrackKind::Audio;
        track->clips = ClipList(std::move(clips));
        s->audioTracks.push_back(track);
    }
    if (multicam) {
        for (std::size_t i = 0; i < multicam->angles.size() && i < s->audioTracks.size(); ++i) {
            multicam->angles[i].audio = {s->audioTracks[i]->id};
        }
        s->multicam = std::move(multicam);
    }
    return s;
}

} // namespace

TEST_CASE("Caption redaction: speech bleeped inside nested and multicam clips, and through subclips (review wave 5 "
          "E5-P1-1, E5-P1-2)",
          "[export][captions][redaction][nested]") {
    const MediaId m{7};
    ProjectState state = bleep(m, TimeRange::fromStartEnd(Ticks::fromSeconds(2), Ticks::fromSeconds(3)));
    const auto near = [](const std::vector<TimeRange>& r, double a, double b) {
        return r.size() == 1 && std::abs(r[0].start.toSeconds() - a) < 1e-6 && std::abs(r[0].end().toSeconds() - b) < 1e-6;
    };
    // N (50): the bleeped media at 0..5 s. N2 (51): N nested at 1..6 s. M (52): a multicam source, angle 1 = the bleeped
    // media, angle 0 = another recording.
    const SequencePtr n = sequenceOf(SequenceId{50}, {{clipOf(m, Ticks{}, Ticks::fromSeconds(5), {})}});
    const SequencePtr n2 = sequenceOf(SequenceId{51}, {{nestedClipOf(ClipId{601}, n->id, Ticks::fromSeconds(1), Ticks::fromSeconds(5), {})}});
    MulticamInfo info;
    info.angles = {MulticamAngle{"Cam 1", {}, {}, Ticks{}}, MulticamAngle{"Cam 2", {}, {}, Ticks{}}};
    const SequencePtr mc = sequenceOf(SequenceId{52}, {{clipOf(MediaId{8}, Ticks{}, Ticks::fromSeconds(5), {})},
                                                       {clipOf(m, Ticks{}, Ticks::fromSeconds(5), {})}},
                                      info);
    state.sequences = {n, n2, mc};
    const auto topWith = [](Clip clip) {
        Sequence top;
        top.id = SequenceId{60};
        top.width = 1920;
        top.height = 1080;
        auto track = std::make_shared<Track>();
        track->kind = TrackKind::Audio;
        track->clips = ClipList({std::move(clip)});
        top.audioTracks = {track};
        return top;
    };

    SECTION("a nested clip: its speech maps out through the nest (it showed the word before)") {
        CHECK(near(redactedSpeechRanges(state, topWith(nestedClipOf(ClipId{600}, n->id, Ticks::fromSeconds(10), Ticks::fromSeconds(5), {}))),
                   12.0, 13.0));
        // Through the nested clip's own speed: at 200 % the word plays at 11..11.5 s.
        Clip fast = nestedClipOf(ClipId{600}, n->id, Ticks::fromSeconds(10), Ticks::fromSeconds(2), {});
        fast.speed = Speed{2, 1};
        const auto r = redactedSpeechRanges(state, topWith(fast));
        REQUIRE(r.size() == 1);
        CHECK(r[0].start.toSeconds() <= 11.0);
        CHECK(r[0].end().toSeconds() >= 11.5);
    }
    SECTION("a nest in a nest") {
        CHECK(near(redactedSpeechRanges(state, topWith(nestedClipOf(ClipId{600}, n2->id, Ticks::fromSeconds(10), Ticks::fromSeconds(6), {}))),
                   13.0, 14.0));
    }
    SECTION("excepting the nested clip excepts what is inside it") {
        state.redactions.edit().front().exceptions = {ClipId{600}};
        CHECK(redactedSpeechRanges(state, topWith(nestedClipOf(ClipId{600}, n->id, Ticks::fromSeconds(10), Ticks::fromSeconds(5), {}))).empty());
    }
    SECTION("a multicam clip: the active audio angle's speech only") {
        CHECK(near(redactedSpeechRanges(state, topWith(nestedClipOf(ClipId{600}, mc->id, Ticks::fromSeconds(10), Ticks::fromSeconds(5), {},
                                                                     AngleSet::of(1)))),
                   12.0, 13.0));
        CHECK(redactedSpeechRanges(state, topWith(nestedClipOf(ClipId{600}, mc->id, Ticks::fromSeconds(10), Ticks::fromSeconds(5), {},
                                                                AngleSet::of(0))))
                  .empty());
    }
    SECTION("a subclip of the bleeped media (E5-P1-1)") {
        auto master = std::make_shared<MediaItem>();
        master->id = m;
        master->name = "Interview";
        auto subclip = std::make_shared<MediaItem>(*master);
        subclip->id = MediaId{9};
        subclip->name = "Interview, the answer";
        subclip->subclip = SubclipSpec{m, TimeRange::fromStartEnd(Ticks::fromSeconds(1), Ticks::fromSeconds(4)), true};
        state.media = {master, subclip};
        CHECK(near(redactedSpeechRanges(state, sequenceWith({clipOf(MediaId{9}, Ticks::fromSeconds(10), Ticks::fromSeconds(3), Ticks::fromSeconds(1))})),
                   11.0, 12.0));
    }
    SECTION("the burned-in caption over the nested word reads [bleep]") {
        Sequence top = topWith(nestedClipOf(ClipId{600}, n->id, Ticks::fromSeconds(10), Ticks::fromSeconds(5), {}));
        CaptionTrack track;
        track.id = CaptionTrackId{70};
        track.enabled = true;
        track.cues = {{Ticks::fromSeconds(12), Ticks::fromSeconds(1), "my name is John"}};
        top.captionTracks = {track};
        const auto frame = playback::evaluateCaption(top, ms(12500), playback::CaptionSettings{}, &state);
        REQUIRE(frame);
        CHECK(frame->text == "[bleep]");
    }
}

TEST_CASE("Caption redaction: words are replaced in place, cues outside the ranges untouched",
          "[export][captions][redaction]") {
    CaptionTrack track;
    track.id = CaptionTrackId{3};
    track.cues = {{ms(0), ms(1000), "one two three"}, {ms(2000), ms(1000), "สวัสดีครับ"}};
    playback::CaptionWordTimings words;
    words.cues[{3, 0}] = {{ms(0), ms(300)}, {ms(300), ms(600)}, {ms(600), ms(1000)}};
    const std::vector<TimeRange> ranges{TimeRange::fromStartEnd(ms(350), ms(450)), TimeRange::fromStartEnd(ms(2100), ms(2200))};
    const CaptionTrack out = redactCaptionTrack(track, ranges, &words, "[bleep]");
    CHECK(out.cues[0].text == "one [bleep] three");
    CHECK(out.cues[1].text == "[bleep]"); // no timings: the whole cue
    CHECK(redactCaptionTrack(track, {}, &words, "[bleep]") == track);
}
