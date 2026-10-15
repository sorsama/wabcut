#include <wabcut/ai/multicam/MulticamAiCommands.h>

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Schema.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace wabcut::ai::multicam {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }
double secondsOf(Ticks t) { return round3(static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond)); }
Ticks ticksOf(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

bool unity(const Clip& c) { return !c.isRemapped() && !c.reversed && c.speed.isUnity(); }

std::vector<std::string> angleNames(const Sequence& source) {
    std::vector<std::string> names;
    if (source.multicam) {
        for (std::size_t i = 0; i < source.multicam->angles.size(); ++i) {
            const std::string& n = source.multicam->angles[i].name;
            names.push_back(n.empty() ? "Angle " + std::to_string(i + 1) : n);
        }
    }
    return names;
}

std::vector<AngleOverride> overridesOf(ExecutionContext& c) {
    std::vector<AngleOverride> out;
    if (!c.has("angles")) {
        return out;
    }
    const Json& list = c.params()["angles"];
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& a = list[i];
        AngleOverride o;
        o.angle = c.indexValue(a["angle"], "/angles/" + std::to_string(i) + "/angle") - 1;
        const std::string role = a.value("role", std::string("closeUp"));
        o.role = role == "wide" ? AngleRole::Wide : role == "unknown" ? AngleRole::Unknown : AngleRole::CloseUp;
        if (a.contains("speaker") && a["speaker"].is_string()) {
            o.speakers.push_back(a["speaker"].get<std::string>());
        }
        out.push_back(std::move(o));
    }
    return out;
}

SwitchOptions switchOptionsOf(ExecutionContext& c) {
    const std::string preset = c.string("preset", "interview");
    auto options = presetOptions(preset);
    if (!options) {
        fail(ErrorCode::InvalidParams, "unknown preset \"" + preset + "\" (calm, interview, energetic)", Json{{"path", "/preset"}});
    }
    const auto seconds = [&](const char* key, Ticks& field) {
        if (c.has(key)) {
            field = ticksOf(c.number(key));
        }
    };
    seconds("minHold", options->minHold);
    seconds("lead", options->lead);
    seconds("overlapMin", options->overlapMin);
    seconds("silenceWide", options->silenceWide);
    seconds("varietyAfter", options->varietyAfter);
    seconds("varietyLength", options->varietyLength);
    seconds("reactionHold", options->reactionHold);
    seconds("openLength", options->openLength);
    seconds("maxSnap", options->maxSnap);
    options->wideOnOverlap = c.boolean("wideOnOverlap", options->wideOnOverlap);
    options->wideOnLaughter = c.boolean("wideOnLaughter", options->wideOnLaughter);
    options->reactions = c.boolean("reactions", options->reactions);
    options->openWide = c.boolean("openWide", options->openWide);
    options->avoidMidWord = c.boolean("avoidMidWord", options->avoidMidWord);
    if (c.has("reactionStrength")) {
        options->reactionStrength = c.number("reactionStrength");
    }
    return *options;
}

/// The multicam clip a command means: "clip", else the sequence's first multicam clip.
ClipId multicamClipOf(ExecutionContext& c) {
    if (c.has("clip")) {
        return c.clip("clip");
    }
    for (const auto* tracks : {&c.sequence().videoTracks, &c.sequence().audioTracks}) {
        for (const TrackPtr& t : *tracks) {
            for (const Clip& clip : t->clips) {
                if (multicamSourceOf(c.state(), clip)) {
                    return clip.id;
                }
            }
        }
    }
    fail(ErrorCode::InvalidParams, "the sequence has no multicam clip: pass \"clip\"", Json{{"path", "/clip"}});
}

EvidenceProgress progressOf(ExecutionContext& c) {
    return [&c](double fraction, const std::string& what) {
        c.checkpoint(what + " (" + std::to_string(static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0))) + "%)");
        return true;
    };
}

