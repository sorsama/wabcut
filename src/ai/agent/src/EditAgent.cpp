#include <wabcut/ai/agent/EditAgent.h>

#include <wabcut/ai/Redact.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/agent/ConstrainedTools.h>
#include <wabcut/ai/agent/Describe.h>
#include <wabcut/ai/agent/ToolRouter.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Schema.h>
#include <wabcut/command/ToolExport.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <thread>

namespace wabcut::ai::agent {

namespace {

using command::CommandError;
using command::ErrorCode;

constexpr std::string_view kRules = R"(You are the editing assistant of Wabcut, a video editor. You edit the user's sequence with the tools: read tools look at the project, edit tools change it. Your edits go to a staged preview that the user reviews; nothing changes their project until they accept it.

How to work:
- Look before you edit: sequence_summary for tracks and clips, transcript_text and transcript_words for what is said (with the word indices the transcript_* edit tools take), transcript_review for filler words and pauses.
- Prefer one command that does the whole job (transcript_cleanup, transcript_keepRanges, timeline_removeSilences) over many small edits.
- Times are timecodes in the sequence's rate ("00:01:02:03") or integer ticks (254016000000 per second). Tracks are "V1", "A2", ...
- Word indices describe the transcript as it is now. After an edit that removes or moves words, read the transcript again before using indices.
- Every edit tool answers with what changed. When a tool fails, read the error and fix the call; never repeat a failing call unchanged.
- Never edit locked tracks. Keep video and audio in sync (ripple edits do). Do not leave gaps or clips shorter than 2 frames unless asked.
- When you are done, stop calling tools and say in one or two sentences what you changed. Say plainly what you could not do. The result is checked automatically after you finish; do not claim checks you did not make.
- If the request is a question, answer it without editing.)";

std::string tc(const Sequence& seq, Ticks t) { return ticksToTimecode(t, seq.frameRate, seq.dropFrame); }

std::string trackLabel(const Sequence& seq, TrackId id) {
    const auto pos = seq.trackPosition(id);
    if (!pos) {
        return "?";
    }
    return (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1);
}

std::string rateText(Rate r) {
    char buf[32];
    if (r.den == 1) {
        std::snprintf(buf, sizeof buf, "%lld fps", static_cast<long long>(r.num));
    } else {
        std::snprintf(buf, sizeof buf, "%.3f fps", static_cast<double>(r.num) / static_cast<double>(r.den));
    }
    return buf;
}

/// The constraints, as the user message restates them to the model.
std::string constraintLines(const Constraints& c, const Sequence* seq) {
    std::string out;
    auto line = [&](const std::string& s) { out += "- " + s + "\n"; };
    auto range = [&](const TimeRange& r) { return seq ? tc(*seq, r.start) + " to " + tc(*seq, r.end()) : std::string("?"); };
    if (c.targetDuration) {
        line("The sequence ends up " + formatDuration(*c.targetDuration) + " long (content length, within a second or 5%).");
    }
    if (c.maxDuration) {
        line("The sequence ends up at most " + formatDuration(*c.maxDuration) + " long.");
    }
    for (const TimeRange& r : c.protectedRanges) {
        line("Keep the content of " + range(r) + " (current times); it may move but not be cut.");
    }
    if (c.scope) {
        line("Change clips only inside " + range(*c.scope) + " (current times).");
    }
    if (!c.onlyTracks.empty() && seq) {
        std::string labels;
        for (TrackId id : c.onlyTracks) {
            labels += (labels.empty() ? "" : ", ") + trackLabel(*seq, id);
        }
        line("Change clips only on " + labels + ".");
    }
    if (!c.allowClipEdits) {
        line("Do not change any clip: only markers and captions.");
    }
    if (c.keepTrackCount) {
        line("Do not add or remove tracks.");
    }
    if (!c.allowGaps) {
        line("Leave no new gaps between clips.");
    }
    line("No new clip shorter than " + std::to_string(c.minClipFrames) + " frames; locked tracks untouched.");
    return out;
}

std::string isoNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

Json usageJson(const Usage& u) {
    return Json{{"inputTokens", u.inputTokens},         {"outputTokens", u.outputTokens},
                {"cacheReadTokens", u.cacheReadTokens}, {"cacheWriteTokens", u.cacheWriteTokens},
                {"reasoningTokens", u.reasoningTokens}};
}

Json verificationJson(const VerificationReport& r) {
    Json items = Json::array();
    for (const auto& i : r.items) {
        items.push_back(Json{{"id", i.id}, {"label", i.label}, {"passed", i.passed}, {"detail", i.detail}, {"blocking", i.blocking}});
    }
    return Json{{"passed", r.passed()}, {"items", items}};
}

Json constraintsJson(const Constraints& c) {
    Json j = Json::object();
    if (c.targetDuration) {
        j["targetDuration"] = c.targetDuration->value;
    }
    if (c.maxDuration) {
        j["maxDuration"] = c.maxDuration->value;
    }
    Json ranges = Json::array();
    for (const auto& r : c.protectedRanges) {
        ranges.push_back(Json::array({r.start.value, r.end().value}));
    }
    j["protectedRanges"] = ranges;
    if (c.scope) {
        j["scope"] = Json::array({c.scope->start.value, c.scope->end().value});
    }
    Json tracks = Json::array();
    for (TrackId t : c.onlyTracks) {
        tracks.push_back(t.value);
    }
    j["onlyTracks"] = tracks;
    j["keepTrackCount"] = c.keepTrackCount;
    j["allowClipEdits"] = c.allowClipEdits;
    j["allowGaps"] = c.allowGaps;
    j["minClipFrames"] = c.minClipFrames;
    j["expectChanges"] = c.expectChanges;
    return j;
}

std::string_view stepKind(AgentStep::Kind k) {
    switch (k) {
    case AgentStep::Kind::Read: return "read";
    case AgentStep::Kind::Edit: return "edit";
    case AgentStep::Kind::Error: return "error";
    case AgentStep::Kind::Verify: return "verify";
    case AgentStep::Kind::Note: return "note";
    }
    return "note";
}

/// `j` with every string passed through the redactor: a key the user pasted into a request must not reach the
/// transcript, wherever the model echoed it (tool parameters included).
Json redactedJson(const Json& j, const Redactor& redactor) {
    if (j.is_string()) {
        return redactor.redact(j.get_ref<const std::string&>());
    }
    if (j.is_array()) {
        Json out = Json::array();
        for (const Json& e : j) {
            out.push_back(redactedJson(e, redactor));
        }
        return out;
    }
    if (j.is_object()) {
        Json out = Json::object();
        for (const auto& [key, value] : j.items()) {
            out[key] = redactedJson(value, redactor);
        }
        return out;
    }
    return j;
}

Json errorJson(const CommandError& e) {
    Json err{{"code", std::string(command::toString(e.code))}, {"message", e.message}};
    if (!e.details.is_null()) {
        err["details"] = e.details;
    }
    return Json{{"error", err}};
}

/// Stops tool execution when the run is cancelled or times out.
class RunControl final : public command::ExecutionControl {
public:
    RunControl(std::stop_token token, const std::atomic<bool>& timedOut) : m_token(std::move(token)), m_timedOut(timedOut) {}
    std::optional<CommandError> checkpoint(std::string_view) override {
        if (!m_token.stop_requested()) {
            return std::nullopt;
        }
        return m_timedOut.load() ? CommandError{ErrorCode::TimedOut, "the assistant ran out of time", nullptr}
                                 : CommandError{ErrorCode::Cancelled, "cancelled", nullptr};
    }

private:
    std::stop_token m_token;
    const std::atomic<bool>& m_timedOut;
};

bool allowsNull(const Json& schema) {
    if (!schema.is_object() || !schema.contains("type")) {
        return false;
    }
    const Json& type = schema["type"];
    return type == "null" || (type.is_array() && std::find(type.begin(), type.end(), Json("null")) != type.end());
}

/// Strict tool modes make optional properties nullable (ToolSchema.h, OpenAiStrict), so models send "from": null
/// for a property they leave out. Drops those nulls where the catalog's schema does not allow null.
Json withoutStrictNulls(Json value, const Json& schema) {
    if (value.is_object() && schema.is_object() && schema.contains("properties")) {
        const Json& properties = schema["properties"];
        for (auto it = value.begin(); it != value.end();) {
            const Json* sub = properties.contains(it.key()) ? &properties[it.key()] : nullptr;
            if (it->is_null() && !(sub && allowsNull(*sub))) {
                it = value.erase(it);
                continue;
            }
            if (sub) {
                *it = withoutStrictNulls(std::move(*it), *sub);
            }
            ++it;
        }
    } else if (value.is_array() && schema.is_object() && schema.contains("items")) {
        for (Json& item : value) {
            item = withoutStrictNulls(std::move(item), schema["items"]);
        }
    }
    return value;
}

struct Signature {
    std::vector<std::uint64_t> removed;
    std::vector<std::uint64_t> modified;
    std::size_t added = 0;
    bool markers = false;
    bool captions = false;
    bool trackList = false;
    std::size_t otherSequences = 0;
    bool media = false;

