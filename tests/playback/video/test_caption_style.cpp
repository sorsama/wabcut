// C.7 caption styles in evaluation (I.8; docs/internal/c7-caption-handoff.md): the drawn track's own style reaches the renderer
// unless an explicit one overrides it - sizes in 1080-line pixels, the custom position's curves at sequence time -
// overlapping cues stack when the style allows it, speakers' names lead their cues in their colours, karaoke follows
// every stacked cue, and the reserved animations play: pop scales in, fade fades in and out, word by word hides the
// words not spoken yet. Bleeped words stay masked through all of it.

#include <wabcut/model/ProjectState.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::playback;
using Catch::Approx;

namespace {

Ticks ms(std::int64_t v) { return Ticks{v * (kTicksPerSecond / 1000)}; }

struct Captioned {
    ProjectState state;
    Sequence seq;
    SpeakerId alice;

    explicit Captioned(std::vector<CaptionCue> cues, CaptionStyle style = {}) {
        alice = state.ids.allocate<SpeakerId>();
        state.speakers.push_back(Speaker{alice, "Alice", 0x3366FF});
        seq.id = state.ids.allocate<SequenceId>();
        seq.width = 1280;
        seq.height = 720;
        seq.duration = Ticks::fromSeconds(10);
        CaptionTrack track;
        track.id = state.ids.allocate<CaptionTrackId>();
        track.name = "English";
        track.cues = std::move(cues);
        track.style = std::move(style);
        seq.captionTracks.push_back(std::move(track));
    }
    std::optional<render::CaptionFrame> at(Ticks t, const CaptionSettings& settings = {}) const {
        return evaluateCaption(seq, t, settings, &state);
    }
};

} // namespace

TEST_CASE("Caption style: the track's own style, mapped for the renderer; an explicit style overrides it",
          "[playback][captions]") {
    CaptionStyle style;
    style.fontFamily = "Georgia";
    style.fontWeight = 700;
    style.fontSize = 0.05;
    style.fill = {1.0, 0.9, 0.2};
    style.outlineWidth = 0.1;
    style.shadowOpacity = 0.6;
    style.shadowDistance = 0.12;
    style.shadowAngle = 45.0;
    style.shadowSoftness = 0.2;
    style.box = false;
    style.boxPadding = 0.5;
    style.boxRadius = 0.25;
    style.alignment = CaptionAlign::Left;
    style.placement = CaptionPlacement::Custom;
    Curve x;
    x.keys = {Keyframe{Ticks{}, 0.2}, Keyframe{Ticks::fromSeconds(2), 0.6}};
    style.positionX = ParamValue(x);
    style.positionY = ParamValue(0.3);
    Captioned c({{Ticks{}, Ticks::fromSeconds(4), "Hello"}}, style);

    const auto frame = c.at(Ticks::fromSeconds(1));
    REQUIRE(frame);
    const render::CaptionStyle& s = frame->style;
    CHECK(s.fontFamily == "Georgia");
    CHECK(s.weight == 700);
    CHECK(s.sizePx == Approx(54.0)); // 0.05 of a 1080-line frame
    CHECK(s.fill == std::array<double, 3>{1.0, 0.9, 0.2});
    CHECK(s.strokeWidth == Approx(5.4));
    CHECK(s.shadowOpacity == 0.6);
    CHECK(s.shadowDistance == 0.12);
    CHECK(s.shadowAngle == 45.0);
    CHECK(s.shadowSoftness == 0.2);
    CHECK_FALSE(s.box);
    CHECK(s.boxPadding == 0.5);
    CHECK(s.boxRadius == 0.25);
    CHECK(s.align == render::CaptionAlign::Left);
    CHECK(s.position == render::CaptionPosition::Custom);
    CHECK(s.centerX == Approx(0.4)); // the curve, keyed in sequence time, at 1 s
    CHECK(s.centerY == Approx(0.3));
    CHECK(frame->frameWidth == 1280);

    // The default model style is the renderer's default: projects from before C.7 look as before.
    render::CaptionStyle mapped = captionStyleOf(CaptionStyle{}, Ticks{});
    CHECK(mapped.sizePx == Approx(48.0));
    mapped.sizePx = 48.0; // 48 / 1080 * 1080 need not round-trip exactly; the font takes whole pixels
    CHECK(mapped == render::CaptionStyle{});

    // An explicit style (a panel's live preview) overrides the track's.
    CaptionSettings settings;
    settings.style = render::CaptionStyle{};
    settings.style->sizePx = 30.0;
    const auto overridden = c.at(Ticks::fromSeconds(1), settings);
    REQUIRE(overridden);
    CHECK(overridden->style.sizePx == 30.0);
    CHECK(overridden->style.position == render::CaptionPosition::Bottom);
}