SpeakerRun runSpeakers(ExecutionContext& c, SequenceId source, const std::vector<AngleOverride>& overrides) {
    SpeakerOptions options;
    options.overrides = overrides;
    EvidenceOptions evidence;
    evidence.faces = c.boolean("faces", true);
    auto run = speakersOf(c.state(), source, c.services(), options, progressOf(c), {}, evidence);
    if (!run) {
        fail(run.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled
             : run.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound
                                                               : ErrorCode::EditRejected,
             run.error().message);
    }
    return std::move(*run);
}

} // namespace

std::expected<SpeakerRun, Error> speakersOf(const ProjectState& state, SequenceId source, command::CommandServices* services,
                                            const SpeakerOptions& options, const EvidenceProgress& progress, std::stop_token stop,
                                            const EvidenceOptions& evidenceOptions) {
    auto* transcripts = command::findService<ai::edit::TranscriptServices>(services);
    auto* speakerServices = command::findService<SpeakerServices>(services);
    auto evidence = gatherEvidence(state, source, transcripts ? transcripts->transcripts.get() : nullptr,
                                   speakerServices ? speakerServices->angleSignals.get() : nullptr, evidenceOptions, progress, stop);
    if (!evidence) {
        return std::unexpected(evidence.error());
    }
    SpeakerRun run;
    run.timeline = analyzeSpeakers(*evidence, options);
    run.evidence = std::move(*evidence);
    return run;
}

