// Consolidate Duplicates (0.7, card 025): merging media items that are the same file into one. See ProjectOps.h.

#include <wabcut/model/ProjectOps.h>

#include <wabcut/model/Auditions.h>
#include <wabcut/model/RawSourceSettings.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

namespace wabcut::edit {

using detail::notFound;

namespace {

using Repoint = std::function<bool(MediaId&)>; // true when it changed the id

MediaPtr* findMediaSlot(ProjectState& state, MediaId id) {
    const auto it = std::lower_bound(state.media.begin(), state.media.end(), id,
                                     [](const MediaPtr& m, MediaId want) { return m->id < want; });
    return it != state.media.end() && (*it)->id == id ? &*it : nullptr;
}

// ---- Where a media id hides ----------------------------------------------------------------------------------------
// Clip::media and the media of audition takes are the common ones; the others are masks that show an AI matte made
// from a media stream (MaskMatte::media: on clip, track and sequence effects, in Master Clip Effects, redaction regions
// and graphic layers), the images of graphics and graphic templates, the brand kit's logos, redactions, subclips'
// masters and provenance.

bool repointMasks(std::vector<Mask>& masks, const Repoint& to) {
    bool changed = false;
    for (Mask& m : masks) {
        if (m.matte && m.matte->media.isValid()) {
            changed = to(m.matte->media) || changed;
        }
    }
    return changed;
}

bool repointEffects(EffectStack& stack, const Repoint& to) {
    bool changed = false;
    for (EffectInstance& e : stack) {
        changed = repointMasks(e.masks, to) || changed;
    }
    return changed;
}

bool repointLayers(std::vector<GraphicLayer>& layers, const Repoint& to) {
    bool changed = false;
    for (GraphicLayer& l : layers) {
        changed = repointMasks(l.masks, to) || changed;
        if (l.kind == LayerKind::Image && l.image.media.isValid()) {
            changed = to(l.image.media) || changed;
        }
        changed = repointLayers(l.children, to) || changed;
    }
    return changed;
}

bool repointGraphic(Graphic& g, const Repoint& to) {
    bool changed = repointLayers(g.layers, to);
    if (g.instance) {
        for (TemplateField& field : g.instance->fields) {
            if (field.kind == FieldKind::Image && field.value.media.isValid()) {
                changed = to(field.value.media) || changed;
            }
        }
    }
    return changed;
}

/// A clip content block with every media id passed through `to`, or null when none of them changed.
std::shared_ptr<const ClipContent> repointContent(const ClipContent& content, const Repoint& to) {
    ClipContent next = content;
    bool changed = repointEffects(next.effects, to);
    if (next.graphic) {
        Graphic g = *next.graphic;
        if (repointGraphic(g, to)) {
            next.graphic = std::make_shared<const Graphic>(std::move(g));
            changed = true;
        }
    }
    if (next.audition) {
        Audition a = *next.audition;
        bool takesChanged = false;
        for (AuditionTake& take : a.others) {
            if (take.media.isValid()) {
                takesChanged = to(take.media) || takesChanged;
            }
            if (take.content) {
                if (auto replaced = repointContent(*take.content, to)) {
                    take.content = std::move(replaced);
                    takesChanged = true;
                }
            }
        }
        if (takesChanged) {
            next.audition = std::make_shared<const Audition>(std::move(a));
            changed = true;
        }
    }
    return changed ? std::make_shared<const ClipContent>(std::move(next)) : nullptr;
}

// ---- Which items are the same ----------------------------------------------------------------------------------------

/// Why `other` cannot be folded into `keeper` (what a clip of it would play differently), or empty.
std::string incompatibility(const MediaItem& keeper, const MediaItem& other) {
    if (keeper.interpretation.value_or(MediaInterpretation{}) != other.interpretation.value_or(MediaInterpretation{})) {
        return "it is interpreted differently (Interpret Footage)";
    }
    if (keeper.timecode != other.timecode) {
        return "it has another timecode or reel name";
    }
    if (keeper.channels != other.channels) {
        return "its audio channels are mapped differently";
    }
    if (keeper.scaling != other.scaling) {
        return "it is scaled to the frame differently";
    }
    if (rawSourceSettingsOf(keeper.locator.path) != rawSourceSettingsOf(other.locator.path)) {
        return "its camera RAW source settings differ";
    }
    if (keeper.masterEffects != other.masterEffects) {
        return "it has other Master Clip Effects";
    }
    if (keeper.info.video.size() != other.info.video.size() || keeper.info.audio.size() != other.info.audio.size()) {
        return "it has other streams";
    }
    return {};
}

std::map<std::uint64_t, int> clipCounts(const ProjectState& state) {
    std::map<std::uint64_t, int> counts;
    for (const SequencePtr& seq : state.sequences) {
        seq->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                forEachTakeSource(c, [&](MediaId m, SequenceId) {
                    if (m.isValid()) {
                        ++counts[m.value];
                    }
                });
            }
        });
    }
    return counts;
}

