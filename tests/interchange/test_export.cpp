// Timeline export (X.1): Wabcut -> FCP7 XML / FCPXML / OTIO / EDL -> Wabcut. Each test builds a project with the
// model's own operations, exports a sequence, imports the file again and compares the two structurally: what the
// export report calls carried must come back the same. Formats that cannot say something (FCPXML has no tracks, an
// EDL no names or effects) are compared on what they can.

#include "InterchangeTestSupport.h"
#include "ScaleProject.h"
#include "TimelineFixture.h"

#include <wabcut/interchange/Export.h>
#include <wabcut/interchange/ImportSummary.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace wabcut;
using namespace wabcut::interchange;
using wabcut::test::f;
using wabcut::test::clipAt;
using wabcut::test::ok;
namespace it = wabcut::interchange::test;

namespace {

/// What a comparison looks at.
struct Compare {
    bool trackState = true; ///< track names, output, lock and target
    bool clipNames = true;
    bool links = true;
    bool markers = true;
    bool marks = true;
    bool workArea = true; ///< the sequence's work area (0.7)
    bool paths = true;      ///< media file paths
    bool pan = true;
    bool enabled = true;      ///< clips' on/off state
    bool mediaMarkers = true; ///< the markers of a clip's source (Premiere's clip markers)
    bool markerDetails = true; ///< marker comments and durations (an EDL's locators have neither)
    /// Empty tracks the other side has more of are fine (FCPXML always has a primary storyline's sound).
    bool extraEmptyTracks = false;
    /// Compare the multicam sources clips play, inside (FCPXML angles carry one audio track, without links).
    bool multicamSources = true;
    /// Only these tracks ("V1", "A2"); empty: all, and the track counts must match.
    std::set<std::string> tracks;
};

std::string label(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
}

std::string paramText(const ParamValue& v) {
    std::ostringstream out;
    if (const Curve* c = v.curve()) {
        out << "keys[";
        for (const Keyframe& k : c->keys) {
            out << k.time.value << ":" << k.value << ":" << static_cast<int>(k.interpolation) << " ";
        }
        out << "]";
    } else {
        out << v.constant();
    }
    return out.str();
}

std::string clipText(const ProjectState& s, const Clip& c, const Compare& opt) {
    std::ostringstream out;
    out << c.start().value << "+" << c.duration().value << " in " << c.sourceIn.value;
    if (opt.clipNames) {
        out << " '" << c.name() << "'";
    }
    out << (c.enabled || !opt.enabled ? "" : " off") << " stream " << (c.stream.kind == StreamKind::Video ? "v" : "a") << c.stream.index;
    if (!c.speed.isUnity() || c.reversed) {
        out << " speed " << c.speed.num << "/" << c.speed.den << (c.reversed ? " rev" : "");
    }
    if (const Curve* remap = c.timeRemap()) {
        out << " remap " << paramText(ParamValue(*remap));
    }
    const ClipProperties defaults;
    for (std::size_t i = 0; i < kParamCount; ++i) {
        const auto id = static_cast<ParamId>(i);
        if (!opt.pan && id == ParamId::Pan) {
            continue;
        }
        if (!(c.properties()[id] == defaults[id])) {
            out << " " << parameterInfo(id).key << "=" << paramText(c.properties()[id]);
        }
    }
    if (c.content().channels) {
        out << " channels " << c.content().channels->channelCount();
    }
    for (const EffectInstance& e : c.effects()) {
        if (e.effect == edit::kTitleStyleEffect) {
            continue; // compared with the title
        }
        out << " fx[" << e.effect << (e.enabled ? "" : " off");
        for (const EffectParam& p : e.params) {
            out << " " << p.key << "=" << paramText(p.value);
        }
        out << "]";
    }
    if (c.isNested()) {
        out << " plays '" << s.findSequence(c.sequence)->name << "' angles " << c.angles.bits;
    } else if (const MediaItem* m = s.findMedia(c.media)) {
        if (m->isTitle()) {
            out << " title '" << m->generator->text << "' " << m->generator->fontFamily << " " << m->generator->fontStyle;
            for (const EffectInstance& e : c.effects()) {
                if (e.effect == edit::kTitleStyleEffect) {
                    for (const EffectParam& p : e.params) {
                        out << " " << p.key << "=" << paramText(p.value);
                    }
                }
            }
        } else if (opt.paths) {
            std::string path = m->locator.path; // a file found by the relinker has native separators
            std::replace(path.begin(), path.end(), '\\', '/');
            out << " " << path;
        }
        if (opt.mediaMarkers && !m->isTitle()) {
            for (const Marker& k : m->markers) {
                out << " mk[" << k.start.value << "+" << k.duration.value << " '" << k.name << "' '" << k.comment << "' c"
                    << k.colorIndex << "]";
            }
        }
    }
    return out.str();
}

/// Differences between two sequences (and the sequences they nest), one line each; empty when they are the same.
void differences(const ProjectState& a, SequenceId sa, const ProjectState& b, SequenceId sb, const Compare& opt,
                 std::vector<std::string>& out, std::set<std::uint64_t>& seen) {
    if (!seen.insert(sa.value).second) {
        return;
    }
    const Sequence& x = *a.findSequence(sa);
    const Sequence& y = *b.findSequence(sb);
    const std::string where = "'" + x.name + "'";
    if (!opt.multicamSources && x.multicam) {
        return;
    }
    const auto diff = [&](const std::string& what, const auto& l, const auto& r) {
        if (!(l == r)) {
            std::ostringstream s;
            s << where << " " << what << ": " << l << " != " << r;
            out.push_back(s.str());
        }
    };
    diff("name", x.name, y.name);
    diff("rate", std::to_string(x.frameRate.num) + "/" + std::to_string(x.frameRate.den),
         std::to_string(y.frameRate.num) + "/" + std::to_string(y.frameRate.den));
    diff("drop frame", x.dropFrame, y.dropFrame);
    diff("size", std::to_string(x.width) + "x" + std::to_string(x.height), std::to_string(y.width) + "x" + std::to_string(y.height));
    diff("audio rate", x.audioRate.num, y.audioRate.num);
    diff("duration", x.duration.value, y.duration.value);
    diff("start timecode", x.startTimecode.value, y.startTimecode.value);
    if (opt.marks) {
        diff("mark in", x.markIn.value_or(Ticks{-1}).value, y.markIn.value_or(Ticks{-1}).value);
        diff("mark out", x.markOut.value_or(Ticks{-1}).value, y.markOut.value_or(Ticks{-1}).value);
    }
    if (opt.workArea) {
        const auto text = [](const std::optional<TimeRange>& w) {
            return w ? std::to_string(w->start.value) + "+" + std::to_string(w->duration.value) : std::string("none");
        };
        diff("work area", text(x.workArea), text(y.workArea));
    }
    if (opt.markers) {
        diff("markers", x.markers.size(), y.markers.size());
        for (std::size_t i = 0; i < std::min(x.markers.size(), y.markers.size()); ++i) {
            const Marker& m = x.markers[i];
            const Marker& n = y.markers[i];
            const auto markerText = [&](const Marker& k) {
                return std::to_string(k.start.value) + (opt.markerDetails ? "+" + std::to_string(k.duration.value) : std::string()) +
                       " '" + k.name + "'" + (opt.markerDetails ? " '" + k.comment + "'" : std::string()) + " c" +
                       std::to_string(k.colorIndex);
            };
            diff("marker " + std::to_string(i), markerText(m), markerText(n));
        }
    }
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tx = x.tracks(kind);
        const auto& ty = y.tracks(kind);
        if (opt.tracks.empty()) {
            const auto clipsFrom = [](const auto& tracks, std::size_t from) {
                std::size_t n = 0;
                for (std::size_t i = from; i < tracks.size(); ++i) {
                    n += tracks[i]->clips.size();
                }
                return n;
            };
            const bool onlyEmptyExtra = opt.extraEmptyTracks && clipsFrom(tx, std::min(tx.size(), ty.size())) == 0 &&
                                        clipsFrom(ty, std::min(tx.size(), ty.size())) == 0;
            if (!onlyEmptyExtra) {
                diff(std::string(kind == TrackKind::Video ? "video" : "audio") + " tracks", tx.size(), ty.size());
            }
        }
        for (std::size_t i = 0; i < tx.size(); ++i) {
            const std::string l = label(kind, i);
            if (!opt.tracks.empty() && !opt.tracks.contains(l)) {
                continue;
            }
            if (i >= ty.size()) {
                out.push_back(where + " " + l + " is missing");
                continue;
            }
            const Track& p = *tx[i];
            const Track& q = *ty[i];
            if (opt.trackState) {
                diff(l + " name", p.name, q.name);
                diff(l + " output", p.enabled, q.enabled);
                diff(l + " lock", p.locked, q.locked);
                diff(l + " target", p.targeted, q.targeted);
            }
            diff(l + " clips", p.clips.size(), q.clips.size());
            for (std::size_t c = 0; c < std::min(p.clips.size(), q.clips.size()); ++c) {
                diff(l + " clip " + std::to_string(c), clipText(a, p.clips[c], opt), clipText(b, q.clips[c], opt));
                if (p.clips[c].isNested() && q.clips[c].isNested()) {
                    Compare inner = opt;
                    inner.tracks.clear();
                    differences(a, p.clips[c].sequence, b, q.clips[c].sequence, inner, out, seen);
                }
            }
            diff(l + " transitions", p.transitions.size(), q.transitions.size());
            for (std::size_t t = 0; t < std::min(p.transitions.size(), q.transitions.size()); ++t) {
                const Transition& u = p.transitions[t];
                const Transition& v = q.transitions[t];
                diff(l + " transition " + std::to_string(t),
                     u.effect + " " + std::to_string(transitionEditPoint(p, u)->value) + " " + std::to_string(u.before.value) + "/" +
                         std::to_string(u.after.value),
                     v.effect + " " + std::to_string(transitionEditPoint(q, v)->value) + " " + std::to_string(v.before.value) + "/" +
                         std::to_string(v.after.value));
            }
        }
    }
    if (opt.links) {
        diff("links", wabcut::test::links(a, sa), wabcut::test::links(b, sb));
    }
}

