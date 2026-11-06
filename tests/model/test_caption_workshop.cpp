// The caption workshop model (C.7): cue operations and their rules, styles and presets (saved, migrated), the
// line-break seam (Thai through a fake breaker), captions following every kind of edit (with undo), and speakers
// and transcript edits.

#include "TimelineFixture.h"

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <string>
#include <tuple>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

struct Cue {
    int start;
    int end;
    std::string text;
};

/// `state` with a caption track holding `cues` (frames) on `tl`'s sequence.
ProjectState withCaptions(const ProjectState& state, SequenceId seq, const std::vector<Cue>& cues,
                          CaptionStyle style = {}) {
    CaptionTrack track;
    track.name = "Captions";
    track.language = "en";
    track.style = std::move(style);
    for (const Cue& c : cues) {
        track.cues.push_back(CaptionCue{f(c.start), f(c.end - c.start), c.text});
    }
    return ok(edit::setCaptionTrack(state, seq, std::move(track))).state;
}

const CaptionTrack& captionsOf(const ProjectState& state, SequenceId seq) {
    return state.findSequence(seq)->captionTracks.front();
}

/// The first caption track's cues as "start-end text" in frames (fractions of a frame shown as "~").
std::string cues(const ProjectState& state, SequenceId seq) {
    std::string out;
    for (const CaptionCue& c : captionsOf(state, seq).cues) {
        const auto frame = [](Ticks t) {
            const std::int64_t n = ticksToFrames(t, kRate);
            return std::to_string(n) + (f(n) == t ? "" : "~");
        };
        out += (out.empty() ? "" : " | ") + frame(c.start) + "-" + frame(c.end()) + " " + c.text;
    }
    return out;
}

std::string errorOf(const edit::EditOutcome& outcome) { return outcome ? std::string() : outcome.error().message; }

template <class T>
std::string errorOf(const std::expected<T, Error>& outcome) {
    return outcome ? std::string() : outcome.error().message;
}

CaptionTrackId trackId(const ProjectState& state, SequenceId seq) { return captionsOf(state, seq).id; }

/// A breaker that only knows the offsets it is given (stands in for ICU / Qt's line breaking).
class FakeBreaker final : public LineBreaker {
public:
    explicit FakeBreaker(std::vector<std::string> words) : m_words(std::move(words)) {}
    std::vector<std::size_t> breakOpportunities(std::string_view text, std::string_view) const override {
        std::vector<std::size_t> out;
        std::size_t at = 0;
        for (const std::string& w : m_words) {
            const std::size_t found = text.find(w, at);
            if (found == std::string_view::npos) {
                break;
            }
            at = found + w.size();
            if (at < text.size()) {
                out.push_back(at);
            }
        }
        return out;
    }

private:
    std::vector<std::string> m_words;
};

const std::string kSawasdee = "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5"; // สวัสดี
const std::string kKrap = "\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A";                                // ครับ
const std::string kWanNi = "\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\x99\xE0\xB8\x99\xE0\xB8\xB5\xE0\xB9\x89";       // วันนี้
const std::string kAkat = "\xE0\xB8\xAD\xE0\xB8\xB2\xE0\xB8\x81\xE0\xB8\xB2\xE0\xB8\xA8";                    // อากาศ
const std::string kDiMak = "\xE0\xB8\x94\xE0\xB8\xB5\xE0\xB8\xA1\xE0\xB8\xB2\xE0\xB8\x81";                   // ดีมาก

} // namespace

// --- Cues -----------------------------------------------------------------------------------------------------

TEST_CASE("Caption cues: add, update, remove, with the cue rules", "[model][captions]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    ProjectState s = withCaptions(tl.state, tl.seq, {{0, 4, "one"}, {10, 14, "three"}});
    const CaptionTrackId id = trackId(s, tl.seq);
    s = ok(edit::addCaptionCue(s, tl.seq, id, CaptionCue{f(5), f(3), "two"})).state;
    CHECK(cues(s, tl.seq) == "0-4 one | 5-8 two | 10-14 three");
    CHECK(errorOf(edit::addCaptionCue(s, tl.seq, id, CaptionCue{f(7), f(2), "overlap"})).find("overlap") != std::string::npos);
    CHECK_FALSE(edit::addCaptionCue(s, tl.seq, id, CaptionCue{f(20), f(0), "empty"}));
    CHECK_FALSE(edit::addCaptionCue(s, tl.seq, id, CaptionCue{f(20), f(1), std::string(kMaxCaptionTextBytes + 1, 'x')}));
    CHECK_FALSE(edit::addCaptionCue(s, tl.seq, id, CaptionCue{f(20), f(1), "bad \xFF utf-8"}));
    CHECK(errorOf(edit::addCaptionCue(s, tl.seq, id, CaptionCue{f(20), f(1), "who", SpeakerId{999}})).find("speaker") !=
          std::string::npos);

    edit::CaptionCueChange change;
    change.text = "TWO";
    change.start = f(15);
    change.end = f(18); // moves after "three"
    s = ok(edit::updateCaptionCue(s, tl.seq, id, 1, change)).state;
    CHECK(cues(s, tl.seq) == "0-4 one | 10-14 three | 15-18 TWO");
    change = {};
    change.end = f(15);
    change.start = f(16);
    CHECK_FALSE(edit::updateCaptionCue(s, tl.seq, id, 2, change)); // ends before it starts
    CHECK(errorOf(edit::updateCaptionCue(s, tl.seq, id, 7, {})).find("no cue 7") != std::string::npos);
    s = ok(edit::removeCaptionCue(s, tl.seq, id, 0)).state;
    CHECK(cues(s, tl.seq) == "10-14 three | 15-18 TWO");

    // A style that allows overlaps lets cues stack.
    CaptionStyle stacked;
    stacked.allowOverlap = true;
    s = ok(edit::setCaptionStyle(s, tl.seq, id, stacked)).state;
    s = ok(edit::addCaptionCue(s, tl.seq, id, CaptionCue{f(11), f(2), "over"})).state;
    CHECK(cues(s, tl.seq) == "10-14 three | 11-13 over | 15-18 TWO");
    CHECK_FALSE(edit::setCaptionStyle(s, tl.seq, id, CaptionStyle{})); // they overlap now
}