    bool operator==(const Signature&) const = default;
};

Signature signatureOf(const ChangeSet& changes, SequenceId sequence) {
    Signature s;
    for (const SequenceChange& sc : changes.sequences) {
        if (sc.sequence != sequence) {
            ++s.otherSequences;
            continue;
        }
        for (ClipId id : sc.removedClips) {
            s.removed.push_back(id.value);
        }
        for (ClipId id : sc.modifiedClips) {
            s.modified.push_back(id.value);
        }
        std::sort(s.removed.begin(), s.removed.end());
        std::sort(s.modified.begin(), s.modified.end());
        s.added = sc.addedClips.size();
        s.markers = sc.markersChanged;
        s.captions = sc.captionsChanged;
        s.trackList = sc.trackListChanged;
    }
    s.media = !changes.addedMedia.empty() || !changes.removedMedia.empty() || !changes.modifiedMedia.empty();
    return s;
}

} // namespace

std::string_view toString(AgentStatus status) {
    switch (status) {
    case AgentStatus::Staged: return "Staged";
    case AgentStatus::NoChanges: return "NoChanges";
    case AgentStatus::VerificationFailed: return "VerificationFailed";
    case AgentStatus::StepLimit: return "StepLimit";
    case AgentStatus::BudgetExceeded: return "BudgetExceeded";
    case AgentStatus::TimedOut: return "TimedOut";
    case AgentStatus::Cancelled: return "Cancelled";
    case AgentStatus::Failed: return "Failed";
    }
    return "Failed";
}

bool AgentOutcome::canApply() const {
    return hasChanges() && staged && status != AgentStatus::Cancelled && status != AgentStatus::Failed;
}

std::string undoLabelFor(std::string_view prompt) {
    std::string text(prompt);
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
    }
    constexpr std::size_t kMax = 48;
    if (text.size() > kMax) {
        std::size_t cut = text.rfind(' ', kMax);
        if (cut == std::string::npos || cut < kMax / 2) {
            cut = kMax;
        }
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut; // never split a UTF-8 sequence
        }
        text = text.substr(0, cut) + "…";
    }
    return "AI: " + text;
}

Json conversationToJson(const std::vector<ChatMessage>& messages) {
    const Redactor& redactor = Redactor::global();
    Json out = Json::array();
    for (const ChatMessage& m : messages) {
        Json parts = Json::array();
        for (const ContentPart& part : m.parts) {
            if (const auto* t = std::get_if<TextPart>(&part)) {
                parts.push_back(Json{{"type", "text"}, {"text", redactor.redact(t->text)}});
            } else if (const auto* i = std::get_if<ImagePart>(&part)) {
                parts.push_back(Json{{"type", "image"}, {"mediaType", i->mediaType}, {"bytes", i->bytes.size()},
                                     {"url", redactor.redactUrl(i->url)}});
            } else if (const auto* v = std::get_if<VideoPart>(&part)) {
                parts.push_back(Json{{"type", "video"}, {"mediaType", v->mediaType}, {"file", !v->localFile.empty() || !v->fileUri.empty()}});
            } else if (const auto* c = std::get_if<ToolCall>(&part)) {
                parts.push_back(Json{{"type", "tool_call"}, {"id", c->id}, {"name", c->name},
                                     {"arguments", redactor.redact(c->argumentsText.empty() ? c->arguments.dump() : c->argumentsText)}});
            } else if (const auto* r = std::get_if<ToolResult>(&part)) {
                parts.push_back(Json{{"type", "tool_result"}, {"callId", r->callId}, {"name", r->name},
                                     {"isError", r->isError}, {"content", redactor.redact(r->content)}});
            } else if (const auto* rp = std::get_if<ReasoningPart>(&part)) {
                // The signature is opaque provider state, not something to keep.
                parts.push_back(Json{{"type", "reasoning"}, {"provider", rp->provider}, {"text", redactor.redact(rp->text)},
                                     {"redacted", rp->redacted}});
            }
        }
        out.push_back(Json{{"role", m.role == Role::User ? "user" : "assistant"}, {"parts", parts}});
    }
    return out;
}

