// Built-in collaboration commands (C.9): version history (history.*), review notes and packages (review.*) and the
// three-way project merge (project.merge). Model: History.h, ProjectDiff.h, ReviewOps.h, Merge.h.

#include "Catalog.h"

#include <wabcut/command/ReviewPackage.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectDiff.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ReviewOps.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <tuple>

namespace wabcut::command::detail {

namespace {

std::string nowUtc() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

CommandOutput readOutput(ExecutionContext& c, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    return out;
}

Json boundedString(std::string description, std::size_t maxBytes, bool required = false) {
    Json s = schema::string(std::move(description));
    s["maxLength"] = static_cast<std::int64_t>(maxBytes);
    if (required) {
        s["minLength"] = 1;
    }
    return s;
}

// --- History -----------------------------------------------------------------------------------------------

ProjectHistory& historyOf(ExecutionContext& c) {
    const auto* services = findService<HistoryServices>(c.services());
    if (!services || !services->history()) {
        fail(ErrorCode::InvalidRequest, "this host keeps no version history here (it needs a saved project)");
    }
    return *services->history();
}

Json revisionJson(const HistoryRevision& r) {
    return Json::array({r.id, r.createdAt, std::string(toString(r.kind)), r.label, r.sequences, r.clips, r.addedBytes});
}

std::uint64_t revisionParam(ExecutionContext& c, const char* key) {
    const Json& v = c.params()[key];
    if (!v.is_number_unsigned() && !(v.is_number_integer() && v.get<std::int64_t>() > 0)) {
        fail(ErrorCode::InvalidParams, "expected a revision id", Json{{"path", std::string("/") + key}});
    }
    return v.get<std::uint64_t>();
}

ProjectState loadRevision(ExecutionContext& c, std::uint64_t id) { return c.take(historyOf(c).load(id)); }

/// A revision id or "current" (the project as it is).
ProjectState versionParam(ExecutionContext& c, const char* key) {
    const Json& v = c.params()[key];
    if (v.is_string() && v.get<std::string>() == "current") {
        return c.state();
    }
    return loadRevision(c, revisionParam(c, key));
}

Json diffJson(const ProjectDiff& diff) {
    Json sequences = Json::array();
    for (const SequenceDiff& s : diff.sequences) {
        std::size_t added = 0;
        std::size_t removed = 0;
        std::size_t changed = 0;
        for (const ClipDiff& d : s.clips) {
            (d.kind == ClipDiff::Kind::Added ? added : d.kind == ClipDiff::Kind::Removed ? removed : changed) += 1;
        }
        const char* kind = s.kind == SequenceDiff::Kind::Added ? "added" : s.kind == SequenceDiff::Kind::Removed ? "removed" : "changed";
        Json clips = Json::array();
        for (const ClipDiff& d : s.clips) {
            const char* k = d.kind == ClipDiff::Kind::Added ? "added" : d.kind == ClipDiff::Kind::Removed ? "removed" : "changed";
            const auto tc = [&](Ticks t) { return ticksToTimecode(t, s.frameRate, s.dropFrame); };
            clips.push_back(Json{{"clip", d.clip.value},
                                 {"name", d.name},
                                 {"kind", k},
                                 {"track", d.trackAfter.empty() ? d.trackBefore : d.trackAfter},
                                 {"before", d.kind == ClipDiff::Kind::Added ? Json(nullptr) : Json::array({tc(d.before.start), tc(d.before.end())})},
                                 {"after", d.kind == ClipDiff::Kind::Removed ? Json(nullptr) : Json::array({tc(d.after.start), tc(d.after.end())})},
                                 {"moved", d.moved},
                                 {"trimmed", d.trimmed},
                                 {"retimed", d.retimed},
                                 {"effects", d.effectsAdded.size() + d.effectsRemoved.size() + d.effectsChanged.size()},
                                 {"keyframes", d.keyframes}});
        }
        sequences.push_back(Json{{"id", s.id.value},
                                 {"name", s.name},
                                 {"kind", kind},
                                 {"clipsAdded", added},
                                 {"clipsRemoved", removed},
                                 {"clipsChanged", changed},
                                 {"settings", s.settings},
                                 {"clips", std::move(clips)}});
    }
    return Json{{"summary", diff.summary()},
                {"sequences", std::move(sequences)},
                {"mediaAdded", diff.mediaAdded},
                {"mediaRemoved", diff.mediaRemoved},
                {"reviewNotes", Json::array({diff.reviewNotesAdded, diff.reviewNotesRemoved, diff.reviewNotesChanged})}};
}

std::vector<std::string> kindNames() { return {"save", "auto", "agent", "manual", "restore"}; }

// --- Review notes -------------------------------------------------------------------------------------------

ReviewNoteId noteParam(ExecutionContext& c) {
    const Json& v = c.params()["note"];
    if (!v.is_number_integer() || v.get<std::int64_t>() <= 0) {
        fail(ErrorCode::InvalidParams, "expected a review note id", Json{{"path", "/note"}});
    }
    return ReviewNoteId{v.get<std::uint64_t>()};
}

Json noteJson(ExecutionContext& c, const ReviewNote& n) {
    Json replies = Json::array();
    for (const ReviewReply& r : n.replies) {
        replies.push_back(Json::array({r.author, r.text, r.createdAt}));
    }
    const Sequence* s = c.state().findSequence(n.sequence);
    const auto tc = [&](Ticks t) { return s ? ticksToTimecode(t, s->frameRate, s->dropFrame) : std::to_string(t.value); };
    return Json{{"id", n.id.value},
                {"sequence", n.sequence.value},
                {"start", tc(n.start)},
                {"end", tc(n.end())},
                {"mark", n.mark ? Json::array({n.mark->x, n.mark->y, n.mark->width, n.mark->height}) : Json(nullptr)},
                {"author", n.author},
                {"text", n.text},
                {"createdAt", n.createdAt},
                {"status", std::string(toString(n.status))},
                {"replies", std::move(replies)}};
}

Json markSchema() {
    return Json{{"type", "object"},
                {"description", "Where on the frame: a rectangle in frame fractions from the top left (width = height = 0: "
                                "a point)."},
                {"properties", Json{{"x", schema::number("Left, 0..1.", 0.0, 1.0)},
                                    {"y", schema::number("Top, 0..1.", 0.0, 1.0)},
                                    {"width", schema::number("Width, 0..1 (default 0).", 0.0, 1.0)},
                                    {"height", schema::number("Height, 0..1 (default 0).", 0.0, 1.0)}}},
                {"required", Json::array({"x", "y"})},
                {"additionalProperties", false}};
}

} // namespace

void registerCollaborationCommands(CommandRegistry& registry) {
    // --- Version history -------------------------------------------------------------------------------------
    addBuiltin(registry,
               {"history.list", 1, "Version History",
                "The project's saved versions, oldest first, as arrays: id, createdAt (UTC), kind (save, auto, agent, "
                "manual, restore), label (named versions are kept), sequences, clips, the bytes the version added.",
                Capability::Read, withSequence({}), Json::object(), {},
                [](ExecutionContext& c) {
                    const ProjectHistory& h = historyOf(c);
                    Json revisions = Json::array();
                    for (const HistoryRevision& r : h.revisions()) {
                        revisions.push_back(revisionJson(r));
                    }
                    return readOutput(c, Json{{"revisionFields", Json::array({"id", "createdAt", "kind", "label", "sequences",
                                                                              "clips", "addedBytes"})},
                                              {"revisions", std::move(revisions)},
                                              {"diskBytes", h.diskBytes()}});
                }});
    addBuiltin(registry,
               {"history.checkpoint", 1, "Save Version",
                "Records the project as it is now in the version history (unchanged since the last version: nothing is "
                "added). Give \"label\" to name it (\"Client cut v2\"): named versions are never pruned. Hosts call it on "
                "save, every few minutes of editing and before an AI agent applies a plan (kind).",
                Capability::EditProject,
                withSequence({{"label", boundedString("A name for the version.", kMaxHistoryLabelBytes)},
                              {"kind", schema::enumeration(kindNames(), "What made it (default manual).")}}),
                Json{{"label", "Client cut v2"}}, {},
                [](ExecutionContext& c) {
                    const HistoryKind kind = historyKindFromString(c.string("kind", "manual")).value_or(HistoryKind::Manual);
                    const HistoryRevision r = c.take(historyOf(c).checkpoint(c.state(), kind, c.string("label")));
                    return readOutput(c, Json{{"revision", revisionJson(r)}});
                }});
    addBuiltin(registry,
               {"history.name", 1, "Name Version",
                "Names a saved version (an empty label unnames it). Named versions are kept when the history prunes.",
                Capability::EditProject,
                withSequence({{"revision", schema::integer("The version's id (history.list).", 1)},
                              {"label", boundedString("The name.", kMaxHistoryLabelBytes)}},
                             {"revision", "label"}),
                Json{{"revision", 12}, {"label", "Client cut v2"}}, {},
                [](ExecutionContext& c) {
                    c.take(historyOf(c).name(revisionParam(c, "revision"), c.string("label")));
                    return readOutput(c, Json{{"revision", revisionParam(c, "revision")}, {"label", c.string("label")}});
                }});
    addBuiltin(
        registry,
        {"history.restore", 1, "Restore Version",
         "Goes back to a saved version: the whole project (one undo step; the project as it was is saved as a version "
         "first), or, with \"sequence\", that sequence of the version as a new sequence next to the current ones (the "
         "media it plays is the project's own where the project still has it). The no-cloud lock stays on.",
         Capability::EditProject,
         withSequence({{"revision", schema::integer("The version's id (history.list).", 1)},
                       {"from", schema::integer("Restore only this sequence of the version, as a new sequence.", 1)},
                       {"name", boundedString("The new sequence's name (default: \"<name> (version <id>)\").", 256)}},
                      {"revision"}),
         Json{{"revision", 12}}, {},
         [](ExecutionContext& c) {
             ProjectHistory& h = historyOf(c);
             const std::uint64_t id = revisionParam(c, "revision");
             const ProjectState version = loadRevision(c, id);
             CommandOutput out;
             if (c.has("from")) {
                 const SequenceId seq{static_cast<std::uint64_t>(c.integer("from"))};
                 const Sequence* original = version.findSequence(seq);
                 if (!original) {
                     fail(ErrorCode::NotFound, "that version has no sequence " + std::to_string(seq.value), Json{{"path", "/from"}});
                 }
                 const std::string name = c.has("name") ? c.string("name") : original->name + " (version " + std::to_string(id) + ")";
                 edit::Created<SequenceId> copy = c.take(edit::copySequenceFrom(c.state(), version, seq, name));
                 out.state = std::move(copy.state);
                 out.createdIds = {copy.id.value};
                 out.data = Json{{"sequence", copy.id.value}, {"name", name}};
                 return out;
             }
             // The project as it was is saved as a version once the restore commits (E5-P2-7): a dry run or a batch
             // that fails later writes nothing to disk. The undo step keeps it should the save fail.
             out.afterCommit.push_back([&h, before = c.state()] { (void)h.checkpoint(before, HistoryKind::Restore, {}); });
             ProjectState restored = version;
             restored.ids.next = std::max(restored.ids.next, c.state().ids.next); // ids keep growing
             restored.aiNoCloud = restored.aiNoCloud || c.state().aiNoCloud;     // a restore never unlocks the cloud
             const ProjectDiff diff = diffProjects(c.state(), restored);
             out.state = std::move(restored);
             out.data = Json{{"revision", id}, {"changes", diff.summary()}};
             return out;
         }});
    addBuiltin(registry,
               {"history.diff", 1, "Compare Versions",
                "What changed from one version to another (\"current\" is the project as it is): per sequence the clips "
                "added, removed, moved, trimmed or retimed, effects and keyframes changed, settings, tracks, markers and "
                "captions; media and review notes. \"summary\" says it in lines with timecodes.",
                Capability::Read,
                withSequence({{"from", Json{{"type", Json::array({"integer", "string"})},
                                            {"description", "A version id, or \"current\"."}}},
                              {"to", Json{{"type", Json::array({"integer", "string"})},
                                          {"description", "A version id, or \"current\" (default)."}}}},
                             {"from"}),
                Json{{"from", 12}, {"to", "current"}}, {},
                [](ExecutionContext& c) {
                    const ProjectState from = versionParam(c, "from");
                    const ProjectState to = c.has("to") ? versionParam(c, "to") : c.state();
                    return readOutput(c, diffJson(diffProjects(from, to)));
                }});
    addBuiltin(registry,
               command::touchesFiles({"history.export", 1, "Save Version As",
                "Writes a saved version as a project file of its own (\"restore as a new project\"). Refuses to replace a "
                "file unless \"overwrite\".",
                Capability::EditProject,
                withSequence({{"revision", schema::integer("The version's id (history.list).", 1)},
                              {"path", boundedString("The new project file (UTF-8 path).", 4096, true)},
                              {"overwrite", schema::boolean("Replace an existing file (default false).")}},
                             {"revision", "path"}),
                Json{{"revision", 12}, {"path", "C:/Projects/Film (client cut v2).wabcut"}}, {},
                [](ExecutionContext& c) {
                    const ProjectState version = loadRevision(c, revisionParam(c, "revision"));
                    const std::filesystem::path file = pathFromUtf8(c.string("path"));
                    std::error_code ec;
                    if (std::filesystem::exists(file, ec) && !c.boolean("overwrite", false)) {
                        fail(ErrorCode::EditRejected, "the file exists; pass \"overwrite\" to replace it", Json{{"path", "/path"}});
                    }
                    c.take(io::saveProject(version, file, io::SaveOptions{0}));
                    return readOutput(c, Json{{"path", c.string("path")}});
                }}, {"path"}));

    // --- Review notes -------------------------------------------------------------------------------------------
    addBuiltin(registry,
               {"review.add", 1, "Add Review Note",
                "A frame-accurate comment on the sequence: at \"time\" (a frame) or from \"start\" to \"end\", optionally "
                "pointing at a spot or rectangle of the frame (\"mark\"), by \"author\". Returns its id.",
                Capability::EditProject,
                withSequence({{"time", timeParam("The frame it is about.")},
                              {"start", timeParam("Or the start of a range.")},
                              {"end", timeParam("The end of the range (exclusive).")},
                              {"text", boundedString("The comment (UTF-8).", kMaxReviewTextBytes, true)},
                              {"author", boundedString("Who wrote it.", kMaxReviewAuthorBytes)},
                              {"mark", markSchema()}},
                             {"text"}),
                Json{{"time", "00:00:12:05"}, {"text", "The logo is too small"}, {"author", "Client"},
                     {"mark", Json{{"x", 0.7}, {"y", 0.05}, {"width", 0.2}, {"height", 0.1}}}},
                {},
                [](ExecutionContext& c) {
                    ReviewNote n;
                    n.sequence = c.sequenceId();
                    if (c.has("time")) {
                        n.start = c.time("time");
                    } else if (c.has("start")) {
                        n.start = c.time("start");
                        if (c.has("end")) {
                            const Ticks end = c.time("end");
                            if (end < n.start) {
                                fail(ErrorCode::InvalidParams, "the range ends before it starts", Json{{"path", "/end"}});
                            }
                            n.duration = end - n.start;
                        }
                    } else {
                        fail(ErrorCode::InvalidParams, "pass \"time\" or \"start\"", Json{{"path", "/time"}});
                    }
                    n.text = c.string("text");
                    n.author = c.string("author");
                    n.createdAt = nowUtc();
                    if (c.has("mark")) {
                        const Json& m = c.params()["mark"];
                        n.mark = ReviewMark{m["x"].get<double>(), m["y"].get<double>(), m.value("width", 0.0), m.value("height", 0.0)};
                    }
                    edit::Created<ReviewNoteId> added = c.take(edit::addReviewNote(c.state(), std::move(n)));
                    CommandOutput out;
                    out.state = std::move(added.state);
                    out.createdIds = {added.id.value};
                    out.data = Json{{"note", added.id.value}};
                    return out;
                }});
    addBuiltin(registry,
               {"review.reply", 1, "Reply to Review Note", "Adds a reply to a note's thread.", Capability::EditProject,
                withSequence({{"note", schema::integer("The note's id.", 1)},
                              {"text", boundedString("The reply (UTF-8).", kMaxReviewTextBytes, true)},
                              {"author", boundedString("Who wrote it.", kMaxReviewAuthorBytes)}},
                             {"note", "text"}),
                Json{{"note", 31}, {"text", "Bigger in v3"}, {"author", "Editor"}}, {},
                [](ExecutionContext& c) {
                    CommandOutput out;
                    out.state = c.take(edit::replyToReviewNote(c.state(), noteParam(c),
                                                               ReviewReply{c.string("author"), c.string("text"), nowUtc()}));
                    return out;
                }});
    addBuiltin(registry,
               {"review.resolve", 1, "Resolve Review Note", "Marks a note resolved (or open again with \"resolved\": false).",
                Capability::EditProject,
                withSequence({{"note", schema::integer("The note's id.", 1)},
                              {"resolved", schema::boolean("true (default): resolved; false: open.")}},
                             {"note"}),
                Json{{"note", 31}}, {},
                [](ExecutionContext& c) {
                    CommandOutput out;
                    out.state = c.take(edit::setReviewNoteStatus(
                        c.state(), noteParam(c), c.boolean("resolved", true) ? ReviewStatus::Resolved : ReviewStatus::Open));
                    return out;
                }});
    addBuiltin(registry,
               {"review.remove", 1, "Delete Review Note", "Deletes a note and its replies.", Capability::EditProject,
                withSequence({{"note", schema::integer("The note's id.", 1)}}, {"note"}), Json{{"note", 31}}, {},
                [](ExecutionContext& c) {
                    CommandOutput out;
                    out.state = c.take(edit::removeReviewNote(c.state(), noteParam(c)));
                    return out;
                }});
    addBuiltin(registry,
               {"review.list", 1, "Review Notes",
                "The review notes of the sequence (\"sequence\"; all the project's without it), in time order, with their "
                "timecodes, mark, author, status and replies ([author, text, createdAt]).",
                Capability::Read,
                withSequence({{"status", schema::enumeration({"open", "resolved"}, "Only notes with this status.")}}),
                Json{{"status", "open"}}, {},
                [](ExecutionContext& c) {
                    std::vector<const ReviewNote*> notes;
                    const std::optional<SequenceId> only = c.has("sequence") ? std::optional(c.sequenceId()) : std::nullopt;
                    const std::optional<ReviewStatus> status =
                        c.has("status") ? reviewStatusFromString(c.string("status")) : std::nullopt;
                    for (const ReviewNote& n : c.state().reviewNotes) {
                        if ((!only || n.sequence == *only) && (!status || n.status == *status)) {
                            notes.push_back(&n);
                        }
                    }
                    std::stable_sort(notes.begin(), notes.end(), [](const ReviewNote* a, const ReviewNote* b) {
                        return std::tuple(a->sequence, a->start) < std::tuple(b->sequence, b->start);
                    });
                    Json list = Json::array();
                    for (const ReviewNote* n : notes) {
                        list.push_back(noteJson(c, *n));
                    }
                    return readOutput(c, Json{{"notes", std::move(list)}});
                }});
    addBuiltin(registry,
               {"review.toMarkers", 1, "Review Notes to Markers",
                "Turns the sequence's review notes (\"notes\", or all of them) into sequence markers: named after the "
                "note's first line, the comment holding the text, author and replies, red while open, green once resolved. "
                "With \"remove\" the notes go.",
                Capability::EditSequence,
                withSequence({{"notes", schema::array(schema::integer("A note id.", 1), "The notes (default: all).")},
                              {"remove", schema::boolean("Delete the notes too (default false).")}}),
                Json{{"remove", true}}, {},
                [](ExecutionContext& c) {
                    std::vector<ReviewNoteId> ids;
                    if (c.has("notes")) {
                        for (const Json& v : c.params()["notes"]) {
                            ids.push_back(ReviewNoteId{v.get<std::uint64_t>()});
                        }
                    }
                    edit::EditResult r = c.take(edit::reviewNotesToMarkers(c.state(), c.sequenceId(), ids, c.boolean("remove", false)));
                    const std::size_t markers = r.state.findSequence(c.sequenceId())->markers.size();
                    return edited(std::move(r), Json{{"markers", markers}});
                }});
    addBuiltin(
        registry,
        command::touchesFiles({"review.exportPackage", 1, "Export Review Package",
         "Writes a review package folder for a client or reviewer: notes.json (the notes with timecodes) and index.html (one "
         "self-contained page with a thumbnail per note, its mark drawn on it), plus the thumbnails in frames/. Notes: the "
         "sequence's (\"sequence\"; all the project's without it), or \"status\" only. Thumbnails come from the host's "
         "renderer, else placeholder cards.",
         Capability::EditProject, // it writes files (like color.cdlExport): never for a read-only client
         withSequence({{"path", boundedString("The package folder (UTF-8 path).", 4096, true)},
                       {"title", boundedString("The page's title.", 256)},
                       {"status", schema::enumeration({"open", "resolved"}, "Only notes with this status.")}},
                      {"path"}),
         Json{{"path", "C:/Review/Client cut v2.wabreview"}, {"title", "Client cut v2"}}, {},
         [](ExecutionContext& c) {
             std::vector<ReviewNote> notes;
             const std::optional<SequenceId> only = c.has("sequence") ? std::optional(c.sequenceId()) : std::nullopt;
             const std::optional<ReviewStatus> status = c.has("status") ? reviewStatusFromString(c.string("status")) : std::nullopt;
             for (const ReviewNote& n : c.state().reviewNotes) {
                 if ((!only || n.sequence == *only) && (!status || n.status == *status)) {
                     notes.push_back(n);
                 }
             }
             std::stable_sort(notes.begin(), notes.end(), [](const ReviewNote& a, const ReviewNote& b) {
                 return std::tuple(a.sequence, a.start) < std::tuple(b.sequence, b.start);
             });
             static const PlaceholderFrames kPlaceholder;
             const auto* services = findService<ReviewServices>(c.services());
             const ReviewFrameSource* frames = services && services->frames() ? services->frames() : &kPlaceholder;
             const std::string title = c.has("title") ? c.string("title") : std::string("Review notes");
             const ReviewPackageWritten written =
                 c.take(writeReviewPackage(c.state(), pathFromUtf8(c.string("path")), notes, frames, title, nowUtc()));
             CommandOutput out = readOutput(c, Json{{"notes", written.notes}, {"frames", written.frames}});
             out.warnings = written.warnings;
             return out;
         }}, {"path"}));
    addBuiltin(registry,
               command::touchesFiles({"review.importPackage", 1, "Import Review Package",
                "Reads a review package's notes back (one undo step): replies and status update the matching notes (by id, "
                "else by sequence, time and text), new notes are added at their timecodes, and notes that cannot be "
                "placed or that changed on both sides are listed as conflicts and left out.",
                Capability::EditProject,
                withSequence({{"path", boundedString("The package folder (UTF-8 path).", 4096, true)}}, {"path"}),
                Json{{"path", "C:/Review/Client cut v2.wabreview"}}, {},
                [](ExecutionContext& c) {
                    ReviewImported r = c.take(importReviewPackage(c.state(), pathFromUtf8(c.string("path"))));
                    CommandOutput out;
                    out.state = std::move(r.state);
                    out.data = Json{{"added", r.added}, {"updated", r.updated}, {"unchanged", r.unchanged}, {"conflicts", r.conflicts}};
                    return out;
                }}, {"path"}));

    // --- Merge -----------------------------------------------------------------------------------------------
    addBuiltin(
        registry,
        command::touchesFiles({"project.merge", 1, "Merge Projects",
         "Three-way merge of another copy of this project (\"theirs\", a project file) into this one, both edited from "
         "\"base\" (the file they started from): edits of different sequences, tracks or clips merge; the same thing "
         "changed differently on both sides is a conflict with a stable id. With unresolved conflicts the result is only "
         "staged (the project does not change; the conflicts and what would change are returned) unless \"apply\"; "
         "\"choices\" resolves them ([{conflict: id, take: \"mine\" | \"theirs\"}]; unresolved ones keep mine). A merge "
         "that cannot give a valid project with those choices is staged too, with \"invalid\" saying why. One undo step.",
         Capability::EditProject,
         withSequence({{"theirs", boundedString("Their project file (UTF-8 path).", 4096, true)},
                       {"base", boundedString("The common ancestor's project file.", 4096, true)},
                       {"choices", schema::array(schema::object({{"conflict", boundedString("A conflict's id.", 512, true)},
                                                                 {"take", schema::enumeration({"mine", "theirs"}, "Which side wins.")}},
                                                                {"conflict", "take"}),
                                                  "How conflicts resolve (the others keep mine).")},
                       {"apply", schema::boolean("Apply even with unresolved conflicts (they keep mine). Default false.")}},
                      {"theirs", "base"}),
         Json{{"theirs", "C:/Projects/Film (Alex).wabcut"}, {"base", "C:/Projects/Film (sent).wabcut"},
              {"choices", Json::array({Json{{"conflict", "clip:812"}, {"take", "theirs"}}})}},
         {},
         [](ExecutionContext& c) {
             const ProjectState theirs = c.take(io::loadProject(pathFromUtf8(c.string("theirs"))));
             const ProjectState base = c.take(io::loadProject(pathFromUtf8(c.string("base"))));
             std::map<std::string, MergeChoice> choices;
             if (c.has("choices")) {
                 const Json& list = c.params()["choices"];
                 if (list.size() > 100000) {
                     fail(ErrorCode::InvalidParams, "too many choices", Json{{"path", "/choices"}});
                 }
                 for (const Json& k : list) {
                     choices[k["conflict"].get<std::string>()] =
                         k["take"].get<std::string>() == "theirs" ? MergeChoice::Theirs : MergeChoice::Mine;
                 }
             }
             MergeResult r = c.take(mergeProjects(base, c.state(), theirs, choices));
             std::size_t unresolved = 0;
             Json conflicts = Json::array();
             for (const MergeConflict& k : r.conflicts) {
                 unresolved += choices.contains(k.id) ? 0 : 1;
                 conflicts.push_back(Json{{"id", k.id},
                                          {"what", k.what},
                                          {"sequence", k.sequence.isValid() ? Json(k.sequence.value) : Json(nullptr)},
                                          {"resolved", k.resolved == MergeChoice::Theirs ? "theirs" : "mine"},
                                          {"chosen", choices.contains(k.id)}});
             }
             // Not valid with these choices: the conflicts still come back (staged) so other choices can resolve it.
             const bool staged = r.invalid.has_value() || (unresolved > 0 && !c.boolean("apply", false));
             CommandOutput out;
             out.data = Json{{"staged", staged},
                             {"invalid", r.invalid ? Json(*r.invalid) : Json(nullptr)},
                             {"conflicts", std::move(conflicts)},
                             {"unresolved", unresolved},
                             {"fromTheirs", r.fromTheirs},
                             {"adjustments", r.adjustments},
                             {"changes", diffProjects(c.state(), r.merged).summary()}};
             out.state = staged ? c.state() : std::move(r.merged);
             return out;
         }}, {"theirs", "base"}));
}

} // namespace wabcut::command::detail