TEST_CASE("Caption cues: split, merge and shift", "[model][captions]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    ProjectState s = withCaptions(tl.state, tl.seq, {{0, 10, "Hello there,\ngeneral Kenobi"}, {12, 16, "Bold"}, {20, 24, "one"}});
    const CaptionTrackId id = trackId(s, tl.seq);

    SECTION("split at the opportunity nearest the time's share, or at a byte offset") {
        const auto r = ok(edit::splitCaptionCue(s, tl.seq, id, 0, f(5), std::nullopt, simpleLineBreaker()));
        CHECK(cues(r.state, tl.seq) == "0-5 Hello there, | 5-10 general Kenobi | 12-16 Bold | 20-24 one");
        const auto offset = ok(edit::splitCaptionCue(s, tl.seq, id, 0, f(2), std::size_t{6}, simpleLineBreaker()));
        CHECK(cues(offset.state, tl.seq) == "0-2 Hello | 2-10 there,\ngeneral Kenobi | 12-16 Bold | 20-24 one");
        CHECK(errorOf(edit::splitCaptionCue(s, tl.seq, id, 1, f(14), std::nullopt, simpleLineBreaker()))
                  .find("nowhere to break") != std::string::npos);
        CHECK_FALSE(edit::splitCaptionCue(s, tl.seq, id, 0, f(10), std::nullopt, simpleLineBreaker())); // not inside
    }
    SECTION("merge joins and re-wraps") {
        const auto r = ok(edit::mergeCaptionCues(s, tl.seq, id, 1, 2, simpleLineBreaker(), 6, 2));
        CHECK(cues(r.state, tl.seq) == "0-10 Hello there,\ngeneral Kenobi | 12-24 Bold\none");
        CHECK_FALSE(edit::mergeCaptionCues(s, tl.seq, id, 2, 2, simpleLineBreaker()));
        CHECK_FALSE(edit::mergeCaptionCues(s, tl.seq, id, 0, 1, simpleLineBreaker()));
    }
    SECTION("shift the cues starting in a range") {
        const auto r = ok(edit::shiftCaptionCues(s, tl.seq, id, TimeRange::fromStartEnd(f(10), f(30)), f(-1)));
        CHECK(cues(r.state, tl.seq) == "0-10 Hello there,\ngeneral Kenobi | 11-15 Bold | 19-23 one");
        CHECK(r.appliedDelta == f(-1));
        CHECK(errorOf(edit::shiftCaptionCues(s, tl.seq, id, TimeRange::fromStartEnd(f(12), f(13)), f(6))).find("overlap") !=
              std::string::npos);
        CHECK_FALSE(edit::shiftCaptionCues(s, tl.seq, id, TimeRange::fromStartEnd(f(0), f(1)), f(-1))); // before 0
        CHECK_FALSE(edit::shiftCaptionCues(s, tl.seq, id, TimeRange::fromStartEnd(f(25), f(30)), f(1))); // none there
    }
}

// --- Line breaking ----------------------------------------------------------------------------------------------

TEST_CASE("Caption line breaking: balanced lines, and Thai through the line-break seam", "[model][captions][thai]") {
    CHECK(breakCaptionLines("Hello there, general Kenobi", "en", 16, 2, simpleLineBreaker()) ==
          std::vector<std::string>{"Hello there,", "general Kenobi"});
    CHECK(breakCaptionLines("A short one", "en", 42, 2, simpleLineBreaker()) == std::vector<std::string>{"A short one"});
    CHECK_FALSE(breakCaptionLines("one two three four five six", "en", 5, 2, simpleLineBreaker()));
    CHECK(captionDisplayLength("\xE0\xB8\x97\xE0\xB8\xB5\xE0\xB9\x88") == 1); // ที่: a consonant with two marks
    CHECK((isUnspacedLanguage("th") && isUnspacedLanguage("ja-JP") && !isUnspacedLanguage("en")));
    CHECK(flattenCaptionText("a\nb  c ", "en") == "a b c");
    CHECK(flattenCaptionText(kSawasdee + "\n" + kKrap, "th") == kSawasdee + kKrap);

    // Thai has no spaces: the simple breaker cannot break it; a real (here: fake) breaker knows the words.
    const std::string thai = kSawasdee + kKrap + kWanNi + kAkat + kDiMak;
    CHECK(breakCaptionLines(thai, "th", 12, 2, simpleLineBreaker()) == std::vector<std::string>{thai});
    const FakeBreaker words({kSawasdee, kKrap, kWanNi, kAkat, kDiMak});
    // 4 + 3 + 3 cells, then 5 + 4: the most balanced pair of lines (the vowel and tone marks take no cell).
    CHECK(breakCaptionLines(thai, "th", 12, 2, words) ==
          std::vector<std::string>{kSawasdee + kKrap + kWanNi, kAkat + kDiMak});
    CHECK(wrapCaptionText(thai, "th", 12, 2, words) == kSawasdee + kKrap + kWanNi + "\n" + kAkat + kDiMak);

    // Splitting a Thai cue goes through the same seam.
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    ProjectState s = withCaptions(tl.state, tl.seq, {{0, 10, thai}});
    s = ok(edit::setCaptionTrackInfo(s, tl.seq, trackId(s, tl.seq), {std::nullopt, std::string("th"), std::nullopt})).state;
    CHECK_FALSE(edit::splitCaptionCue(s, tl.seq, trackId(s, tl.seq), 0, f(4), std::nullopt, simpleLineBreaker()));
    const auto split = ok(edit::splitCaptionCue(s, tl.seq, trackId(s, tl.seq), 0, f(4), std::nullopt, words));
    CHECK(captionsOf(split.state, tl.seq).cues[0].text == kSawasdee + kKrap);
}

// --- Styles and presets -----------------------------------------------------------------------------------------