std::expected<AutoSwitchResult, Error> autoSwitch(const ProjectState& state, SequenceId sequence, ClipId clipId, const SpeakerRun& speakers,
                                                  const SwitchOptions& options, wabcut::edit::AngleTarget target) {
    const auto invalid = [](std::string m) { return std::unexpected(Error{wabcut::ErrorCode::InvalidArgument, std::move(m)}); };
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return std::unexpected(Error{wabcut::ErrorCode::NotFound, "no sequence " + std::to_string(sequence.value)});
    }
    const auto loc = seq->findClip(clipId);
    if (!loc) {
        return std::unexpected(Error{wabcut::ErrorCode::NotFound, "no clip " + std::to_string(clipId.value) + " in '" + seq->name + "'"});
    }
    Clip clip = loc->clip();
    const auto source = multicamSourceOf(state, clip);
    if (!source) {
        return invalid("clip " + std::to_string(clipId.value) + " is not a multicam clip");
    }
    // The picture: the clip itself, or its linked picture.
    const Track* track = loc->track;
    if (clip.stream.kind == StreamKind::Audio) {
        bool found = false;
        for (const TrackPtr& t : seq->videoTracks) {
            for (const Clip& v : t->clips) {
                if (clip.linkGroup.isValid() && v.linkGroup == clip.linkGroup && v.sequence == clip.sequence) {
                    clip = v;
                    track = t.get();
                    found = true;
                }
            }
        }
        if (!found && target == wabcut::edit::AngleTarget::Video) {
            return invalid("the multicam clip has no picture to switch");
        }
    }
    if (track->locked) {
        return invalid(track->name + " is locked");
    }
    AutoSwitchResult result;
    result.sequence = sequence;
    result.source = *source;
    // Every piece of this multicam source on the track, in time order.
    std::vector<Clip> pieces;
    for (const Clip& p : track->clips) {
        if (p.sequence == *source) {
            if (!unity(p)) {
                result.warnings.push_back("a piece at " + std::to_string(secondsOf(p.start())) + " s is retimed: left as it is");
                continue;
            }
            pieces.push_back(p);
        }
    }
    if (pieces.empty()) {
        return invalid("the multicam clip has no piece at normal speed");
    }
    Ticks from = pieces.front().sourceIn;
    Ticks to = pieces.front().sourceOut();
    for (const Clip& p : pieces) {
        from = std::min(from, p.sourceIn);
        to = std::max(to, p.sourceOut());
    }
    const Sequence* src = state.findSequence(*source);
    const std::vector<std::string> names = angleNames(*src);
    const std::vector<TimeRange> words = sourceWords(speakers.evidence);
    result.plan = planSwitches(speakers.timeline, words, names, TimeRange::fromStartEnd(from, to), options);
    result.warnings.insert(result.warnings.end(), result.plan.warnings.begin(), result.plan.warnings.end());
    if (result.plan.cuts.empty()) {
        result.state = state;
        return result;
    }
    const auto inForce = [&](Ticks t) -> const PlannedCut& {
        const PlannedCut* c = &result.plan.cuts.front();
        for (const PlannedCut& x : result.plan.cuts) {
            if (x.at <= t) {
                c = &x;
            }
        }
        return *c;
    };
    const std::size_t angleCount = src->multicam ? src->multicam->angles.size() : 0;
    ProjectState s = state;
    const auto pieceAt = [&](Ticks t) -> std::optional<Clip> {
        const Track* tr = s.findSequence(sequence)->findTrack(track->id);
        for (const Clip& p : tr->clips) {
            if (p.sequence == *source && p.start() <= t && t < p.end()) {
                return p;
            }
        }
        return std::nullopt;
    };
    // Cuts closer than this to a piece's edge become the piece's own angle (the next piece's, at its end): no slivers.
    const Ticks margin{kTicksPerSecond / 2};
    std::optional<std::size_t> showing; // the angle on screen so far, in timeline order
    for (const Clip& original : pieces) {
        const Ticks a = original.sourceIn;
        const Ticks b = original.sourceOut();
        // The piece starts on the angle in force just after its start (a cut within the margin moves to the start).
        const PlannedCut& first = inForce(std::min(a + margin, b - Ticks{1}));
        if (first.angle < angleCount) {
            if (auto p = pieceAt(original.start())) {
                if (p->angles.first() != first.angle) {
                    auto switched = wabcut::edit::switchMulticamAngle(s, sequence, p->id, original.start(), first.angle, target);
                    if (!switched) {
                        return std::unexpected(switched.error());
                    }
                    s = std::move(switched->state);
                }
                if (showing != first.angle) {
                    AppliedCut ac;
                    ac.cut = first;
                    ac.timeline = original.start();
                    if (auto now = pieceAt(original.start())) {
                        ac.clip = now->id;
                    }
                    result.cuts.push_back(std::move(ac));
                    showing = first.angle;
                }
            }
        }
        // Then every change inside it, away from its edges.
        for (const PlannedCut& cut : result.plan.cuts) {
            if (cut.at <= a + margin || cut.at >= b - margin || cut.angle >= angleCount || showing == cut.angle) {
                continue;
            }
            const Ticks t = original.start() + (cut.at - a);
            const auto p = pieceAt(t);
            if (!p) {
                continue;
            }
            auto switched = wabcut::edit::switchMulticamAngle(s, sequence, p->id, t, cut.angle, target);
            if (!switched) {
                return std::unexpected(switched.error());
            }
            s = std::move(switched->state);
            AppliedCut ac;
            ac.cut = cut;
            ac.timeline = t;
            if (auto now = pieceAt(t)) {
                ac.clip = now->id;
            }
            result.cuts.push_back(std::move(ac));
            showing = cut.angle;
        }
    }
    std::sort(result.cuts.begin(), result.cuts.end(), [](const AppliedCut& a, const AppliedCut& b) { return a.timeline < b.timeline; });
    result.state = std::move(s);
    return result;
}

