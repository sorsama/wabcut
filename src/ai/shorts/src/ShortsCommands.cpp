#include <wabcut/ai/shorts/ShortsCommands.h>

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/ai/shorts/Rerank.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/EditSession.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace wabcut::ai::shorts {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

double rounded(double v, double scale) { return std::round(v * scale) / scale; }
Ticks fromSeconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

edit::TranscriptServices& transcriptsOf(ExecutionContext& c) {
    auto* services = command::findService<edit::TranscriptServices>(c.services());
    if (!services || !services->transcripts) {
        fail(ErrorCode::InvalidRequest, "transcripts are not available here: the host provides none");
    }
    return *services;
}

/// Subjects for the finder: cached analyses (or analysed now when asked), remembered for this command.
std::function<const vision::SubjectAnalysis*(const MediaItem&, int)> subjectsFor(ExecutionContext& c, bool analyse,
                                                                                  std::map<std::pair<std::uint64_t, int>, std::optional<vision::SubjectAnalysis>>& memo) {
    auto* services = command::findService<reframe::ReframeServices>(c.services());
    if (!services || !services->subjects) {
        return {};
    }
    vision::SubjectProvider* provider = services->subjects.get();
    return [&c, provider, analyse, &memo](const MediaItem& media, int stream) -> const vision::SubjectAnalysis* {
        auto [it, inserted] = memo.try_emplace({media.id.value, stream});
        if (inserted) {
            if (analyse) {
                auto found = provider->subjects(media, stream, [&c, name = media.name](double f) {
                    char text[32];
                    std::snprintf(text, sizeof text, " (%d%%)", static_cast<int>(std::lround(f * 100.0)));
                    c.checkpoint("finding subjects in '" + name + "'" + text);
                    return true;
                });
                if (found) {
                    it->second = std::move(*found);
                }
            } else {
                it->second = provider->cached(media, stream);
            }
        }
        return it->second ? &*it->second : nullptr;
    };
}

CommandOutput findCommand(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    edit::TranscriptServices& services = transcriptsOf(c);
    ShortsOptions options;
    options.count = static_cast<std::size_t>(c.integer("count", 5));
    if (c.has("minDuration")) {
        options.minSeconds = c.number("minDuration");
    }
    if (c.has("maxDuration")) {
        options.maxSeconds = c.number("maxDuration");
    }
    if (options.minSeconds >= options.maxSeconds) {
        fail(ErrorCode::InvalidParams, "\"minDuration\" must be below \"maxDuration\"");
    }
    options.targetSeconds = c.has("targetDuration") ? c.number("targetDuration") : std::clamp(45.0, options.minSeconds, options.maxSeconds);
    options.query = c.string("query");

    const edit::SequenceTranscript transcript = edit::SequenceTranscript::build(c.state(), seq.id, *services.transcripts);
    if (transcript.empty()) {
        fail(ErrorCode::EditRejected, "sequence '" + seq.name + "' has no transcript: transcribe its media first");
    }
    c.checkpoint("reading the transcript");
    const std::vector<edit::ReviewItem> review = edit::review(transcript, c.state(), edit::ReviewOptions{}, services.energy.get());
    std::map<std::pair<std::uint64_t, int>, std::optional<vision::SubjectAnalysis>> memo;
    ShortsSignals signals;
    signals.energy = services.energy.get();
    signals.subjects = subjectsFor(c, c.boolean("analyze", false), memo);
    ShortsReport report = findShorts(c.state(), seq.id, transcript, review, signals, options);

    std::vector<std::string> warnings = report.warnings;
    bool reranked = false;
    std::string model;
    if (c.boolean("rerank", false) && !report.shorts.empty()) {
        auto* cloud = command::findService<ShortsServices>(c.services());
        if (c.state().aiNoCloud) {
            warnings.push_back("this project is locked to no-cloud AI: the shorts were ranked on this computer only");
        } else if (!cloud || !cloud->chat) {
            warnings.push_back("no cloud chat model here: the shorts were ranked on this computer only");
        } else if (auto chat = cloud->chat(); !chat) {
            warnings.push_back("the cloud re-rank did not run (" + chat.error().message + "): the local ranking stands");
        } else {
            RerankOptions rerank;
            rerank.query = options.query;
            rerank.language = std::string(transcript.language(report.shorts.front().firstWord));
            c.checkpoint("asking " + (*chat)->spec().id + " to rank the shorts");
            auto outcome = rerankShorts(**chat, report.shorts, rerank);
            if (outcome) {
                reranked = true;
                model = outcome->model;
            } else {
                warnings.push_back("the cloud re-rank failed (" + outcome.error().message + "): the local ranking stands");
            }
        }
    }
    Json shorts = Json::array();
    for (std::size_t i = 0; i < report.shorts.size(); ++i) {
        shorts.push_back(shortToJson(report.shorts[i], i + 1));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"sequence", seq.id.value},
                    {"shorts", std::move(shorts)},
                    {"sentences", report.sentences},
                    {"topics", report.topics},
                    {"candidates", report.candidates},
                    {"reranked", reranked},
                    {"milliseconds", rounded(report.milliseconds, 10.0)}};
    if (reranked) {
        out.data["model"] = model;
    }
    if (!options.query.empty()) {
        out.data["query"] = options.query;
    }
    out.warnings = std::move(warnings);
    return out;
}