TEST_CASE("Caption styles: the default is today's look; presets built in and saved", "[model][captions][style]") {
    const CaptionStyle look;
    CHECK(look.fontSize == 48.0 / 1080.0);
    CHECK(look.fontWeight == 600);
    CHECK(look.fill == CaptionRgb{1.0, 1.0, 1.0});
    CHECK(look.box);
    CHECK(look.boxOpacity == 0.78);
    CHECK(look.placement == CaptionPlacement::Bottom);
    CHECK(look.maxWidth == 0.8);
    CHECK_FALSE(captionStyleProblem(look));

    std::vector<std::string> names;
    for (const CaptionPreset& p : builtinCaptionPresets()) {
        names.push_back(p.name);
        CHECK_FALSE(captionStyleProblem(p.style));
    }
    // The five that always were there, then the animated gallery (see "the animated gallery" test below).
    REQUIRE(names.size() >= 5);
    CHECK(std::vector<std::string>(names.begin(), names.begin() + 5) ==
          std::vector<std::string>{"Clean", "Bold Box", "Podcast", "Shorts", "Thai-friendly"});
    CHECK(findBuiltinCaptionPreset("bold box"));
    CHECK(findBuiltinCaptionPreset("Thai-friendly")->style.fontFamily == "Noto Sans Thai");

    CaptionStyle bad;
    bad.fontSize = 2.0;
    CHECK(captionStyleProblem(bad));
    bad = {};
    bad.fill = {1.5, 0.0, 0.0};
    CHECK(captionStyleProblem(bad));
    bad = {};
    bad.positionY = ParamValue(Curve{{Keyframe{f(0), 0.5}, Keyframe{f(10), 1.5}}});
    CHECK(captionStyleProblem(bad));

    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    CaptionStyle mine;
    mine.fontWeight = 700;
    auto saved = edit::saveCaptionPreset(tl.state, {"Channel", mine});
    REQUIRE(saved);
    CHECK(edit::captionPresetStyle(*saved, "channel")->fontWeight == 700);
    CHECK(edit::captionPresetStyle(*saved, "Podcast")->showSpeakers);
    mine.fontWeight = 800;
    saved = edit::saveCaptionPreset(*saved, {"CHANNEL", mine}); // the same name replaces it
    REQUIRE(saved);
    CHECK(saved->captionPresets.size() == 1);
    CHECK(saved->captionPresets[0].style.fontWeight == 800);
    CHECK(errorOf(edit::saveCaptionPreset(*saved, {"Clean", mine})).find("built-in") != std::string::npos);
    CHECK_FALSE(edit::saveCaptionPreset(*saved, {"", mine}));
    CHECK(errorOf(edit::removeCaptionPreset(*saved, "Shorts")).find("built-in") != std::string::npos);
    CHECK_FALSE(edit::removeCaptionPreset(*saved, "nope"));
    CHECK(edit::removeCaptionPreset(*saved, "channel")->captionPresets.empty());
}

TEST_CASE("Caption styles, speakers and transcript edits are saved; older projects get today's look",
          "[model][captions][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    auto speaker = edit::addSpeaker(tl.state, "Host");
    REQUIRE(speaker);
    ProjectState s = speaker->state;
    CaptionStyle style;
    style.placement = CaptionPlacement::Custom;
    style.positionY = ParamValue(Curve{{Keyframe{f(0), 0.2}, Keyframe{f(10), 0.8, Interpolation::EaseInOut}}});
    style.showSpeakers = true;
    style.highlight = {0.0, 1.0, 0.5};
    style.animation = CaptionAnimation::WordByWord;
    s = withCaptions(s, tl.seq, {{0, 4, "hello"}}, style);
    edit::CaptionCueChange who;
    who.speaker = speaker->id;
    s = ok(edit::updateCaptionCue(s, tl.seq, trackId(s, tl.seq), 0, who)).state;
    s = *edit::assignSpeaker(s, tl.media('A'), 0, TimeRange{f(1), f(2)}, speaker->id);
    s = *edit::correctTranscript(s, tl.media('A'), 0, WordCorrection{TimeRange{f(3), f(1)}, "teh", "the"});
    s = *edit::saveCaptionPreset(s, {"Mine", style});

    const auto json = io::toJson(s);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == s);
    requireValid(*loaded);

    // Schema 8 had no styles (nor speakers): its caption tracks load with the look they had.
    const std::string current = "\"schemaVersion\": " + std::to_string(io::kSchemaVersion);
    std::string old = *io::toJson(withCaptions(tl.state, tl.seq, {{0, 4, "hello"}}));
    old.replace(old.find(current), current.size(), "\"schemaVersion\": 8");
    old = std::regex_replace(old, std::regex(R"(,\s*"style": \{[^{}]*(\{[^{}]*\}[^{}]*)*\})"), "");
    REQUIRE(old.find("\"style\"") == std::string::npos);
    const auto migrated = io::fromJson(old);
    INFO((migrated ? std::string() : migrated.error().message));
    REQUIRE(migrated);
    CHECK(captionsOf(*migrated, tl.seq).style == CaptionStyle{});
    CHECK(captionsOf(*migrated, tl.seq).cues.size() == 1);

    // Untrusted files: out-of-range styles and dangling speakers are refused.
    std::string broken = *json;
    broken.replace(broken.find("\"fontWeight\": 600"), 17, "\"fontWeight\": 5000");
    CHECK_FALSE(io::fromJson(broken));
}

// --- Captions follow edits --------------------------------------------------------------------------------------

