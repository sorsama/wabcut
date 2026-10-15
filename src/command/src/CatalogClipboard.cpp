// Built-in clipboard commands (C.11): clipboard.copy / cut / paste / get and Premiere's Paste Attributes / Remove
// Attributes (clip.pasteAttributes, clip.removeAttributes). Model: Clipboard.h. The clipboard is the host's
// ClipboardServices (ExecutionContext.h): the app shares one between its timeline and its assistant; a CLI or MCP
// session has its engine's own.

#include "Catalog.h"

#include <wabcut/model/Clipboard.h>
#include <wabcut/model/Effects.h>

#include <algorithm>
#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

ClipboardServices& clipboardOf(ExecutionContext& c) {
    auto* services = findService<ClipboardServices>(c.services());
    if (!services) {
        fail(ErrorCode::InvalidRequest, "this host has no clipboard");
    }
    return *services;
}

/// Puts `doc` on the clipboard once the request has committed (E5-P2-7): a dry run (an assistant's preview) or a
/// batch that fails later never replaces what the user copied.
std::function<void()> fillClipboard(ExecutionContext& c, ClipboardDocument doc) {
    ClipboardServices* services = &clipboardOf(c); // no clipboard: the command fails now, not after the commit
    auto held = std::make_shared<const ClipboardDocument>(std::move(doc));
    if (ExecutionContext::Stage* stage = c.stage()) {
        (*stage)["clipboard"] = held; // a paste later in the same batch reads it
    }
    return [services, held] { services->set(*held); };
}

/// The clipboard as this request sees it: what an earlier step staged, else the host's.
std::optional<ClipboardDocument> heldClipboard(ExecutionContext& c) {
    if (const ExecutionContext::Stage* stage = c.stage()) {
        if (const auto it = stage->find("clipboard"); it != stage->end()) {
            return *std::static_pointer_cast<const ClipboardDocument>(it->second);
        }
    }
    return clipboardOf(c).get();
}

/// The clipboard as this caller may have it (heldClipboard): clips copied from a no-cloud project are refused to a
/// cloud-bound caller (the app's assistant) outside a no-cloud project (review wave 5, S5-P2-12).
std::optional<ClipboardDocument> heldFor(ExecutionContext& c) {
    auto held = heldClipboard(c);
    if (held && held->noCloud && clipboardOf(c).cloudBound() && !c.state().aiNoCloud) {
        fail(ErrorCode::PermissionDenied,
             "the clipboard holds clips from a project locked to no-cloud AI: the assistant cannot read or paste them in "
             "this project (paste them yourself; the project then becomes no-cloud too)");
    }
    return held;
}

Json clipboardSummary(const ClipboardDocument& doc, bool withDocument) {
    Json media = Json::array();
    for (const MediaPtr& m : doc.content.media) {
        media.push_back(m->name);
    }
    const Sequence& seq = doc.sequence();
    std::size_t video = 0;
    std::size_t audio = 0;
    for (const TrackPtr& t : seq.videoTracks) {
        video += t->clips.empty() ? 0 : 1;
    }
    for (const TrackPtr& t : seq.audioTracks) {
        audio += t->clips.empty() ? 0 : 1;
    }
    Json out{{"empty", false},
             {"clips", doc.clipCount()},
             {"duration", doc.duration.value},
             {"videoTracks", video},
             {"audioTracks", audio},
             {"markers", seq.markers.size()},
             {"captions", doc.hasCaptions},
             {"media", std::move(media)}};
    if (withDocument) {
        auto text = clipboardToJson(doc);
        if (!text) {
            fail(ErrorCode::EditRejected, text.error().message);
        }
        out["document"] = *text;
    }
    return out;
}

ClipboardDocument copyFrom(ExecutionContext& c) {
    CopyOptions options;
    options.links = c.boolean("linked", true) ? edit::LinkMode::FollowLinks : edit::LinkMode::IgnoreLinks;
    options.markers = c.boolean("markers", true);
    options.captions = c.boolean("captions", false);
    return c.take(copyClips(c.state(), c.sequenceId(), c.clips("clips"), options));
}

Json copyParams() {
    return withSequence({{"clips", clipsParam("The clips to copy.")},
                         {"linked", linkedParam(true)},
                         {"markers", schema::boolean("Copy the sequence markers inside the copied range (default true).")},
                         {"captions", schema::boolean("Copy the caption cues inside the copied range (default false).")},
                         {"export", schema::boolean("Also return the clipboard as JSON text (\"document\"), to paste it "
                                                    "into another project or Wabcut (default false).")}},
                        {"clips"});
}