/// One nested command on the short's session; a failure becomes a warning (the short is still made).
bool runStep(command::CommandEngine& engine, const std::string& name, Json params, std::vector<std::string>& warnings,
             command::CommandResult* result = nullptr) {
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    command::CommandResult r = engine.execute(request);
    if (!r.ok()) {
        warnings.push_back(name + ": " + r.error->message);
        return false;
    }
    if (result) {
        *result = std::move(r);
    }
    return true;
}

CommandOutput createCommand(ExecutionContext& c) {
    const Sequence& source = c.sequence();
    const Json& list = c.params()["shorts"];
    if (list.size() > kMaxShortsPerCall) {
        fail(ErrorCode::InvalidParams, "at most " + std::to_string(kMaxShortsPerCall) + " shorts per call");
    }
    reframe::ReframeOptions reframing;
    reframing.aspect = reframe::aspectFromString(c.string("aspect", "9:16")).value_or(reframe::Aspect{9, 16});
    if (c.has("aspect") && !reframe::aspectFromString(c.string("aspect"))) {
        fail(ErrorCode::InvalidParams, "\"aspect\" must look like \"9:16\"");
    }
    reframing.mode = reframe::reframeModeFromString(c.string("mode", "speaker")).value_or(reframe::ReframeMode::Speaker);
    reframing.fit = reframe::fitModeFromString(c.string("fit", "crop")).value_or(reframe::FitMode::Crop);
    const bool cleanup = c.boolean("cleanup", false);
    const bool captions = c.boolean("captions", true);
    const bool hookTitle = c.boolean("hookTitle", true);
    const bool normalize = c.boolean("normalize", false);
    const bool doReframe = c.boolean("reframe", true);

    auto* reframeServices = command::findService<reframe::ReframeServices>(c.services());
    vision::SubjectProvider* subjects = reframeServices && reframeServices->subjects ? reframeServices->subjects.get() : nullptr;
    const reframe::SubjectSource subjectSource =
        subjects ? reframe::SubjectSource([&c, subjects](const MediaItem& media, int stream) {
            return subjects->subjects(media, stream, [&c, name = media.name](double f) {
                char text[32];
                std::snprintf(text, sizeof text, " (%d%%)", static_cast<int>(std::lround(f * 100.0)));
                c.checkpoint("finding subjects in '" + name + "'" + text);
                return true;
            });
        })
                 : reframe::SubjectSource{};

    ProjectState state = c.state();
    std::vector<std::string> warnings;
    if (!subjects && doReframe) {
        warnings.push_back("no subject analysis here: the shorts are centred");
    }
    Json made = Json::array();
    CommandOutput out;
    const std::shared_ptr<command::CommandServices> services(c.services(), [](command::CommandServices*) {}); // borrowed
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& item = list[i];
        const double startSeconds = item.at("start").get<double>();
        const double endSeconds = item.at("end").get<double>();
        if (!(endSeconds > startSeconds) || startSeconds < 0.0 || endSeconds - startSeconds > 600.0) {
            fail(ErrorCode::InvalidParams, "short " + std::to_string(i + 1) + ": \"end\" must follow \"start\", at most 10 minutes later");
        }
        const Ticks frame{source.frameRate.ticksPerUnit()};
        const Ticks start{floorDiv(fromSeconds(startSeconds).value, frame.value) * frame.value};
        const Ticks end{(floorDiv(fromSeconds(endSeconds).value - 1, frame.value) + 1) * frame.value};
        const std::string title = item.contains("title") && item["title"].is_string() ? item["title"].get<std::string>() : std::string();
        c.checkpoint("making short " + std::to_string(i + 1) + " of " + std::to_string(list.size()));

        const auto [width, height] = reframe::outputSize(source.width, source.height, reframing);
        std::string name = source.name + " - Short " + std::to_string(i + 1) + (title.empty() ? "" : ": " + title);
        auto cut = cutSequence(state, source.id, TimeRange::fromStartEnd(start, end), std::move(name), width, height);
        if (!cut) {
            fail(cut.error().code == wabcut::ErrorCode::InvalidArgument ? ErrorCode::InvalidParams : ErrorCode::EditRejected,
                 "short " + std::to_string(i + 1) + ": " + cut.error().message);
        }
        const SequenceId id = cut->sequence;
        std::vector<std::string> notes;
        EditSession session(std::move(cut->state));
        command::CommandEngine engine(session, edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 16, services});
        Json report{{"sequence", id.value}, {"start", rounded(start.toSeconds(), 1000.0)}, {"end", rounded(end.toSeconds(), 1000.0)}};
        if (cleanup) {
            report["cleaned"] = runStep(engine, "transcript.cleanup",
                                        Json{{"sequence", id.value}, {"fillers", true}, {"silences", c.has("cleanupSilences") ? c.number("cleanupSilences") : 0.8}},
                                        notes);
        }
        if (doReframe) {
            auto framed = reframe::reframeSequence(*session.snapshot(), id, reframing, subjectSource, id);
            if (framed) {
                (void)session.commit("Reframe", framed->state);
                report["reframed"] = framed->clipsReframed;
                notes.insert(notes.end(), framed->warnings.begin(), framed->warnings.end());
            } else {
                notes.push_back("reframe: " + framed.error().message);
            }
        }
        if (captions) {
            report["captions"] = runStep(engine, "captions.generate",
                                         Json{{"sequence", id.value}, {"maxCharsPerLine", width < height ? 24 : 37}, {"maxLines", 2}}, notes);
        }
        if (hookTitle && !title.empty()) {
            command::CommandResult track;
            if (runStep(engine, "sequence.addTrack", Json{{"sequence", id.value}, {"kind", "video"}, {"name", "Hook title"}}, notes, &track)) {
                command::CommandResult added;
                const Ticks length = std::min(fromSeconds(c.has("titleSeconds") ? c.number("titleSeconds") : 3.0), end - start);
                if (runStep(engine, "title.add",
                            Json{{"sequence", id.value},
                                 {"text", title},
                                 {"at", 0},
                                 {"duration", length.value},
                                 {"track", track.data["track"]},
                                 {"style", Json{{"size", std::max(48, height / 20)}}}},
                            notes, &added) &&
                    !added.createdIds.empty()) {
                    runStep(engine, "clip.setParam",
                            Json{{"sequence", id.value}, {"clips", Json::array({added.createdIds.front()})}, {"param", "motion.position.y"},
                                 {"value", -0.3 * height}},
                            notes);
                    report["title"] = title;
                }
            }
        }
        if (normalize) {
            report["normalized"] = runStep(engine, "audio.normalize", Json{{"sequence", id.value}, {"preset", "youtube"}}, notes);
        }
        // The record: what it was cut from and how.
        ProjectState next = *session.snapshot();
        auto seq = std::make_shared<Sequence>(*next.findSequence(id));
        Json params{{"start", rounded(start.toSeconds(), 1000.0)},
                    {"end", rounded(end.toSeconds(), 1000.0)},
                    {"aspect", reframe::toString(reframing.aspect)},
                    {"mode", reframe::toString(reframing.mode)},
                    {"cleanup", cleanup},
                    {"captions", captions}};
        if (!title.empty()) {
            params["title"] = title;
        }
        seq->derivedFrom = SequenceDerivation{source.id, std::string(kGenerator), params.dump()};
        state = next.withSequence(std::move(seq));
        report["name"] = state.findSequence(id)->name;
        for (const std::string& n : notes) {
            warnings.push_back("short " + std::to_string(i + 1) + ": " + n);
        }
        out.createdIds.push_back(id.value);
        made.push_back(std::move(report));
    }
    out.state = std::move(state);
    out.data = Json{{"source", source.id.value}, {"sequences", std::move(made)}};
    out.warnings = std::move(warnings);
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

