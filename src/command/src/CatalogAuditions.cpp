// Built-in audition commands (0.6, card #74; model: Auditions.h): take stacks at a cut - create one from clips, add
// clips or a source as takes, make a take active, remove, reorder, rename and duplicate takes, flatten, describe.

#include "Catalog.h"

#include <wabcut/model/Auditions.h>
#include <wabcut/model/Timecode.h>

#include <string>
#include <utility>
#include <vector>

namespace wabcut::command::detail {

namespace {

Json takeParam(std::string description) {
    return schema::integer(std::move(description) + " Takes are numbered from 1 in take order.", 1,
                           static_cast<std::int64_t>(kMaxAuditionTakes));
}

/// A 1-based take number from the params, as an index.
std::size_t takeIndex(ExecutionContext& c, const char* key) {
    const std::string path = std::string("/") + key;
    const std::size_t n = c.indexValue(c.params()[key], path);
    if (n < 1 || n > kMaxAuditionTakes) {
        fail(ErrorCode::InvalidParams, "takes are numbered from 1 to " + std::to_string(kMaxAuditionTakes),
             Json{{"path", path}});
    }
    return n - 1;
}

/// The audition named by "clip" in the context's sequence (any clip: one that is not an audition has one take).
const Clip& clipOf(ExecutionContext& c) {
    const ClipId id = c.clip("clip");
    return c.sequence().findClip(id)->clip();
}

/// Every take of an audition clip: number, name, source, In at the audition's start, how far it is short of the
/// audition's range at each end, its effects; and which one is active.
Json auditionJson(ExecutionContext& c, const Clip& clip) {
    const Sequence& seq = c.sequence();
    const Audition* a = clip.audition();
    Json takes = Json::array();
    for (std::size_t i = 0; i < takeCount(clip); ++i) {
        const Clip take = auditionTakeClip(clip, i);
        const TakeFit fit = takeFit(c.state(), clip, i, seq.frameRate);
        Json t{{"take", i + 1},
               {"name", take.name()},
               {"active", !a || i == a->active},
               {"sourceIn", take.sourceIn.value},
               {"effects", take.effects().size()},
               {"short", Json{{"head", timeJson(c, fit.head)}, {"tail", timeJson(c, fit.tail)}}}};
        if (take.isNested()) {
            t["sequence"] = take.sequence.value;
        } else {
            t["media"] = take.media.value;
        }
        takes.push_back(std::move(t));
    }
    const TimeRange whole = auditionRange(clip);
    return Json{{"clip", clip.id.value},
                {"audition", a != nullptr},
                {"active", a ? a->active + 1 : 1},
                {"count", takeCount(clip)},
                {"start", timeJson(c, whole.start)},
                {"duration", timeJson(c, whole.duration)},
                {"takes", std::move(takes)}};
}

/// An edit of one audition, answering with the audition as it is afterwards.
CommandOutput editedAudition(ExecutionContext& c, edit::EditResult r, ClipId clip) {
    Json data{{"clip", clip.value}};
    if (const Sequence* seq = r.state.findSequence(c.sequenceId())) {
        if (const auto loc = seq->findClip(clip)) {
            const Clip& after = loc->clip();
            data["count"] = takeCount(after);
            data["active"] = after.audition() ? after.audition()->active + 1 : 1;
        }
    }
    return edited(std::move(r), std::move(data));
}

} // namespace

void registerAuditionCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"audition.create", 1, "New Audition",
         "Final Cut's Create Audition: the clips (with their linked partners) become the takes of one audition at the "
         "place of the earliest of them (take 1, active); the others follow in timeline order, lined up with its start "
         "at their own In points, and leave the timeline (no ripple). Every take must have as many picture and sound "
         "clips as the first. One clip alone becomes an audition with a copy of itself, the copy active (Duplicate as "
         "Audition). The audition moves, trims, ripples, razors and copies like a clip; playback and export show only "
         "its active take.",
         Capability::EditSequence, withSequence({{"clips", clipsParam("The clips that become the takes.")}}, {"clips"}),
         Json{{"clips", Json::array({42, 57, 63})}}, {},
         [](ExecutionContext& c) {
             const std::vector<ClipId> clips = c.clips("clips");
             edit::EditResult r = c.take(edit::createAudition(c.state(), c.sequenceId(), clips));
             // The audition is the clip that stays: the first named clip that is still there and is an audition.
             ClipId audition;
             if (const Sequence* seq = r.state.findSequence(c.sequenceId())) {
                 for (const ClipId id : clips) {
                     const auto loc = seq->findClip(id);
                     if (loc && loc->clip().isAudition()) {
                         audition = id;
                         break;
                     }
                 }
             }
             return editedAudition(c, std::move(r), audition);
         }});
    addBuiltin(
        registry,
        {"audition.addTakes", 1, "Add to Audition",
         "Final Cut's Add to Audition: the clips (with their linked partners) become takes after the audition's last "
         "take, lined up with its start at their own In points, and leave the timeline. The active take stays. A clip "
         "that is not an audition yet becomes one.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The audition.")}, {"clips", clipsParam("The clips to add as takes.")}},
                      {"clip", "clips"}),
         Json{{"clip", 42}, {"clips", Json::array({70})}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             edit::EditResult r = c.take(edit::addAuditionTakes(c.state(), c.sequenceId(), clip, c.clips("clips")));
             return editedAudition(c, std::move(r), clip);
         }});
    addBuiltin(
        registry,
        {"audition.addSource", 1, "Add Source to Audition",
         "A media item (its picture and sound streams, in order, for the audition's picture and sound) or a sequence "
         "becomes a take after the last one, its \"in\" at the audition's start (default: the source's start).",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The audition.")},
                       {"media", mediaParam("The media item to add (or give \"source\").")},
                       {"source", schema::integer("A sequence to add as a nested take instead of media.", 1)},
                       {"in", sourceTimeParam("Source time at the audition's start.")},
                       {"name", schema::string("The take's name (default: the source's).")}},
                      {"clip"}),
         Json{{"clip", 42}, {"media", 7}, {"in", "00:00:12:00"}, {"name", "Take 3 (wide)"}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             edit::TakeSource source;
             Rate rate = c.sequence().frameRate;
             if (c.has("source")) {
                 source.sequence = SequenceId{static_cast<std::uint64_t>(c.integer("source"))};
                 const Sequence* s = c.state().findSequence(source.sequence);
                 if (!s) {
                     fail(ErrorCode::NotFound, "no sequence with id " + std::to_string(source.sequence.value),
                          Json{{"path", "/source"}});
                 }
                 rate = s->frameRate;
             } else {
                 if (!c.has("media")) {
                     fail(ErrorCode::InvalidParams, "pass \"media\" or \"source\"", Json{{"path", "/media"}});
                 }
                 source.media = c.media("media");
                 if (const MediaItem* m = c.state().findMedia(source.media); m && !m->info.video.empty() &&
                                                                         m->info.video.front().frameRate.num > 0) {
                     rate = m->info.video.front().frameRate;
                 }
             }
             if (c.has("in")) {
                 source.sourceIn = sourceTimeValue(c.params()["in"], "/in", rate).value_or(Ticks{});
             }
             source.name = c.string("name");
             edit::EditResult r = c.take(edit::addAuditionSource(c.state(), c.sequenceId(), clip, source));
             return editedAudition(c, std::move(r), clip);
         }});
    addBuiltin(
        registry,
        {"audition.setActive", 1, "Use Take",
         "Makes a take of the audition (and of its linked picture or sound) the active take: the clip keeps its place "
         "and links and plays that take over the audition's range, as far as the take has media there (a shorter take "
         "leaves the rest empty, and a longer take used later fills it again). The playhead does not move.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The audition.")}, {"take", takeParam("The take to use.")}}, {"clip", "take"}),
         Json{{"clip", 42}, {"take", 2}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             edit::EditResult r =
                 c.take(edit::setActiveTake(c.state(), c.sequenceId(), clip, takeIndex(c, "take")));
             return editedAudition(c, std::move(r), clip);
         }});
    addBuiltin(
        registry,
        {"audition.removeTake", 1, "Remove Take",
         "Removes a take. The active take passes to the next one (the previous one for the last); an audition left "
         "with one take becomes a plain clip of it.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The audition.")}, {"take", takeParam("The take to remove.")}}, {"clip", "take"}),
         Json{{"clip", 42}, {"take", 3}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             edit::EditResult r =
                 c.take(edit::removeAuditionTake(c.state(), c.sequenceId(), clip, takeIndex(c, "take")));
             return editedAudition(c, std::move(r), clip);
         }});
    addBuiltin(
        registry,
        {"audition.moveTake", 1, "Move Take",
         "Moves a take to another place in take order; the active take stays active.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The audition.")},
                       {"take", takeParam("The take to move.")},
                       {"to", takeParam("Its new place.")}},
                      {"clip", "take", "to"}),
         Json{{"clip", 42}, {"take", 3}, {"to", 1}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             edit::EditResult r = c.take(
                 edit::moveAuditionTake(c.state(), c.sequenceId(), clip, takeIndex(c, "take"), takeIndex(c, "to")));
             return editedAudition(c, std::move(r), clip);
         }});
    Json nameSchema = schema::string("The take's new name.");
    nameSchema["minLength"] = 1;
    nameSchema["maxLength"] = static_cast<std::int64_t>(kMaxTakeNameBytes);
    addBuiltin(
        registry,
        {"audition.renameTake", 1, "Rename Take",
         "Renames a take (of the picture and the sound together; the clip's name for the active take).",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The audition.")}, {"take", takeParam("The take.")}, {"name", nameSchema}},
                      {"clip", "take", "name"}),
         Json{{"clip", 42}, {"take", 2}, {"name", "Angry read"}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             edit::EditResult r = c.take(
                 edit::renameAuditionTake(c.state(), c.sequenceId(), clip, takeIndex(c, "take"), c.string("name")));
             return editedAudition(c, std::move(r), clip);
         }});
    addBuiltin(
        registry,
        {"audition.duplicateTake", 1, "Duplicate Take",
         "Final Cut's Duplicate as Audition: a copy of a take (default: the active one), named \"<name> copy\", goes "
         "right after it and becomes the active take - to try other effects on it.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The audition (or a clip: it becomes one).")},
                       {"take", takeParam("The take to copy (default: the active one).")}},
                      {"clip"}),
         Json{{"clip", 42}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             const Clip& current = clipOf(c);
             if (!current.isAudition()) {
                 const ClipId one[] = {clip};
                 edit::EditResult r = c.take(edit::createAudition(c.state(), c.sequenceId(), one));
                 return editedAudition(c, std::move(r), clip);
             }
             const std::size_t take = c.has("take") ? takeIndex(c, "take") : current.audition()->active;
             edit::EditResult r = c.take(edit::duplicateAuditionTake(c.state(), c.sequenceId(), clip, take));
             return editedAudition(c, std::move(r), clip);
         }});
    addBuiltin(
        registry,
        {"audition.flatten", 1, "Flatten Audition",
         "Final Cut's Finalize Audition: the auditions among the clips (with their linked picture or sound) keep only "
         "their active take and become plain clips.",
         Capability::EditSequence, withSequence({{"clips", clipsParam("The auditions.")}}, {"clips"}),
         Json{{"clips", Json::array({42})}}, {},
         [](ExecutionContext& c) {
             edit::EditResult r = c.take(edit::flattenAuditions(c.state(), c.sequenceId(), c.clips("clips")));
             return edited(std::move(r));
         }});
    addBuiltin(registry,
               {"audition.describe", 1, "Describe Audition",
                "The takes of an audition in take order: name, source (media or sequence), source In at the audition's "
                "start, effect count, how far each is short of the audition's range at its head and tail, and which "
                "one is active. A clip that is not an audition answers with its one take.",
                Capability::Read, withSequence({{"clip", clipParam("The audition.")}}, {"clip"}), Json{{"clip", 42}},
                {},
                [](ExecutionContext& c) {
                    CommandOutput out;
                    out.state = c.state();
                    out.data = auditionJson(c, clipOf(c));
                    return out;
                }});
}

} // namespace wabcut::command::detail