Json speakersToJson(const SpeakerRun& run, std::size_t segmentLimit) {
    const SpeakerTimeline& t = run.timeline;
    Json speakers = Json::array();
    for (std::size_t i = 0; i < t.speakers.size(); ++i) {
        const Speaker& s = t.speakers[i];
        Json row{{"index", i}, {"key", s.key}, {"name", s.name}, {"seconds", round3(s.seconds)}};
        if (s.project.isValid()) {
            row["speaker"] = s.project.value;
        }
        if (s.mic) {
            row["mic"] = *s.mic + 1;
        }
        speakers.push_back(std::move(row));
    }
    Json angles = Json::array();
    for (const AngleAssignment& a : t.angles) {
        Json row{{"angle", a.angle + 1},
                 {"name", a.angle < run.evidence.angles.size() ? run.evidence.angles[a.angle].name : std::string()},
                 {"role", std::string(toString(a.role))},
                 {"confidence", round3(a.confidence)},
                 {"confirmed", a.confirmed},
                 {"reason", a.reason}};
        Json who = Json::array();
        for (const std::size_t s : a.speakers) {
            who.push_back(t.speakers[s].name);
        }
        row["speakers"] = std::move(who);
        if (a.face) {
            row["face"] = Json{{"time", secondsOf(a.face->time)}, {"ticks", a.face->time.value}, {"x", round3(a.face->x)},
                               {"y", round3(a.face->y)}, {"w", round3(a.face->w)}, {"h", round3(a.face->h)}};
        }
        angles.push_back(std::move(row));
    }
    Json segments = Json::array();
    double overlap = 0, silence = 0, nonSpeech = 0;
    for (const SpeakerSegment& s : t.segments) {
        const double d = static_cast<double>(s.range.duration.value) / static_cast<double>(kTicksPerSecond);
        overlap += s.kind == SegmentKind::Overlap ? d : 0;
        silence += s.kind == SegmentKind::Silence ? d : 0;
        nonSpeech += s.kind == SegmentKind::NonSpeech ? d : 0;
        if (segments.size() >= segmentLimit) {
            continue;
        }
        Json who = Json::array();
        for (const std::size_t i : s.speakers) {
            who.push_back(i);
        }
        segments.push_back(Json{{"start", secondsOf(s.range.start)}, {"end", secondsOf(s.range.end())}, {"kind", std::string(toString(s.kind))},
                                {"speakers", std::move(who)}, {"confidence", round3(s.confidence)}});
    }
    Json reactions = Json::array();
    for (const Reaction& r : t.reactions) {
        reactions.push_back(Json{{"start", secondsOf(r.range.start)}, {"end", secondsOf(r.range.end())}, {"speaker", r.speaker},
                                 {"kind", r.kind}, {"strength", round3(r.strength)}});
    }
    return Json{{"duration", secondsOf(t.duration)},
                {"speakers", std::move(speakers)},
                {"angles", std::move(angles)},
                {"segments", std::move(segments)},
                {"segmentCount", t.segments.size()},
                {"reactions", std::move(reactions)},
                {"overlapSeconds", round3(overlap)},
                {"silenceSeconds", round3(silence)},
                {"nonSpeechSeconds", round3(nonSpeech)},
                {"words", run.evidence.words.size()},
                {"mics", run.evidence.mics.size()},
                {"faceTracks", run.evidence.faces.size()},
                {"notes", t.notes}};
}

Json switchesToJson(const AutoSwitchResult& result, const std::function<std::string(Ticks)>& timecode) {
    Json cuts = Json::array();
    for (const AppliedCut& a : result.cuts) {
        Json row{{"time", timecode ? Json(timecode(a.timeline)) : Json(nullptr)},
                 {"ticks", a.timeline.value},
                 {"seconds", secondsOf(a.timeline)},
                 {"angle", a.cut.angle + 1},
                 {"reason", std::string(toString(a.cut.reason))},
                 {"explanation", a.cut.explanation},
                 {"confidence", round3(a.cut.confidence)},
                 {"clip", a.clip.value}};
        if (a.cut.speaker) {
            row["speaker"] = *a.cut.speaker;
        }
        if (a.cut.moved.value != 0) {
            row["moved"] = secondsOf(a.cut.moved);
        }
        cuts.push_back(std::move(row));
    }
    // The plan in the multicam source's time (the cuts above are where it landed on the clip's pieces).
    Json planned = Json::array();
    for (const PlannedCut& c : result.plan.cuts) {
        planned.push_back(Json{{"source", secondsOf(c.at)}, {"angle", c.angle + 1}, {"reason", std::string(toString(c.reason))}});
    }
    const SwitchStats& st = result.plan.stats;
    return Json{{"source", result.source.value},
                {"cuts", std::move(cuts)},
                {"planned", std::move(planned)},
                {"stats", Json{{"cuts", result.cuts.empty() ? 0 : result.cuts.size() - 1},
                               {"speaker", st.speakerCuts},
                               {"wide", st.wideCuts},
                               {"variety", st.varietyCuts},
                               {"reaction", st.reactionCuts},
                               {"snapped", st.snapped},
                               {"midWord", st.midWord},
                               {"averageShot", round3(st.averageShot)},
                               {"shortestShot", round3(st.shortestShot)}}},
                {"warnings", result.warnings}};
}

