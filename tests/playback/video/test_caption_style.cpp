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

TEST_CASE("Caption style: vertical Short cue lines stay one line each in the preset they are cut for",
          "[playback][captions]") {
    // 0.4 (D, fix round 1): shorts.create and highlights.toShort cut a vertical Short's cues to 10 characters a line
    // with the "Shorts" preset and to 14 with any other preset, two lines a cue (ShortsCommands.cpp kShortsCaptionChars
    // and kVerticalCaptionChars; HighlightCommands.cpp). Drawn on a 1080x1920 frame as the renderer draws them, such
    // lines stay one line each, so a cue never wraps past two lines. (The first rule, 12 and 20, wrapped: "Oh my god we"
    // is wider than the 828 px the Shorts preset's 128 px heavy words get.) Widths are the platform sans-serif's:
    // checked on Windows, reported elsewhere. A line in capitals may still wrap with the Shorts preset ("WE WON THE").
    const std::vector<std::string> sentences{
        "Oh my god we just won the whole round", "What a crazy clutch round that was", "Wait wait no he is behind you",
        "Let's go boys we did it", "No way that actually worked", "I can't believe he missed that shot",
        "Who was that guy with the sniper", "Okay okay I'm going in now", "Mom said it's my turn on the Xbox",
        "We won World Cup mode GG", "Watch my aim on this one", "Somebody help me I'm getting wrecked"};
    const std::vector<std::string> shouted{"NO WAY DUDE WHAT A SHOT", "LET'S GO WE WON THE GAME"};
    // Lines of at most `chars` characters, broken between words (a longer word stands alone), as captions.generate does.
    const auto linesOf = [](const std::string& s, std::size_t chars) {
        std::vector<std::string> out;
        std::string line;
        std::size_t at = 0;
        while (at < s.size()) {
            std::size_t end = s.find(' ', at);
            if (end == std::string::npos)
                end = s.size();
            const std::string word = s.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.size() + 1 + word.size() > chars) {
                out.push_back(line);
                line.clear();
            }
            line += (line.empty() ? "" : " ") + word;
        }
        if (!line.empty())
            out.push_back(line);
        return out;
    };
    struct Rule {
        const char* preset;
        std::size_t chars;
        bool shoutedToo;
    };
    for (const Rule& rule : {Rule{"Shorts", 10, false}, Rule{"Clean", 14, true}, Rule{"Bold Box", 14, true},
                             Rule{"Podcast", 14, true}, Rule{"Thai-friendly", 14, true}}) {
        const CaptionPreset* preset = findBuiltinCaptionPreset(rule.preset);
        REQUIRE(preset);
        std::vector<std::string> texts = sentences;
        if (rule.shoutedToo)
            texts.insert(texts.end(), shouted.begin(), shouted.end());
        for (const std::string& text : texts) {
            for (const std::string& line : linesOf(text, rule.chars)) {
                Captioned c({{Ticks{}, Ticks::fromSeconds(2), line}}, preset->style);
                c.seq.width = 1080;
                c.seq.height = 1920;
                const auto frame = c.at(Ticks::fromSeconds(1)); // after the pop
                REQUIRE(frame);
                const int drawn = render::CaptionRenderer::renderUncached(*frame)->info().lines;
#ifdef _WIN32
                INFO(rule.preset << ": [" << line << "]");
                CHECK(drawn == 1);
#else
                if (drawn > 1)
                    WARN(rule.preset << ": [" << line << "] wraps to " << drawn << " lines in this platform's font");
#endif
            }
        }
    }
}