struct RoundTrip {
    ExportResult exported;
    ImportResult imported;
    std::vector<std::string> differences;
};

/// Exports `seq` as `format`, imports the file and compares the result's first sequence with `seq`.
RoundTrip roundTrip(const ProjectState& state, SequenceId seq, Format format, const Compare& opt = {}) {
    auto exported = exportSequence(state, seq, format);
    INFO((exported ? std::string() : exported.error().message));
    REQUIRE(exported.has_value());
    RoundTrip r;
    r.exported = std::move(*exported);
    INFO(r.exported.data);
    r.imported = it::importText(r.exported.data, format, {}, "export" + r.exported.extension);
    REQUIRE_FALSE(r.imported.sequences.empty());
    std::set<std::uint64_t> seen;
    differences(state, seq, r.imported.project, r.imported.sequences.front(), opt, r.differences, seen);
    return r;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) {
        out += l + "\n";
    }
    return out;
}

/// A project with most of what the formats carry: two video and two audio tracks, linked A/V pairs, a gap, a
/// disabled and a renamed clip, named, locked and targeted tracks, markers, marks and a trailing gap.
wabcut::test::Timeline basicTimeline() {
    wabcut::test::Timeline tl(R"(
        V2          |....Cccc............|
        V1 tgt      |AaaaB10bbbbb..Dddd..|
        A1 tgt      |AaaaB10bbbbb..Dddd..|
        A2 lock     |..........Eeee......|
    )");
    tl.linkMatching();
    ProjectState& s = tl.state;
    edit::TrackStateChange named;
    named.name = "Dialogue";
    s = ok(edit::setTrackState(s, tl.seq, tl.track("A1"), named)).state;
    named.name = "Titles";
    s = ok(edit::setTrackState(s, tl.seq, tl.track("V2"), named)).state;
    // D (picture and sound) is switched off; B (picture and sound) renamed.
    const std::vector<ClipId> d{tl.clip("V1@14"), tl.clip("A1@14")};
    edit::ClipAttributeChange off;
    off.enabled = false;
    s = ok(edit::setClipAttributes(s, tl.seq, d, off)).state;
    const std::vector<ClipId> b{tl.clip("V1@4"), tl.clip("A1@4")};
    edit::ClipAttributeChange renamed;
    renamed.name = "Interview";
    s = ok(edit::setClipAttributes(s, tl.seq, b, renamed)).state;
    Marker intro{f(2), Ticks{}, "Intro"};
    intro.comment = "music in";
    intro.colorIndex = 6;
    s = ok(edit::addMarker(s, tl.seq, intro)).state;
    Marker chapter{f(10), f(3), "Chapter 2"};
    chapter.colorIndex = 3;
    s = ok(edit::addMarker(s, tl.seq, chapter)).state;
    s = ok(edit::markIn(s, tl.seq, f(1))).state;
    s = ok(edit::markOut(s, tl.seq, f(16) - Ticks{1})).state;
    s = ok(edit::setSequenceDuration(s, tl.seq, f(22))).state;
    // Markers of a source (Premiere's clip markers): they travel with its clips.
    Marker slate{f(1), Ticks{}, "Slate"};
    slate.comment = "take 3";
    slate.colorIndex = 4;
    s = ok(edit::setMediaMarkers(s, tl.media('A'), {slate, Marker{f(3), f(1), "Clap"}})).state;
    s = ok(edit::setSequenceStartTimecode(s, tl.seq, Ticks::fromSeconds(3600))).state;
    return tl;
}

} // namespace