TEST_CASE("Captions follow ripples: insert, extract, ripple delete, gaps, Q/W, ripple trim, insert moves",
          "[model][captions][follow]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb..........|
        A1 tgt |AaaaaaaaaaBbbbbbbbbb..........|
    )");
    tl.linkMatching();
    const ProjectState s = withCaptions(tl.state, tl.seq, {{0, 4, "one"}, {5, 12, "two"}, {14, 18, "three"}});

    SECTION("an insert moves later cues and cuts through the one it lands in") {
        edit::SourceClip src;
        src.media = tl.media('S');
        src.sourceRange = {f(0), f(3)};
        src.patches = {{{StreamKind::Video, 0}, tl.track("V1")}, {{StreamKind::Audio, 0}, tl.track("A1")}};
        const auto r = ok(edit::insertEdit(s, tl.seq, src, f(6)));
        CHECK(cues(r.state, tl.seq) == "0-4 one | 5-6 two | 9-15 two | 17-21 three");
        REQUIRE(r.changes.sequences.size() == 1);
        CHECK(r.changes.sequences[0].captionsChanged);
        // An overwrite changes no timing: the captions stay.
        CHECK(cues(ok(edit::overwriteEdit(s, tl.seq, src, f(6))).state, tl.seq) == cues(s, tl.seq));
    }
    SECTION("extract (and transcript deletes, which extract) take the cues' time out") {
        ProjectState marked = ok(edit::markIn(s, tl.seq, f(2))).state;
        marked = ok(edit::markOut(marked, tl.seq, f(7))).state;
        const auto r = ok(edit::extract(marked, tl.seq));
        CHECK(cues(r.state, tl.seq) == "0-2 one | 2-6 two | 8-12 three");
        CHECK(cues(ok(edit::lift(marked, tl.seq)).state, tl.seq) == cues(s, tl.seq)); // a lift leaves a gap
    }
    SECTION("ripple delete of clips removes the cues inside and pulls later ones back") {
        const ClipId b[] = {tl.clip("V1@10")};
        const auto r = ok(edit::rippleDeleteClips(s, tl.seq, b));
        CHECK(cues(r.state, tl.seq) == "0-4 one | 5-10 two");
        CHECK(cues(ok(edit::deleteClips(s, tl.seq, b)).state, tl.seq) == cues(s, tl.seq)); // a delete does not
    }
    SECTION("W ripple-trims to the playhead") {
        const auto r = ok(edit::rippleTrimNextEditToPlayhead(s, tl.seq, f(6)));
        CHECK(cues(r.state, tl.seq) == "0-4 one | 5-8 two | 10-14 three");
    }
    SECTION("ripple trim: shorter takes time out, longer cuts in") {
        const auto shorter = ok(edit::rippleTrim(s, tl.seq, tl.clip("V1@0"), edit::Edge::Tail, f(-2)));
        CHECK(cues(shorter.state, tl.seq) == "0-4 one | 5-10 two | 12-16 three");
        const auto longer = ok(edit::rippleTrim(s, tl.seq, tl.clip("V1@0"), edit::Edge::Tail, f(3)));
        CHECK(cues(longer.state, tl.seq) == "0-4 one | 5-10 two | 13-15 two | 17-21 three");
        const auto head = ok(edit::rippleTrim(s, tl.seq, tl.clip("V1@10"), edit::Edge::Head, f(2)));
        CHECK(cues(head.state, tl.seq) == "0-4 one | 5-10 two | 12-16 three");
    }
    SECTION("a move in insert mode opens room for the captions too; a razor changes nothing") {
        const ClipId b[] = {tl.clip("V1@10")};
        const auto moved = ok(edit::moveClips(s, tl.seq, b, f(-10), {}, edit::MoveMode::Insert));
        CHECK(cues(moved.state, tl.seq) == "10-14 one | 15-22 two | 24-28 three");
        const TrackId all[] = {tl.track("V1"), tl.track("A1")};
        CHECK(cues(ok(edit::addEdit(s, tl.seq, f(6), all)).state, tl.seq) == cues(s, tl.seq));
    }
    SECTION("undo restores the captions with the clips") {
        EditSession session(s);
        REQUIRE(session.apply("Extract", [&](const ProjectState& in) {
            ProjectState marked = ok(edit::markIn(in, tl.seq, f(2))).state;
            marked = ok(edit::markOut(marked, tl.seq, f(7))).state;
            return edit::extract(marked, tl.seq);
        }));
        CHECK(cues(*session.snapshot(), tl.seq) == "0-2 one | 2-6 two | 8-12 three");
        session.undo();
        CHECK(*session.snapshot() == s);
    }
}

TEST_CASE("Captions follow gaps closed and frame-hold segments", "[model][captions][follow]") {
    Timeline tl(R"(
        V1 tgt |Aaaa....Bbbb|
        A1 tgt |Aaaa....Bbbb|
    )");
    const ProjectState s = withCaptions(tl.state, tl.seq, {{0, 3, "a"}, {5, 7, "gap"}, {9, 11, "b"}});
    CHECK(cues(ok(edit::rippleDeleteGap(s, tl.seq, tl.track("V1"), f(5))).state, tl.seq) == "0-3 a | 5-7 b");
    CHECK(cues(ok(edit::frameHold(s, tl.seq, tl.clip("V1@0"), f(2), f(3))).state, tl.seq) ==
          "0-2 a | 5-6 a | 8-10 gap | 12-14 b");
}

TEST_CASE("Captions follow the speech of retimed audio clips", "[model][captions][follow]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb|
        A1 tgt |AaaaaaaaaaBbbbbbbbbb|
    )");
    tl.linkMatching();
    const ProjectState s = withCaptions(tl.state, tl.seq, {{0, 4, "one"}, {6, 12, "two"}, {14, 18, "three"}});
    const ClipId a[] = {tl.clip("A1@0")};
    edit::SpeedChange twice;
    twice.speed = Speed{2, 1};

    SECTION("Speed/Duration with ripple scales the cues over the clip and moves the rest") {
        twice.ripple = true;
        const auto r = ok(edit::setSpeed(s, tl.seq, a, twice));
        CHECK(cues(r.state, tl.seq) == "0-2 one | 3-7 two | 9-13 three");
    }
    SECTION("without ripple only the cues over the clip scale") {
        const auto r = ok(edit::setSpeed(s, tl.seq, a, twice));
        CHECK(cues(r.state, tl.seq) == "0-2 one | 3-12 two | 14-18 three");
    }
    SECTION("a video-only retime moves captions like a sync-locked track") {
        Timeline video(R"(
            V1 tgt |AaaaaaaaaaBbbbbbbbbb|
        )");
        const ProjectState v = withCaptions(video.state, video.seq, {{6, 12, "two"}});
        const ClipId va[] = {video.clip("V1@0")};
        twice.ripple = true;
        CHECK(cues(ok(edit::setSpeed(v, video.seq, va, twice)).state, video.seq) == "5-7 two");
    }
    SECTION("time remapping inside an audio clip moves the cues after the change") {
        Timeline audio(R"(
            A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        )");
        ProjectState r = withCaptions(audio.state, audio.seq, {{2, 4, "a"}, {6, 10, "b"}, {15, 19, "c"}});
        const ClipId clip = audio.clip("A1@0");
        r = ok(edit::setTimeRemapEnabled(r, audio.seq, clip, true)).state;
        const auto held = ok(edit::addTimeRemapHold(r, audio.seq, clip, f(5), f(4)));
        CHECK(cues(held.state, audio.seq) == "2-4 a | 10-14 b | 19-20 c");
        // Without a clip on an audio track (video remaps) the captions stay.
        Timeline pic(R"(
            V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        )");
        ProjectState p = withCaptions(pic.state, pic.seq, {{6, 10, "b"}});
        p = ok(edit::setTimeRemapEnabled(p, pic.seq, pic.clip("V1@0"), true)).state;
        CHECK(cues(ok(edit::addTimeRemapHold(p, pic.seq, pic.clip("V1@0"), f(5), f(4))).state, pic.seq) == "6-10 b");
    }
}