Json shortToJson(const ShortCandidate& s, std::size_t rank) {
    Json parts{{"structure", rounded(s.parts.structure, 1000.0)}, {"hook", rounded(s.parts.hook, 1000.0)},
               {"topic", rounded(s.parts.topic, 1000.0)},         {"energy", rounded(s.parts.energy, 1000.0)},
               {"visual", rounded(s.parts.visual, 1000.0)},       {"clean", rounded(s.parts.clean, 1000.0)},
               {"length", rounded(s.parts.length, 1000.0)}};
    if (s.parts.query > 0.0) {
        parts["query"] = rounded(s.parts.query, 1000.0);
    }
    Json j{{"rank", rank},
           {"start", rounded(s.range.start.toSeconds(), 1000.0)},
           {"end", rounded(s.range.end().toSeconds(), 1000.0)},
           {"duration", rounded(s.range.duration.toSeconds(), 1000.0)},
           {"startTicks", s.range.start.value},
           {"endTicks", s.range.end().value},
           {"score", rounded(s.score, 1000.0)},
           {"parts", std::move(parts)},
           {"reasons", s.reasons},
           {"title", s.title},
           {"hook", s.hook},
           {"text", s.text},
           {"words", Json::array({s.firstWord, s.lastWord})}};
    if (s.cloudScore) {
        j["cloudScore"] = rounded(*s.cloudScore, 1000.0);
        j["cloudReason"] = s.cloudReason;
    }
    return j;
}