TEST_CASE("Export: FCP7 XML brings cuts, links, gaps, markers and track settings back unchanged (X.1)", "[interchange][export]") {
    const auto tl = basicTimeline();
    const RoundTrip r = roundTrip(tl.state, tl.seq, Format::Fcp7Xml);
    INFO(joined(r.differences));
    CHECK(r.differences.empty());
    CHECK(r.exported.report.count(ReportOutcome::Dropped) == 0);
    CHECK(r.exported.report.count(ReportOutcome::Approximated) == 0);
    CHECK(r.exported.data.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE xmeml>\n<xmeml version=\"5\">"));
    CHECK(r.exported.data.find("<pathurl>file://localhost/D%3a/footage/A.mov</pathurl>") != std::string::npos);
}

TEST_CASE("Export: FCP7 XML names the sequence's pixel aspect ratio from Apple's list (145)", "[interchange][export]") {
    const auto tl = basicTimeline();
    const auto exportedWith = [&](int width, int height, Rate par) {
        ProjectState s = tl.state;
        auto copy = std::make_shared<Sequence>(*s.findSequence(tl.seq));
        copy->width = width;
        copy->height = height;
        copy->pixelAspect = par;
        for (SequencePtr& seq : s.sequences) {
            if (seq->id == tl.seq) {
                seq = copy;
            }
        }
        return roundTrip(s, tl.seq, Format::Fcp7Xml).exported.data;
    };
    const auto has = [](const std::string& xml, const char* text) { return xml.find(text) != std::string::npos; };
    const std::string square = exportedWith(1920, 1080, Rate{1, 1});
    CHECK(has(square, "<pixelaspectratio>square</pixelaspectratio>"));
    CHECK(has(square, "<anamorphic>FALSE</anamorphic>"));
    const std::string hdv = exportedWith(1440, 1080, Rate{4, 3});
    CHECK(has(hdv, "<pixelaspectratio>HD-(1440x1080)</pixelaspectratio>"));
    CHECK(has(exportedWith(960, 720, Rate{4, 3}), "<pixelaspectratio>HD-(960x720)</pixelaspectratio>"));
    CHECK(has(exportedWith(1280, 1080, Rate{3, 2}), "<pixelaspectratio>HD-(1280x1080)</pixelaspectratio>"));
    const std::string dvWide = exportedWith(720, 480, Rate{40, 33});
    CHECK(has(dvWide, "<pixelaspectratio>NTSC-601</pixelaspectratio>"));
    CHECK(has(dvWide, "<anamorphic>TRUE</anamorphic>"));
    const std::string dv = exportedWith(720, 480, Rate{10, 11});
    CHECK(has(dv, "<pixelaspectratio>NTSC-601</pixelaspectratio>"));
    CHECK(has(exportedWith(720, 576, Rate{59, 54}), "<pixelaspectratio>PAL-601</pixelaspectratio>"));
    // A ratio the list has no name for is written square (the size still travels).
    CHECK(has(exportedWith(1920, 1080, Rate{2, 1}), "<pixelaspectratio>square</pixelaspectratio>"));
}

TEST_CASE("Export: OTIO brings the timeline back unchanged (X.1)", "[interchange][export]") {
    const auto tl = basicTimeline();
    const RoundTrip r = roundTrip(tl.state, tl.seq, Format::Otio);
    INFO(joined(r.differences));
    CHECK(r.differences.empty());
    CHECK(r.exported.report.count(ReportOutcome::Dropped) == 0);
}

TEST_CASE("Export: FCPXML brings the storyline, connected clips, links and markers back (X.1)", "[interchange][export]") {
    const auto tl = basicTimeline();
    Compare opt;
    opt.trackState = false;    // FCPXML has no tracks
    opt.marks = false;         // nor sequence In/Out marks
    opt.mediaMarkers = false;  // nor markers of a source
    const RoundTrip r = roundTrip(tl.state, tl.seq, Format::FcpXml, opt);
    INFO(joined(r.differences));
    CHECK(r.differences.empty());
    CHECK(r.exported.report.count(ReportOutcome::Dropped) == 0);
    // The track settings it cannot say are reported.
    CHECK(it::hasEntry(r.exported.report, ReportOutcome::Approximated, ReportCategory::Track, "A2"));
    CHECK(r.exported.data.find("<fcpxml version=\"1.10\">") != std::string::npos);
}

TEST_CASE("Export: an EDL brings V1 and its audio back as events (X.1)", "[interchange][export]") {
    const auto tl = basicTimeline();
    Compare opt;
    opt.trackState = false;
    opt.marks = false;
    opt.paths = false;     // an EDL names reels and clips, not files
    opt.clipNames = false;
    opt.enabled = false;       // an EDL has no disabled events
    opt.mediaMarkers = false;
    opt.markerDetails = false; // its locators have a position, a colour and a name
    opt.tracks = {"V1", "A1", "A2"};
    ImportOptions options;
    options.edlFrameRate = wabcut::test::kRate;
    auto exported = exportSequence(tl.state, tl.seq, Format::Edl);
    REQUIRE(exported.has_value());
    INFO(exported->data);
    // V2 cannot be written: one video track.
    CHECK(it::hasEntry(exported->report, ReportOutcome::Dropped, ReportCategory::Clip, ""));
    const ImportResult back = it::importText(exported->data, Format::Edl, options, "export.edl");
    REQUIRE_FALSE(back.sequences.empty());
    std::vector<std::string> diffs;
    std::set<std::uint64_t> seen;
    differences(tl.state, tl.seq, back.project, back.sequences.front(), opt, diffs, seen);
    // An EDL keeps events, not the sequence's trailing gap; what is left must match.
    std::erase_if(diffs, [](const std::string& d) { return d.find("' duration") != std::string::npos; });
    INFO(joined(diffs));
    CHECK(diffs.empty());
}