std::string projectSummary(const ProjectState& state, SequenceId sequence, std::size_t maxClipsPerTrack) {
    std::string out;
    const Sequence* seq = state.findSequence(sequence);
    out += "Project: " + std::to_string(state.media.size()) + " media, " + std::to_string(state.sequences.size()) +
           (state.sequences.size() == 1 ? " sequence.\n" : " sequences.\n");
    if (!seq) {
        return out + "The sequence to edit is missing.\n";
    }
    out += "Sequence to edit: \"" + seq->name + "\" (id " + std::to_string(seq->id.value) + "), " + rateText(seq->frameRate) +
           (seq->dropFrame ? " drop-frame" : "") + ", " + std::to_string(seq->width) + "x" + std::to_string(seq->height) +
           ", content " + tc(*seq, Ticks{}) + " to " + tc(*seq, seq->contentEnd()) + ".\n";
    if (seq->markIn || seq->markOut) {
        out += "In/Out: " + (seq->markIn ? tc(*seq, *seq->markIn) : std::string("none")) + " / " +
               (seq->markOut ? tc(*seq, *seq->markOut) : std::string("none")) + ".\n";
    }
    if (!seq->markers.empty()) {
        out += "Markers (" + std::to_string(seq->markers.size()) + "):";
        for (std::size_t i = 0; i < seq->markers.size() && i < 12; ++i) {
            out += (i ? ";" : "") + std::string(" ") + tc(*seq, seq->markers[i].start) +
                   (seq->markers[i].name.empty() ? "" : " \"" + seq->markers[i].name + "\"");
        }
        out += seq->markers.size() > 12 ? "; ...\n" : "\n";
    }
    out += "Tracks (clip: [id] \"name\" timeline start-end):\n";
    auto listTrack = [&](const Track& t, const std::string& label) {
        out += "  " + label + " (id " + std::to_string(t.id.value) + ")" + (t.name.empty() ? "" : " \"" + t.name + "\"") +
               ": " + std::to_string(t.clips.size()) + (t.clips.size() == 1 ? " clip" : " clips");
        if (!t.clips.empty()) {
            out += ", " + tc(*seq, t.clips.front().start()) + " to " + tc(*seq, t.clips.back().end());
        }
        out += t.locked ? ", LOCKED" : "";
        out += t.syncLocked ? "" : ", sync lock off";
        out += "\n";
        std::size_t shown = 0;
        for (const Clip& c : t.clips) {
            if (shown == maxClipsPerTrack) {
                out += "    ... " + std::to_string(t.clips.size() - shown) + " more (use sequence_summary)\n";
                break;
            }
            out += "    [" + std::to_string(c.id.value) + "] \"" + c.name() + "\" " + tc(*seq, c.start()) + "-" +
                   tc(*seq, c.end()) + (c.enabled ? "" : " disabled") + "\n";
            ++shown;
        }
    };
    for (std::size_t i = 0; i < seq->videoTracks.size(); ++i) {
        listTrack(*seq->videoTracks[i], "V" + std::to_string(i + 1));
    }
    for (std::size_t i = 0; i < seq->audioTracks.size(); ++i) {
        listTrack(*seq->audioTracks[i], "A" + std::to_string(i + 1));
    }
    if (!seq->captionTracks.empty()) {
        out += "Caption tracks:";
        for (const CaptionTrack& ct : seq->captionTracks) {
            out += " \"" + ct.name + "\" (id " + std::to_string(ct.id.value) + ", " +
                   (ct.language.empty() ? std::string("unknown language") : ct.language) + ", " +
                   std::to_string(ct.cues.size()) + " cues);";
        }
        out += "\n";
    }
    if (!state.media.empty()) {
        out += "Media:";
        for (std::size_t i = 0; i < state.media.size() && i < 20; ++i) {
            out += (i ? "," : "") + std::string(" \"") + state.media[i]->name + "\" (id " +
                   std::to_string(state.media[i]->id.value) + ")";
        }
        out += state.media.size() > 20 ? ", ... (media_list finds more)\n" : "\n";
    }
    return out;
}

std::vector<std::string> defaultLibraryCommands() {
    return {"bin.create", "bin.rename", "bin.move", "media.setAttributes", "media.makeSubclip", "media.setSubclip"};
}

std::vector<std::string> defaultAdditiveProjectCommands() {
    std::vector<std::string> out{"reframe.createSequence", "shorts.create", "roughcut.fromScript", "workflow.podcast",
                                 "highlights.toReel", "highlights.toShort", // G.2: new sequences only
                                 "sequence.makeVersion"};                    // C.16: a vertical / square version
    for (std::string& name : defaultLibraryCommands()) {
        out.push_back(std::move(name));
    }
    return out;
}

std::vector<std::string> defaultAnalysisCommands() { return {"audio.loudness", "analysis.speakers", "audio.channelLevels"}; }