std::expected<reframe::SequenceCopy, Error> cutSequence(const ProjectState& state, SequenceId source, TimeRange range, std::string name,
                                                        int width, int height) {
    if (range.duration <= Ticks{}) {
        return std::unexpected(Error(wabcut::ErrorCode::InvalidArgument, "the range is empty"));
    }
    auto copy = reframe::duplicateSequence(state, source, std::move(name), width, height);
    if (!copy) {
        return copy;
    }
    auto seq = std::make_shared<Sequence>(*copy->state.findSequence(copy->sequence));
    const Ticks from = range.start;
    const Ticks to = range.end();
    std::map<std::uint64_t, int> groupMembers;
    std::size_t clipsKept = 0;
    const auto cutTrack = [&](TrackPtr& trackPtr) {
        auto track = std::make_shared<Track>(*trackPtr);
        std::vector<Clip> kept;
        std::set<std::uint64_t> headCut;
        std::set<std::uint64_t> tailCut;
        for (Clip clip : trackPtr->clips) {
            if (clip.end() <= from || clip.start() >= to) {
                continue;
            }
            if (clip.start() < from) {
                clip.moveHead(from - clip.start());
                headCut.insert(clip.id.value);
            }
            if (clip.end() > to) {
                clip.moveTail(to - clip.end());
                tailCut.insert(clip.id.value);
            }
            clip.timelineRange.start = clip.timelineRange.start - from;
            kept.push_back(std::move(clip));
        }
        std::set<std::uint64_t> ids;
        for (const Clip& clip : kept) {
            ids.insert(clip.id.value);
            if (clip.linkGroup.isValid()) {
                ++groupMembers[clip.linkGroup.value];
            }
        }
        std::vector<Transition> transitions;
        for (Transition t : trackPtr->transitions) {
            const bool outgoingOk = !t.outgoing.isValid() || (ids.contains(t.outgoing.value) && !tailCut.contains(t.outgoing.value));
            const bool incomingOk = !t.incoming.isValid() || (ids.contains(t.incoming.value) && !headCut.contains(t.incoming.value));
            if (outgoingOk && incomingOk && (t.outgoing.isValid() || t.incoming.isValid())) {
                transitions.push_back(std::move(t));
            }
        }
        clipsKept += kept.size();
        track->clips = ClipList(std::move(kept));
        track->transitions = std::move(transitions);
        trackPtr = std::move(track);
    };
    for (TrackPtr& t : seq->videoTracks) {
        cutTrack(t);
    }
    for (TrackPtr& t : seq->audioTracks) {
        cutTrack(t);
    }
    if (clipsKept == 0) {
        return std::unexpected(Error(wabcut::ErrorCode::InvalidArgument, "nothing of the sequence lies in that range"));
    }
    // A link group needs two clips: a partner left outside the range unlinks the one inside.
    const auto unlinkLonely = [&](TrackPtr& trackPtr) {
        std::vector<Clip> clips = trackPtr->clips.toVector();
        bool changed = false;
        for (Clip& clip : clips) {
            if (clip.linkGroup.isValid() && groupMembers[clip.linkGroup.value] < 2) {
                clip.linkGroup = LinkGroupId{};
                changed = true;
            }
        }
        if (changed) {
            auto track = std::make_shared<Track>(*trackPtr);
            track->clips = ClipList(std::move(clips));
            trackPtr = std::move(track);
        }
    };
    for (TrackPtr& t : seq->videoTracks) {
        unlinkLonely(t);
    }
    for (TrackPtr& t : seq->audioTracks) {
        unlinkLonely(t);
    }
    for (CaptionTrack& captions : seq->captionTracks) {
        std::vector<CaptionCue> cues;
        for (CaptionCue cue : captions.cues) {
            const Ticks a = std::max(cue.start, from);
            const Ticks b = std::min(cue.end(), to);
            if (b > a) {
                cues.push_back(CaptionCue{a - from, b - a, std::move(cue.text)});
            }
        }
        captions.cues = std::move(cues);
    }
    std::vector<Marker> markers;
    for (Marker m : seq->markers) {
        if (m.start >= from && m.start < to) {
            m.start = m.start - from;
            m.duration = std::min(m.duration, to - (m.start + from));
            if (m.highlight) {
                // C.15: a highlight's peak moves with its marker and stays inside it (the marker may have been cut short).
                m.highlight->peak = std::clamp(m.highlight->peak - from, m.start, m.end());
            }
            markers.push_back(std::move(m));
        }
    }
    seq->markers = std::move(markers);
    seq->markIn.reset();
    seq->markOut.reset();
    seq->workArea.reset();
    seq->beatGrid.reset();
    seq->duration = range.duration;
    copy->state = copy->state.withSequence(std::move(seq));
    return copy;
}