TEST_CASE("Export: transitions, crossfades and fades survive FCP7 XML, FCPXML and OTIO (X.1)", "[interchange][export]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |Aaaaaaaa.B10bbbbbbbb....|
        A1 tgt |Aaaaaaaa.B10bbbbbbbb....|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    edit::TransitionSpec dissolve{tl.clip("V1@0"), tl.clip("V1@9"), f(2), f(2)};
    // The clips do not touch: close the gap first so A and B meet at frame 8.
    s = ok(edit::moveClips(s, tl.seq, std::vector<ClipId>{tl.clip("V1@9")}, -f(1))).state;
    dissolve.incoming = clipAt(s, tl.seq, "V1@8");
    s = ok(edit::addTransition(s, tl.seq, tl.track("V1"), dissolve)).state;
    edit::TransitionSpec crossfade{clipAt(s, tl.seq, "A1@0"), clipAt(s, tl.seq, "A1@8"), f(2), f(2)};
    crossfade.effect = "constantPower";
    s = ok(edit::addTransition(s, tl.seq, tl.track("A1"), crossfade)).state;
    edit::TransitionSpec fade;
    fade.outgoing = clipAt(s, tl.seq, "V1@8");
    fade.before = f(3);
    fade.effect = "dipToBlack";
    s = ok(edit::addTransition(s, tl.seq, tl.track("V1"), fade)).state;
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(toString(format));
        Compare opt;
        if (format == Format::FcpXml) {
            opt.trackState = false;
            opt.marks = false;
            opt.extraEmptyTracks = true;
        }
        const RoundTrip r = roundTrip(s, tl.seq, format, opt);
        INFO(joined(r.differences));
        CHECK(r.differences.empty());
        CHECK(r.exported.report.count(ReportOutcome::Dropped) == 0);
    }
}

TEST_CASE("Export: speeds, reverse, freeze frames and time remaps survive FCP7 XML, FCPXML and OTIO (X.1)", "[interchange][export]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |Aaaa..B20bbbbb..C30ccc..D40ddd..E50eeeeeeee|
    )");
    ProjectState s = tl.state;
    edit::SpeedChange half;
    half.speed = Speed{1, 2};
    half.keepDuration = true;
    const ClipId a = tl.clip("V1@0");
    s = ok(edit::setSpeed(s, tl.seq, std::span(&a, 1), half)).state;
    edit::SpeedChange twice;
    twice.speed = Speed{2, 1};
    twice.keepDuration = true;
    const ClipId b = tl.clip("V1@6");
    s = ok(edit::setSpeed(s, tl.seq, std::span(&b, 1), twice)).state;
    edit::SpeedChange reversed;
    reversed.speed = Speed{1, 1};
    reversed.reversed = true;
    reversed.keepDuration = true;
    const ClipId c = tl.clip("V1@16");
    s = ok(edit::setSpeed(s, tl.seq, std::span(&c, 1), reversed)).state;
    s = ok(edit::frameHold(s, tl.seq, clipAt(s, tl.seq, "V1@24"), f(26))).state;
    // E: a ramp from 100 % to 200 % (keys at whole frames of media time).
    Curve ramp;
    ramp.keys = {Keyframe{f(50), static_cast<double>(f(50).value)}, Keyframe{f(54), static_cast<double>(f(54).value)},
                 Keyframe{f(58), static_cast<double>(f(62).value)}};
    s = ok(edit::setTimeRemap(s, tl.seq, clipAt(s, tl.seq, "V1@32"), ramp, f(50))).state;
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(toString(format));
        Compare opt;
        if (format == Format::FcpXml) {
            opt.trackState = false;
            opt.extraEmptyTracks = true;
            opt.multicamSources = false;
        }
        const RoundTrip r = roundTrip(s, tl.seq, format, opt);
        INFO(joined(r.differences));
        CHECK(r.differences.empty());
    }
}

TEST_CASE("Export: opacity, motion, volume and their keyframes survive FCP7 XML, FCPXML and OTIO (X.1)", "[interchange][export]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |A10aaaaaaaaB30bbbbbbb|
        A1 tgt |A10aaaaaaaaB30bbbbbbb|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId va = tl.clip("V1@0");
    ClipProperties moved;
    moved.set(ParamId::Opacity, 0.5);
    moved.set(ParamId::Scale, 1.25);
    moved.set(ParamId::Rotation, 10.0);
    moved.set(ParamId::PositionX, 192.0);
    moved.set(ParamId::PositionY, -108.0);
    edit::ClipAttributeChange change;
    change.properties = moved;
    s = ok(edit::setClipAttributes(s, tl.seq, std::span(&va, 1), change)).state;
    const ClipId aa = tl.clip("A1@0");
    ClipProperties louder;
    louder.set(ParamId::VolumeDb, -6.0);
    change.properties = louder;
    s = ok(edit::setClipAttributes(s, tl.seq, std::span(&aa, 1), change)).state;
    // B fades out over its last four frames (keys in its media time: source frames 33 and 37).
    const ClipId vb = tl.clip("V1@11");
    ClipProperties fading;
    Curve opacity;
    opacity.keys = {Keyframe{f(33), 1.0}, Keyframe{f(37), 0.0}};
    fading[ParamId::Opacity] = ParamValue(opacity);
    change.properties = fading;
    s = ok(edit::setClipAttributes(s, tl.seq, std::span(&vb, 1), change)).state;
    const ClipId ab = tl.clip("A1@11");
    ClipProperties dipping;
    Curve volume;
    volume.keys = {Keyframe{f(31), 0.0}, Keyframe{f(35), -12.0}};
    dipping[ParamId::VolumeDb] = ParamValue(volume);
    change.properties = dipping;
    s = ok(edit::setClipAttributes(s, tl.seq, std::span(&ab, 1), change)).state;
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(toString(format));
        Compare opt;
        if (format == Format::FcpXml) {
            opt.trackState = false;
            opt.extraEmptyTracks = true;
            opt.multicamSources = false;
        }
        const RoundTrip r = roundTrip(s, tl.seq, format, opt);
        INFO(joined(r.differences));
        CHECK(r.differences.empty());
    }
}