namespace {

/// Whether a command is in reach: its capability is allowed, or it is an additive project command, or (A.3) an allowed
/// analysis read.
bool permitted(const command::CommandSpec& spec, command::CapabilitySet allowed, const std::vector<std::string>& additive,
               const std::vector<std::string>& analysis = {}) {
    return allowed.has(spec.capability) ||
           (spec.capability == command::Capability::EditProject && std::ranges::find(additive, spec.name) != additive.end()) ||
           (spec.capability == command::Capability::MediaAnalysis && std::ranges::find(analysis, spec.name) != analysis.end());
}

/// The engine's capabilities: the allowed ones, plus EditProject for the additive commands and MediaAnalysis for the
/// analysis reads (both checked per command).
command::CapabilitySet widened(command::CapabilitySet allowed, const std::vector<std::string>& additive,
                               const std::vector<std::string>& analysis = {}) {
    if (!additive.empty()) {
        allowed.bits |= command::CapabilitySet::of({command::Capability::EditProject}).bits;
    }
    if (!analysis.empty()) {
        allowed.bits |= command::CapabilitySet::of({command::Capability::MediaAnalysis}).bits;
    }
    return allowed;
}

/// Sequences in `staged` that `base` does not have.
std::size_t newSequences(const ProjectState& base, const ProjectState& staged) {
    return static_cast<std::size_t>(std::ranges::count_if(staged.sequences, [&](const SequencePtr& s) { return !base.findSequence(s->id); }));
}

} // namespace

EditAgent::EditAgent(AgentOptions options) : m_options(std::move(options)) {
    if (!m_options.registry) {
        m_options.registry = &edit::editingCommands();
    }
    if (!m_options.today) {
        m_options.today = [] { return todayUtc(); };
    }
}

std::vector<ToolSpec> EditAgent::tools() const {
    std::lock_guard lock(m_mutex);
    if (m_tools.empty()) {
        Json exported = command::exportTools(*m_options.registry, command::ToolFormat::JsonSchema,
                                             widened(m_options.allowed, m_options.additiveProjectCommands,
                                                     m_options.analysisCommands));
        Json kept = Json::array();
        for (Json& tool : exported) {
            const command::CommandSpec* spec = m_options.registry->find(tool["name"].get<std::string>());
            if (spec && permitted(*spec, m_options.allowed, m_options.additiveProjectCommands, m_options.analysisCommands)) {
                kept.push_back(std::move(tool));
            }
        }
        m_tools = toolSpecsFromCommandExport(kept);
    }
    return m_tools;
}

std::string EditAgent::systemPrompt(const ProjectState& state, std::uint64_t revision, SequenceId sequence) const {
    std::lock_guard lock(m_mutex);
    const auto key = std::make_pair(revision, sequence.value);
    auto it = m_summaries.find(key);
    if (it == m_summaries.end()) {
        if (m_summaries.size() > 16) {
            m_summaries.erase(m_summaries.begin()); // oldest revision first
        }
        it = m_summaries.emplace(key, std::string(kRules) + "\n\n" + projectSummary(state, sequence)).first;
    }
    return it->second;
}

AgentOutcome failedOutcome(const AgentRequest& request, std::string modelId, std::string error) {
    AgentOutcome out;
    out.status = AgentStatus::Failed;
    out.error = std::move(error);
    out.model = std::move(modelId);
    out.constraints = request.constraints;
    const AgentOutcome* previous = request.previous.get();
    out.base = previous ? previous->base : request.base;
    out.baseRevision = previous ? previous->baseRevision : request.baseRevision;
    out.sequence = previous ? previous->sequence : request.sequence;
    return out;
}

AgentOutcome EditAgent::run(IChatModel& model, const AgentRequest& request, const AgentEventHandler& onEvent,
                            std::stop_token stop) {
    // The last line of defence (review wave 3, P1-2): nothing a model sends, and no bug in describing or checking it,
    // may escape the worker thread and terminate the editor.
    std::string modelId;
    try {
        modelId = model.spec().id;
        return runUnguarded(model, request, onEvent, stop);
    } catch (const std::exception& e) {
        return failedOutcome(request, modelId, std::string("The assistant stopped on an internal error: ") + e.what());
    } catch (...) {
        return failedOutcome(request, modelId, "The assistant stopped on an internal error.");
    }
}