namespace {

Json angleOverridesSchema() {
    return schema::array(schema::object({{"angle", schema::integer("The angle (1 = the first).", 1, static_cast<std::int64_t>(kMaxMulticamAngles))},
                                         {"role", schema::enumeration({"closeUp", "wide", "unknown"}, "What it shows.")},
                                         {"speaker", Json{{"type", "string"}, {"maxLength", 128},
                                                          {"description", "Whom a close-up shows: a speaker's key or name (analysis.speakers lists them)."}}}},
                                        {"angle"}),
                         "Your angle assignments; they win over the guesses.");
}

CommandOutput speakersCommand(ExecutionContext& c) {
    SequenceId source;
    if (c.has("multicam")) {
        source = SequenceId{static_cast<std::uint64_t>(c.integer("multicam"))};
    } else {
        const ClipId clip = multicamClipOf(c);
        const auto loc = c.sequence().findClip(clip);
        if (!loc) {
            fail(ErrorCode::NotFound, "no clip " + std::to_string(clip.value), Json{{"path", "/clip"}});
        }
        const auto s = multicamSourceOf(c.state(), loc->clip());
        if (!s) {
            fail(ErrorCode::InvalidParams, "clip " + std::to_string(clip.value) + " is not a multicam clip", Json{{"path", "/clip"}});
        }
        source = *s;
    }
    const SpeakerRun run = runSpeakers(c, source, overridesOf(c));
    CommandOutput out;
    out.state = c.state();
    out.data = speakersToJson(run, static_cast<std::size_t>(c.integer("segments", 400)));
    out.data["multicam"] = source.value;
    out.warnings = run.timeline.notes;
    return out;
}

CommandOutput autoSwitchCommand(ExecutionContext& c) {
    const ClipId clip = multicamClipOf(c);
    const auto loc = c.sequence().findClip(clip);
    if (!loc) {
        fail(ErrorCode::NotFound, "no clip " + std::to_string(clip.value) + " in '" + c.sequence().name + "'", Json{{"path", "/clip"}});
    }
    const auto source = multicamSourceOf(c.state(), loc->clip());
    if (!source) {
        fail(ErrorCode::InvalidParams, "clip " + std::to_string(clip.value) + " is not a multicam clip", Json{{"path", "/clip"}});
    }
    const SwitchOptions options = switchOptionsOf(c);
    const std::string targetName = c.string("target", "video");
    const wabcut::edit::AngleTarget target = targetName == "both" ? wabcut::edit::AngleTarget::Both : wabcut::edit::AngleTarget::Video;
    const SpeakerRun run = runSpeakers(c, *source, overridesOf(c));
    c.checkpoint("Planning the cuts");
    auto result = autoSwitch(c.state(), c.sequenceId(), clip, run, options, target);
    if (!result) {
        fail(result.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::EditRejected, result.error().message);
    }
    CommandOutput out;
    out.data = switchesToJson(*result, [&c](Ticks t) { return c.timecode(t); });
    out.data["clip"] = clip.value;
    out.data["preset"] = options.preset;
    out.data["target"] = targetName;
    const Json people = speakersToJson(run, 0);
    out.data["speakers"] = people["speakers"];
    out.data["angles"] = people["angles"];
    out.data["notes"] = people["notes"];
    out.warnings = result->warnings;
    out.state = std::move(result->state);
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    if (auto r = registry.add(std::move(spec)); !r) {
        throw std::logic_error(r.error());
    }
}

} // namespace