TEST_CASE("Export: a clip stretched with Scale Width survives FCP7 XML, FCPXML and OTIO, keys included (card 12)",
          "[interchange][export][scalewidth]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |A10aaaaaaaaB30bbbbbbb|
    )");
    ProjectState s = tl.state;
    // A: 150 % wide, 50 % high, Uniform Scale off. B: both keyed (width 100 % -> 200 %, height 50 % -> 75 %).
    ClipProperties wide;
    wide.set(ParamId::UniformScale, 0.0);
    wide.set(ParamId::ScaleWidth, 1.5);
    wide.set(ParamId::Scale, 0.5);
    edit::ClipAttributeChange change;
    change.properties = wide;
    const ClipId a = tl.clip("V1@0");
    s = ok(edit::setClipAttributes(s, tl.seq, std::span(&a, 1), change)).state;
    ClipProperties growing;
    growing.set(ParamId::UniformScale, 0.0);
    Curve width;
    width.keys = {Keyframe{f(33), 1.0}, Keyframe{f(37), 2.0}};
    Curve height;
    height.keys = {Keyframe{f(33), 0.5}, Keyframe{f(37), 0.75}};
    growing[ParamId::ScaleWidth] = ParamValue(width);
    growing[ParamId::Scale] = ParamValue(height);
    change.properties = growing;
    const ClipId b = tl.clip("V1@11");
    s = ok(edit::setClipAttributes(s, tl.seq, std::span(&b, 1), change)).state;
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(toString(format));
        Compare opt;
        if (format == Format::FcpXml) {
            opt.trackState = false;
            opt.extraEmptyTracks = true;
            opt.multicamSources = false;
        }
        const RoundTrip r = roundTrip(s, tl.seq, format, opt);
        INFO(joined(r.differences));
        CHECK(r.differences.empty());
        CHECK(r.exported.report.count(ReportOutcome::Dropped) == 0);
    }
}

TEST_CASE("Export: titles survive FCP7 XML, FCPXML and OTIO (X.1)", "[interchange][export]") {
    wabcut::test::Timeline tl(R"(
        V2     |....................|
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    std::vector<EffectParam> style{EffectParam{"alignment", ParamValue(1.0)}, EffectParam{"fill.alpha", ParamValue(1.0)},
                                   EffectParam{"fill.blue", ParamValue(0.0)}, EffectParam{"fill.green", ParamValue(0.6)},
                                   EffectParam{"fill.red", ParamValue(1.0)}, EffectParam{"size", ParamValue(80.0)}};
    const ProjectState s =
        ok(edit::addTitle(tl.state, tl.seq, tl.track("V2"), f(2), f(10), "Opening Night", style, edit::TitleFont{"Arial", "Bold"}))
            .state;
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(toString(format));
        Compare opt;
        if (format == Format::FcpXml) {
            opt.trackState = false;
            opt.extraEmptyTracks = true;
            opt.multicamSources = false;
        }
        const RoundTrip r = roundTrip(s, tl.seq, format, opt);
        INFO(joined(r.differences));
        CHECK(r.differences.empty());
    }
}

TEST_CASE("Export: nested sequences and multicam clips go with the sequence (X.1)", "[interchange][export]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |AaaaBbbb....................|
        A1 tgt |AaaaBbbb....................|
    )");
    tl.linkMatching();
    const std::vector<ClipId> selection{tl.clip("V1@0"), tl.clip("V1@4")};
    auto nest = edit::nestClips(tl.state, tl.seq, selection, "Nest");
    REQUIRE(nest);
    ProjectState s = nest->edit.state;
    edit::MulticamSpec spec;
    spec.name = "Concert";
    spec.angles = {tl.media('C'), tl.media('D'), tl.media('E')};
    spec.sync = MulticamSync::Manual;
    auto made = edit::createMulticam(s, spec);
    REQUIRE(made);
    edit::SourceClip clip;
    clip.sequence = made->id;
    clip.sourceRange = {Ticks{}, f(12)};
    clip.patches = edit::defaultSourcePatches(*made->state.findSequence(tl.seq), *made->state.findSequence(made->id));
    s = ok(edit::overwriteEdit(made->state, tl.seq, clip, f(10))).state;
    s = ok(edit::switchMulticamAngle(s, tl.seq, clipAt(s, tl.seq, "V1@10"), f(16), 1, edit::AngleTarget::Both)).state;
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(toString(format));
        Compare opt;
        if (format == Format::FcpXml) {
            opt.trackState = false;
            opt.extraEmptyTracks = true;
            opt.multicamSources = false;
        }
        const RoundTrip r = roundTrip(s, tl.seq, format, opt);
        INFO(joined(r.differences));
        CHECK(r.differences.empty());
        CHECK(r.exported.sequences.size() == 3); // the timeline, the nest and the multicam source
    }
}

TEST_CASE("Export: the same project gives the same bytes; graphics and effects are reported (X.1)", "[interchange][export]") {
    const auto tl = basicTimeline();
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio, Format::Edl}) {
        INFO(toString(format));
        const auto first = exportSequence(tl.state, tl.seq, format);
        const auto second = exportSequence(tl.state, tl.seq, format);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->data == second->data);
    }
    // A graphic clip has no interchange equivalent; an effect is not written yet: both reported.
    Graphic g;
    GraphicLayer text;
    text.id = 1;
    text.kind = LayerKind::Text;
    text.text.runs = {TextRun{"Lower third", TextStyle{}}};
    g.layers = {text};
    ProjectState s = ok(edit::addGraphic(tl.state, tl.seq, tl.track("V2"), f(12), f(4), g)).state;
    const ClipId a = tl.clip("V1@0");
    s = ok(edit::addEffect(s, tl.seq, std::span<const ClipId>(&a, 1), "wabcut.color.basic")).state;
    const auto exported = exportSequence(s, tl.seq, Format::Fcp7Xml);
    REQUIRE(exported.has_value());
    CHECK(it::hasEntry(exported->report, ReportOutcome::Dropped, ReportCategory::Graphic, "motion graphic"));
    // 0.6 (card #66): Basic Correction goes out as Premiere's Lumetri Color.
    CHECK(exported->data.find("<effectid>Lumetri</effectid>") != std::string::npos); // PF: Premiere's own effect id
    CHECK(formatSummary(*exported).find("Exported 1 sequence as Final Cut Pro 7 XML") != std::string::npos);
    CHECK_FALSE(exportSequence(s, SequenceId{999'999}, Format::Fcp7Xml).has_value());
    CHECK_FALSE(exportSequence(s, tl.seq, Format::PremiereProject).has_value());
}

// Golden files: the basic timeline's export in each format, byte for byte (tests/interchange/fixtures/export). On a
// difference the new export is written to test-output/export/ next to the test's working folder; after checking it,
// set WABCUT_UPDATE_GOLDEN=1 and run again to replace the golden file.
TEST_CASE("Export: each format's output matches its golden file (X.1)", "[interchange][export]") {
    const auto tl = basicTimeline();
    const bool update = std::getenv("WABCUT_UPDATE_GOLDEN") != nullptr;
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio, Format::Edl}) {
        INFO(toString(format));
        const auto exported = exportSequence(tl.state, tl.seq, format);
        REQUIRE(exported.has_value());
        const std::string name = "export/basic" + exported->extension;
        const std::filesystem::path golden = it::fixturePath(name);
        if (update) {
            std::filesystem::create_directories(golden.parent_path());
            std::ofstream(golden, std::ios::binary) << exported->data;
        }
        const std::string want = std::filesystem::exists(golden) ? it::readFile(golden) : std::string();
        if (exported->data != want) {
            const std::filesystem::path out = std::filesystem::path("test-output") / "export" / ("basic" + exported->extension);
            std::filesystem::create_directories(out.parent_path());
            std::ofstream(out, std::ios::binary) << exported->data;
            INFO("golden file " << golden.string() << " differs; the new export is " << std::filesystem::absolute(out).string());
            CHECK(exported->data == want);
        }
    }
}