TEST_CASE("Caption style: overlapping cues stack, speakers lead in their colours, karaoke follows each cue",
          "[playback][captions]") {
    CaptionStyle style;
    style.allowOverlap = true;
    style.showSpeakers = true;
    Captioned c({{Ticks{}, Ticks::fromSeconds(3), "One two"}, {Ticks::fromSeconds(1), Ticks::fromSeconds(3), "Three four"}},
                style);
    c.seq.captionTracks[0].cues[0].speaker = c.alice;

    const auto first = c.at(ms(500));
    REQUIRE(first);
    CHECK(first->text == "Alice: One two");
    REQUIRE(first->colorSpans.size() == 1);
    CHECK(first->colorSpans[0].begin == 0);
    CHECK(first->colorSpans[0].end == 5);
    CHECK(first->colorSpans[0].color[2] == Approx(1.0));
    const auto both = c.at(ms(1500));
    REQUIRE(both);
    CHECK(both->text == "Alice: One two\nThree four"); // stacked in start order
    const auto second = c.at(ms(3500));
    REQUIRE(second);
    CHECK(second->text == "Three four");

    // Karaoke: each cue's spoken words, after its speaker's name.
    CaptionSettings karaoke;
    karaoke.karaoke = true;
    const auto lit = c.at(ms(2500), karaoke);
    REQUIRE(lit);
    CHECK(lit->highlightEnd == -1);
    REQUIRE(lit->highlights.size() == 2);
    CHECK(lit->highlights[0] == std::pair{7, 14}); // "One two" spoken (estimated: the cue's time shared by length)
    CHECK(lit->highlights[1].first == 15);         // "Three" of the second cue

    // Without overlap, only the last cue that started is drawn, as before.
    Captioned plain({{Ticks{}, Ticks::fromSeconds(3), "One two"}, {Ticks::fromSeconds(4), Ticks::fromSeconds(1), "Five"}});
    const auto one = plain.at(ms(1000), karaoke);
    REQUIRE(one);
    CHECK(one->text == "One two");
    CHECK(one->highlightEnd == 3); // "One" ("two" starts at 1.5 s)
    CHECK(one->colorSpans.empty());
}

TEST_CASE("Caption style: pop, fade and word-by-word animations", "[playback][captions]") {
    const std::vector<CaptionCue> cue{{Ticks::fromSeconds(1), Ticks::fromSeconds(2), "Alpha beta gamma"}};
    SECTION("fade in and out over 0.2 s") {
        CaptionStyle style;
        style.animation = CaptionAnimation::Fade;
        Captioned c(cue, style);
        CHECK(c.at(Ticks::fromSeconds(1))->opacity == Approx(0.0));
        CHECK(c.at(ms(1100))->opacity == Approx(0.5));
        CHECK(c.at(ms(2000))->opacity == Approx(1.0));
        CHECK(c.at(ms(2950))->opacity == Approx(0.25));
        CHECK(c.at(ms(2000))->scale == 1.0);
    }
    SECTION("pop: scaled in from 80 % over 0.15 s, with a slight overshoot") {
        CaptionStyle style;
        style.animation = CaptionAnimation::Pop;
        Captioned c(cue, style);
        const auto start = c.at(Ticks::fromSeconds(1));
        CHECK(start->scale == Approx(0.8));
        CHECK(start->opacity == Approx(0.0));
        double peak = 0.0;
        for (int k = 0; k <= 15; ++k) {
            peak = std::max(peak, c.at(ms(1000 + 10 * k))->scale);
        }
        CHECK(peak > 1.0);  // the overshoot
        CHECK(peak < 1.06);
        CHECK(c.at(ms(1150))->scale == Approx(1.0));
        CHECK(c.at(ms(1500))->scale == 1.0);
        CHECK(c.at(ms(1500))->opacity == 1.0);
    }
    SECTION("word by word: the words not spoken yet are hidden, the text (and its layout) stays whole") {
        CaptionStyle style;
        style.animation = CaptionAnimation::WordByWord;
        Captioned c(cue, style);
        const auto first = c.at(Ticks::fromSeconds(1));
        REQUIRE(first);
        CHECK(first->text == "Alpha beta gamma");
        REQUIRE(first->hidden.size() == 1);
        CHECK(first->hidden[0] == std::pair{6, 16}); // "beta gamma" not spoken yet
        const auto later = c.at(ms(2900));
        REQUIRE(later);
        CHECK(later->hidden.empty());
        CHECK(later->opacity == 1.0);
    }
}