Json attributesParam(bool remove) {
    Json effects = remove ? schema::array(schema::string("An effect id, e.g. \"wabcut.blur.gaussian\"."),
                                          "Effects to remove, by id (every instance of each).")
                          : schema::array(schema::integer("An index in the source clip's effect stack (0 = first).", 0),
                                          "Effects to paste, by their place in the source clip's stack.");
    return schema::object({{"motion", schema::boolean("Motion (Transform): position, scale, rotation, anchor; video.")},
                           {"opacity", schema::boolean("Opacity; video.")},
                           {"timeRemap", schema::boolean("Time Remapping.")},
                           {"volume", schema::boolean("Volume; audio.")},
                           {"panner", schema::boolean("Panner (Premiere's Channel Volume and Panner); audio.")},
                           {"effects", std::move(effects)},
                           {"allEffects", schema::boolean(remove ? "Every effect in the stack." : "Every effect of the source clip.")}},
                          {}, remove ? "Which attributes to remove." : "Which attributes to paste.");
}

AttributeSet attributesFrom(ExecutionContext& c, bool remove) {
    if (!c.has("attributes")) {
        fail(ErrorCode::InvalidParams, "choose the attributes", Json{{"path", "/attributes"}});
    }
    const Json& a = c.params()["attributes"];
    AttributeSet set;
    set.motion = a.value("motion", false);
    set.opacity = a.value("opacity", false);
    set.timeRemap = a.value("timeRemap", false);
    set.volume = a.value("volume", false);
    set.panner = a.value("panner", false);
    set.allEffects = a.value("allEffects", false);
    if (a.contains("effects")) {
        for (const Json& e : a["effects"]) {
            if (remove) {
                set.effectIds.push_back(e.get<std::string>());
            } else {
                set.effects.push_back(c.indexValue(e, "/attributes/effects"));
            }
        }
    }
    if (set.empty()) {
        fail(ErrorCode::InvalidParams, "choose at least one attribute", Json{{"path", "/attributes"}});
    }
    return set;
}

Json changesJson(const std::vector<AttributeChange>& changes) {
    Json out = Json::array();
    for (const AttributeChange& change : changes) {
        out.push_back(Json{{"clip", change.clip.value}, {"applied", change.applied}, {"skipped", change.skipped}});
    }
    return out;
}

} // namespace

void registerClipboardCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"clipboard.copy", 1, "Copy",
                "Copies clips (with their link partners unless \"linked\": false) to the clipboard exactly: speed, "
                "reverse, freeze frames, time remapping, effects and keyframes, names, labels, links, the transitions "
                "between them and the markers over them (captions on request). The project does not change.",
                Capability::Read, copyParams(), Json{{"clips", Json::array({42, 43})}}, {},
                [](ExecutionContext& c) {
                    ClipboardDocument doc = copyFrom(c);
                    Json data = clipboardSummary(doc, c.boolean("export", false));
                    CommandOutput out;
                    out.afterCommit.push_back(fillClipboard(c, std::move(doc)));
                    out.state = c.state();
                    out.data = std::move(data);
                    return out;
                }});
    addBuiltin(registry,
               {"clipboard.cut", 1, "Cut",
                "Copies clips to the clipboard (like clipboard.copy) and deletes them, leaving gaps (Premiere's Cut). One "
                "undo step.",
                Capability::EditSequence, copyParams(), Json{{"clips", Json::array({42})}}, {},
                [](ExecutionContext& c) {
                    ClipboardDocument doc = copyFrom(c);
                    std::vector<ClipId> ids;
                    doc.sequence().forEachTrack([&](const Track& t) {
                        for (const Clip& clip : t.clips) {
                            ids.push_back(clip.id);
                        }
                    });
                    Json data = clipboardSummary(doc, c.boolean("export", false));
                    edit::EditResult removed =
                        c.take(edit::deleteClips(c.state(), c.sequenceId(), ids, edit::LinkMode::IgnoreLinks));
                    std::function<void()> fill = fillClipboard(c, std::move(doc));
                    CommandOutput out = edited(std::move(removed), std::move(data));
                    out.afterCommit.push_back(std::move(fill));
                    return out;
                }});
    addBuiltin(
        registry,
        {"clipboard.paste", 1, "Paste",
         "Pastes the clipboard (or a \"document\" from clipboard.copy's export) at \"at\": overwrite (Ctrl+V) replaces "
         "what lies under the pasted clips, insert (Ctrl+Shift+V) first opens the clipboard's length on the destination "
         "and sync-locked tracks. Clips go to the lowest targeted track of their kind (or \"videoTrack\" / "
         "\"audioTrack\") plus their copied offsets; missing tracks are added. Media is the project's own when it has "
         "the file (by id, fingerprint or path), else it is added; \"notes\" say which. One undo step.",
         Capability::EditSequence,
         withSequence({{"at", timeParam("Where the first copied clip lands (the playhead).")},
                       {"mode", schema::enumeration({"overwrite", "insert"}, "Overwrite (default) or insert.")},
                       {"videoTrack", trackParam("Destination of the lowest copied video track.")},
                       {"audioTrack", trackParam("Destination of the lowest copied audio track.")},
                       {"markers", schema::boolean("Paste the copied markers (default true).")},
                       {"captions", schema::boolean("Paste the copied caption cues (default true).")},
                       {"document", Json{{"type", "string"},
                                         {"maxLength", static_cast<std::int64_t>(kMaxClipboardBytes)},
                                         {"description", "A clipboard as JSON text (clipboard.copy with \"export\"); "
                                                         "default: this session's clipboard."}}}},
                      {"at"}),
         Json{{"at", "00:00:10:00"}, {"mode", "insert"}}, {},
         [](ExecutionContext& c) {
             ClipboardDocument doc;
             if (c.has("document")) {
                 doc = c.take(clipboardFromJson(c.string("document")));
             } else {
                 auto held = heldFor(c);
                 if (!held) {
                     fail(ErrorCode::EditRejected, "the clipboard is empty: copy clips first");
                 }
                 doc = std::move(*held);
             }
             PasteOptions options;
             options.mode = c.string("mode", "overwrite") == "insert" ? PasteMode::Insert : PasteMode::Overwrite;
             if (c.has("videoTrack")) {
                 options.videoTrack = c.track("videoTrack");
             }
             if (c.has("audioTrack")) {
                 options.audioTrack = c.track("audioTrack");
             }
             options.markers = c.boolean("markers", true);
             options.captions = c.boolean("captions", true);
             const Ticks at = c.time("at");
             PasteResult pasted = c.take(pasteClips(c.state(), c.sequenceId(), doc, at, options));
             Json clips = Json::array();
             for (const ClipId id : pasted.report.pasted) {
                 clips.push_back(id.value);
             }
             Json media = Json::array();
             for (const MediaId id : pasted.report.mediaAdded) {
                 media.push_back(id.value);
             }
             Json tracks = Json::array();
             const Sequence& seq = *pasted.edit.state.findSequence(c.sequenceId());
             for (const TrackId id : pasted.report.tracksAdded) {
                 const auto pos = seq.trackPosition(id);
                 tracks.push_back(pos ? (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1)
                                      : std::to_string(id.value));
             }
             Json data{{"pasted", std::move(clips)},
                       {"mediaAdded", std::move(media)},
                       {"tracksAdded", std::move(tracks)},
                       {"notes", pasted.report.notes},
                       {"playhead", timeJson(c, at + doc.duration)}};
             return edited(std::move(pasted.edit), std::move(data));
         }});
    addBuiltin(registry,
               {"clipboard.get", 1, "Clipboard",
                "What the clipboard holds: clips, length, tracks, markers, captions, media; with \"export\" also the "
                "clipboard as JSON text.",
                Capability::Read,
                withSequence({{"export", schema::boolean("Also return the clipboard as JSON text (default false).")}}),
                Json::object(), {},
                [](ExecutionContext& c) {
                    CommandOutput out;
                    out.state = c.state();
                    const auto held = heldFor(c);
                    out.data = held ? clipboardSummary(*held, c.boolean("export", false)) : Json{{"empty", true}};
                    return out;
                }});
    addBuiltin(
        registry,
        {"clip.pasteAttributes", 1, "Paste Attributes",
         "Premiere's Paste Attributes (Ctrl+Alt+V): the chosen attributes of the clipboard's clip (the first one of each "
         "target's kind: video attributes from the copied video clip, audio ones from the audio clip) or of \"source\" "
         "onto \"clips\". Parameter values and keyframes replace the target's; effects are added after its own; time "
         "remapping replaces its speed. Keyframes keep their time from the clip's start, or stretch to the target's "
         "length with \"scaleTimes\". One undo step; \"changes\" lists what each clip took and skipped (dryRun previews).",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The clips to paste onto.")},
                       {"source", clipParam("Take the attributes from this clip of the sequence instead of the clipboard.")},
                       {"attributes", attributesParam(false)},
                       {"scaleTimes", schema::boolean("Scale Attribute Times: keyframes stretched to the target's length.")}},
                      {"clips", "attributes"}),
         Json{{"clips", Json::array({44, 45})}, {"attributes", Json{{"motion", true}, {"effects", Json::array({0})}}}}, {},
         [](ExecutionContext& c) {
             const AttributeSet attributes = attributesFrom(c, false);
             const bool scale = c.boolean("scaleTimes", false);
             const std::vector<ClipId> targets = c.clips("clips");
             const SequenceId seqId = c.sequenceId();
             if (c.has("source")) {
                 const ClipId sourceId = c.clip("source");
                 const Clip source = c.sequence().findClip(sourceId)->clip();
                 AttributesResult r = c.take(pasteAttributes(c.state(), seqId, source, targets, attributes, scale));
                 Json data{{"changes", changesJson(r.changes)}};
                 return edited(std::move(r.edit), std::move(data));
             }
             const auto held = heldClipboard(c);
             if (!held) {
                 fail(ErrorCode::EditRejected, "the clipboard is empty: copy a clip first, or give \"source\"");
             }
             // The clipboard's first clip of each kind serves the targets of that kind.
             const auto firstOf = [&](StreamKind kind) -> const Clip* {
                 for (const TrackPtr& t : held->sequence().tracks(kind == StreamKind::Video ? TrackKind::Video : TrackKind::Audio)) {
                     if (!t->clips.empty()) {
                         return &*t->clips.begin();
                     }
                 }
                 return nullptr;
             };
             std::vector<ClipId> videos;
             std::vector<ClipId> audios;
             for (const ClipId id : targets) {
                 const auto loc = c.sequence().findClip(id);
                 (loc->clip().stream.kind == StreamKind::Video ? videos : audios).push_back(id);
             }
             ProjectState state = c.state();
             std::vector<AttributeChange> changes;
             std::vector<EffectInstanceId> createdEffects;
             std::string lastError;
             bool any = false;
             for (const auto& [kind, group] : {std::pair{StreamKind::Video, &videos}, std::pair{StreamKind::Audio, &audios}}) {
                 if (group->empty()) {
                     continue;
                 }
                 const Clip* source = firstOf(kind);
                 if (!source) {
                     source = firstOf(kind == StreamKind::Video ? StreamKind::Audio : StreamKind::Video);
                 }
                 // Effect indexes are per source clip: the copied video and audio clips have stacks of their own.
                 AttributeSet groupAttributes = attributes;
                 std::vector<std::string> notStacked;
                 std::erase_if(groupAttributes.effects, [&](std::size_t i) {
                     if (i < source->effects().size()) {
                         return false;
                     }
                     notStacked.push_back("effect #" + std::to_string(i + 1) + ": the copied " +
                                          (kind == StreamKind::Video ? "video" : "audio") + " clip has none");
                     return true;
                 });
                 if (groupAttributes.empty()) {
                     for (const ClipId id : *group) {
                         changes.push_back({id, {}, notStacked});
                     }
                     continue;
                 }
                 auto r = pasteAttributes(state, seqId, *source, *group, groupAttributes, scale);
                 if (!r) {
                     for (const ClipId id : *group) {
                         changes.push_back({id, {}, {r.error().message}});
                     }
                     lastError = r.error().message;
                     continue;
                 }
                 any = true;
                 state = std::move(r->edit.state);
                 createdEffects.insert(createdEffects.end(), r->edit.createdEffects.begin(), r->edit.createdEffects.end());
                 for (AttributeChange& change : r->changes) {
                     change.skipped.insert(change.skipped.end(), notStacked.begin(), notStacked.end());
                     changes.push_back(std::move(change));
                 }
             }
             if (!any) {
                 fail(ErrorCode::EditRejected, lastError.empty() ? "nothing to paste" : lastError);
             }
             edit::EditResult result;
             result.state = std::move(state);
             result.createdEffects = std::move(createdEffects);
             return edited(std::move(result), Json{{"changes", changesJson(changes)}});
         }});
    addBuiltin(registry,
               {"clip.removeAttributes", 1, "Remove Attributes",
                "Premiere's Remove Attributes: the chosen parameters go back to their defaults (keyframes too), time "
                "remapping is turned off (the clip plays at 100 % from the frame it started on), effects are removed by "
                "id (or all). One undo step; \"changes\" lists what each clip lost.",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("The clips.")}, {"attributes", attributesParam(true)}},
                             {"clips", "attributes"}),
                Json{{"clips", Json::array({44})}, {"attributes", Json{{"opacity", true}, {"allEffects", true}}}}, {},
                [](ExecutionContext& c) {
                    const AttributeSet attributes = attributesFrom(c, true);
                    AttributesResult r = c.take(removeAttributes(c.state(), c.sequenceId(), c.clips("clips"), attributes));
                    Json data{{"changes", changesJson(r.changes)}};
                    return edited(std::move(r.edit), std::move(data));
                }});
}

} // namespace wabcut::command::detail