TEST_CASE("Export: every import fixture exports to each format and imports again (X.1)", "[interchange][export]") {
    struct Fixture {
        const char* name;
        ImportOptions options;
    };
    ImportOptions edl;
    edl.edlFrameRate = Rate{25, 1};
    ImportOptions edlDf;
    edlDf.edlFrameRate = Rate{30000, 1001};
    const std::vector<Fixture> fixtures{{"fcp7_23976_mac.xml", {}},       {"premiere_2997df.xml", {}},
                                        {"fcpx_1_10_23976.fcpxml", {}},  {"resolve_timeline.otio", {}},
                                        {"avid_25fps.edl", edl},         {"promo_2997df.edl", edlDf},
                                        {"Wedding Teaser.fcpxmld", {}}};
    for (const Fixture& fx : fixtures) {
        INFO(fx.name);
        const ImportResult first = it::importFixture(fx.name, fx.options);
        REQUIRE_FALSE(first.sequences.empty());
        for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio, Format::Edl}) {
            INFO(toString(format));
            const auto exported = exportSequence(first.project, first.sequences.front(), format);
            REQUIRE(exported.has_value());
            ImportOptions options;
            options.edlFrameRate = first.project.findSequence(first.sequences.front())->frameRate;
            const ImportResult again = it::importText(exported->data, format, options, "again" + exported->extension);
            REQUIRE_FALSE(again.sequences.empty());
            if (format == Format::Fcp7Xml || format == Format::Otio) {
                // These two carry everything the first import made: the same timeline again.
                std::vector<std::string> diffs;
                std::set<std::uint64_t> seen;
                Compare opt;
                differences(first.project, first.sequences.front(), again.project, again.sequences.front(), opt, diffs, seen);
                INFO(joined(diffs));
                CHECK(diffs.empty());
            }
        }
    }
}

TEST_CASE("Export: effect stacks survive FCP7 XML: blur, crop and drop shadow as FCP filters, the rest by Wabcut id (X.1)",
          "[interchange][export]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |A10aaaaaaaaB30bbbbbbb|
        A1 tgt |A10aaaaaaaaB30bbbbbbb|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId va = tl.clip("V1@0");
    const ClipId vb = tl.clip("V1@11");
    const ClipId aa = tl.clip("A1@0");
    s = ok(edit::addEffect(s, tl.seq, std::span(&va, 1), "wabcut.blur.gaussian", {EffectParam{"radius", ParamValue(12.0)}})).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&va, 1), "wabcut.crop",
                           {EffectParam{"bottom", ParamValue(0.05)}, EffectParam{"left", ParamValue(0.1)}}))
            .state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&vb, 1), "wabcut.dropShadow",
                           {EffectParam{"blue", ParamValue(0.2)}, EffectParam{"distance", ParamValue(20.0)}}))
            .state;
    Curve exposure;
    exposure.keys = {Keyframe{f(31), 0.0}, Keyframe{f(36), 1.5}};
    s = ok(edit::addEffect(s, tl.seq, std::span(&vb, 1), "wabcut.color.basic",
                           {EffectParam{"exposure", ParamValue(exposure)}, EffectParam{"saturation", ParamValue(120.0)}}))
            .state;
    const auto eq = ok(edit::addEffect(s, tl.seq, std::span(&aa, 1), "wabcut.audio.eq3", {EffectParam{"lowGain", ParamValue(-3.0)}}));
    s = eq.state;
    edit::EffectChange off;
    off.enabled = false;
    s = ok(edit::setEffect(s, tl.seq, aa, eq.createdEffects.front(), off)).state;
    const RoundTrip r = roundTrip(s, tl.seq, Format::Fcp7Xml);
    INFO(joined(r.differences));
    CHECK(r.differences.empty());
    CHECK(r.exported.data.find("<effectid>GaussianBlur</effectid>") != std::string::npos);
    // 0.6 (card #66): colour and EQ effects go out as Premiere's filters (Wabcut reads its own parameters back).
    CHECK(r.exported.data.find("<effectid>Lumetri</effectid>") != std::string::npos);
    CHECK(it::hasEntry(r.exported.report, ReportOutcome::Approximated, ReportCategory::Clip,
                       "Parametric EQ (3-band) is written as Premiere's Parametric Equalizer"));
}

namespace {

/// Times an export and the import of it for a sequence of `pairs` linked video + audio clips (ScaleProject.h).
void measure(int pairs, Format format) {
    wabcut::test::ScaleSpec spec;
    spec.media = 50;
    spec.sequences = 1;
    spec.clipPairsPerSequence = pairs;
    spec.nestingDepth = 0;
    spec.multicamSources = 0;
    spec.multicamClipsPerSource = 0;
    spec.effectsPerSequence = 0;
    spec.transitionsPerSequence = 0;
    spec.markersPerSequence = 0;
    spec.notesPerSequence = 0;
    const wabcut::test::ScaleProject project = wabcut::test::makeScaleProject(spec);
    const SequenceId seq = project.sequences.front();
    const auto t0 = std::chrono::steady_clock::now();
    const auto exported = exportSequence(project.state, seq, format);
    REQUIRE(exported.has_value());
    const auto t1 = std::chrono::steady_clock::now();
    const ImportResult back = it::importText(exported->data, format, {}, "perf" + exported->extension);
    const auto t2 = std::chrono::steady_clock::now();
    const auto ms = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    std::printf("X.1 perf: %d clips as %s: export %lld ms (%zu bytes), import %lld ms\n", pairs * 2,
                std::string(toString(format)).c_str(), static_cast<long long>(ms(t1 - t0)), exported->data.size(),
                static_cast<long long>(ms(t2 - t1)));
    CHECK(back.project.findSequence(back.sequences.front())->videoTracks.front()->clips.size() == static_cast<std::size_t>(pairs));
}

} // namespace

TEST_CASE("Export and import 3000 clips (X.1 perf)", "[interchange][export][perf]") {
    measure(1500, Format::Fcp7Xml);
    measure(1500, Format::Otio);
}

TEST_CASE("Export and import 50000 clips (X.1 perf - run by hand)", "[.perf]") {
    measure(25000, Format::Fcp7Xml);
    measure(25000, Format::Otio);
}