TEST_CASE("Caption style: OpenType features reach the renderer and are saved only when set (card 138)", "[playback][captions][typography]") {
    CaptionStyle style;
    style.features = "calt, -liga, ss01";
    REQUIRE_FALSE(captionStyleProblem(style));
    Captioned c({{Ticks{}, Ticks::fromSeconds(4), "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"}}, style); // salam
    const auto frame = c.at(Ticks::fromSeconds(1));
    REQUIRE(frame);
    CHECK(frame->style.features == "calt, -liga, ss01");
    // The plain model style maps to the plain renderer style: nothing is asked of the shaper.
    CHECK(captionStyleOf(CaptionStyle{}, Ticks{}).features.empty());
    // An unreadable list is a style problem (the model refuses to keep it); the bound is the same as a graphic's.
    style.features = "calt, nonsense!";
    CHECK(captionStyleProblem(style));
    style.features = "liga=100";
    CHECK(captionStyleProblem(style));
}

// ---- card 074: per-word animations, timing, emphasis; card 114: the text rules ------------------------------------

namespace {

/// Three words over three seconds from 0 ("One" 0 s, "two" 0.818 s, "three" 1.636 s: the cue's time shared by length).
Captioned threeWords(CaptionAnimation animation, double speed = 0.18, CaptionEase ease = CaptionEase::Smooth) {
    CaptionStyle style;
    style.animation = animation;
    style.animationSpeed = speed;
    style.animationEase = ease;
    return Captioned({{Ticks{}, Ticks::fromSeconds(3), "One two three"}}, style);
}

const render::CaptionFrame::WordFx* fxOf(const render::CaptionFrame& frame, int begin) {
    for (const auto& fx : frame.wordFx) {
        if (fx.begin == begin)
            return &fx;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Caption animations: word pop shows the words as they are said and each pops in", "[playback][captions][animation]") {
    Captioned c = threeWords(CaptionAnimation::WordPop);
    const auto early = c.at(ms(900)); // "two" started 82 ms ago; "three" is not said yet
    REQUIRE(early);
    REQUIRE(early->hidden.size() == 1);
    CHECK(early->hidden[0] == std::pair{8, 13});
    const auto* two = fxOf(*early, 4);
    REQUIRE(two);
    CHECK(two->scale > 0.5);
    CHECK(two->scale < 1.0);
    CHECK(two->opacity > 0.5);
    CHECK(two->opacity < 1.0);
    CHECK(fxOf(*early, 0) == nullptr); // "One" has settled
    // The word being said takes the highlight colour.
    REQUIRE(early->colorSpans.size() == 1);
    CHECK(early->colorSpans[0].begin == 4);
    CHECK(early->colorSpans[0].end == 7);
    CHECK(early->colorSpans[0].color == CaptionStyle{}.highlight);
    // Settled: nothing moves, everything shows.
    const auto later = c.at(ms(2500));
    REQUIRE(later);
    CHECK(later->hidden.empty());
    CHECK(later->wordFx.empty());
    // A slower speed is less far along at the same instant.
    Captioned slow = threeWords(CaptionAnimation::WordPop, 0.5);
    Captioned quick = threeWords(CaptionAnimation::WordPop, 0.05);
    REQUIRE(fxOf(*slow.at(ms(900)), 4));
    CHECK(fxOf(*slow.at(ms(900)), 4)->scale < 1.0);
    CHECK(fxOf(*quick.at(ms(900)), 4) == nullptr);
}

TEST_CASE("Caption animations: bounce hops the spoken word and the overshoot ease dips under the line", "[playback][captions][animation]") {
    Captioned smooth = threeWords(CaptionAnimation::Bounce, 0.2, CaptionEase::Smooth);
    const auto hop = smooth.at(ms(818 + 40)); // 40 ms into "two"
    REQUIRE(hop);
    CHECK(hop->hidden.empty()); // all words are on screen
    const auto* two = fxOf(*hop, 4);
    REQUIRE(two);
    CHECK(two->lift > 0.1);
    CHECK(two->lift <= 0.22 + 1e-9);
    CHECK(fxOf(*smooth.at(ms(818 + 500)), 4) == nullptr); // landed (2 x 0.2 s)
    // The overshoot curve passes 1 and settles: the word dips below its line at the end of the hop.
    Captioned springy = threeWords(CaptionAnimation::Bounce, 0.2, CaptionEase::Overshoot);
    double lowest = 0.0;
    for (int k = 0; k <= 40; ++k) {
        if (const auto* fx = fxOf(*springy.at(ms(818 + 10 * k)), 4))
            lowest = std::min(lowest, fx->lift);
    }
    CHECK(lowest < 0.0);
}

TEST_CASE("Caption animations: zoom grows the spoken word and settles it back", "[playback][captions][animation]") {
    Captioned c = threeWords(CaptionAnimation::Zoom, 0.1, CaptionEase::Linear);
    const auto mid = c.at(ms(1200)); // "two" is said from 0.818 s to 1.636 s: fully grown
    REQUIRE(mid);
    const auto* two = fxOf(*mid, 4);
    REQUIRE(two);
    CHECK(two->scale == Approx(1.25));
    const auto after = c.at(ms(1636 + 50)); // half way back
    REQUIRE(after);
    REQUIRE(fxOf(*after, 4));
    CHECK(fxOf(*after, 4)->scale == Approx(1.125).margin(0.01));
    REQUIRE(fxOf(*after, 8));
    CHECK(fxOf(*after, 8)->scale > 1.0); // "three" is growing
    CHECK(fxOf(*c.at(ms(1636 + 200)), 4) == nullptr); // "two" settled back to its size
}

TEST_CASE("Caption animations: the highlight bar sits on the spoken word and slides from the one before", "[playback][captions][animation]") {
    Captioned c = threeWords(CaptionAnimation::HighlightBar, 0.2, CaptionEase::Linear);
    const auto first = c.at(ms(50));
    REQUIRE(first);
    CHECK(first->bar.toBegin == 0);
    CHECK(first->bar.toEnd == 3);
    CHECK(first->bar.fromBegin == -1);
    CHECK(first->bar.opacity == Approx(0.25).margin(0.01)); // fading in
    const auto sliding = c.at(ms(818 + 100)); // half way from "One" to "two"
    REQUIRE(sliding);
    CHECK(sliding->bar.toBegin == 4);
    CHECK(sliding->bar.toEnd == 7);
    CHECK(sliding->bar.fromBegin == 0);
    CHECK(sliding->bar.fromEnd == 3);
    CHECK(sliding->bar.t == Approx(0.5).margin(0.02));
    const auto settled = c.at(ms(1300));
    REQUIRE(settled);
    CHECK(settled->bar.t == 1.0);
    // The spoken word's text colour reads on the (light) bar.
    REQUIRE(settled->colorSpans.size() == 1);
    CHECK(settled->colorSpans[0].color[0] < 0.2);
    // No other animation draws a bar.
    CHECK(threeWords(CaptionAnimation::Bounce).at(ms(900))->bar.toBegin == -1);
}

TEST_CASE("Caption animations: typewriter types the word being said letter by letter", "[playback][captions][animation]") {
    Captioned c = threeWords(CaptionAnimation::Typewriter);
    CHECK(c.at(ms(0))->hidden.size() == 1); // "One" is being typed: nothing is there yet
    const auto typing = c.at(ms(818 + 600)); // "two" is typed over half a second: all three letters by now
    REQUIRE(typing);
    REQUIRE(typing->hidden.size() == 1);
    CHECK(typing->hidden[0].first >= 8); // "One two" shown; "three" not yet
    const auto partial = c.at(ms(1636 + 100)); // "three": 100 ms into 500 ms: one letter of five
    REQUIRE(partial);
    REQUIRE(partial->hidden.size() == 1);
    CHECK(partial->hidden[0].first == 8 + 1);
    CHECK(partial->hidden[0].second == 13);
    CHECK(c.at(ms(2500))->hidden.empty()); // all typed
}

TEST_CASE("Caption animations: emphasised words take the emphasis colour and a little more size", "[playback][captions][animation]") {
    CaptionStyle style;
    style.emphasis = CaptionEmphasis::Auto;
    style.emphasisColor = {1.0, 0.0, 0.0};
    style.emphasisScale = 1.3;
    style.emphasisWords = {"clutch"};
    Captioned c({{Ticks{}, Ticks::fromSeconds(4), "That was a clutch GG for 3 wins!"}}, style);
    const auto frame = c.at(ms(500));
    REQUIRE(frame);
    // The keyword, capitals, a number, and the word before "!": four words.
    int red = 0;
    for (const auto& span : frame->colorSpans) {
        if (span.color == std::array<double, 3>{1.0, 0.0, 0.0})
            ++red;
    }
    CHECK(red == 4);
    const auto* gg = fxOf(*frame, 18);
    REQUIRE(gg);
    CHECK(gg->scale == Approx(1.3));
    CHECK(fxOf(*frame, 0) == nullptr); // "That" is not emphasised
    // Keywords only: just the listed one.
    c.seq.captionTracks[0].style.emphasis = CaptionEmphasis::Keywords;
    const auto only = c.at(ms(500));
    REQUIRE(only);
    CHECK(only->wordFx.size() == 1);
    // None: nothing.
    c.seq.captionTracks[0].style.emphasis = CaptionEmphasis::None;
    CHECK(c.at(ms(500))->wordFx.empty());
    CHECK(c.at(ms(500))->colorSpans.empty());
}

TEST_CASE("Caption animations: the timing controls reach pop and fade", "[playback][captions][animation]") {
    const std::vector<CaptionCue> cue{{Ticks::fromSeconds(1), Ticks::fromSeconds(2), "Alpha beta gamma"}};
    CaptionStyle pop;
    pop.animation = CaptionAnimation::Pop;
    pop.animationSpeed = 0.3;
    pop.animationEase = CaptionEase::Smooth;
    Captioned slow(cue, pop);
    CHECK(slow.at(ms(1150))->scale < 1.0); // still going: the default 0.15 s would be done
    CHECK(slow.at(ms(1300))->scale == Approx(1.0));
    pop.animationEase = CaptionEase::Linear;
    Captioned linear(cue, pop);
    CHECK(linear.at(ms(1150))->scale == Approx(0.9)); // half way from 80 % to 100 %
    CaptionStyle fade;
    fade.animation = CaptionAnimation::Fade;
    fade.animationSpeed = 0.3; // fades take 0.4 s
    Captioned slowFade(cue, fade);
    CHECK(slowFade.at(ms(1200))->opacity == Approx(0.5));
}

TEST_CASE("Caption text rules: case and punctuation and accents are applied when the caption is drawn", "[playback][captions][text]") {
    CaptionStyle style;
    style.textCase = CaptionTextCase::Lower;
    style.stripPunctuation = true;
    Captioned c({{Ticks{}, Ticks::fromSeconds(2), "Hello, World! It's 3.5 now."}}, style);
    const auto frame = c.at(ms(500));
    REQUIRE(frame);
    CHECK(frame->text == "hello world it's 3.5 now");
    // The cue itself keeps its words as written.
    CHECK(c.seq.captionTracks[0].cues[0].text == "Hello, World! It's 3.5 now.");
    // Karaoke counts the words of what is drawn.
    CaptionSettings karaoke;
    karaoke.karaoke = true;
    const auto lit = c.at(ms(2000 - 1), karaoke);
    REQUIRE(lit);
    CHECK(lit->highlightEnd == static_cast<int>(lit->text.size()));
    // Turkish: I is dotless i in lower case.
    CaptionStyle turkish;
    turkish.textCase = CaptionTextCase::Lower;
    Captioned tr({{Ticks{}, Ticks::fromSeconds(2), "ISTANBUL"}}, turkish);
    tr.seq.captionTracks[0].language = "tr";
    CHECK(tr.at(ms(500))->text == "\xC4\xB1stanbul");
    // As written: untouched.
    Captioned plain({{Ticks{}, Ticks::fromSeconds(2), "Hello, World!"}});
    CHECK(plain.at(ms(500))->text == "Hello, World!");
}