void registerShortsCommands(command::CommandRegistry& registry) {
    using command::Capability;
    Json find{{"type", "object"},
              {"properties",
               Json{{"sequence", schema::integer("The sequence to search (a podcast, an interview); only needed when the project has several.", 1)},
                    {"count", schema::integer("How many shorts (default 5).", 1, 20)},
                    {"minDuration", schema::number("Shortest short in seconds (default 20).", 5.0, 600.0)},
                    {"maxDuration", schema::number("Longest short in seconds (default 90).", 5.0, 600.0)},
                    {"targetDuration", schema::number("Preferred length in seconds (default 45).", 5.0, 600.0)},
                    {"query", Json{{"type", "string"}, {"description", "What the shorts should be about, e.g. \"the pricing discussion\"."}, {"maxLength", 300}}},
                    {"analyze", schema::boolean("Analyse faces and motion of media not analysed yet (slow for long media; default false: cached analyses only).", false)},
                    {"rerank", schema::boolean("Also let the user's cloud chat model rank and title the best ones from their transcript text "
                                               "(needs consent; never for a no-cloud project; default false).",
                                               false)}}},
              {"additionalProperties", false}};
    add(registry,
        command::runsModel({"shorts.find", 1, "Find Shorts",
         "Finds the best moments of a long sequence for vertical shorts (20-90 s each): whole sentences, a hook in the first "
         "seconds, one topic, lively delivery, faces, few fillers; optionally about a topic (\"query\"). Returns them best "
         "first with a score breakdown, reasons, a suggested title, the words and a start/end (seconds; ticks too) snapped to "
         "sentences and cuts, ready for shorts.create. Needs the media's transcripts. A read: it changes nothing.",
         Capability::Read, find, Json{{"count", 5}, {"query", "pricing"}}, {}, findCommand}));

    Json item{{"type", "object"},
              {"properties", Json{{"start", schema::number("Start on the source sequence, in seconds.", 0.0)},
                                  {"end", schema::number("End on the source sequence, in seconds.", 0.0)},
                                  {"title", Json{{"type", "string"}, {"description", "Its title (the hook title and the sequence name)."}, {"maxLength", 120}}}}},
              {"required", Json::array({"start", "end"})},
              {"additionalProperties", false}};
    Json create{{"type", "object"},
                {"properties",
                 Json{{"sequence", schema::integer("The source sequence; only needed when the project has several.", 1)},
                      {"shorts", Json{{"type", "array"}, {"description", "The shorts to make (from shorts.find), at most 10."}, {"items", item}, {"minItems", 1}, {"maxItems", 10}}},
                      {"aspect", Json{{"type", "string"}, {"description", "Aspect of the shorts (default \"9:16\")."}, {"minLength", 3}, {"maxLength", 7}}},
                      {"mode", schema::enumeration({"follow", "faces", "speaker", "anchor"}, "What the frame follows (default \"speaker\").")},
                      {"fit", schema::enumeration({"crop", "letterbox", "blur"}, "Where subjects cannot fit (default \"crop\").")},
                      {"reframe", schema::boolean("Reframe to the aspect (default true).", true)},
                      {"cleanup", schema::boolean("Remove fillers and silences first (default false).", false)},
                      {"cleanupSilences", schema::number("With cleanup: the shortest pause removed, in seconds (default 0.8).", 0.2, 10.0)},
                      {"captions", schema::boolean("Add captions from the transcript (default true).", true)},
                      {"hookTitle", schema::boolean("Show the title for the first seconds (default true).", true)},
                      {"titleSeconds", schema::number("How long the hook title shows (default 3).", 0.5, 30.0)},
                      {"normalize", schema::boolean("Normalise the loudness to -14 LUFS (default false).", false)}}},
                {"required", Json::array({"shorts"})},
                {"additionalProperties", false}};
    add(registry,
        command::runsModel({"shorts.create", 1, "Create Shorts",
         "Makes one new sequence per short (from shorts.find): the range cut out of the source sequence, reframed to 9:16 "
         "(or \"aspect\") with editable keyframes following the speaker, captioned, with the title as a hook for its first "
         "seconds; optionally cleaned of fillers and silences and normalised to -14 LUFS. The source is not changed; all the "
         "shorts of one call are one undo step. Export them afterwards (vertical 1080x1920).",
         Capability::EditProject, create,
         Json{{"shorts", Json::array({Json{{"start", 12.5}, {"end", 58.0}, {"title", "Why we doubled our price"}}})}}, {}, createCommand}));
}

} // namespace wabcut::ai::shorts