AgentOutcome EditAgent::runUnguarded(IChatModel& model, const AgentRequest& request, const AgentEventHandler& onEvent,
                                     std::stop_token stop) {
    const AgentLimits& limits = m_options.limits;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + limits.timeout;
    auto emit = [&](AgentEvent e) {
        if (onEvent) {
            onEvent(e);
        }
    };
    auto phase = [&](std::string text) {
        AgentEvent e;
        e.kind = AgentEvent::Kind::Phase;
        e.text = std::move(text);
        emit(std::move(e));
    };

    AgentOutcome out;
    out.model = model.spec().id;
    out.constraints = request.constraints;
    const AgentOutcome* previous = request.previous.get();
    out.base = previous ? previous->base : request.base;
    out.baseRevision = previous ? previous->baseRevision : request.baseRevision;
    out.sequence = previous ? previous->sequence : request.sequence;
    if (!out.base || !out.base->findSequence(out.sequence)) {
        out.status = AgentStatus::Failed;
        out.error = "The sequence to edit was not found.";
        return out;
    }
    const ProjectState& base = *out.base;
    const Sequence& baseSeq = *base.findSequence(out.sequence);

    // Cancellation and the timeout share one stop source; the watchdog fires it at the deadline.
    std::stop_source local;
    std::stop_callback forward(stop, [&local] { local.request_stop(); });
    std::atomic<bool> timedOut{false};
    std::jthread watchdog([&](std::stop_token watchdogStop) {
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock lock(m);
        cv.wait_until(lock, watchdogStop, deadline, [] { return false; });
        if (!watchdogStop.stop_requested()) {
            timedOut = true;
            local.request_stop();
        }
    });
    const std::stop_token token = local.get_token();
    RunControl control(token, timedOut);

    // The staging branch: a session of its own, starting from the base (or the previous staged result).
    EditSessionOptions stagingOptions;
    stagingOptions.validateEdits = true;
    stagingOptions.deferRelease = false;
    stagingOptions.undoLimit = 8;
    EditSession staging(previous && previous->staged ? *previous->staged : base, stagingOptions);
    command::EnginePolicy stagingPolicy{widened(m_options.allowed, m_options.additiveProjectCommands, m_options.analysisCommands),
                                        64, m_options.services};
    stagingPolicy.allowFileCommands = false; // S5-P1-1: file commands wait for the user (needsConfirmation)
    command::CommandEngine engine(staging, *m_options.registry, std::move(stagingPolicy));
    if (previous) {
        out.batch = previous->batch;
        out.steps = previous->steps;
        out.usage = previous->usage;
        out.cost = previous->cost;
        out.modelCalls = previous->modelCalls;
        out.toolCalls = previous->toolCalls;
        out.invalidToolCalls = previous->invalidToolCalls;
        out.malformedReplies = previous->malformedReplies;
        out.repairedReplies = previous->repairedReplies;
    }
    out.batch.label = undoLabelFor(previous ? previous->batch.label.substr(previous->batch.label.starts_with("AI: ") ? 4 : 0)
                                            : request.prompt);

    ChatRequest chat;
    out.systemPrompt = systemPrompt(base, out.baseRevision, out.sequence);
    chat.system = out.systemPrompt;
    chat.tools = tools();
    chat.maxOutputTokens = limits.maxOutputTokens;
    const ModelSpec& modelSpec = model.spec();
    const bool localModel = modelSpec.provider == "local-llm";
    // AI-15: a small context gets the tools the request is about (the full catalog stays available through MCP and
    // the CLI), and a model that asks for it calls them through the constrained envelope.
    const std::size_t budget =
        m_options.toolBudget.value_or(localModel ? toolBudgetFor(static_cast<int>(modelSpec.contextTokens)) : 0);
    if (budget > 0) {
        RouteOptions route;
        route.maxTools = budget;
        chat.tools = routeTools(chat.tools, *m_options.registry, request.prompt, route);
    }
    for (const ToolSpec& t : chat.tools) {
        out.offeredTools.push_back(t.name);
    }
    if (localModel) {
        chat.maxOutputTokens = std::min(chat.maxOutputTokens, 1024);
    }
    std::optional<ConstrainedToolChat> constrained;
    if (m_options.constrainedTools.value_or(modelSpec.caps.constrainedTools)) {
        constrained.emplace(model);
    }
    IChatModel& caller = constrained ? static_cast<IChatModel&>(*constrained) : model;
    // The local context holds the prompt and the reply; the constrained envelope is a grammar, not prompt text.
    const std::int64_t contextLimit = modelSpec.contextTokens > 0 ? modelSpec.contextTokens - 592 : 7600;
    chat.cachePrompt = m_options.promptCaching;
    if (previous) {
        chat.messages = previous->conversation;
        AgentStep note;
        note.kind = AgentStep::Kind::Note;
        note.title = "Refine: " + request.prompt;
        out.steps.push_back(note);
    }
    chat.messages.push_back(ChatMessage::user(request.prompt + "\n\nChecked automatically when you finish:\n" +
                                              constraintLines(request.constraints, &baseSeq)));

    const auto today = m_options.today();
    // p02 integration: native tool calls carry every offered tool's full JSON schema, and the 0.2 catalog's routed
    // groups no longer always fit a small local context. The lowest-ranked routed tools are left out until the first
    // call fits (the constrained envelope's compact signatures need no trimming); a request that still does not fit
    // stops below as before.
    if (localModel && !constrained && budget > 0 && !previous) {
        const std::vector<ToolSpec> all = tools();
        RouteOptions route;
        route.maxTools = std::min(budget, chat.tools.size());
        while (route.maxTools > 1 &&
               estimateChatCost(model.spec(), chat, today).inputTokens + chat.maxOutputTokens > contextLimit) {
            --route.maxTools;
            chat.tools = routeTools(all, *m_options.registry, request.prompt, route);
        }
        out.offeredTools.clear();
        for (const ToolSpec& t : chat.tools) {
            out.offeredTools.push_back(t.name);
        }
    }
    int verificationsLeft = limits.verificationRetries;
    std::optional<std::uint64_t> verifiedAt; // staging revision the report describes
    bool finished = false;
    int stepsThisRun = 0;
    int toolCallsThisRun = 0;

    auto stopStatus = [&] { return timedOut.load() ? AgentStatus::TimedOut : AgentStatus::Cancelled; };
    auto stopMessage = [&] {
        return timedOut.load() ? "Stopped at the time limit (" + std::to_string(std::chrono::ceil<std::chrono::seconds>(limits.timeout).count()) +
                                     " s)."
                               : std::string("Cancelled.");
    };
    auto stagedNow = [&] { return staging.snapshot(); };

    while (!finished) {
        if (token.stop_requested()) {
            out.status = stopStatus();
            out.error = stopMessage();
            break;
        }
        if (stepsThisRun >= limits.maxSteps) {
            out.status = AgentStatus::StepLimit;
            out.error = "Stopped after " + std::to_string(limits.maxSteps) + " model calls (the step limit).";
            break;
        }
        // Budget: stop before a call that is expected to exceed what is left.
        ChatRequest sent = constrained ? constrained->prepare(chat) : chat;
        sent.responseFormat.reset();
        const CostEstimate estimate = estimateChatCost(model.spec(), sent, today);
        if (localModel && estimate.inputTokens + chat.maxOutputTokens > contextLimit) {
            out.status = AgentStatus::BudgetExceeded;
            out.error = "This request exceeds the local assistant context. Use a shorter request or a smaller sequence.";
            break;
        }
        if (limits.maxCost > 0 && estimate.priced && out.cost + estimate.expected > limits.maxCost) {
            out.status = AgentStatus::BudgetExceeded;
            char buf[96];
            std::snprintf(buf, sizeof buf, "Stopped at the budget: US$%.2f of US$%.2f used.",
                          static_cast<double>(out.cost) / kMicroUsdPerUsd, static_cast<double>(limits.maxCost) / kMicroUsdPerUsd);
            out.error = buf;
            break;
        }
        if (limits.maxTokens > 0 && out.usage.inputTokens + out.usage.outputTokens + estimate.inputTokens > limits.maxTokens) {
            out.status = AgentStatus::BudgetExceeded;
            out.error = "Stopped at the token budget (" + std::to_string(limits.maxTokens) + " tokens).";
            break;
        }

        phase(stepsThisRun == 0 ? "Thinking" : "Thinking about the results");
        auto response = caller.chat(
            chat,
            [&](const ChatEvent& e) {
                if (const auto* t = std::get_if<TextDelta>(&e)) {
                    AgentEvent ev;
                    ev.kind = AgentEvent::Kind::Text;
                    ev.text = t->text;
                    emit(std::move(ev));
                } else if (const auto* r = std::get_if<ReasoningDelta>(&e)) {
                    AgentEvent ev;
                    ev.kind = AgentEvent::Kind::Reasoning;
                    ev.text = r->text;
                    emit(std::move(ev));
                }
            },
            token);
        ++stepsThisRun;
        ++out.modelCalls;
        if (!response) {
            const AiError& error = response.error();
            if (error.code == AiErrorCode::Cancelled || token.stop_requested()) {
                out.status = stopStatus();
                out.error = stopMessage();
            } else if (error.code == AiErrorCode::SpendCapExceeded) {
                out.status = AgentStatus::BudgetExceeded;
                out.error = error.message;
            } else {
                out.status = AgentStatus::Failed;
                out.error = std::string(toString(error.code)) + ": " + error.message;
            }
            break;
        }
        out.usage += response->usage;
        out.cost += costOf(model.spec(), response->usage, today);
        {
            AgentEvent ev;
            ev.kind = AgentEvent::Kind::Usage;
            ev.usage = out.usage;
            ev.cost = out.cost;
            emit(std::move(ev));
        }
        chat.messages.push_back(response->message);
        const std::vector<ToolCall> calls = response->message.toolCalls();

        if (calls.empty()) {
            out.answer = response->message.text();
            if (response->stopReason == StopReason::Refusal || response->stopReason == StopReason::ContentFilter) {
                out.status = AgentStatus::Failed;
                out.error = "The model declined: " + out.answer;
                break;
            }
            // Done: check the staged result.
            phase("Checking the result");
            const ProjectSnapshot staged = stagedNow();
            out.verification = verify(base, *staged, out.sequence, request.constraints, out.batch.commands.size());
            out.verified = true;
            verifiedAt = staging.revision();
            const bool changed = !diff(base, *staged).empty() && !out.batch.commands.empty();
            if (changed && !out.verification.passed() && verificationsLeft > 0) {
                --verificationsLeft;
                AgentStep check;
                check.kind = AgentStep::Kind::Verify;
                check.title = "Checks failed; asking the model to fix the edit";
                check.detail = out.verification.failuresForModel();
                check.ok = false;
                out.steps.push_back(check);
                AgentEvent ev;
                ev.kind = AgentEvent::Kind::StepFinished;
                ev.step = check;
                emit(std::move(ev));
                chat.messages.push_back(ChatMessage::user(
                    "The automatic checks of the staged edit failed:\n" + out.verification.failuresForModel() +
                    "Fix the edit with the tools (the staged edit so far stays; undo parts by editing again), or explain "
                    "why it cannot be done."));
                continue;
            }
            out.status = !changed ? AgentStatus::NoChanges
                                  : (out.verification.passed() ? AgentStatus::Staged : AgentStatus::VerificationFailed);
            finished = true;
            break;
        }

        std::vector<ToolResult> results;
        bool stopped = false;
        for (const ToolCall& call : calls) {
            ToolResult result;
            result.callId = call.id;
            result.name = call.name;
            if (stopped) {
                result.content = errorJson({ErrorCode::Cancelled, "not run: the turn was stopped", nullptr}).dump();
                result.isError = true;
                results.push_back(std::move(result));
                continue;
            }
            if (toolCallsThisRun >= limits.maxToolCalls) {
                out.status = AgentStatus::StepLimit;
                out.error = "Stopped after " + std::to_string(limits.maxToolCalls) + " tool calls (the limit).";
                stopped = true;
                result.content = errorJson({ErrorCode::Cancelled, "not run: the tool call limit was reached", nullptr}).dump();
                result.isError = true;
                results.push_back(std::move(result));
                continue;
            }
            ++toolCallsThisRun;
            ++out.toolCalls;

            AgentStep step;
            step.tool = call.name;
            const command::CommandSpec* spec = m_options.registry->find(call.name);
            const ProjectSnapshot before = stagedNow();
            CommandError error{ErrorCode::InvalidRequest, {}, nullptr};
            bool failed = false;
            if (!spec) {
                failed = true;
                ++out.invalidToolCalls;
                error = {ErrorCode::UnknownCommand, "There is no tool named " + call.name + ".", nullptr};
                step.title = call.name;
            } else if (!call.arguments.is_object()) {
                failed = true;
                ++out.invalidToolCalls;
                error = {ErrorCode::InvalidParams, "The arguments are not a JSON object: " + call.argumentsText.substr(0, 200), nullptr};
                step.title = spec->title;
                step.tool = spec->name;
            } else {
                step.tool = spec->name;
                Json params = withoutStrictNulls(call.arguments, spec->params);
                const bool takesSequence = spec->params.contains("properties") && spec->params["properties"].contains("sequence");
                if (takesSequence && !params.contains("sequence")) {
                    params["sequence"] = out.sequence.value;
                }
                step.params = params;
                // The schema first (review wave 3, P1-2): descriptions and commands only ever see well-typed params.
                const auto issues = command::validateAgainstSchema(spec->params, params);
                if (!issues.empty()) {
                    failed = true;
                    ++out.invalidToolCalls;
                    Json details = Json::array();
                    std::string summary;
                    for (const command::SchemaIssue& issue : issues) {
                        details.push_back(Json{{"path", issue.path}, {"message", issue.message}});
                        if (details.size() <= 3) {
                            summary += (summary.empty() ? "" : "; ") +
                                       (issue.path.empty() ? std::string("params") : issue.path) + ": " + issue.message;
                        }
                    }
                    error = {ErrorCode::InvalidParams, "The arguments do not match the tool's schema: " + summary,
                             Json{{"issues", std::move(details)}}};
                    step.title = spec->title;
                } else {
                    step.title = describeCall(*spec, params, *before, out.sequence);
                }
                // A sequence this request made itself (roughcut.fromScript, shorts.create) may be edited further: "make a
                // first cut and pick the cleanest takes" swaps takes in the new cut.
                const auto named = params.find("sequence"); // never operator[]: it would add "sequence": null
                const bool madeHere = named != params.end() && named->is_number_unsigned() &&
                                      !base.findSequence(SequenceId{named->get<std::uint64_t>()}) &&
                                      before->findSequence(SequenceId{named->get<std::uint64_t>()});
                if (!failed && spec->mutates() && takesSequence && params["sequence"] != Json(out.sequence.value) && !madeHere) {
                    failed = true;
                    error = {ErrorCode::PermissionDenied,
                             "This request edits sequence " + std::to_string(out.sequence.value) + " only.", nullptr};
                }
                if (!failed &&
                    !permitted(*spec, m_options.allowed, m_options.additiveProjectCommands, m_options.analysisCommands)) {
                    failed = true;
                    error = {ErrorCode::PermissionDenied, "The assistant may not use " + spec->name + " here.", nullptr};
                }
                if (!failed && spec->capability == command::Capability::EditProject && !m_options.allowed.has(spec->capability)) {
                    // An additive command: it may only add sequences, at most maxNewSequences per request (C.11: library
                    // commands add none).
                    const auto library = defaultLibraryCommands();
                    const std::size_t adding = std::ranges::find(library, spec->name) != library.end() ? 0
                                               : params.contains("shorts") && params["shorts"].is_array() ? params["shorts"].size()
                                                                                                          : 1;
                    const std::size_t made = newSequences(base, *before);
                    if (made + adding > m_options.maxNewSequences) {
                        failed = true;
                        error = {ErrorCode::PermissionDenied,
                                 "At most " + std::to_string(m_options.maxNewSequences) + " new sequences per request (" +
                                     std::to_string(made) + " made so far).",
                                 nullptr};
                    }
                }
                if (!failed && spec->touchesFiles) {
                    // S5-P1-1: a model never makes the app read or write a file it picked. Put aside for the user, who
                    // runs it (and sees the path) through the app; neither the preview nor Accept runs it.
                    out.needsConfirmation.push_back(AgentConfirmation{spec->name, params, step.title});
                    step.kind = AgentStep::Kind::Note;
                    step.detail = "Needs your confirmation: it reads or writes files, so you run it yourself";
                    result.content =
                        Json{{"ok", false},
                             {"needsConfirmation", true},
                             {"note", "This tool reads or writes files, so it never runs in the preview: it was put aside "
                                      "for the user to confirm and run. Continue without it, and say in your answer that it "
                                      "waits for the user's confirmation."}}
                            .dump();
                } else if (!failed) {
                    AgentEvent startedEvent;
                    startedEvent.kind = AgentEvent::Kind::StepStarted;
                    startedEvent.step = step;
                    emit(std::move(startedEvent));
                    command::CommandRequest req;
                    req.command = spec->name;
                    req.version = spec->version;
                    req.params = params;
                    const command::CommandResult r = engine.execute(req, &control);
                    if (!r.ok()) {
                        failed = true;
                        error = *r.error;
                        if (error.code == ErrorCode::Cancelled || error.code == ErrorCode::TimedOut) {
                            stopped = true;
                            out.status = stopStatus();
                            out.error = stopMessage();
                        }
                    } else if (!spec->mutates()) {
                        step.kind = AgentStep::Kind::Read;
                        std::string data = r.data.dump();
                        if (data.size() > limits.maxToolResultChars) {
                            std::size_t cut = limits.maxToolResultChars;
                            while (cut > 0 && (static_cast<unsigned char>(data[cut]) & 0xC0) == 0x80) {
                                --cut;
                            }
                            data = Json{{"truncated", true},
                                        {"note", "Cut at " + std::to_string(cut) +
                                                     " characters: narrow the request (from/to, tracks, first/last, limit)."},
                                        {"partial", data.substr(0, cut)}}
                                       .dump();
                        }
                        result.content = std::move(data);
                        step.detail = "Read";
                    } else {
                        step.kind = AgentStep::Kind::Edit;
                        const ProjectSnapshot after = stagedNow();
                        Json content{{"ok", true}};
                        if (r.committed) {
                            out.batch.commands.push_back(req);
                            step.detail = describeChanges(r.changes, *before, *after);
                            content["changed"] = step.detail;
                            if (const Sequence* s = after->findSequence(out.sequence)) {
                                content["sequenceLength"] = tc(*s, s->contentEnd());
                            }
                        } else {
                            step.detail = "Nothing changed";
                            content["changed"] = "nothing";
                        }
                        if (!r.createdIds.empty()) {
                            content["createdIds"] = r.createdIds;
                        }
                        if (!r.warnings.empty()) {
                            content["warnings"] = r.warnings;
                        }
                        if (!r.data.is_null()) {
                            content["data"] = r.data;
                        }
                        result.content = content.dump();
                    }
                }
            }
            if (failed) {
                step.kind = AgentStep::Kind::Error;
                step.ok = false;
                step.detail = error.message;
                result.content = errorJson(error).dump();
                result.isError = true;
            }
            out.steps.push_back(step);
            AgentEvent finishedEvent;
            finishedEvent.kind = AgentEvent::Kind::StepFinished;
            finishedEvent.step = step;
            emit(std::move(finishedEvent));
            results.push_back(std::move(result));
        }
        chat.messages.push_back(ChatMessage::toolResults(std::move(results)));
        if (stopped) {
            break;
        }
    }

    // The staged result, and the checks for runs that stopped before the model finished.
    out.staged = std::make_shared<const ProjectState>(staging.stateForSave());
    out.changes = diff(base, *out.staged);
    if (out.hasChanges() && verifiedAt != staging.revision()) {
        out.verification = verify(base, *out.staged, out.sequence, request.constraints, out.batch.commands.size());
        out.verified = true;
    }
    if (out.verified) {
        AgentStep check;
        check.kind = AgentStep::Kind::Verify;
        check.ok = out.verification.passed();
        check.title = check.ok ? "All checks passed" : "Some checks failed";
        check.detail = out.verification.failuresForModel();
        out.steps.push_back(check);
    }
    out.conversation = chat.messages;
    if (constrained) {
        const ConstrainedStats stats = constrained->stats();
        out.malformedReplies += stats.malformed;
        out.repairedReplies += stats.repaired;
    }

    // The redacted record of the run.
    const Redactor& redactor = Redactor::global();
    Json steps = Json::array();
    for (const AgentStep& s : out.steps) {
        steps.push_back(Json{{"kind", stepKind(s.kind)}, {"title", redactor.redact(s.title)}, {"detail", redactor.redact(s.detail)},
                             {"tool", s.tool}, {"ok", s.ok}});
    }
    Json toolNames = Json::array();
    for (const ToolSpec& t : chat.tools) {
        toolNames.push_back(t.name);
    }
    out.transcript = Json{{"version", 1},
                          {"time", isoNow()},
                          {"model", out.model},
                          {"status", std::string(toString(out.status))},
                          {"error", redactor.redact(out.error)},
                          {"prompt", redactor.redact(request.prompt)},
                          {"sequence", out.sequence.value},
                          {"baseRevision", out.baseRevision},
                          {"constraints", constraintsJson(request.constraints)},
                          {"system", redactor.redact(out.systemPrompt)},
                          {"tools", toolNames},
                          {"messages", conversationToJson(out.conversation)},
                          {"steps", steps},
                          {"verification", verificationJson(out.verification)},
                          {"usage", usageJson(out.usage)},
                          {"costMicroUsd", out.cost},
                          {"modelCalls", out.modelCalls},
                          {"toolCalls", out.toolCalls},
                          {"invalidToolCalls", out.invalidToolCalls},
                          {"malformedReplies", out.malformedReplies},
                          {"batch", redactedJson(command::toJson(out.batch), redactor)},
                          {"elapsedMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - started).count()}};
    if (!m_options.transcriptDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(m_options.transcriptDir, ec);
        static std::atomic<int> counter{0};
        std::string stamp = isoNow();
        for (char& ch : stamp) {
            if (ch == ':') {
                ch = '-';
            }
        }
        const auto file = m_options.transcriptDir / ("agent-" + stamp + "-" + std::to_string(++counter) + ".json");
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (stream << out.transcript.dump(2) << '\n') {
            out.transcriptFile = file;
        }
    }
    return out;
}

ApplyResult applyStaged(EditSession& live, const AgentOutcome& outcome, const ApplyOptions& options, std::string label) {
    ApplyResult result;
    result.revision = live.revision();
    if (!outcome.canApply()) {
        result.status = ApplyResult::Status::Refused;
        result.message = "There is nothing to apply.";
        return result;
    }
    const command::CommandRegistry& registry = options.registry ? *options.registry : edit::editingCommands();
    for (const command::CommandRequest& request : outcome.batch.commands) {
        const command::CommandSpec* spec = registry.find(request.command, request.version);
        if (!spec || !permitted(*spec, options.allowed, options.additiveProjectCommands)) {
            result.status = ApplyResult::Status::Refused;
            result.message = "The edit uses " + request.command + ", which is not allowed here.";
            return result;
        }
    }
    command::EnginePolicy livePolicy{widened(options.allowed, options.additiveProjectCommands), 16, options.services};
    livePolicy.allowFileCommands = false; // S5-P1-1: a staged batch never holds a file command (needsConfirmation)
    command::CommandEngine engine(live, registry, std::move(livePolicy));
    command::CommandBatch batch = outcome.batch;
    batch.label = label.empty() ? outcome.batch.label : std::move(label);
    batch.idempotencyKey.reset();

    if (live.revision() == outcome.baseRevision) {
        batch.expectedRevision = outcome.baseRevision;
        batch.dryRun = false;
        const command::CommandResult r = engine.execute(batch);
        if (!r.ok()) {
            result.status = ApplyResult::Status::Failed;
            result.message = "The edit could not be applied: " + r.error->message;
            return result;
        }
        // The replay on the live session must make exactly the staged result (same state, same ids).
        if (!diff(*outcome.staged, *live.snapshot()).empty()) {
            if (r.committed) {
                live.undo();
            }
            result.status = ApplyResult::Status::Failed;
            result.message = "The applied edit did not match the preview, so it was undone.";
            result.revision = live.revision();
            return result;
        }
        result.status = ApplyResult::Status::Applied;
        result.message = "Applied as one undo step.";
        result.revision = live.revision();
        return result;
    }

    // Stale: the user edited meanwhile. Rebase by re-running the batch as a dry run on the current state.
    const ProjectState current = live.stateForSave();
    batch.expectedRevision = live.revision();
    batch.dryRun = true;
    const command::CommandResult dry = engine.execute(batch);
    if (!dry.ok() || !dry.preview) {
        result.status = ApplyResult::Status::Refused;
        result.message = "The timeline changed since the preview and the edit no longer applies (" +
                         (dry.error ? dry.error->message : std::string("no preview")) + "). Run the request again.";
        return result;
    }
    const ChangeSet intended = diff(*outcome.base, *outcome.staged);
    const ChangeSet now = diff(current, *dry.preview);
    if (!(signatureOf(intended, outcome.sequence) == signatureOf(now, outcome.sequence))) {
        result.status = ApplyResult::Status::Refused;
        result.message = "The timeline changed since the preview in a way that changes what this edit does. Run the "
                         "request again.";
        return result;
    }
    result.verification = verify(current, *dry.preview, outcome.sequence, outcome.constraints, batch.commands.size());
    if (!result.verification.passed()) {
        result.status = ApplyResult::Status::Refused;
        result.message = "On the changed timeline the edit fails its checks: " + result.verification.failuresForModel();
        return result;
    }
    batch.dryRun = false;
    const command::CommandResult r = engine.execute(batch);
    if (!r.ok()) {
        result.status = ApplyResult::Status::Failed;
        result.message = "The edit could not be applied: " + r.error->message;
        return result;
    }
    result.status = ApplyResult::Status::Rebased;
    result.message = "The timeline had changed; the edit was re-run on it and applied as one undo step.";
    result.revision = live.revision();
    return result;
}

} // namespace wabcut::ai::agent