TEST_CASE("Export: marker highlights survive OTIO and go into the comment elsewhere (C.15)", "[interchange][export]") {
    auto tl = basicTimeline();
    Marker clutch{f(4), f(2), "Clutch"};
    clutch.comment = "1v4";
    clutch.highlight = MarkerHighlight{0.87, HighlightConfidence::High, {"kill streak", "mic peak"}, f(5), "highlights"};
    const ProjectState s = ok(edit::addMarker(tl.state, tl.seq, clutch)).state;
    // OTIO: carried exactly.
    const auto otio = exportSequence(s, tl.seq, Format::Otio);
    REQUIRE(otio.has_value());
    const ImportResult back = it::importText(otio->data, Format::Otio, {}, "export.otio");
    const Sequence& seq = *back.project.findSequence(back.sequences.front());
    const auto found = std::find_if(seq.markers.begin(), seq.markers.end(), [](const Marker& m) { return m.name == "Clutch"; });
    REQUIRE(found != seq.markers.end());
    REQUIRE(found->highlight.has_value());
    CHECK(*found->highlight == *clutch.highlight);
    // FCP7 XML and FCPXML: in the comment.
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml}) {
        INFO(toString(format));
        const auto exported = exportSequence(s, tl.seq, format);
        REQUIRE(exported.has_value());
        CHECK(exported->data.find("1v4 [highlight 0.87 high: kill streak; mic peak]") != std::string::npos);
    }
}

TEST_CASE("Export: Master Clip Effects are folded onto every clip and reported (0.6)", "[interchange][export]") {
    // Card #71: none of these formats has master clip effects, so each clip of A carries A's Gaussian Blur in its own
    // stack (before its own effects, as it plays) and the report says how many clips took them.
    wabcut::test::Timeline tl(R"(
        V1 tgt |A10aaaaaaaaB30bbbbbbbA50aaaaaaa|
        A1 tgt |A10aaaaaaaaB30bbbbbbbA50aaaaaaa|
    )");
    tl.linkMatching();
    ProjectState s = ok(edit::addEffect(tl.state, tl.seq, edit::EffectTarget::ofMedia(tl.media('A')), "wabcut.blur.gaussian",
                                        {EffectParam{"radius", ParamValue(12.0)}}))
                         .state;
    const ClipId last = tl.clip("V1@21");
    s = ok(edit::addEffect(s, tl.seq, std::span(&last, 1), "wabcut.color.basic", {EffectParam{"saturation", ParamValue(120.0)}}))
            .state;
    std::size_t folded = 0;
    const ProjectState flat = flattenMasterEffects(s, &folded);
    CHECK(folded == 2);
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(displayName(format));
        const auto exported = exportSequence(s, tl.seq, format);
        REQUIRE(exported.has_value());
        CHECK(it::hasEntry(exported->report, ReportOutcome::Approximated, ReportCategory::Effect,
                           "Master Clip Effects folded onto 2 clips"));
        // The file is the flattened project's.
        const auto expected = exportSequence(flat, tl.seq, format);
        REQUIRE(expected.has_value());
        CHECK(exported->data == expected->data);
        CHECK_FALSE(it::hasEntry(expected->report, ReportOutcome::Approximated, ReportCategory::Effect, "Master Clip Effects"));
    }
    // FCP7 XML brings the folded stacks back as each clip's own effects.
    const RoundTrip r = roundTrip(flat, tl.seq, Format::Fcp7Xml);
    INFO(joined(r.differences));
    CHECK(r.differences.empty());
    CHECK(r.exported.data.find("<effectid>GaussianBlur</effectid>") != std::string::npos);
}

// ---- 0.7 (card 046): the sequence work area and split edits (L/J cuts) -----------------------------------------------------

namespace {

/// A picture and its sound over three clips whose sound starts and ends apart from the picture: A's sound runs two frames
/// into B (an L cut, so B's sound starts late), C's sound starts two frames before its picture (a J cut). Linked in pairs.
wabcut::test::Timeline splitEditTimeline() {
    wabcut::test::Timeline tl(R"(
        V1 tgt |AaaaBbbbbbC2cc|
        A1 tgt |AaaaaaB2Cccccc|
    )");
    ProjectState& s = tl.state;
    const std::vector<std::pair<std::string, std::string>> pairs{{"V1@0", "A1@0"}, {"V1@4", "A1@6"}, {"V1@10", "A1@8"}};
    for (const auto& [video, audio] : pairs) {
        s = ok(edit::linkClips(s, tl.seq, std::vector<ClipId>{tl.clip(video), tl.clip(audio)})).state;
    }
    return tl;
}

std::string workAreaText(const ProjectState& s, SequenceId seq) {
    const auto& w = s.findSequence(seq)->workArea;
    return w ? std::to_string(w->start.value) + "+" + std::to_string(w->duration.value) : std::string("none");
}

} // namespace

TEST_CASE("Export: the sequence work area comes back from FCP7 XML, FCPXML and OTIO; an EDL says it has none (card 046)",
          "[interchange][export][workarea]") {
    auto tl = basicTimeline();
    tl.state = ok(edit::setWorkArea(tl.state, tl.seq, TimeRange::fromStartEnd(f(3), f(12)))).state;
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(displayName(format));
        Compare opt;
        opt.trackState = format != Format::FcpXml;
        opt.marks = format != Format::FcpXml;
        opt.mediaMarkers = format != Format::FcpXml;
        const RoundTrip r = roundTrip(tl.state, tl.seq, format, opt);
        INFO(joined(r.differences));
        CHECK(r.differences.empty());
        CHECK(workAreaText(r.imported.project, r.imported.sequences.front()) == workAreaText(tl.state, tl.seq));
        // The report says how far it travels.
        CHECK(it::hasEntry(r.exported.report, format == Format::Fcp7Xml ? ReportOutcome::Carried : ReportOutcome::Approximated,
                           ReportCategory::SequenceSetting, "work area"));
        CHECK(it::hasEntry(r.imported.report, ReportOutcome::Carried, ReportCategory::SequenceSetting, "work area"));
    }
    // FCP7 XML writes Premiere's own attributes (its ticks are Wabcut's).
    const auto fcp7 = exportSequence(tl.state, tl.seq, Format::Fcp7Xml);
    REQUIRE(fcp7.has_value());
    CHECK(fcp7->data.find("MZ.WorkInPoint=\"" + std::to_string(f(3).value) + "\"") != std::string::npos);
    CHECK(fcp7->data.find("MZ.WorkOutPoint=\"" + std::to_string(f(12).value) + "\"") != std::string::npos);
    // An EDL cannot say it.
    const auto edl = exportSequence(tl.state, tl.seq, Format::Edl);
    REQUIRE(edl.has_value());
    CHECK(it::hasEntry(edl->report, ReportOutcome::Dropped, ReportCategory::SequenceSetting, "work area"));
    // No work area: nothing is written, nothing comes back.
    const auto plain = basicTimeline();
    const auto plainXml = exportSequence(plain.state, plain.seq, Format::Fcp7Xml);
    REQUIRE(plainXml.has_value());
    CHECK(plainXml->data.find("MZ.Work") == std::string::npos);
    const auto plainFcpx = exportSequence(plain.state, plain.seq, Format::FcpXml);
    REQUIRE(plainFcpx.has_value());
    CHECK(plainFcpx->data.find("com.wabcut.workArea") == std::string::npos);
}