// --- Speakers and transcript edits ------------------------------------------------------------------------------

TEST_CASE("Speakers: add, rename, labels, assign and merge", "[model][captions][speakers]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    const MediaId a = tl.media('A');
    auto host = edit::addSpeaker(tl.state, "Host");
    REQUIRE(host);
    CHECK(host->state.speakers.front().color == defaultSpeakerColor(0));
    CHECK_FALSE(edit::addSpeaker(tl.state, ""));
    CHECK_FALSE(edit::addSpeaker(tl.state, "two\nlines"));
    CHECK_FALSE(edit::addSpeaker(tl.state, "Red", 0x1000000));

    const std::int32_t labels[] = {0, 1, 1};
    auto registered = edit::registerSpeakerLabels(host->state, a, 0, labels);
    REQUIRE(registered);
    REQUIRE(registered->speakers.size() == 3);
    CHECK(registered->speakers[1].name == "Speaker 2");
    const TranscriptEdits* edits = registered->findTranscriptEdits(a, 0);
    REQUIRE(edits);
    const SpeakerId guest = edits->speakerOfLabel(1);
    CHECK(guest == registered->speakers[2].id);
    CHECK(edit::registerSpeakerLabels(*registered, a, 0, labels)->speakers.size() == 3); // nothing new
    CHECK_FALSE(edit::registerSpeakerLabels(*registered, a, 7, labels));                 // no such stream

    auto renamed = edit::renameSpeaker(*registered, guest, std::string("Dr. Somchai"), 0x27AE60u);
    REQUIRE(renamed);
    CHECK(renamed->findSpeaker(guest)->name == "Dr. Somchai");
    CHECK_FALSE(edit::renameSpeaker(*registered, SpeakerId{999}, std::string("x"), std::nullopt));

    // Assigned spans win and cut what was there.
    auto assigned = edit::assignSpeaker(*renamed, a, 0, TimeRange{f(0), f(10)}, host->id);
    assigned = edit::assignSpeaker(*assigned, a, 0, TimeRange{f(4), f(2)}, guest);
    REQUIRE(assigned);
    const TranscriptEdits& e = *assigned->findTranscriptEdits(a, 0);
    REQUIRE(e.speakerSpans.size() == 3);
    CHECK(e.speakerAt(f(3)) == host->id);
    CHECK(e.speakerAt(f(5)) == guest);
    CHECK(e.speakerAt(f(7)) == host->id);
    CHECK(edit::assignSpeaker(*assigned, a, 0, TimeRange{f(0), f(10)}, SpeakerId{})->findTranscriptEdits(a, 0)->speakerSpans.empty());

    // Merging moves every reference, captions included.
    ProjectState s = withCaptions(*assigned, tl.seq, {{0, 4, "hi"}});
    edit::CaptionCueChange who;
    who.speaker = guest;
    s = ok(edit::updateCaptionCue(s, tl.seq, trackId(s, tl.seq), 0, who)).state;
    auto merged = edit::mergeSpeakers(s, guest, host->id);
    REQUIRE(merged);
    requireValid(*merged);
    CHECK_FALSE(merged->findSpeaker(guest));
    CHECK(captionsOf(*merged, tl.seq).cues[0].speaker == host->id);
    CHECK(merged->findTranscriptEdits(a, 0)->speakerOfLabel(1) == host->id);
    CHECK(merged->findTranscriptEdits(a, 0)->speakerAt(f(5)) == host->id);
    CHECK_FALSE(edit::mergeSpeakers(s, guest, guest));
    const ChangeSet changes = diff(s, *merged);
    CHECK(changes.speakersChanged);
    CHECK(changes.transcriptEditsChanged);
}

TEST_CASE("Transcript corrections replace the ones they overlap", "[model][captions][corrections]") {
    Timeline tl(R"(
        A1 tgt |Aaaaaaaaaa|
    )");
    const MediaId a = tl.media('A');
    auto s = edit::correctTranscript(tl.state, a, 0, WordCorrection{TimeRange{f(1), f(2)}, "teh", "the"});
    s = edit::correctTranscript(*s, a, 0, WordCorrection{TimeRange{f(5), f(1)}, "wabkut", "Wabcut"});
    s = edit::correctTranscript(*s, a, 0, WordCorrection{TimeRange{f(2), f(4)}, "the cat", "the bat"});
    REQUIRE(s);
    const TranscriptEdits& e = *s->findTranscriptEdits(a, 0);
    REQUIRE(e.corrections.size() == 1); // the third covered both
    CHECK(e.corrections[0].text == "the bat");
    CHECK_FALSE(edit::correctTranscript(tl.state, a, 0, WordCorrection{TimeRange{f(1), f(0)}, "x", "y"}));
    CHECK_FALSE(edit::correctTranscript(tl.state, a, 0, WordCorrection{TimeRange{f(1), f(1)}, "x", "\xFF"}));
    CHECK_FALSE(edit::correctTranscript(tl.state, MediaId{999}, 0, WordCorrection{TimeRange{f(1), f(1)}, "x", "y"}));
}

