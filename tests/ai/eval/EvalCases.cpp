// The eval set (AI-15): 56 editing requests a person types into the assistant, over four fixture projects
// (EvalHarness.h). Each check reads the staged project and accepts any edit that does what was asked (a track fader or
// the clip's volume, any effect that carries the parameter); the reference is one way to do it, played by the oracle.
// "$media:A", "$bin:Footage" in a reference are the fixture's ids.

#include "EvalHarness.h"

#include <wabcut/model/Effects.h>

#include <cctype>
#include <cmath>

namespace wabcut::ai::eval {

using namespace wabcut::test;

namespace {

using Finding = std::optional<std::string>;

long long frameOf(Ticks t) { return t.value / f(1).value; }

const Sequence& seqOf(const ProjectState& s, SequenceId id) { return *s.findSequence(id); }

const Track* trackOf(const Sequence& seq, std::string_view label) {
    const auto index = static_cast<std::size_t>(std::stoi(std::string(label.substr(1))) - 1);
    const auto& list = label[0] == 'V' ? seq.videoTracks : seq.audioTracks;
    return index < list.size() ? list[index].get() : nullptr;
}

/// "0-125 150-275": the clips of a track in frames.
std::string spans(const ProjectState& s, SequenceId id, std::string_view label) {
    const Track* t = trackOf(seqOf(s, id), label);
    if (!t) {
        return "(no track)";
    }
    std::string out;
    for (const Clip& c : t->clips) {
        out += (out.empty() ? "" : " ") + std::to_string(frameOf(c.start())) + "-" + std::to_string(frameOf(c.end()));
    }
    return out;
}

Finding expectSpans(const ProjectState& s, SequenceId id, std::string_view label, std::string_view want) {
    const std::string got = spans(s, id, label);
    if (got == want) {
        return std::nullopt;
    }
    return std::string(label) + " is \"" + got + "\", expected \"" + std::string(want) + "\"";
}

const Clip* clipAt(const ProjectState& s, SequenceId id, std::string_view label, long long frame) {
    const Track* t = trackOf(seqOf(s, id), label);
    if (!t) {
        return nullptr;
    }
    for (const Clip& c : t->clips) {
        if (frameOf(c.start()) <= frame && frame < frameOf(c.end())) {
            return &c;
        }
    }
    return nullptr;
}

bool near(double a, double b, double tolerance = 0.01) { return std::abs(a - b) <= tolerance; }

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool contains(const std::string& text, std::string_view what) { return lower(text).find(lower(std::string(what))) != std::string::npos; }

const Marker* markerNamed(const Sequence& seq, std::string_view name) {
    for (const Marker& m : seq.markers) {
        if (lower(m.name) == lower(std::string(name))) {
            return &m;
        }
    }
    return nullptr;
}

const Marker* markerAt(const Sequence& seq, long long frame) {
    for (const Marker& m : seq.markers) {
        if (frameOf(m.start) == frame) {
            return &m;
        }
    }
    return nullptr;
}

Finding markerCount(const Sequence& seq, std::size_t n) {
    if (seq.markers.size() == n) {
        return std::nullopt;
    }
    return std::to_string(seq.markers.size()) + " markers, expected " + std::to_string(n);
}

/// The value of an effect parameter `key` on the clip, from any effect of its stack that has it.
std::optional<double> effectParam(const Clip& c, std::string_view key, std::string_view effect = {}) {
    for (const EffectInstance& e : c.content().effects) {
        if (!effect.empty() && e.effect != effect) {
            continue;
        }
        if (const ParamValue* v = e.find(key)) {
            return v->constant();
        }
    }
    return std::nullopt;
}

Finding expectEffect(const ProjectState& s, SequenceId id, long long frame, std::string_view key, double value,
                     std::string_view effect = {}) {
    const Clip* c = clipAt(s, id, "V1", frame);
    if (!c) {
        return "no clip on V1 at frame " + std::to_string(frame);
    }
    const auto v = effectParam(*c, key, effect);
    if (!v) {
        return "the clip at frame " + std::to_string(frame) + " has no " + std::string(key) + " effect parameter";
    }
    if (!near(*v, value)) {
        return std::string(key) + " is " + std::to_string(*v) + ", expected " + std::to_string(value);
    }
    return std::nullopt;
}

const CaptionTrack* firstCaptions(const ProjectState& s, SequenceId id) {
    const Sequence& seq = seqOf(s, id);
    return seq.captionTracks.empty() ? nullptr : &seq.captionTracks.front();
}

const MediaItem* mediaById(const ProjectState& s, MediaId id) { return s.findMedia(id); }

const MediaItem* mediaNamed(const ProjectState& s, std::string_view name) {
    for (const MediaPtr& m : s.media) {
        if (lower(m->name) == lower(std::string(name))) {
            return m.get();
        }
    }
    return nullptr;
}

Json clipRef(std::string_view track, std::string_view time) { return Json{{"track", track}, {"time", time}}; }

EvalCase make(std::string id, std::string category, std::string fixture, std::string prompt, std::vector<ToolUse> reference,
              Check check, std::string answer = "Done.") {
    EvalCase c;
    c.id = std::move(id);
    c.category = std::move(category);
    c.fixture = std::move(fixture);
    c.prompt = std::move(prompt);
    c.reference = std::move(reference);
    c.check = std::move(check);
    c.referenceAnswer = std::move(answer);
    return c;
}

EvalCase question(std::string id, std::string fixture, std::string prompt, std::vector<ToolUse> reference, std::string answer,
                  std::vector<std::string> anyOf) {
    EvalCase c = make(std::move(id), "question", std::move(fixture), std::move(prompt), std::move(reference),
                      [anyOf](const Fixture&, const ProjectState&, const agent::AgentOutcome& out) -> Finding {
                          for (const std::string& want : anyOf) {
                              if (contains(out.answer, want)) {
                                  return std::nullopt;
                              }
                          }
                          return "the answer \"" + out.answer.substr(0, 160) + "\" does not say " + anyOf.front();
                      },
                      std::move(answer));
    c.question = true;
    return c;
}

std::vector<EvalCase> buildCases() {
    std::vector<EvalCase> v;
    const std::string I = "interview";

    // --- Markers (interview: markers Hook at frame 10 and Outro at 350) ------------------------------------------
    v.push_back(make("markers-01", "markers", I, "Add a marker named Intro at 00:00:01:00.",
                     {{"marker_add", {{"time", "00:00:01:00"}, {"name", "Intro"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Sequence& q = seqOf(s, b.seq);
                         if (auto finding = markerCount(q, 3)) return finding;
                         const Marker* m = markerNamed(q, "Intro");
                         if (!m || frameOf(m->start) != 25) return std::string("no marker Intro at 00:00:01:00");
                         return std::nullopt;
                     }));
    v.push_back(make("markers-02", "markers", I, "Add a chapter marker called Part 2 at 6 seconds.",
                     {{"marker_add", {{"time", "00:00:06:00"}, {"name", "Part 2"}, {"type", "chapter"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Marker* m = markerNamed(seqOf(s, b.seq), "Part 2");
                         if (!m || frameOf(m->start) != 150) return std::string("no marker Part 2 at 00:00:06:00");
                         if (m->type != MarkerType::Chapter) return std::string("Part 2 is not a chapter marker");
                         return std::nullopt;
                     }));
    v.push_back(make("markers-03", "markers", I, "Put markers at 00:00:02:00, 00:00:04:00 and 00:00:08:00.",
                     {{"marker_add", {{"time", "00:00:02:00"}}}, {"marker_add", {{"time", "00:00:04:00"}}},
                      {"marker_add", {{"time", "00:00:08:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Sequence& q = seqOf(s, b.seq);
                         if (auto finding = markerCount(q, 5)) return finding;
                         for (long long at : {50, 100, 200}) {
                             if (!markerAt(q, at)) return "no marker at frame " + std::to_string(at);
                         }
                         return std::nullopt;
                     }));
    v.push_back(make("markers-04", "markers", I, "Add a red marker at the start of the third clip on V1.",
                     {{"marker_add", {{"time", "00:00:11:00"}, {"colorIndex", 1}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Sequence& q = seqOf(s, b.seq);
                         if (auto finding = markerCount(q, 3)) return finding;
                         const Marker* m = markerAt(q, 275);
                         if (!m) return std::string("no marker at 00:00:11:00 (the third clip starts there)");
                         if (m->colorIndex != 1) return "the marker's colour is " + std::to_string(m->colorIndex) + ", not red (1)";
                         return std::nullopt;
                     }));
    v.push_back(make("markers-05", "markers", I, "Rename the Outro marker to Credits.",
                     {{"marker_update", {{"index", 1}, {"name", "Credits"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Sequence& q = seqOf(s, b.seq);
                         if (auto finding = markerCount(q, 2)) return finding;
                         const Marker* m = markerNamed(q, "Credits");
                         if (!m || frameOf(m->start) != 350) return std::string("no marker Credits at 00:00:14:00");
                         if (markerNamed(q, "Outro")) return std::string("Outro is still there");
                         return std::nullopt;
                     }));
    v.push_back(make("markers-06", "markers", I, "Delete the marker named Hook.", {{"marker_remove", {{"index", 0}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Sequence& q = seqOf(s, b.seq);
                         if (auto finding = markerCount(q, 1)) return finding;
                         if (!markerNamed(q, "Outro")) return std::string("Outro was removed instead");
                         return std::nullopt;
                     }));
    v.push_back(make("markers-07", "markers", I, "Move the Outro marker to 00:00:13:00.",
                     {{"marker_update", {{"index", 1}, {"time", "00:00:13:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Sequence& q = seqOf(s, b.seq);
                         if (auto finding = markerCount(q, 2)) return finding;
                         const Marker* m = markerNamed(q, "Outro");
                         if (!m || frameOf(m->start) != 325) return std::string("Outro is not at 00:00:13:00");
                         return std::nullopt;
                     }));
    v.push_back(make("markers-08", "markers", I, "Add a range marker named Highlight that starts at 00:00:03:00 and lasts 2 seconds.",
                     {{"marker_add", {{"time", "00:00:03:00"}, {"name", "Highlight"}, {"duration", "00:00:02:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Marker* m = markerNamed(seqOf(s, b.seq), "Highlight");
                         if (!m || frameOf(m->start) != 75) return std::string("no marker Highlight at 00:00:03:00");
                         if (frameOf(m->duration) != 50) return "Highlight lasts " + std::to_string(frameOf(m->duration)) + " frames, not 50";
                         return std::nullopt;
                     }));

    // --- Trim (interview: V1/A1 A 0-125, gap, B 150-275, C 275-400; A2 music 0-400) ------------------------------
    v.push_back(make("trim-01", "trim", I, "Trim 10 frames off the end of the first clip.",
                     {{"clip_trim", {{"clip", clipRef("V1", "00:00:00:00")}, {"edge", "tail"}, {"delta", "-00:00:00:10"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-115 150-275 275-400")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-115 150-275 275-400");
                     }));
    v.push_back(make("trim-02", "trim", I, "Shorten the second clip by one second from its start, without moving anything else.",
                     {{"clip_trim", {{"clip", clipRef("V1", "00:00:06:00")}, {"edge", "head"}, {"delta", "00:00:01:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-125 175-275 275-400")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-125 175-275 275-400");
                     }));
    v.push_back(make("trim-03", "trim", I, "Extend the first clip by 10 frames into the gap after it.",
                     {{"clip_trim", {{"clip", clipRef("V1", "00:00:00:00")}, {"edge", "tail"}, {"delta", "00:00:00:10"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-135 150-275 275-400")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-135 150-275 275-400");
                     }));
    v.push_back(make("trim-04", "trim", I, "Ripple trim the first clip so it ends at 00:00:04:00 and the later clips move up.",
                     {{"clip_rippleTrim", {{"clip", clipRef("V1", "00:00:00:00")}, {"edge", "tail"}, {"delta", "-00:00:01:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-100 125-250 250-375")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-100 125-250 250-375");
                     }));
    v.push_back(make("trim-05", "trim", I, "Cut the third clip in two at 00:00:13:00.",
                     {{"sequence_addEdit", {{"time", "00:00:13:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         return expectSpans(s, b.seq, "V1", "0-125 150-275 275-325 325-400");
                     }));
    v.push_back(make("trim-06", "trim", I, "Split all tracks at 00:00:03:00.", {{"sequence_addEditToAllTracks", {{"time", "00:00:03:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-75 75-125 150-275 275-400")) return finding;
                         if (auto finding = expectSpans(s, b.seq, "A1", "0-75 75-125 150-275 275-400")) return finding;
                         return expectSpans(s, b.seq, "A2", "0-75 75-400");
                     }));
    v.push_back(make("trim-07", "trim", I, "Make the second clip play at half speed and push the later clips back.",
                     {{"clip_speedDuration", {{"clips", Json::array({clipRef("V1", "00:00:06:00")})}, {"speed", 50}, {"ripple", true}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-125 150-400 400-525")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-125 150-400 400-525");
                     }));

    // --- Ripple and delete ----------------------------------------------------------------------------------------
    v.push_back(make("ripple-01", "ripple", I, "Close the gap between the first and second clips.",
                     {{"timeline_rippleDelete", {{"gap", clipRef("V1", "00:00:05:12")}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-125 125-250 250-375")) return finding;
                         if (auto finding = expectSpans(s, b.seq, "A1", "0-125 125-250 250-375")) return finding;
                         return expectSpans(s, b.seq, "A2", "0-400");
                     }));
    v.push_back(make("ripple-02", "ripple", I, "Delete the second clip and close the gap it leaves.",
                     {{"timeline_rippleDelete", {{"clips", Json::array({clipRef("V1", "00:00:06:00")})}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-125 150-275")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-125 150-275");
                     }));
    v.push_back(make("ripple-03", "ripple", I, "Delete the first clip but leave a gap in its place.",
                     {{"edit_clear", {{"clips", Json::array({clipRef("V1", "00:00:00:00")})}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "150-275 275-400")) return finding;
                         return expectSpans(s, b.seq, "A1", "150-275 275-400");
                     }));
    v.push_back(make("ripple-04", "ripple", I, "Remove 00:00:02:00 to 00:00:03:00 from the video and its audio and close the gap.",
                     {{"sequence_extract", {{"in", "00:00:02:00"}, {"out", "00:00:03:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-50 50-100 125-250 250-375")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-50 50-100 125-250 250-375");
                     }));
    v.push_back(make("ripple-05", "ripple", I, "Lift 00:00:08:00 to 00:00:09:00 out of the video and its audio, leaving a gap.",
                     {{"sequence_lift", {{"in", "00:00:08:00"}, {"out", "00:00:09:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-125 150-200 225-275 275-400")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-125 150-200 225-275 275-400");
                     }));
    v.push_back(make("ripple-06", "ripple", I, "Remove the music clip on A2.",
                     {{"edit_clear", {{"clips", Json::array({clipRef("A2", "00:00:00:00")})}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "A2", "")) return finding;
                         return expectSpans(s, b.seq, "V1", "0-125 150-275 275-400");
                     }));
    v.push_back(make("ripple-07", "ripple", I, "Move the third clip so it starts at 00:00:20:00 and leave a gap before it.",
                     {{"clip_move", {{"clips", Json::array({clipRef("V1", "00:00:12:00")})}, {"delta", "00:00:09:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (auto finding = expectSpans(s, b.seq, "V1", "0-125 150-275 500-625")) return finding;
                         return expectSpans(s, b.seq, "A1", "0-125 150-275 500-625");
                     }));

    // --- Captions (captions: English cues 0-37 "Welcome...", 50-100, 150-200 "Let's get startd", 250-300) ---------
    const std::string C = "captions";
    v.push_back(make("captions-01", "captions", C, "Fix the typo in the third caption: it should read \"Let's get started.\"",
                     {{"captions_updateCue", {{"cue", 2}, {"text", "Let's get started."}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const CaptionTrack* t = firstCaptions(s, b.seq);
                         if (!t || t->cues.size() != 4) return std::string("the caption track does not have 4 cues");
                         const CaptionCue& c = t->cues[2];
                         if (c.text != "Let's get started.") return "the third caption reads \"" + c.text + "\"";
                         if (frameOf(c.start) != 150 || frameOf(c.end()) != 200) return std::string("the third caption moved");
                         return std::nullopt;
                     }));
    v.push_back(make("captions-02", "captions", C, "Add a caption \"See you next time.\" from 00:00:13:00 to 00:00:15:00.",
                     {{"captions_addCue", {{"start", "00:00:13:00"}, {"end", "00:00:15:00"}, {"text", "See you next time."}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const CaptionTrack* t = firstCaptions(s, b.seq);
                         if (!t || t->cues.size() != 5) return std::string("the caption track does not have 5 cues");
                         const CaptionCue& c = t->cues.back();
                         if (c.text != "See you next time." || frameOf(c.start) != 325 || frameOf(c.end()) != 375)
                             return "the new caption is \"" + c.text + "\" at " + std::to_string(frameOf(c.start)) + "-" + std::to_string(frameOf(c.end()));
                         return std::nullopt;
                     }));
    v.push_back(make("captions-03", "captions", C, "Delete the last caption.", {{"captions_removeCue", {{"cue", 3}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const CaptionTrack* t = firstCaptions(s, b.seq);
                         if (!t || t->cues.size() != 3) return std::string("the caption track does not have 3 cues");
                         if (t->cues.back().text != "Let's get startd") return std::string("another caption was deleted");
                         return std::nullopt;
                     }));
    v.push_back(make("captions-04", "captions", C, "Shift all the captions 10 frames later.",
                     {{"captions_shiftCues", {{"delta", "00:00:00:10"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const CaptionTrack* t = firstCaptions(s, b.seq);
                         if (!t || t->cues.size() != 4) return std::string("the caption track does not have 4 cues");
                         const long long want[] = {10, 60, 160, 260};
                         for (std::size_t i = 0; i < 4; ++i) {
                             if (frameOf(t->cues[i].start) != want[i])
                                 return "caption " + std::to_string(i + 1) + " starts at frame " + std::to_string(frameOf(t->cues[i].start));
                         }
                         return std::nullopt;
                     }));
    v.push_back(make("captions-05", "captions", C, "Make the first caption end at 00:00:02:00.",
                     {{"captions_updateCue", {{"cue", 0}, {"end", "00:00:02:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const CaptionTrack* t = firstCaptions(s, b.seq);
                         if (!t || t->cues.size() != 4) return std::string("the caption track does not have 4 cues");
                         if (frameOf(t->cues[0].start) != 0 || frameOf(t->cues[0].end()) != 50)
                             return "the first caption is " + std::to_string(frameOf(t->cues[0].start)) + "-" + std::to_string(frameOf(t->cues[0].end()));
                         return std::nullopt;
                     }));
    v.push_back(make("captions-06", "captions", C, "Add a new caption track called Thai for the language th.",
                     {{"captions_addTrack", {{"name", "Thai"}, {"language", "th"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Sequence& q = seqOf(s, b.seq);
                         if (q.captionTracks.size() != 2) return std::to_string(q.captionTracks.size()) + " caption tracks, expected 2";
                         for (const CaptionTrack& t : q.captionTracks) {
                             if (t.name == "Thai" && t.language == "th") return std::nullopt;
                         }
                         return std::string("no caption track Thai (th)");
                     }));
    v.push_back(make("captions-07", "captions", C, "Make the caption text bigger: font size 0.06 of the frame height.",
                     {{"captions_setStyle", {{"style", {{"fontSize", 0.06}}}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const CaptionTrack* t = firstCaptions(s, b.seq);
                         if (!t || !near(t->style.fontSize, 0.06, 1e-4)) return std::string("the font size is not 0.06");
                         return std::nullopt;
                     }));

    // --- Audio --------------------------------------------------------------------------------------------------
    v.push_back(make("audio-01", "audio", I, "Lower the music on A2 by 6 dB.", {{"track_setVolume", {{"track", "A2"}, {"db", -6}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Track* t = trackOf(seqOf(s, b.seq), "A2");
                         const Clip* m = clipAt(s, b.seq, "A2", 0);
                         const bool fader = t && t->volume.isConstant() && near(t->volume.constant(), -6.0);
                         const bool clip = m && near(m->parameterAt(ParamId::VolumeDb, m->start()), -6.0);
                         if (!fader && !clip) return std::string("neither A2's fader nor the music clip is at -6 dB");
                         return std::nullopt;
                     }));
    v.push_back(make("audio-02", "audio", I, "Mute track A2.", {{"track_setState", {{"track", "A2"}, {"muted", true}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Track* t = trackOf(seqOf(s, b.seq), "A2");
                         if (!t || !t->muted) return std::string("A2 is not muted");
                         return std::nullopt;
                     }));
    v.push_back(make("audio-03", "audio", I, "Fade out the music over the last 2 seconds.",
                     {{"clip_fade", {{"clips", Json::array({clipRef("A2", "00:00:00:00")})}, {"duration", "00:00:02:00"}, {"edge", "out"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Clip* m = clipAt(s, b.seq, "A2", 0);
                         if (!m) return std::string("the music clip is gone");
                         const double before = m->parameterAt(ParamId::VolumeDb, f(345));
                         const double end = m->parameterAt(ParamId::VolumeDb, f(399));
                         if (end > -40.0) return "the music ends at " + std::to_string(end) + " dB";
                         if (before < -1.0) return std::string("the fade starts before the last 2 seconds");
                         return std::nullopt;
                     }));
    v.push_back(make("audio-04", "audio", I, "Set the first clip's audio to -3 dB.",
                     {{"clip_setParam", {{"clips", Json::array({clipRef("A1", "00:00:00:00")})}, {"param", "volume.level"}, {"value", -3}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Clip* c = clipAt(s, b.seq, "A1", 0);
                         if (!c || !near(c->parameterAt(ParamId::VolumeDb, c->start()), -3.0)) return std::string("the first clip's audio is not at -3 dB");
                         return std::nullopt;
                     }));
    v.push_back(make("audio-05", "audio", I, "Fade in the first clip's audio over half a second.",
                     {{"clip_fade", {{"clips", Json::array({clipRef("A1", "00:00:00:00")})}, {"duration", "00:00:00:12"}, {"edge", "in"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Clip* c = clipAt(s, b.seq, "A1", 0);
                         if (!c) return std::string("the first clip is gone");
                         if (c->parameterAt(ParamId::VolumeDb, f(0)) > -40.0) return std::string("the audio does not start silent");
                         if (c->parameterAt(ParamId::VolumeDb, f(20)) < -1.0) return std::string("the fade is longer than half a second");
                         return std::nullopt;
                     }));
    v.push_back(make("audio-06", "audio", I, "Pan the music on A2 fully to the left.", {{"track_setPan", {{"track", "A2"}, {"pan", -1}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Track* t = trackOf(seqOf(s, b.seq), "A2");
                         const Clip* m = clipAt(s, b.seq, "A2", 0);
                         const bool track = t && t->pan.isConstant() && near(t->pan.constant(), -1.0);
                         const bool clip = m && near(m->parameterAt(ParamId::Pan, m->start()), -1.0);
                         if (!track && !clip) return std::string("the music is not panned fully left");
                         return std::nullopt;
                     }));
    v.push_back(make("audio-07", "audio", I, "Add a volume keyframe on track A2 at 00:00:10:00 with the level at -12 dB.",
                     {{"track_setVolume", {{"track", "A2"}, {"db", -12}, {"time", "00:00:10:00"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Track* t = trackOf(seqOf(s, b.seq), "A2");
                         if (!t || t->volume.isConstant()) return std::string("A2's fader has no keyframes");
                         if (!near(t->volume.valueAt(f(250)), -12.0)) return "A2 is at " + std::to_string(t->volume.valueAt(f(250))) + " dB at 00:00:10:00";
                         return std::nullopt;
                     }));

    // --- Colour ---------------------------------------------------------------------------------------------------
    v.push_back(make("colour-01", "colour", I, "Make the first clip warmer: colour temperature +30.",
                     {{"clip_addEffect", {{"clips", Json::array({clipRef("V1", "00:00:00:00")})}, {"effect", "wabcut.color.basic"}, {"params", {{"temperature", 30}}}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding { return expectEffect(s, b.seq, 0, "temperature", 30); }));
    v.push_back(make("colour-02", "colour", I, "Make the second clip black and white.",
                     {{"clip_addEffect", {{"clips", Json::array({clipRef("V1", "00:00:06:00")})}, {"effect", "wabcut.color.basic"}, {"params", {{"saturation", 0}}}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding { return expectEffect(s, b.seq, 150, "saturation", 0); }));
    v.push_back(make("colour-03", "colour", I, "Brighten the third clip by one stop of exposure.",
                     {{"clip_addEffect", {{"clips", Json::array({clipRef("V1", "00:00:12:00")})}, {"effect", "wabcut.color.basic"}, {"params", {{"exposure", 1}}}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding { return expectEffect(s, b.seq, 275, "exposure", 1); }));
    v.push_back(make("colour-04", "colour", I, "Raise the contrast of all three video clips by 20.",
                     {{"clip_addEffect", {{"clips", Json::array({clipRef("V1", "00:00:00:00"), clipRef("V1", "00:00:06:00"), clipRef("V1", "00:00:12:00")})},
                                          {"effect", "wabcut.color.basic"}, {"params", {{"contrast", 20}}}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         for (long long at : {0, 150, 275}) {
                             if (auto finding = expectEffect(s, b.seq, at, "contrast", 20)) return finding;
                         }
                         return std::nullopt;
                     }));
    v.push_back(make("colour-05", "colour", I, "Darken the edges of the first clip with a vignette, amount -1.5.",
                     {{"clip_addEffect", {{"clips", Json::array({clipRef("V1", "00:00:00:00")})}, {"effect", "wabcut.vignette"}, {"params", {{"amount", -1.5}}}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         return expectEffect(s, b.seq, 0, "amount", -1.5, "wabcut.vignette");
                     }));
    v.push_back(make("colour-06", "colour", I, "Set the label colour of the second clip to 4.",
                     {{"clip_setColor", {{"clips", Json::array({clipRef("V1", "00:00:06:00")})}, {"colorIndex", 4}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const Clip* c = clipAt(s, b.seq, "V1", 150);
                         if (!c || c->colorIndex != 4) return std::string("the second clip's label colour is not 4");
                         return std::nullopt;
                     }));

    // --- Multicam (one multicam clip 0-1000 on V1/A1, angles Host, Guest, Wide; angle 1 plays) ---------------------
    const std::string M = "multicam";
    auto angleSpans = [](const ProjectState& s, SequenceId seq, std::string_view label) {
        std::string out;
        const Track* t = trackOf(seqOf(s, seq), label);
        if (t) {
            for (const Clip& c : t->clips) {
                out += (out.empty() ? "" : " ") + std::to_string(frameOf(c.start())) + "-" + std::to_string(frameOf(c.end())) + ":" +
                       std::to_string(c.angles.first() + 1);
            }
        }
        return out;
    };
    auto expectAngles = [angleSpans](std::string label, std::string want) {
        return [angleSpans, label, want](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
            const std::string got = angleSpans(s, b.seq, label);
            if (got == want) return std::nullopt;
            return label + " plays \"" + got + "\", expected \"" + want + "\" (range:angle)";
        };
    };
    v.push_back(make("multicam-01", "multicam", M, "Switch the multicam clip to angle 2.",
                     {{"multicam_switchAngle", {{"clip", clipRef("V1", "00:00:00:00")}, {"angle", 2}}}}, expectAngles("V1", "0-1000:2")));
    v.push_back(make("multicam-02", "multicam", M, "Cut to camera 3 at 00:00:05:00.",
                     {{"multicam_switchAngle", {{"clip", clipRef("V1", "00:00:00:00")}, {"angle", 3}, {"at", "00:00:05:00"}}}},
                     expectAngles("V1", "0-125:1 125-1000:3")));
    v.push_back(make("multicam-03", "multicam", M, "Switch to angle 2 at 00:00:04:00 and back to angle 1 at 00:00:08:00.",
                     {{"multicam_switchAngle", {{"clip", clipRef("V1", "00:00:00:00")},
                                                {"switches", Json::array({{{"angle", 2}, {"at", "00:00:04:00"}}, {{"angle", 1}, {"at", "00:00:08:00"}}})}}}},
                     expectAngles("V1", "0-100:1 100-200:2 200-1000:1")));
    v.push_back(make("multicam-04", "multicam", M, "Show the Wide angle (angle 3) in the picture only and keep the sound from angle 1.",
                     {{"multicam_switchAngle", {{"clip", clipRef("V1", "00:00:00:00")}, {"angle", 3}, {"target", "video"}}}},
                     [expectAngles](const Fixture& b, const ProjectState& s, const agent::AgentOutcome& o) -> Finding {
                         if (auto finding = expectAngles("V1", "0-1000:3")(b, s, o)) return finding;
                         return expectAngles("A1", "0-1000:1")(b, s, o);
                     }));

    // --- Library (bins Footage and Music; media A, B, C, M at the root) --------------------------------------------
    const std::string L = "library";
    v.push_back(make("library-01", "library", L, "Create a bin called Selects.", {{"bin_create", {{"name", "Selects"}}}},
                     [](const Fixture&, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         if (s.bins.size() != 3) return std::to_string(s.bins.size()) + " bins, expected 3";
                         for (const Bin& bin : s.bins) {
                             if (bin.name == "Selects") return std::nullopt;
                         }
                         return std::string("no bin Selects");
                     }));
    v.push_back(make("library-02", "library", L, "Move media A, B and C into the Footage bin.",
                     {{"bin_move", {{"media", Json::array({"$media:A", "$media:B", "$media:C"})}, {"target", "$bin:Footage"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const BinId footage = b.bins.at("Footage");
                         for (char letter : {'A', 'B', 'C'}) {
                             const MediaItem* m = mediaById(s, b.media.at(letter));
                             if (!m || m->bin != footage) return std::string("media ") + letter + " is not in Footage";
                         }
                         const MediaItem* music = mediaById(s, b.media.at('M'));
                         if (!music || music->bin.isValid()) return std::string("media M moved too");
                         return std::nullopt;
                     }));
    v.push_back(make("library-03", "library", L, "Add the comment \"best take\" to media B.",
                     {{"media_setAttributes", {{"media", Json::array({"B"})}, {"comment", "best take"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const MediaItem* m = mediaById(s, b.media.at('B'));
                         if (!m || lower(m->comment) != "best take") return std::string("media B's comment is not \"best take\"");
                         return std::nullopt;
                     }));
    v.push_back(make("library-04", "library", L, "Rename media C to Closing shot.",
                     {{"media_setAttributes", {{"media", Json::array({"C"})}, {"name", "Closing shot"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const MediaItem* m = mediaById(s, b.media.at('C'));
                         if (!m || m->name != "Closing shot") return std::string("media C is not named Closing shot");
                         return std::nullopt;
                     }));
    v.push_back(make("library-05", "library", L, "Make a subclip of media A from 00:00:01:00 to 00:00:03:00 named Laugh.",
                     {{"media_makeSubclip", {{"media", "A"}, {"in", "00:00:01:00"}, {"out", "00:00:03:00"}, {"name", "Laugh"}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const MediaItem* m = mediaNamed(s, "Laugh");
                         if (!m || !m->subclip) return std::string("no subclip Laugh");
                         if (m->subclip->master != b.media.at('A')) return std::string("Laugh is not a subclip of A");
                         if (frameOf(m->subclip->range.start) != 25 || frameOf(m->subclip->range.end()) != 75)
                             return "Laugh covers " + std::to_string(frameOf(m->subclip->range.start)) + "-" + std::to_string(frameOf(m->subclip->range.end()));
                         return std::nullopt;
                     }));
    v.push_back(make("library-06", "library", L, "Set the label colour of media M to 6.",
                     {{"media_setAttributes", {{"media", Json::array({"M"})}, {"colorIndex", 6}}}},
                     [](const Fixture& b, const ProjectState& s, const agent::AgentOutcome&) -> Finding {
                         const MediaItem* m = mediaById(s, b.media.at('M'));
                         if (!m || m->colorIndex != 6) return std::string("media M's label colour is not 6");
                         return std::nullopt;
                     }));

    // --- Questions (answered without editing) ---------------------------------------------------------------------
    v.push_back(question("question-01", I, "How many clips are on track V1? Just tell me, do not change anything.",
                         {{"sequence_summary", Json::object()}}, "There are 3 clips on V1.", {"3", "three"}));
    v.push_back(question("question-02", I, "How long is the sequence? Do not change anything.", {{"sequence_summary", Json::object()}},
                         "The sequence is 16 seconds long (00:00:16:00).", {"16", "00:00:16:00"}));
    v.push_back(question("question-03", I, "What is the marker at 00:00:14:00 called? Do not change anything.",
                         {{"sequence_summary", Json::object()}}, "It is called Outro.", {"Outro"}));
    v.push_back(question("question-04", C, "What does the second caption say? Do not change anything.", {{"captions_get", Json::object()}},
                         "It says: Today we talk about editing.", {"talk about editing"}));
    return v;
}

} // namespace

const std::vector<EvalCase>& evalCases() {
    static const std::vector<EvalCase> cases = buildCases();
    return cases;
}

} // namespace wabcut::ai::eval