void registerMulticamAiCommands(command::CommandRegistry& registry) {
    Json speakerParams = schema::object(
        {{"clip", schema::integer("A multicam clip of the sequence (default: its first).", 1)},
         {"multicam", schema::integer("Or the multicam source sequence itself.", 1)},
         {"angles", angleOverridesSchema()},
         {"segments", schema::integer("At most this many segments listed (default 400).", 0, 100000)},
         {"faces", schema::boolean("Look at the cameras' faces (default true; false is quicker on long recordings: the angles' roles then "
                                   "come from your assignments and the microphones).")},
         {"sequence", schema::integer("Sequence id; only needed when the project has several.", 1)}});
    add(registry,
        command::runsModel({"analysis.speakers", 1, "Who Speaks When",
         "Active-speaker detection for a multicam source, on this computer: who speaks when (with crosstalk, silence and laughter) "
         "and which angle shows whom (close-up or wide), from the transcripts with their speakers, each angle's microphone level "
         "and each camera's faces and lip motion. Nothing changes. \"angles\" sets angles yourself; they win over the guesses.",
         command::Capability::MediaAnalysis, std::move(speakerParams), Json::object(), {}, speakersCommand}));

    Json switchParams = schema::object(
        {{"clip", schema::integer("The multicam clip (its picture or sound; default: the sequence's first).", 1)},
         {"preset", schema::enumeration({"calm", "interview", "energetic"}, "Podcast calm, Interview (default) or Energetic.")},
         {"minHold", schema::number("Seconds a shot is held at least.", 0.2, 60.0)},
         {"lead", schema::number("Seconds a cut comes before the speaker starts (a J-cut).", 0.0, 2.0)},
         {"wideOnOverlap", schema::boolean("The wide shot when people talk over each other.")},
         {"overlapMin", schema::number("Seconds of crosstalk before going wide.", 0.0, 10.0)},
         {"wideOnLaughter", schema::boolean("The wide shot for laughter.")},
         {"silenceWide", schema::number("Seconds of silence before going wide (0 = never).", 0.0, 60.0)},
         {"varietyAfter", schema::number("Seconds on one close-up before a short wide shot (0 = never).", 0.0, 600.0)},
         {"varietyLength", schema::number("Seconds that variety wide shot lasts.", 0.5, 30.0)},
         {"reactions", schema::boolean("Cut to a listener's close-up when they react strongly (laughter, a quick word).")},
         {"reactionStrength", schema::number("How strong a reaction must be (0..1).", 0.0, 1.0)},
         {"reactionHold", schema::number("Seconds a reaction shot lasts at least.", 0.3, 10.0)},
         {"openWide", schema::boolean("Open on the wide shot.")},
         {"openLength", schema::number("Seconds of that opening wide shot.", 0.0, 30.0)},
         {"avoidMidWord", schema::boolean("Move cuts to the nearest pause between words (default true).")},
         {"maxSnap", schema::number("Seconds a cut may move to reach a pause.", 0.0, 2.0)},
         {"target", schema::enumeration({"video", "both"}, "Switch the picture only (default: the sound stays the mic mix) or both.")},
         {"faces", schema::boolean("Look at the cameras' faces (default true; false is quicker on long recordings).")},
         {"angles", angleOverridesSchema()},
         {"sequence", schema::integer("Sequence id; only needed when the project has several.", 1)}});
    add(registry,
        command::runsModel({"multicam.autoSwitch", 1, "Auto Switch Multicam",
         "Cuts a multicam clip to whoever is speaking, on this computer: the active speaker's close-up after a minimum hold, cut a "
         "little before they start, the wide shot for crosstalk, laughter, long pauses and for variety on long monologues, optional "
         "reaction shots, never in the middle of a word. Presets: calm (podcast), interview, energetic; every rule tunable. The result "
         "is ordinary angle cuts (editable afterwards), one undo step, each with its reason (\"Cut to Cam B: Speaker 2 starts "
         "talking\"); dryRun previews them.",
         command::Capability::EditSequence, std::move(switchParams), Json{{"preset", "interview"}}, {}, autoSwitchCommand}));
}

} // namespace wabcut::ai::multicam