// 0.5 P0 fix round (stream A, QA #11): Add Edit cuts every overlapping cue under the playhead; each split keeps the
// cues in start order where overlaps are allowed.
TEST_CASE("Caption cues: a split on a track with overlaps keeps the cues in start order", "[model][captions]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    CaptionStyle stacked;
    stacked.allowOverlap = true;
    const ProjectState s = withCaptions(tl.state, tl.seq, {{0, 10, "one two"}, {4, 14, "three four"}, {20, 24, "five"}}, stacked);
    const CaptionTrackId id = trackId(s, tl.seq);
    // The earlier cue cut inside the later one: its second half goes after the later one.
    const auto r = ok(edit::splitCaptionCue(s, tl.seq, id, 0, f(6), std::nullopt, simpleLineBreaker()));
    CHECK(cues(r.state, tl.seq) == "0-6 one | 4-14 three four | 6-10 two | 20-24 five");
    // Both cut at one time (the later first): every half in start order.
    const auto later = ok(edit::splitCaptionCue(s, tl.seq, id, 1, f(6), std::nullopt, simpleLineBreaker()));
    const auto both = ok(edit::splitCaptionCue(later.state, tl.seq, id, 0, f(6), std::nullopt, simpleLineBreaker()));
    CHECK(cues(both.state, tl.seq) == "0-6 one | 4-6 three | 6-14 four | 6-10 two | 20-24 five");
}

// --- 0.7: the animated gallery, timing, emphasis and text rules (cards 074 and 114) --------------------------------------------

TEST_CASE("Caption styles: the animated gallery and timing and emphasis and text rules are valid and named and saved",
          "[model][captions][style][animation]") {
    // The names projects save: new values are only ever appended.
    for (const auto& [animation, name] : {std::pair{CaptionAnimation::None, "none"}, {CaptionAnimation::Pop, "pop"},
                                           {CaptionAnimation::Fade, "fade"}, {CaptionAnimation::WordByWord, "wordByWord"},
                                           {CaptionAnimation::WordPop, "wordPop"}, {CaptionAnimation::Bounce, "bounce"},
                                           {CaptionAnimation::HighlightBar, "highlightBar"},
                                           {CaptionAnimation::Typewriter, "typewriter"}, {CaptionAnimation::Zoom, "zoom"}}) {
        INFO(name);
        CHECK(toString(animation) == name);
        CHECK(captionAnimationFromString(name) == animation);
    }
    CHECK_FALSE(captionAnimationFromString("spin"));
    CHECK(captionEaseFromString("overshoot") == CaptionEase::Overshoot);
    CHECK(captionEmphasisFromString("auto") == CaptionEmphasis::Auto);
    CHECK(captionTextCaseFromString("upper") == CaptionTextCase::Upper);
    CHECK(toString(CaptionTextCase::AsWritten) == "asWritten");
    CHECK_FALSE(captionTextCaseFromString("title"));

    // The defaults are today's look: no new effect unless asked for.
    const CaptionStyle look;
    CHECK(look.animationSpeed == 0.15);
    CHECK(look.animationEase == CaptionEase::Overshoot);
    CHECK(look.emphasis == CaptionEmphasis::None);
    CHECK(look.emphasisWords.empty());
    CHECK(look.textCase == CaptionTextCase::AsWritten);
    CHECK_FALSE(look.stripPunctuation);
    CHECK_FALSE(look.stripAccents);

    // The gallery after the five that always were there.
    std::vector<std::string> names;
    for (const CaptionPreset& p : builtinCaptionPresets()) {
        names.push_back(p.name);
        INFO(p.name);
        CHECK_FALSE(captionStyleProblem(p.style));
    }
    CHECK(names == std::vector<std::string>{"Clean", "Bold Box", "Podcast", "Shorts", "Thai-friendly", "Word Pop", "Highlighter",
                                            "Bounce", "Typewriter", "Zoom", "Hype"});
    CHECK(findBuiltinCaptionPreset("word pop")->style.animation == CaptionAnimation::WordPop);
    CHECK(findBuiltinCaptionPreset("Highlighter")->style.animation == CaptionAnimation::HighlightBar);
    CHECK(findBuiltinCaptionPreset("Bounce")->style.animation == CaptionAnimation::Bounce);
    CHECK(findBuiltinCaptionPreset("Typewriter")->style.animation == CaptionAnimation::Typewriter);
    CHECK(findBuiltinCaptionPreset("Zoom")->style.animation == CaptionAnimation::Zoom);
    const CaptionStyle& hype = findBuiltinCaptionPreset("Hype")->style;
    CHECK(hype.textCase == CaptionTextCase::Upper);
    CHECK(hype.stripPunctuation);
    CHECK(hype.emphasis == CaptionEmphasis::Auto);
    // The five older ones keep their names reserved; the gallery's can be a project's own (they may already be).
    CHECK(isOriginalBuiltinCaptionPreset("shorts"));
    CHECK(isOriginalBuiltinCaptionPreset("Thai-friendly"));
    CHECK_FALSE(isOriginalBuiltinCaptionPreset("Zoom"));

    // Out-of-range values, and keywords that are not one short line of UTF-8, are refused.
    const auto problem = [](auto change) {
        CaptionStyle s;
        change(s);
        return captionStyleProblem(s).has_value();
    };
    CHECK(problem([](CaptionStyle& s) { s.animationSpeed = 0.0; }));
    CHECK(problem([](CaptionStyle& s) { s.animationSpeed = 1.5; }));
    CHECK_FALSE(problem([](CaptionStyle& s) { s.animationSpeed = 0.05; }));
    CHECK(problem([](CaptionStyle& s) { s.emphasisScale = 0.9; }));
    CHECK(problem([](CaptionStyle& s) { s.emphasisScale = 1.6; }));
    CHECK(problem([](CaptionStyle& s) { s.emphasisColor = {2.0, 0.0, 0.0}; }));
    CHECK(problem([](CaptionStyle& s) { s.emphasisWords = std::vector<std::string>(kMaxCaptionEmphasisWords + 1, "gg"); }));
    CHECK_FALSE(problem([](CaptionStyle& s) { s.emphasisWords = std::vector<std::string>(kMaxCaptionEmphasisWords, "gg"); }));
    CHECK(problem([](CaptionStyle& s) { s.emphasisWords = {""}; }));
    CHECK(problem([](CaptionStyle& s) { s.emphasisWords = {"two\nlines"}; }));
    CHECK(problem([](CaptionStyle& s) { s.emphasisWords = {std::string(kMaxCaptionEmphasisWordBytes + 1, 'x')}; }));
    CHECK(problem([](CaptionStyle& s) { s.emphasisWords = {"bad\xC3\x28"}; }));
}