TEST_CASE("Import: a work area from Premiere's sequence attributes, snapped to frames; the whole sequence is none (card 046)",
          "[interchange][workarea]") {
    const auto tl = basicTimeline();
    const auto exported = exportSequence(tl.state, tl.seq, Format::Fcp7Xml);
    REQUIRE(exported.has_value());
    const auto withAttributes = [&](const std::string& in, const std::string& out) {
        std::string text = exported->data;
        const std::size_t at = text.find("<sequence ");
        REQUIRE(at != std::string::npos);
        text.insert(at + std::string("<sequence ").size(), "MZ.WorkInPoint=\"" + in + "\" MZ.WorkOutPoint=\"" + out + "\" ");
        return text;
    };
    // Premiere's ticks, a sub-frame In: kept inside the sequence and snapped to a frame, and the report says so.
    {
        const ImportResult r = it::importText(withAttributes(std::to_string(f(2).value + 5), std::to_string(f(9).value)), Format::Fcp7Xml);
        const Sequence& s = it::sequenceAt(r, 0);
        REQUIRE(s.workArea.has_value());
        CHECK(s.workArea->start == f(2));
        CHECK(s.workArea->end() == f(9));
        CHECK(it::hasEntry(r.report, ReportOutcome::Approximated, ReportCategory::SequenceSetting, "work area"));
    }
    // Premiere writes the whole sequence for a sequence nobody gave a work area: that is none.
    {
        const ImportResult r = it::importText(withAttributes("0", "91445760000000000"), Format::Fcp7Xml);
        CHECK_FALSE(it::sequenceAt(r, 0).workArea.has_value());
    }
    // An Out past the end is cut at the end.
    {
        const ImportResult r = it::importText(withAttributes(std::to_string(f(2).value), "91445760000000000"), Format::Fcp7Xml);
        const Sequence& s = it::sequenceAt(r, 0);
        REQUIRE(s.workArea.has_value());
        CHECK(s.workArea->start == f(2));
        CHECK(s.workArea->end() == s.duration);
    }
    // Nonsense (Out before In, negative, not a number) is ignored.
    for (const auto& bad : {std::pair<std::string, std::string>{"500", "100"}, {"-5", "100"}, {"abc", "def"}}) {
        const ImportResult r = it::importText(withAttributes(bad.first, bad.second), Format::Fcp7Xml);
        CHECK_FALSE(it::sequenceAt(r, 0).workArea.has_value());
    }
}

TEST_CASE("Export: a split edit keeps its own picture and sound extents in FCP7 XML, FCPXML and OTIO (card 046)",
          "[interchange][export][split]") {
    const auto tl = splitEditTimeline();
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(displayName(format));
        Compare opt;
        opt.trackState = format != Format::FcpXml;
        opt.marks = format != Format::FcpXml;
        opt.mediaMarkers = format != Format::FcpXml;
        const RoundTrip r = roundTrip(tl.state, tl.seq, format, opt);
        INFO(joined(r.differences));
        CHECK(r.differences.empty());
        CHECK(r.exported.report.count(ReportOutcome::Dropped) == 0);
        // The sound still starts and ends apart from the picture, and each pair is still linked.
        const Sequence& back = it::sequenceAt(r.imported, 0);
        REQUIRE_FALSE(back.audioTracks.empty());
        const auto& sound = back.audioTracks.front()->clips;
        REQUIRE(sound.size() == 3);
        CHECK(sound[0].start() == f(0));
        CHECK(sound[0].duration() == f(6)); // A's sound runs 2 frames into B
        CHECK(sound[1].start() == f(6));    // B's sound starts 2 frames after its picture
        CHECK(sound[1].sourceIn == f(2));
        CHECK(sound[2].start() == f(8));    // C's sound starts 2 frames before its picture
        CHECK(sound[2].sourceIn == Ticks{});
        CHECK(it::links(back).size() == 3);
    }
    // FCPXML: one clip per pair, the sound's extent in audioStart / audioDuration.
    const auto fcpx = exportSequence(tl.state, tl.seq, Format::FcpXml);
    REQUIRE(fcpx.has_value());
    CHECK(fcpx->data.find("audioStart=") != std::string::npos);
    CHECK(fcpx->data.find("audioDuration=") != std::string::npos);
    CHECK(it::hasEntry(fcpx->report, ReportOutcome::Carried, ReportCategory::Clip, "split edit"));
    CHECK_FALSE(it::hasEntry(fcpx->report, ReportOutcome::Approximated, ReportCategory::Clip, "shares time with a different V1 clip"));
}

TEST_CASE("Import: FCPXML audioStart and audioDuration give the sound its own extent; what cannot be is reported (card 046)",
          "[interchange][fcpxml][split]") {
    const auto tl = splitEditTimeline();
    const auto exported = exportSequence(tl.state, tl.seq, Format::FcpXml);
    REQUIRE(exported.has_value());
    const ImportResult r = it::importText(exported->data, Format::FcpXml);
    const Sequence& s = it::sequenceAt(r, 0);
    REQUIRE_FALSE(s.audioTracks.empty());
    const auto& sound = s.audioTracks.front()->clips;
    REQUIRE(sound.size() == 3);
    CHECK(sound[0].duration() == f(6));
    CHECK(sound[1].start() == f(6));
    CHECK(sound[2].start() == f(8));
    CHECK(it::hasEntry(r.report, ReportOutcome::Carried, ReportCategory::Clip, "split edit"));
    // A sound that would start before the media is cut there, and the report says so.
    std::string text = exported->data;
    const std::size_t at = text.find("audioStart=\"");
    REQUIRE(at != std::string::npos);
    const std::size_t end = text.find('"', at + std::string("audioStart=\"").size());
    text.replace(at, end - at + 1, "audioStart=\"-100s\"");
    const ImportResult cut = it::importText(text, Format::FcpXml);
    CHECK(it::hasEntry(cut.report, ReportOutcome::Approximated, ReportCategory::Clip, "split edit"));
}