void unionMarkers(MediaItem& keeper, const MediaItem& donor) {
    for (const Marker& m : donor.markers) {
        if (keeper.markers.size() >= kMaxMarkers) {
            return;
        }
        const bool have = std::any_of(keeper.markers.begin(), keeper.markers.end(),
                                      [&](const Marker& k) { return k.start == m.start && k.name == m.name; });
        if (!have) {
            keeper.markers.push_back(m);
        }
    }
    std::stable_sort(keeper.markers.begin(), keeper.markers.end(),
                     [](const Marker& a, const Marker& b) { return a.start < b.start; });
}

/// Fills what the keeper has not written from a duplicate it replaces; what both wrote and differs is a note.
void absorb(MediaItem& keeper, const MediaItem& donor, std::vector<std::string>& notes) {
    if (keeper.comment.empty()) {
        keeper.comment = donor.comment;
    }
    if (keeper.description.empty()) {
        keeper.description = donor.description;
    }
    if (keeper.colorIndex == 0) {
        keeper.colorIndex = donor.colorIndex;
    }
    ClipMetadata& km = keeper.metadata;
    const ClipMetadata& dm = donor.metadata;
    for (const auto& [mine, theirs] :
         {std::pair{&km.scene, &dm.scene}, std::pair{&km.shot, &dm.shot}, std::pair{&km.take, &dm.take},
          std::pair{&km.logNote, &dm.logNote}, std::pair{&km.client, &dm.client}, std::pair{&km.status, &dm.status},
          std::pair{&km.cameraAngle, &dm.cameraAngle}, std::pair{&km.cameraLabel, &dm.cameraLabel},
          std::pair{&km.cameraModel, &dm.cameraModel}, std::pair{&km.shotDate, &dm.shotDate},
          std::pair{&km.shotLocation, &dm.shotLocation}}) {
        if (mine->empty()) {
            *mine = *theirs;
        }
    }
    km.good = km.good || dm.good;
    unionMarkers(keeper, donor);
    if (!keeper.locator.proxy && donor.locator.proxy) {
        keeper.locator.proxy = donor.locator.proxy;
    }
    if (!keeper.generated) {
        keeper.generated = donor.generated;
    }
    if (!keeper.derived) {
        keeper.derived = donor.derived;
    }
    if (!keeper.recording) {
        keeper.recording = donor.recording;
    }
    if (donor.rights && !donor.rights->isEmpty()) {
        if (!keeper.rights || keeper.rights->isEmpty()) {
            keeper.rights = donor.rights;
        } else {
            MediaRights& k = *keeper.rights;
            const MediaRights& d = *donor.rights;
            bool conflict = false;
            for (const auto& [mine, theirs] :
                 {std::pair{&k.source, &d.source}, std::pair{&k.licence, &d.licence}, std::pair{&k.reference, &d.reference},
                  std::pair{&k.territory, &d.territory}, std::pair{&k.expiry, &d.expiry},
                  std::pair{&k.currency, &d.currency}, std::pair{&k.creditLine, &d.creditLine},
                  std::pair{&k.notes, &d.notes}}) {
                if (mine->empty()) {
                    *mine = *theirs;
                } else if (!theirs->empty() && *mine != *theirs) {
                    conflict = true;
                }
            }
            if (!k.costMinor) {
                k.costMinor = d.costMinor;
            } else if (d.costMinor && *d.costMinor != *k.costMinor) {
                conflict = true;
            }
            if (k.status == ClearanceStatus::Unset) {
                k.status = d.status;
            } else if (d.status != ClearanceStatus::Unset && d.status != k.status) {
                conflict = true;
            }
            if (conflict) {
                notes.push_back("The rights of \"" + donor.name + "\" differ from the rights of \"" + keeper.name +
                                "\": the ones of \"" + keeper.name + "\" were kept.");
            }
        }
    }
}

struct GroupWork {
    const MediaItem* keeper = nullptr;
    std::vector<const MediaItem*> merged;
};

} // namespace