TEST_CASE("Caption styles: the new fields are saved and loaded; older projects get the old look; gallery names can be the project's own",
          "[model][captions][io][animation]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    CaptionStyle style;
    style.animation = CaptionAnimation::HighlightBar;
    style.animationSpeed = 0.3;
    style.animationEase = CaptionEase::Linear;
    style.emphasis = CaptionEmphasis::Keywords;
    style.emphasisWords = {"GG", "clutch", "ชนะ"};
    style.emphasisColor = {1.0, 0.2, 0.2};
    style.emphasisScale = 1.25;
    style.textCase = CaptionTextCase::Lower;
    style.stripPunctuation = true;
    style.stripAccents = true;
    ProjectState s = withCaptions(tl.state, tl.seq, {{0, 4, "hello"}}, style);
    s = *edit::saveCaptionPreset(s, {"Mine", style});

    const auto json = io::toJson(s);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == s);
    CHECK(captionsOf(*loaded, tl.seq).style == style);
    requireValid(*loaded);

    // A project from before the animated gallery has none of these keys: the old look (the defaults) loads.
    std::string old = *io::toJson(withCaptions(tl.state, tl.seq, {{0, 4, "hello"}}));
    for (const char* key : {"animationSpeed", "animationEase", "emphasis", "emphasisWords", "emphasisColor", "emphasisScale",
                            "textCase", "stripPunctuation", "stripAccents"}) {
        const std::string value = R"((\[[^\]]*\]|[^,\n}]*))";
        old = std::regex_replace(old, std::regex(std::string(R"(\s*")") + key + R"(": )" + value + ","), "");  // not the last key
        old = std::regex_replace(old, std::regex(std::string(R"(,\s*")") + key + R"(": )" + value), "");      // the last key
        INFO(key);
        REQUIRE(old.find(std::string("\"") + key + "\"") == std::string::npos);
    }
    const auto migrated = io::fromJson(old);
    INFO((migrated ? std::string() : migrated.error().message));
    REQUIRE(migrated);
    CHECK(captionsOf(*migrated, tl.seq).style == CaptionStyle{});

    // Untrusted files: a bad speed, scale or keyword is refused.
    for (const auto& [from, to] : {std::pair{std::string("\"animationSpeed\": 0.3"), std::string("\"animationSpeed\": 30")},
                                   {std::string("\"emphasisScale\": 1.25"), std::string("\"emphasisScale\": 9")},
                                   {std::string("\"animationEase\": \"linear\""), std::string("\"animationEase\": \"wobble\"")}}) {
        INFO(from);
        std::string broken = *json;
        const auto at = broken.find(from);
        REQUIRE(at != std::string::npos);
        broken.replace(at, from.size(), to);
        CHECK_FALSE(io::fromJson(broken));
    }

    // A project made before the gallery may hold its own preset called "Bounce": it loads, shows instead of the built-in
    // one, can be updated and removed; a preset with one of the five older built-in names is still refused.
    CaptionStyle mine;
    mine.fontWeight = 700;
    ProjectState own = tl.state;
    own.captionPresets.push_back(CaptionPreset{"Bounce", mine});
    const auto ownJson = io::toJson(own);
    REQUIRE(ownJson);
    const auto ownLoaded = io::fromJson(*ownJson);
    INFO((ownLoaded ? std::string() : ownLoaded.error().message));
    REQUIRE(ownLoaded);
    CHECK(edit::captionPresetStyle(*ownLoaded, "bounce")->fontWeight == 700); // the project's, not the gallery's 900
    mine.fontWeight = 800;
    const auto updated = edit::saveCaptionPreset(*ownLoaded, {"Bounce", mine});
    REQUIRE(updated);
    CHECK(updated->captionPresets.size() == 1);
    CHECK(edit::captionPresetStyle(*updated, "Bounce")->fontWeight == 800);
    const auto removed = edit::removeCaptionPreset(*updated, "Bounce");
    REQUIRE(removed);
    CHECK(removed->captionPresets.empty());
    CHECK(edit::captionPresetStyle(*removed, "Bounce")->animation == CaptionAnimation::Bounce); // the gallery's again
    CHECK(errorOf(edit::removeCaptionPreset(*removed, "Bounce")).find("built-in") != std::string::npos);
    CHECK(errorOf(edit::saveCaptionPreset(tl.state, {"Zoom", mine})).find("built-in") != std::string::npos);

    ProjectState clash = tl.state;
    clash.captionPresets.push_back(CaptionPreset{"Clean", mine});
    const auto clashJson = io::toJson(clash);
    if (clashJson) {
        CHECK_FALSE(io::fromJson(*clashJson));
    }
}

// ---- 0.7 (card 061): custom words for transcription -------------------------------------------------------------------

TEST_CASE("Custom words are cleaned, kept in order and saved only when there are some", "[model][captions][words]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    CHECK(tl.state.customWords.empty());
    CHECK(io::toJson(tl.state)->find("customWords") == std::string::npos); // older builds see an unchanged file

    auto set = edit::setCustomWords(tl.state, {"  Malenia ", "elden   ring", "MALENIA", "", "Elden Ring", "Radahn\n"});
    REQUIRE(set);
    // Trimmed, runs of white space collapsed, empty ones and later spellings of a word dropped (any case).
    CHECK(set->customWords == std::vector<std::string>{"Malenia", "elden ring", "Radahn"});
    CHECK(validate(*set, Validation::Structure));

    SECTION("saved with the project and read back") {
        const auto json = io::toJson(*set);
        REQUIRE(json);
        CHECK(json->find("\"customWords\"") != std::string::npos);
        const auto loaded = io::fromJson(*json);
        REQUIRE(loaded);
        CHECK(loaded->customWords == set->customWords);
        CHECK(*loaded == *set);
    }
    SECTION("the same words are no change; another list is one") {
        auto again = edit::setCustomWords(*set, {"Malenia", "elden ring", "Radahn"});
        REQUIRE(again);
        CHECK(*again == *set);
        CHECK_FALSE(diff(*set, *again).customWordsChanged);
        CHECK(diff(*set, *again).empty());
        auto other = edit::setCustomWords(*set, {"Godrick"});
        REQUIRE(other);
        const ChangeSet changes = diff(*set, *other);
        CHECK(changes.customWordsChanged);
        CHECK_FALSE(changes.empty());
    }
    SECTION("an empty list clears them") {
        auto cleared = edit::setCustomWords(*set, {});
        REQUIRE(cleared);
        CHECK(cleared->customWords.empty());
    }
    SECTION("bounds: too many words, a word that is too long, text that is not UTF-8") {
        std::vector<std::string> many;
        for (std::size_t i = 0; i < kMaxCustomWords + 1; ++i) {
            many.push_back("word" + std::to_string(i));
        }
        CHECK_FALSE(edit::setCustomWords(tl.state, many));
        many.pop_back();
        CHECK(edit::setCustomWords(tl.state, many));
        CHECK_FALSE(edit::setCustomWords(tl.state, {std::string(kMaxCustomWordBytes + 1, 'x')}));
        CHECK(edit::setCustomWords(tl.state, {std::string(kMaxCustomWordBytes, 'x')}));
        CHECK_FALSE(edit::setCustomWords(tl.state, {std::string("bad\xC3\x28 utf8")}));
    }
    SECTION("a file with damaged words does not load") {
        std::string json = *io::toJson(*set);
        const auto at = json.find("\"Malenia\"");
        REQUIRE(at != std::string::npos);
        json.replace(at, 9, "\"Radahn\""); // twice
        CHECK_FALSE(io::fromJson(json));
    }
}

TEST_CASE("Free text splits into custom words by line, comma or semicolon", "[model][captions][words]") {
    CHECK(edit::splitCustomWords("Malenia, Elden Ring;Radahn\nGodrick\r\nMorgott") ==
          std::vector<std::string>{"Malenia", " Elden Ring", "Radahn", "Godrick", "", "Morgott"});
    CHECK(edit::splitCustomWords("") == std::vector<std::string>{""});
    // setCustomWords trims and drops the empty ones.
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    const auto set = edit::setCustomWords(tl.state, edit::splitCustomWords("A, B\n\n C ;A"));
    REQUIRE(set);
    CHECK(set->customWords == std::vector<std::string>{"A", "B", "C"});
}

// ---- 0.7 (card 115): caption presets in a file of their own -----------------------------------------------------------

TEST_CASE("Caption presets are written to a file and read back for another project", "[model][captions][presets][io]") {
    CaptionStyle channel;
    channel.fontWeight = 800;
    channel.box = false;
    channel.outlineWidth = 0.12;
    channel.animation = CaptionAnimation::WordPop;
    channel.fill = {1.0, 0.8, 0.1};
    channel.emphasisWords = {"boss", "win"};
    CaptionStyle plain;
    const std::vector<CaptionPreset> presets{{"Channel look", channel}, {"Plain", plain}};

    const auto text = io::captionPresetsToJson(presets);
    REQUIRE(text);
    CHECK(text->find(std::string(io::kCaptionPresetsFileFormat)) != std::string::npos);
    const auto back = io::captionPresetsFromJson(*text);
    REQUIRE(back);
    REQUIRE(back->size() == 2);
    CHECK((*back)[0].name == "Channel look");
    CHECK((*back)[0].style == channel);
    CHECK((*back)[1].style == plain);

    SECTION("a file that is not caption presets, too new, too deep or damaged is refused, not half read") {
        CHECK_FALSE(io::captionPresetsFromJson("not json"));
        CHECK_FALSE(io::captionPresetsFromJson("[]"));
        CHECK_FALSE(io::captionPresetsFromJson(R"({"format": "wabcut-effect-presets", "version": 1, "presets": []})"));
        const auto newer = io::captionPresetsFromJson(R"({"format": "wabcut-caption-presets", "version": 99, "presets": []})");
        REQUIRE_FALSE(newer);
        CHECK(newer.error().code == ErrorCode::VersionMismatch);
        CHECK_FALSE(io::captionPresetsFromJson(std::string(io::kMaxJsonNesting + 8, '[') + std::string(io::kMaxJsonNesting + 8, ']')));
        CHECK_FALSE(io::captionPresetsFromJson(R"({"format": "wabcut-caption-presets", "version": 1, "presets": [{"name": "", "style": {}}]})"));
        CHECK_FALSE(io::captionPresetsFromJson(R"({"format": "wabcut-caption-presets", "version": 1, "presets": [{"name": "x", "style": {"placement": "sideways"}}]})"));
        CHECK_FALSE(io::captionPresetsFromJson(std::string(io::kMaxCaptionPresetsFileBytes + 1, ' ')));
    }
    SECTION("each preset goes into a project as its own") {
        Timeline tl(R"(
            V1 tgt |Aaaaaaaaaa|
        )");
        ProjectState state = tl.state;
        for (const CaptionPreset& p : *back) {
            auto saved = edit::saveCaptionPreset(state, p);
            REQUIRE(saved);
            state = *saved;
        }
        CHECK(state.captionPresets.size() == 2);
        CHECK(edit::captionPresetStyle(state, "channel look")->fontWeight == 800);
    }
}
