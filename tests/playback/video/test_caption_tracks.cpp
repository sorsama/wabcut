// 0.7 (card 116): bilingual captions - several caption tracks on screen together. A track marked "show together" is drawn
// with the main caption track (the one chosen for the monitor or the export, else the first enabled), one caption layer
// per track, stacked in track order (the first topmost) by the edge their style is placed at, each in its own style.

#include <wabcut/model/ProjectState.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::playback;
using Catch::Approx;

namespace {

Ticks ms(std::int64_t v) { return Ticks{v * (kTicksPerSecond / 1000)}; }

/// A sequence with an English and a Spanish caption track (two cues each, the same times); `together` marks Spanish.
struct Bilingual {
    ProjectState state;
    Sequence seq;
    CaptionTrackId english, spanish;

    explicit Bilingual(bool together = true) {
        seq.id = state.ids.allocate<SequenceId>();
        seq.width = 1280;
        seq.height = 720;
        seq.duration = Ticks::fromSeconds(10);
        const auto track = [&](const char* name, const char* language, const char* a, const char* b) {
            CaptionTrack t;
            t.id = state.ids.allocate<CaptionTrackId>();
            t.name = name;
            t.language = language;
            t.cues = {{Ticks{}, Ticks::fromSeconds(2), a}, {Ticks::fromSeconds(4), Ticks::fromSeconds(2), b}};
            seq.captionTracks.push_back(std::move(t));
            return seq.captionTracks.back().id;
        };
        english = track("English", "en", "Hello", "Goodbye");
        spanish = track("Spanish", "es", "Hola", "Adios");
        seq.captionTracks.back().showTogether = together;
    }
    CaptionTrack& spanishTrack() { return seq.captionTracks.back(); }
};

} // namespace

TEST_CASE("Bilingual captions: a track marked to show together is drawn with the main one, the first topmost",
          "[playback][captions][bilingual]") {
    Bilingual b;
    const auto tracks = captionTracksFor(b.seq, {});
    REQUIRE(tracks.size() == 2);
    CHECK(tracks[0]->id == b.english);
    CHECK(tracks[1]->id == b.spanish);

    const auto frames = evaluateCaptions(b.seq, ms(500), {}, &b.state);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].text == "Hello");
    CHECK(frames[1].text == "Hola");
    // Both at the bottom: the last track sits at the edge, the first above it (moved up by the Spanish block).
    CHECK(frames[1].closer.empty());
    REQUIRE(frames[0].closer.size() == 1);
    CHECK(frames[0].closer[0].text == "Hola");
    CHECK(frames[0].closer[0].style == frames[1].style);
}

TEST_CASE("Bilingual captions: without the mark, a disabled track or a track with no cue now, nothing is added",
          "[playback][captions][bilingual]") {
    Bilingual alone(false);
    CHECK(captionTracksFor(alone.seq, {}).size() == 1);
    const auto single = evaluateCaptions(alone.seq, ms(500), {}, &alone.state);
    REQUIRE(single.size() == 1);
    CHECK(single[0].text == "Hello");
    CHECK(single[0].closer.empty());
    // The single-frame call is still the main track's frame.
    CHECK(evaluateCaption(alone.seq, ms(500), {}, &alone.state)->text == "Hello");

    Bilingual off;
    off.spanishTrack().enabled = false; // an off track never draws, marked or not
    CHECK(captionTracksFor(off.seq, {}).size() == 1);
    CHECK(evaluateCaptions(off.seq, ms(500), {}, &off.state).size() == 1);

    Bilingual gap;
    gap.spanishTrack().cues.erase(gap.spanishTrack().cues.begin()); // no Spanish cue at 0.5 s: no gap is left for it
    const auto onlyEnglish = evaluateCaptions(gap.seq, ms(500), {}, &gap.state);
    REQUIRE(onlyEnglish.size() == 1);
    CHECK(onlyEnglish[0].text == "Hello");
    CHECK(onlyEnglish[0].closer.empty());
    CHECK(evaluateCaptions(gap.seq, ms(4500), {}, &gap.state).size() == 2);
    CHECK(evaluateCaptions(gap.seq, ms(3000), {}, &gap.state).empty()); // between cues: nothing
}