std::vector<std::vector<MediaId>> duplicateMediaGroups(const ProjectState& state) {
    // The same file content (size and sample hash) imported more than once. Subclips share their master's file on
    // purpose, generated items have no file, and a file that was never fingerprinted proves nothing.
    std::map<std::pair<std::int64_t, std::string>, std::vector<MediaId>> byContent;
    for (const MediaPtr& m : state.media) {
        if (!m || m->isSubclip() || m->isGenerator() || !m->locator.fingerprint.isKnown()) {
            continue;
        }
        byContent[{m->locator.fingerprint.size, m->locator.fingerprint.sampleHash}].push_back(m->id);
    }
    std::vector<std::vector<MediaId>> groups;
    for (auto& [content, ids] : byContent) {
        if (ids.size() >= 2) {
            groups.push_back(std::move(ids));
        }
    }
    return groups;
}

int ConsolidateResult::removedItems() const {
    int n = 0;
    for (const ConsolidateGroup& g : groups) {
        n += static_cast<int>(g.removed.size());
    }
    return n;
}

int ConsolidateResult::repointedClips() const {
    int n = 0;
    for (const ConsolidateGroup& g : groups) {
        n += g.repointedClips;
    }
    return n;
}

std::expected<ConsolidateResult, Error> consolidateDuplicates(const ProjectState& state, std::span<const MediaId> only) {
    for (const MediaId id : only) {
        if (!state.findMedia(id)) {
            return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
        }
    }
    const std::set<std::uint64_t> wanted = [&] {
        std::set<std::uint64_t> s;
        for (const MediaId id : only) {
            s.insert(id.value);
        }
        return s;
    }();
    const std::map<std::uint64_t, int> counts = clipCounts(state);
    const auto clipsOf = [&](const MediaItem& m) {
        const auto it = counts.find(m.id.value);
        return it == counts.end() ? 0 : it->second;
    };

    ConsolidateResult result;
    result.state = state;
    std::vector<GroupWork> merges;
    std::unordered_map<std::uint64_t, MediaId> keeperOf; // removed id -> keeper

    for (const std::vector<MediaId>& ids : duplicateMediaGroups(state)) {
        if (!wanted.empty() &&
            std::none_of(ids.begin(), ids.end(), [&](MediaId id) { return wanted.contains(id.value); })) {
            continue;
        }
        std::vector<const MediaItem*> remaining;
        for (const MediaId id : ids) {
            remaining.push_back(state.findMedia(id));
        }
        std::map<std::uint64_t, std::string> rejected; // id -> why (the first keeper that could not take it)
        std::set<std::uint64_t> gone;    // folded into a keeper
        std::set<std::uint64_t> keepers; // keeping a set of its own
        std::size_t firstReport = result.groups.size();
        bool reported = false;
        const MediaItem* firstKeeper = nullptr;
        while (remaining.size() >= 2) {
            // Online beats offline, then the most clips, then the lowest id.
            const MediaItem* keeper = *std::min_element(remaining.begin(), remaining.end(), [&](const MediaItem* a, const MediaItem* b) {
                const bool aOffline = a->locator.offline;
                const bool bOffline = b->locator.offline;
                if (aOffline != bOffline) {
                    return !aOffline;
                }
                if (clipsOf(*a) != clipsOf(*b)) {
                    return clipsOf(*a) > clipsOf(*b);
                }
                return a->id.value < b->id.value;
            });
            if (!firstKeeper) {
                firstKeeper = keeper;
            }
            GroupWork work;
            work.keeper = keeper;
            std::vector<const MediaItem*> next;
            for (const MediaItem* other : remaining) {
                if (other == keeper) {
                    continue;
                }
                const std::string why = incompatibility(*keeper, *other);
                if (why.empty()) {
                    work.merged.push_back(other);
                } else {
                    rejected.emplace(other->id.value, "not merged into \"" + keeper->name + "\": " + why);
                    next.push_back(other);
                }
            }
            if (!work.merged.empty()) {
                ConsolidateGroup report;
                report.keeper = keeper->id;
                report.keeperName = keeper->name;
                keepers.insert(keeper->id.value);
                for (const MediaItem* m : work.merged) {
                    report.removed.push_back(m->id);
                    report.removedNames.push_back(m->name);
                    keeperOf[m->id.value] = keeper->id;
                    gone.insert(m->id.value);
                }
                result.groups.push_back(std::move(report));
                merges.push_back(std::move(work));
                reported = true;
            }
            remaining = std::move(next);
        }
        // Whatever was turned away and found no keeper of its own stays: say so.
        std::vector<ConsolidateSkip> skipped;
        for (const auto& [id, why] : rejected) {
            if (!gone.contains(id) && !keepers.contains(id)) {
                const MediaItem* m = state.findMedia(MediaId{id});
                skipped.push_back({MediaId{id}, m ? m->name : std::string(), why});
            }
        }
        if (!skipped.empty()) {
            if (!reported) {
                ConsolidateGroup report;
                report.keeper = firstKeeper ? firstKeeper->id : MediaId{};
                report.keeperName = firstKeeper ? firstKeeper->name : std::string();
                result.groups.push_back(std::move(report));
                firstReport = result.groups.size() - 1;
            }
            result.groups[firstReport].skipped = std::move(skipped);
        }
    }
    if (keeperOf.empty()) {
        return result; // nothing to merge: the state is the project as it is
    }

    ProjectState& out = result.state;
    std::map<std::uint64_t, int> repointedByKeeper;
    const auto to = [&](MediaId& id) {
        const auto it = keeperOf.find(id.value);
        if (it == keeperOf.end()) {
            return false;
        }
        id = it->second;
        return true;
    };

    // 1. Every clip that plays a duplicate (and the takes of auditions, graphics, mattes), track by track.
    std::unordered_map<const ClipContent*, std::shared_ptr<const ClipContent>> contentCache;
    const auto newContent = [&](const std::shared_ptr<const ClipContent>& shared) -> std::shared_ptr<const ClipContent> {
        if (!shared) {
            return nullptr;
        }
        const auto it = contentCache.find(shared.get());
        if (it != contentCache.end()) {
            return it->second;
        }
        auto replaced = repointContent(*shared, to);
        contentCache.emplace(shared.get(), replaced);
        return replaced;
    };
    for (SequencePtr& sp : out.sequences) {
        std::shared_ptr<Sequence> seq;
        const auto edited = [&]() -> Sequence& {
            if (!seq) {
                seq = std::make_shared<Sequence>(*sp);
            }
            return *seq;
        };
        if (EffectStack e = sp->effects; repointEffects(e, to)) {
            edited().effects = std::move(e);
        }
        if (EffectStack e = sp->audioEffects; repointEffects(e, to)) {
            edited().audioEffects = std::move(e);
        }
        const auto rewriteTracks = [&](std::vector<TrackPtr> Sequence::*list) {
            for (std::size_t i = 0; i < (sp.get()->*list).size(); ++i) {
                const TrackPtr& original = (sp.get()->*list)[i];
                if (!original) {
                    continue;
                }
                std::shared_ptr<Track> track;
                std::vector<Clip> clips;
                bool clipsChanged = false;
                if (EffectStack e = original->effects; repointEffects(e, to)) {
                    track = std::make_shared<Track>(*original);
                    track->effects = std::move(e);
                }
                std::size_t index = 0;
                for (const Clip& c : original->clips) {
                    bool changed = false;
                    Clip copy = c;
                    std::set<std::uint64_t> keepers;
                    forEachTakeSource(c, [&](MediaId m, SequenceId) {
                        const auto it = keeperOf.find(m.value);
                        if (m.isValid() && it != keeperOf.end()) {
                            keepers.insert(it->second.value);
                        }
                    });
                    if (copy.media.isValid() && to(copy.media)) {
                        changed = true;
                    }
                    if (const auto& shared = c.sharedContent()) {
                        if (auto replaced = newContent(shared)) {
                            copy.setContent(std::move(replaced));
                            changed = true;
                        }
                    }
                    if (changed) {
                        if (!clipsChanged) {
                            clipsChanged = true;
                            clips.reserve(original->clips.size());
                            for (std::size_t k = 0; k < index; ++k) {
                                clips.push_back(original->clips[k]);
                            }
                        }
                        for (const std::uint64_t keeper : keepers) {
                            ++repointedByKeeper[keeper];
                        }
                    }
                    if (clipsChanged) {
                        clips.push_back(changed ? std::move(copy) : c);
                    }
                    ++index;
                }
                if (clipsChanged || track) {
                    if (!track) {
                        track = std::make_shared<Track>(*original);
                    }
                    if (clipsChanged) {
                        track->clips = ClipList(std::move(clips));
                    }
                    (edited().*list)[i] = std::move(track);
                }
            }
        };
        rewriteTracks(&Sequence::videoTracks);
        rewriteTracks(&Sequence::audioTracks);
        rewriteTracks(&Sequence::submixTracks);
        if (seq) {
            sp = std::move(seq);
        }
    }

    // 2. Items: the keepers absorb their duplicates; other items' references (masters of subclips, provenance, Master
    //    Clip Effects with mattes) follow.
    std::map<std::uint64_t, std::vector<std::string>> notesOf; // keeper id -> notes
    for (const GroupWork& work : merges) {
        MediaItem keeper = *state.findMedia(work.keeper->id);
        for (const MediaItem* donor : work.merged) {
            absorb(keeper, *donor, notesOf[keeper.id.value]);
        }
        *findMediaSlot(out, keeper.id) = std::make_shared<const MediaItem>(std::move(keeper));
    }
    for (MediaPtr& m : out.media) {
        if (keeperOf.contains(m->id.value)) {
            continue; // removed below
        }
        MediaItem copy = *m;
        bool changed = false;
        if (copy.subclip) {
            changed = to(copy.subclip->master) || changed;
        }
        if (copy.generated && copy.generated->source.isValid()) {
            changed = to(copy.generated->source) || changed;
        }
        if (copy.derived && copy.derived->source.isValid()) {
            changed = to(copy.derived->source) || changed;
        }
        changed = repointEffects(copy.masterEffects, to) || changed;
        if (changed) {
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }

    // 3. Redactions follow their media (a redaction of a removed item would apply to nothing: the blur would be lost).
    // (std::as_const: reading a SharedList through its non-const begin() would copy it.)
    const auto& redactionsNow = std::as_const(out.redactions);
    if (std::any_of(redactionsNow.begin(), redactionsNow.end(), [&](const Redaction& r) {
            return keeperOf.contains(r.media.value) || (r.region.matte && keeperOf.contains(r.region.matte->media.value));
        })) {
        for (Redaction& r : out.redactions.edit()) {
            to(r.media);
            if (r.region.matte) {
                to(r.region.matte->media);
            }
        }
    }

    // 4. Transcript corrections move to the keeper when it has none for that audio stream; otherwise the keeper's stay.
    std::map<std::uint64_t, std::vector<std::string>> transcriptNotes;
    const auto& editsNow = std::as_const(out.transcriptEdits);
    if (std::any_of(editsNow.begin(), editsNow.end(),
                    [&](const TranscriptEdits& e) { return keeperOf.contains(e.media.value); })) {
        std::vector<TranscriptEdits> kept;
        std::set<std::pair<std::uint64_t, std::int32_t>> have;
        for (const TranscriptEdits& e : editsNow) {
            if (!keeperOf.contains(e.media.value)) {
                have.insert({e.media.value, e.audioStream});
            }
        }
        for (const TranscriptEdits& e : out.transcriptEdits) {
            if (!keeperOf.contains(e.media.value)) {
                kept.push_back(e);
                continue;
            }
            const MediaId keeper = keeperOf.at(e.media.value);
            if (have.insert({keeper.value, e.audioStream}).second) {
                TranscriptEdits moved = e;
                moved.media = keeper;
                kept.push_back(std::move(moved));
            } else {
                const MediaItem* donor = state.findMedia(e.media);
                const MediaItem* k = state.findMedia(keeper);
                transcriptNotes[keeper.value].push_back("The transcript corrections of \"" + (donor ? donor->name : std::string()) +
                                                        "\" were not moved: \"" + (k ? k->name : std::string()) +
                                                        "\" already has its own.");
            }
        }
        std::sort(kept.begin(), kept.end(), [](const TranscriptEdits& a, const TranscriptEdits& b) {
            return std::pair{a.media, a.audioStream} < std::pair{b.media, b.audioStream};
        });
        out.transcriptEdits.edit() = std::move(kept);
    }

    // 5. Graphic templates and the brand kit's logos.
    for (GraphicTemplate& t : out.templates.edit()) {
        repointGraphic(t.graphic, to);
        for (TemplateField& field : t.fields) {
            if (field.kind == FieldKind::Image && field.value.media.isValid()) {
                to(field.value.media);
            }
        }
    }
    if (out.brandKit) {
        for (BrandLogo& logo : out.brandKit->logos) {
            if (logo.media.isValid()) {
                to(logo.media);
            }
        }
    }

    // 6. The duplicates themselves.
    for (const auto& [removedId, keeperId] : keeperOf) {
        auto next = removeMedia(out, MediaId{removedId});
        if (!next) {
            return std::unexpected(next.error());
        }
        out = std::move(*next);
    }

    for (ConsolidateGroup& g : result.groups) {
        if (const auto it = repointedByKeeper.find(g.keeper.value); it != repointedByKeeper.end()) {
            g.repointedClips = it->second;
        }
        if (const auto it = notesOf.find(g.keeper.value); it != notesOf.end()) {
            g.notes.insert(g.notes.end(), it->second.begin(), it->second.end());
        }
        if (const auto it = transcriptNotes.find(g.keeper.value); it != transcriptNotes.end()) {
            g.notes.insert(g.notes.end(), it->second.begin(), it->second.end());
        }
    }
    return result;
}

} // namespace wabcut::edit