TEST_CASE("Bilingual captions: the chosen track is the main one, the marked tracks join it in track order",
          "[playback][captions][bilingual]") {
    Bilingual b(false);
    CaptionSettings spanishMain;
    spanishMain.track = b.spanish;
    // Nothing is marked: the chosen track alone.
    const auto alone = captionTracksFor(b.seq, spanishMain);
    REQUIRE(alone.size() == 1);
    CHECK(alone[0]->id == b.spanish);
    // Mark English: it joins the chosen Spanish track, still in track order (English first).
    b.seq.captionTracks.front().showTogether = true;
    const auto marked = captionTracksFor(b.seq, spanishMain);
    REQUIRE(marked.size() == 2);
    CHECK(marked[0]->id == b.english);
    CHECK(marked[1]->id == b.spanish);
}

TEST_CASE("Bilingual captions: top placement stacks downwards from the edge, a custom position is never moved",
          "[playback][captions][bilingual]") {
    Bilingual b;
    for (CaptionTrack& t : b.seq.captionTracks) {
        t.style.placement = CaptionPlacement::Top;
    }
    auto frames = evaluateCaptions(b.seq, ms(500), {}, &b.state);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].closer.empty()); // the first track at the top edge
    REQUIRE(frames[1].closer.size() == 1);
    CHECK(frames[1].closer[0].text == "Hello");

    for (CaptionTrack& t : b.seq.captionTracks) {
        t.style.placement = CaptionPlacement::Custom;
    }
    frames = evaluateCaptions(b.seq, ms(500), {}, &b.state);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].closer.empty());
    CHECK(frames[1].closer.empty());

    // English at the bottom, Spanish at the top: two groups of one, nothing to stack.
    b.seq.captionTracks[0].style.placement = CaptionPlacement::Bottom;
    b.seq.captionTracks[1].style.placement = CaptionPlacement::Top;
    frames = evaluateCaptions(b.seq, ms(500), {}, &b.state);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].closer.empty());
    CHECK(frames[1].closer.empty());
}

TEST_CASE("Bilingual captions: each track keeps its own style and the live style preview touches the main track only",
          "[playback][captions][bilingual]") {
    Bilingual b;
    b.spanishTrack().style.fill = {1.0, 0.9, 0.2};
    b.spanishTrack().style.fontSize = 0.03;
    CaptionSettings preview;
    render::CaptionStyle explicitStyle;
    explicitStyle.fill = {0.0, 1.0, 0.0};
    preview.style = explicitStyle;
    const auto frames = evaluateCaptions(b.seq, ms(500), preview, &b.state);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].style.fill == std::array<double, 3>{0.0, 1.0, 0.0}); // the main track: the explicit style
    CHECK(frames[1].style.fill == std::array<double, 3>{1.0, 0.9, 0.2}); // the other: its own
    CHECK(frames[1].style.sizePx == Approx(32.4));
}

TEST_CASE("Bilingual captions: the program monitor and export get one caption layer per track on screen",
          "[playback][captions][bilingual]") {
    Bilingual b;
    CaptionSettings settings;
    std::vector<LayerSpec> specs;
    evaluateVideoLayers(b.state, b.seq, ms(500), specs, true, nullptr, &settings);
    REQUIRE(specs.size() == 2);
    REQUIRE(specs[0].caption);
    REQUIRE(specs[1].caption);
    CHECK(specs[0].caption->text == "Hello");
    CHECK(specs[1].caption->text == "Hola");
    CHECK_FALSE(specs[0].clip.isValid());

    b.spanishTrack().showTogether = false;
    specs.clear();
    evaluateVideoLayers(b.state, b.seq, ms(500), specs, true, nullptr, &settings);
    CHECK(specs.size() == 1);
}
