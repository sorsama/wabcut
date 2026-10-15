// Project-level operations: bins, media items, sequences, tracks and clip attributes.

#include <wabcut/model/ProjectOps.h>

#include <wabcut/model/Nesting.h>

#include "SequenceEditor.h"

#include <wabcut/model/TitleOps.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>
#include <unordered_set>

namespace wabcut::edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

namespace {

bool binExists(const ProjectState& state, BinId bin) { return !bin.isValid() || state.findBin(bin) != nullptr; }

} // namespace

std::expected<Created<BinId>, Error> addBin(const ProjectState& state, std::string name, BinId parent) {
    if (!binExists(state, parent)) {
        return std::unexpected(notFound("parent bin does not exist"));
    }
    Created<BinId> out{state, {}};
    out.id = out.state.ids.allocate<BinId>();
    out.state.bins.push_back({out.id, parent, std::move(name)});
    return out;
}

std::expected<Created<MediaId>, Error> addMedia(const ProjectState& state, MediaItem item) {
    if (!binExists(state, item.bin)) {
        return std::unexpected(notFound("bin does not exist"));
    }
    Created<MediaId> out{state, {}};
    out.id = out.state.ids.allocate<MediaId>();
    item.id = out.id;
    item.clipDefaults = std::make_shared<const ClipContent>(ClipContent{item.name, {}});
    // Ids only grow, so appending keeps the list sorted.
    out.state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    return out;
}

std::expected<ProjectState, Error> removeMedia(const ProjectState& state, MediaId media) {
    const auto it = std::find_if(state.media.begin(), state.media.end(),
                                 [&](const MediaPtr& m) { return m->id == media; });
    if (it == state.media.end()) {
        return std::unexpected(notFound("media " + std::to_string(media.value) + " does not exist"));
    }
    for (const SequencePtr& seq : state.sequences) {
        bool used = false;
        seq->forEachTrack([&](const Track& t) {
            used = used || std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.media == media; });
        });
        if (used) {
            return std::unexpected(invalidArgument("media '" + (*it)->name + "' is used in sequence '" + seq->name + "'"));
        }
    }
    ProjectState out = state;
    out.media.erase(out.media.begin() + (it - state.media.begin()));
    return out;
}

// --- Audio takes (A.4) -------------------------------------------------------------------------------------------

namespace {

std::expected<std::size_t, Error> mediaIndex(const ProjectState& state, MediaId media) {
    const auto it = std::find_if(state.media.begin(), state.media.end(), [&](const MediaPtr& m) { return m->id == media; });
    if (it == state.media.end()) {
        return std::unexpected(notFound("media " + std::to_string(media.value) + " does not exist"));
    }
    return static_cast<std::size_t>(it - state.media.begin());
}

} // namespace

std::expected<ProjectState, Error> setAudioTake(const ProjectState& state, MediaId media, AudioTake take) {
    const auto index = mediaIndex(state, media);
    if (!index) {
        return std::unexpected(index.error());
    }
    const MediaItem& item = *state.media[*index];
    if (item.generator) {
        return std::unexpected(invalidArgument("'" + item.name + "' is generated: it has no audio to render"));
    }
    if (take.stream < 0 || static_cast<std::size_t>(take.stream) >= item.info.audio.size()) {
        return std::unexpected(invalidArgument("'" + item.name + "' has no audio stream " + std::to_string(take.stream)));
    }
    if (take.kind.empty() || take.path.empty() || take.kind.size() > AudioTake::kMaxNameBytes ||
        take.model.size() > AudioTake::kMaxNameBytes || !isValidUtf8(take.kind) || !isValidUtf8(take.model) ||
        !isValidUtf8(take.path) || take.source.size < -1) {
        return std::unexpected(invalidArgument("an audio take needs a kind and a path in UTF-8 (kind and model at most 128 bytes)"));
    }
    MediaItem copy = item;
    std::erase_if(copy.audioTakes, [&](const AudioTake& t) { return t.stream == take.stream && t.kind == take.kind; });
    const auto at = std::find_if(copy.audioTakes.begin(), copy.audioTakes.end(), [&](const AudioTake& t) {
        return std::tie(t.stream, t.kind) > std::tie(take.stream, take.kind);
    });
    copy.audioTakes.insert(at, std::move(take));
    ProjectState out = state;
    out.media[*index] = std::make_shared<const MediaItem>(std::move(copy));
    return out;
}

std::expected<ProjectState, Error> removeAudioTake(const ProjectState& state, MediaId media, int stream, std::string_view kind) {
    const auto index = mediaIndex(state, media);
    if (!index) {
        return std::unexpected(index.error());
    }
    const MediaItem& item = *state.media[*index];
    if (item.audioTake(stream, kind) == nullptr) {
        return state;
    }
    MediaItem copy = item;
    std::erase_if(copy.audioTakes, [&](const AudioTake& t) { return t.stream == stream && t.kind == kind; });
    ProjectState out = state;
    out.media[*index] = std::make_shared<const MediaItem>(std::move(copy));
    return out;
}

// --- Assets (LUT files, I.5) -----------------------------------------------------------------------------------

namespace {

bool refersTo(const EffectStack& stack, const std::string& reference) {
    return std::any_of(stack.begin(), stack.end(), [&](const EffectInstance& e) {
        return std::any_of(e.data.begin(), e.data.end(), [&](const EffectData& d) { return d.text() && *d.text() == reference; });
    });
}

/// The stack without data entries naming `reference` (true when something was removed).
bool clearReference(EffectStack& stack, const std::string& reference) {
    bool changed = false;
    for (EffectInstance& e : stack) {
        const auto before = e.data.size();
        std::erase_if(e.data, [&](const EffectData& d) { return d.text() && *d.text() == reference; });
        changed = changed || e.data.size() != before;
    }
    return changed;
}

std::expected<void, Error> checkAssetLocator(const MediaLocator& locator) {
    if (locator.path.empty()) {
        return std::unexpected(invalidArgument("an asset needs a file path"));
    }
    if (locator.proxy) {
        return std::unexpected(invalidArgument("an asset has no proxy"));
    }
    return {};
}

} // namespace

std::expected<Created<AssetId>, Error> addAsset(const ProjectState& state, ProjectAsset asset) {
    if (asset.kind.empty() || asset.name.empty()) {
        return std::unexpected(invalidArgument("an asset needs a kind and a name"));
    }
    if (auto ok = checkAssetLocator(asset.locator); !ok) {
        return std::unexpected(ok.error());
    }
    Created<AssetId> out{state, {}};
    out.id = out.state.ids.allocate<AssetId>();
    asset.id = out.id;
    out.state.assets.push_back(std::move(asset)); // ids only grow: still sorted
    return out;
}

std::expected<ProjectState, Error> relinkAsset(const ProjectState& state, AssetId asset, MediaLocator locator) {
    if (!state.findAsset(asset)) {
        return std::unexpected(notFound("asset " + std::to_string(asset.value) + " does not exist"));
    }
    if (auto ok = checkAssetLocator(locator); !ok) {
        return std::unexpected(ok.error());
    }
    ProjectState out = state;
    for (ProjectAsset& a : out.assets) {
        if (a.id == asset) {
            a.locator = std::move(locator);
        }
    }
    return out;
}

std::expected<ProjectState, Error> removeAsset(const ProjectState& state, AssetId asset, bool clearReferences) {
    const ProjectAsset* found = state.findAsset(asset);
    if (!found) {
        return std::unexpected(notFound("asset " + std::to_string(asset.value) + " does not exist"));
    }
    const std::string reference = assetReference(asset);
    ProjectState out = state;
    for (SequencePtr& seq : out.sequences) {
        bool used = refersTo(seq->effects, reference);
        seq->forEachTrack([&](const Track& t) {
            used = used || refersTo(t.effects, reference) ||
                   std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return refersTo(c.effects(), reference); });
        });
        if (!used) {
            continue;
        }
        if (!clearReferences) {
            return std::unexpected(invalidArgument("asset '" + found->name + "' is used by effects in sequence '" + seq->name +
                                                   "'"));
        }
        auto next = std::make_shared<Sequence>(*seq);
        clearReference(next->effects, reference);
        for (auto* list : {&next->videoTracks, &next->audioTracks}) {
            for (TrackPtr& track : *list) {
                const bool trackUses = refersTo(track->effects, reference) ||
                                       std::any_of(track->clips.begin(), track->clips.end(),
                                                   [&](const Clip& c) { return refersTo(c.effects(), reference); });
                if (!trackUses) {
                    continue;
                }
                auto t = std::make_shared<Track>(*track);
                clearReference(t->effects, reference);
                for (std::size_t i = 0; i < t->clips.size(); ++i) {
                    if (refersTo(t->clips[i].effects(), reference)) {
                        EffectStack stack = t->clips[i].effects();
                        clearReference(stack, reference);
                        t->clips.mutableAt(i).setEffects(std::move(stack));
                    }
                }
                track = std::move(t);
            }
        }
        seq = std::move(next);
    }
    std::erase_if(out.assets, [&](const ProjectAsset& a) { return a.id == asset; });
    return out;
}

std::expected<Created<SequenceId>, Error> addSequence(const ProjectState& state, const SequenceSettings& settings) {
    if (!settings.frameRate.isExact() || !settings.audioRate.isExact()) {
        return std::unexpected(invalidArgument("unsupported frame or audio rate"));
    }
    if (settings.width <= 0 || settings.height <= 0) {
        return std::unexpected(invalidArgument("the canvas must not be empty"));
    }
    if (settings.videoTracks < 0 || settings.audioTracks < 0) {
        return std::unexpected(invalidArgument("negative track count"));
    }
    if (!binExists(state, settings.bin)) {
        return std::unexpected(notFound("bin does not exist"));
    }
    Created<SequenceId> out{state, {}};
    auto seq = std::make_shared<Sequence>();
    seq->id = out.state.ids.allocate<SequenceId>();
    seq->bin = settings.bin;
    seq->name = settings.name;
    seq->frameRate = settings.frameRate;
    seq->width = settings.width;
    seq->height = settings.height;
    seq->audioRate = settings.audioRate;
    seq->dropFrame = settings.dropFrame;
    for (std::int32_t i = 0; i < settings.videoTracks; ++i) {
        auto t = std::make_shared<Track>();
        t->id = out.state.ids.allocate<TrackId>();
        t->kind = TrackKind::Video;
        t->targeted = i == 0;
        seq->videoTracks.push_back(std::move(t));
    }
    for (std::int32_t i = 0; i < settings.audioTracks; ++i) {
        auto t = std::make_shared<Track>();
        t->id = out.state.ids.allocate<TrackId>();
        t->kind = TrackKind::Audio;
        t->targeted = i == 0;
        seq->audioTracks.push_back(std::move(t));
    }
    out.id = seq->id;
    out.state.sequences.push_back(std::move(seq));
    return out;
}

std::expected<ProjectState, Error> removeSequence(const ProjectState& state, SequenceId sequence) {
    const auto it = std::find_if(state.sequences.begin(), state.sequences.end(),
                                 [&](const SequencePtr& s) { return s->id == sequence; });
    if (it == state.sequences.end()) {
        return std::unexpected(notFound("sequence " + std::to_string(sequence.value) + " does not exist"));
    }
    // C.8: a sequence that another one plays (nested or multicam clips) stays, like media in use.
    for (const SequencePtr& other : state.sequences) {
        if (other->id == sequence) {
            continue;
        }
        bool used = false;
        other->forEachTrack([&](const Track& t) {
            used = used || std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.sequence == sequence; });
        });
        if (used) {
            return std::unexpected(invalidArgument("sequence '" + (*it)->name + "' is used in sequence '" + other->name + "'"));
        }
    }
    ProjectState out = state;
    out.sequences.erase(out.sequences.begin() + (it - state.sequences.begin()));
    // C.9: its notes go too (the list stays shared when it has none).
    if (std::any_of(state.reviewNotes.begin(), state.reviewNotes.end(), [&](const ReviewNote& n) { return n.sequence == sequence; })) {
        std::erase_if(out.reviewNotes.edit(), [&](const ReviewNote& n) { return n.sequence == sequence; });
    }
    return out;
}

EditOutcome setSequenceStartTimecode(const ProjectState& state, SequenceId sequence, Ticks start) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (start.value < 0 || start > kMaxStartTimecode) {
        return std::unexpected(invalidArgument("a start timecode lies between 00:00:00:00 and 24 hours"));
    }
    if ((*seq)->startTimecode == start) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    ed.header().startTimecode = start;
    return ed.finish();
}

EditOutcome setSequenceDuration(const ProjectState& state, SequenceId sequence, Ticks duration) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (duration < (*seq)->contentEnd()) {
        return std::unexpected(invalidArgument("the sequence cannot end before its last clip"));
    }
    if ((*seq)->workArea && (*seq)->workArea->end() > duration) {
        return std::unexpected(invalidArgument("the sequence cannot end inside its work area"));
    }
    if (duration == (*seq)->duration) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    ed.header().duration = duration;
    return ed.finish();
}

EditOutcome fitSequenceDurationToContent(const ProjectState& state, SequenceId sequence) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Ticks end = (*seq)->contentEnd();
    if (end == (*seq)->duration) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    Sequence& h = ed.header();
    h.duration = end;
    if (h.workArea && h.workArea->end() > end) {
        // Keep what is left of the work area inside the new end; drop it if nothing is.
        if (h.workArea->start < end) {
            h.workArea = TimeRange::fromStartEnd(h.workArea->start, end);
        } else {
            h.workArea.reset();
        }
    }
    return ed.finish();
}

EditOutcome setWorkArea(const ProjectState& state, SequenceId sequence, std::optional<TimeRange> workArea) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (workArea && (workArea->start.value < 0 || workArea->isEmpty() || workArea->end() > (*seq)->duration)) {
        return std::unexpected(invalidArgument("the work area must be non-empty and inside the sequence"));
    }
    if (workArea == (*seq)->workArea) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    ed.header().workArea = workArea;
    return ed.finish();
}

std::expected<Created<TrackId>, Error> addTrack(const ProjectState& state, SequenceId sequence, TrackKind kind,
                                                std::string name) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    auto next = std::make_shared<Sequence>(**seq);
    Created<TrackId> out{state, {}};
    auto t = std::make_shared<Track>();
    t->id = out.state.ids.allocate<TrackId>();
    t->kind = kind;
    t->name = std::move(name);
    out.id = t->id;
    (kind == TrackKind::Video ? next->videoTracks : next->audioTracks).push_back(std::move(t));
    out.state = out.state.withSequence(std::move(next));
    return out;
}

EditOutcome removeTrack(const ProjectState& state, SequenceId sequence, TrackId trackId) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Track* track = (*seq)->findTrack(trackId);
    if (!track) {
        return std::unexpected(notFound("track " + std::to_string(trackId.value) + " does not exist"));
    }
    if (track->locked) {
        return std::unexpected(invalidArgument("track " + detail::trackLabel(**seq, trackId) + " is locked"));
    }
    // C.8: an angle's track stays while the angle does; the last track of a kind stays while clips of other sequences
    // play that kind of this sequence (nested video or audio).
    if ((*seq)->multicam) {
        if (const auto angle = multicamAngleOfTrack(*(*seq)->multicam, trackId)) {
            return std::unexpected(invalidArgument("track " + detail::trackLabel(**seq, trackId) + " holds angle " +
                                                   std::to_string(*angle + 1) + " of this multicam source"));
        }
    }
    if ((*seq)->tracks(track->kind).size() == 1) {
        const StreamKind kind = track->kind == TrackKind::Video ? StreamKind::Video : StreamKind::Audio;
        for (const SequencePtr& other : state.sequences) {
            bool used = false;
            other->forEachTrack([&](const Track& t) {
                used = used || std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) {
                           return c.sequence == sequence && c.stream.kind == kind;
                       });
            });
            if (used) {
                return std::unexpected(invalidArgument("sequence '" + other->name + "' plays the " +
                                                       std::string(kind == StreamKind::Video ? "picture" : "sound") +
                                                       " of this sequence: its last track of that kind stays"));
            }
        }
    }
    SequenceEditor ed(state, **seq);
    ed.eraseTrack(trackId);
    return ed.finish();
}

EditOutcome setTrackState(const ProjectState& state, SequenceId sequence, TrackId trackId,
                          const TrackStateChange& change) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (!(*seq)->findTrack(trackId)) {
        return std::unexpected(notFound("track " + std::to_string(trackId.value) + " does not exist"));
    }
    if (change.role && *change.role != TrackRole::None && (*seq)->findTrack(trackId)->kind != TrackKind::Audio) {
        return std::unexpected(detail::invalidArgument("only audio tracks have a role; " +
                                                       detail::trackLabel(**seq, trackId) + " is a video track"));
    }
    if (change.recordArmed && *change.recordArmed && (*seq)->findTrack(trackId)->kind != TrackKind::Audio) {
        return std::unexpected(detail::invalidArgument("only audio tracks record; " + detail::trackLabel(**seq, trackId) +
                                                       " is a video track"));
    }
    if ((change.format && *change.format != ChannelFormat::Stereo) || change.surround) {
        if ((*seq)->findTrack(trackId)->kind != TrackKind::Audio) {
            return std::unexpected(detail::invalidArgument("only audio tracks have a channel format or a 5.1 panner; " +
                                                           detail::trackLabel(**seq, trackId) + " is a video track"));
        }
    }
    if (change.surround && (!change.surround->x.isValid(-1.0, 1.0) || !change.surround->y.isValid(-1.0, 1.0) ||
                            !change.surround->lfeDb.isValid(-144.0, 0.0))) {
        return std::unexpected(detail::invalidArgument("the 5.1 panner takes x and y in -1..1 and an LFE send in -144..0 dB"));
    }
    SequenceEditor ed(state, **seq);
    Track& t = ed.mutableTrack(trackId);
    const bool wasLocked = t.locked;
    if (change.name) {
        t.name = *change.name;
    }
    if (change.enabled) {
        t.enabled = *change.enabled;
    }
    if (change.locked) {
        t.locked = *change.locked;
    }
    if (change.syncLocked) {
        t.syncLocked = *change.syncLocked;
    }
    if (change.muted) {
        t.muted = *change.muted;
    }
    if (change.solo) {
        t.solo = *change.solo;
    }
    if (change.targeted) {
        t.targeted = *change.targeted;
    }
    if (change.role) {
        t.role = *change.role;
    }
    if (change.recordArmed) {
        t.recordArmed = *change.recordArmed;
    }
    if (change.format) {
        t.format = *change.format;
    }
    if (change.surround) {
        t.surround = *change.surround;
    }
    if (wasLocked && !t.locked) {
        // Single-clip link groups were only kept because the track was locked; let them dissolve now.
        for (const Clip& clip : t.clips) {
            ed.touchGroup(clip.linkGroup);
        }
    }
    return ed.finish();
}

namespace {

std::optional<std::string> checkProperties(const ClipProperties& p) {
    if (const auto bad = p.firstInvalid()) {
        return "parameter " + std::string(parameterInfo(*bad).key) + " is outside its range or not finite";
    }
    return std::nullopt;
}

} // namespace

EditOutcome setClipAttributes(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                              const ClipAttributeChange& change) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (change.properties) {
        if (const auto problem = checkProperties(*change.properties)) {
            return std::unexpected(invalidArgument(*problem));
        }
    }
    const auto selection = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    SequenceEditor ed(state, **seq);
    for (const ClipRef& ref : *selection) {
        Clip& c = ed.mutableClip(ref.track, ref.index);
        if (change.blendMode && c.stream.kind != StreamKind::Video) {
            return std::unexpected(invalidArgument("clip " + std::to_string(c.id.value) +
                                                   " is an audio clip: only video clips have a blend mode"));
        }
        if (change.blendMode && c.blendMode() != *change.blendMode) {
            c.setBlendMode(*change.blendMode);
        }
        if (change.name || change.properties) {
            ClipContent content = c.content();
            if (change.name) {
                content.name = *change.name;
            }
            if (change.properties) {
                content.properties = *change.properties;
            }
            c.setContent(std::move(content));
        }
        if (change.colorIndex) {
            c.colorIndex = *change.colorIndex;
        }
        if (change.enabled) {
            c.enabled = *change.enabled;
        }
    }
    return ed.finish();
}

// --- Bins and library metadata (M5k) --------------------------------------------------------------------

namespace {

MediaPtr* findMediaSlot(ProjectState& state, MediaId id) {
    const auto it = std::lower_bound(state.media.begin(), state.media.end(), id,
                                     [](const MediaPtr& m, MediaId value) { return m->id < value; });
    return it != state.media.end() && (*it)->id == id ? &*it : nullptr;
}

/// True when `bin` is `ancestor` or lies inside it.
bool isInside(const ProjectState& state, BinId bin, BinId ancestor) {
    std::size_t guard = 0;
    while (bin.isValid() && guard++ <= state.bins.size()) {
        if (bin == ancestor) {
            return true;
        }
        const Bin* b = state.findBin(bin);
        bin = b ? b->parent : BinId{};
    }
    return false;
}

bool usesMedia(const Sequence& seq, const std::unordered_set<std::uint64_t>& media, MediaId* which) {
    bool found = false;
    seq.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (!found && media.contains(c.media.value)) {
                found = true;
                *which = c.media;
            }
        }
    });
    return found;
}

/// The end of a media item's longest stream (where marks, poster frames and subclip ranges may reach), or nullopt.
std::optional<Ticks> fileExtent(const MediaItem& item) { return sourceMarkExtent(item); }

/// Whether a clip of `item` stays inside `item.sourceBounds()` (the same rules as validate's Strict level).
std::optional<std::string> clipBoundsProblem(const MediaItem& item, const Clip& clip) {
    const SourceBounds bounds = item.sourceBounds(clip.stream);
    const std::string what = "clip '" + clip.name() + "'";
    if (const Curve* remap = clip.timeRemap()) {
        const auto shown = remapShownRange(*remap, clip.sourceIn, clip.sourceOut());
        for (const Keyframe& k : remap->keys) {
            if (k.value < static_cast<double>(bounds.start.value) ||
                (bounds.end && k.value > static_cast<double>(bounds.end->value))) {
                return what + " has a time remap key outside the subclip";
            }
        }
        if (shown.first < bounds.start || (bounds.end && shown.second >= *bounds.end)) {
            return what + " shows media outside the subclip";
        }
        return std::nullopt;
    }
    if (clip.isFreeze()) {
        if (clip.sourceIn < bounds.start || (bounds.end && clip.sourceIn >= *bounds.end)) {
            return what + " holds a frame outside the subclip";
        }
        return std::nullopt;
    }
    if (clip.sourceIn < bounds.start || (bounds.end && clip.sourceOut() > *bounds.end)) {
        return what + " uses media outside the subclip";
    }
    return std::nullopt;
}

} // namespace

std::expected<ProjectState, Error> renameBin(const ProjectState& state, BinId bin, std::string name) {
    const Bin* found = state.findBin(bin);
    if (!found) {
        return std::unexpected(notFound("bin " + std::to_string(bin.value) + " does not exist"));
    }
    if (name.empty()) {
        return std::unexpected(invalidArgument("a bin needs a name"));
    }
    if (found->name == name) {
        return state;
    }
    ProjectState out = state;
    for (Bin& b : out.bins) {
        if (b.id == bin) {
            b.name = std::move(name);
        }
    }
    return out;
}

std::expected<ProjectState, Error> moveToBin(const ProjectState& state, BinId target, std::span<const BinId> bins,
                                             std::span<const MediaId> media, std::span<const SequenceId> sequences) {
    if (!binExists(state, target)) {
        return std::unexpected(notFound("bin " + std::to_string(target.value) + " does not exist"));
    }
    for (const BinId bin : bins) {
        const Bin* b = state.findBin(bin);
        if (!b) {
            return std::unexpected(notFound("bin " + std::to_string(bin.value) + " does not exist"));
        }
        if (isInside(state, target, bin)) {
            return std::unexpected(invalidArgument("bin '" + b->name + "' cannot move into itself"));
        }
    }
    for (const MediaId id : media) {
        if (!state.findMedia(id)) {
            return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
        }
    }
    for (const SequenceId id : sequences) {
        if (!state.findSequence(id)) {
            return std::unexpected(notFound("sequence " + std::to_string(id.value) + " does not exist"));
        }
    }
    ProjectState out = state;
    bool changed = false;
    for (Bin& b : out.bins) {
        if (b.parent != target && std::find(bins.begin(), bins.end(), b.id) != bins.end()) {
            b.parent = target;
            changed = true;
        }
    }
    for (const MediaId id : media) {
        MediaPtr* slot = findMediaSlot(out, id);
        if ((*slot)->bin != target) {
            MediaItem item = **slot;
            item.bin = target;
            *slot = std::make_shared<const MediaItem>(std::move(item));
            changed = true;
        }
    }
    for (SequencePtr& seq : out.sequences) {
        if (seq->bin != target && std::find(sequences.begin(), sequences.end(), seq->id) != sequences.end()) {
            Sequence next = *seq;
            next.bin = target;
            seq = std::make_shared<const Sequence>(std::move(next));
            changed = true;
        }
    }
    return changed ? out : state;
}

std::expected<ProjectState, Error> removeBins(const ProjectState& state, std::span<const BinId> bins) {
    for (const BinId bin : bins) {
        if (!state.findBin(bin)) {
            return std::unexpected(notFound("bin " + std::to_string(bin.value) + " does not exist"));
        }
    }
    const auto removedBin = [&](BinId bin) {
        return std::any_of(bins.begin(), bins.end(), [&](BinId root) { return isInside(state, bin, root); });
    };
    std::unordered_set<std::uint64_t> media;
    for (const MediaPtr& m : state.media) {
        if (m->bin.isValid() && removedBin(m->bin)) {
            media.insert(m->id.value);
        }
    }
    const auto removedSequence = [&](SequenceId id) {
        const Sequence* s = state.findSequence(id);
        return s && s->bin.isValid() && removedBin(s->bin);
    };
    for (const SequencePtr& seq : state.sequences) {
        if (seq->bin.isValid() && removedBin(seq->bin)) {
            continue; // deleted with the bin
        }
        MediaId used;
        if (usesMedia(*seq, media, &used)) {
            return std::unexpected(invalidArgument("media '" + state.findMedia(used)->name + "' is used in sequence '" +
                                                   seq->name + "'"));
        }
        // E5-P2-6: a sequence another one plays (nested or multicam clips) stays, as in removeSequence.
        std::optional<SequenceId> played;
        seq->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                if (!played && c.isNested() && removedSequence(c.sequence)) {
                    played = c.sequence;
                }
            }
        });
        if (played) {
            return std::unexpected(invalidArgument("sequence '" + state.findSequence(*played)->name +
                                                   "' is used in sequence '" + seq->name + "'"));
        }
    }
    ProjectState out = state;
    std::erase_if(out.bins, [&](const Bin& b) { return removedBin(b.id); });
    std::erase_if(out.media, [&](const MediaPtr& m) { return media.contains(m->id.value); });
    std::erase_if(out.sequences, [&](const SequencePtr& s) { return s->bin.isValid() && removedBin(s->bin); });
    // Their review notes go too, as in removeSequence (the list stays shared when none is affected).
    if (std::any_of(state.reviewNotes.begin(), state.reviewNotes.end(),
                    [&](const ReviewNote& n) { return removedSequence(n.sequence); })) {
        std::erase_if(out.reviewNotes.edit(), [&](const ReviewNote& n) { return removedSequence(n.sequence); });
    }
    return out;
}

std::expected<ProjectState, Error> setMediaAttributes(const ProjectState& state, std::span<const MediaId> media,
                                                      const MediaAttributeChange& change) {
    if (change.name && change.name->empty()) {
        return std::unexpected(invalidArgument("a media item needs a name"));
    }
    if (change.colorIndex && (*change.colorIndex < 0 || *change.colorIndex > 15)) {
        return std::unexpected(invalidArgument("label colours are 0..15"));
    }
    if ((change.comment && change.comment->size() > MediaItem::kMaxTextBytes) ||
        (change.description && change.description->size() > MediaItem::kMaxTextBytes)) {
        return std::unexpected(invalidArgument("a comment or description is longer than " +
                                               std::to_string(MediaItem::kMaxTextBytes) + " bytes"));
    }
    ProjectState out = state;
    bool changed = false;
    for (const MediaId id : media) {
        MediaPtr* slot = findMediaSlot(out, id);
        if (!slot) {
            return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
        }
        MediaItem item = **slot;
        if (change.posterFrame && *change.posterFrame) {
            const Ticks poster = **change.posterFrame;
            const auto extent = fileExtent(item);
            if (poster.value < 0 || (extent && poster >= *extent)) {
                return std::unexpected(invalidArgument("the poster frame of '" + item.name + "' is outside the media"));
            }
        }
        if (change.name) {
            item.name = *change.name;
            // New clips take the new name; clips already placed keep theirs (Premiere).
            item.clipDefaults = std::make_shared<const ClipContent>(ClipContent{item.name, {}});
        }
        if (change.colorIndex) {
            item.colorIndex = *change.colorIndex;
        }
        if (change.comment) {
            item.comment = *change.comment;
        }
        if (change.description) {
            item.description = *change.description;
        }
        if (change.posterFrame) {
            item.posterFrame = *change.posterFrame;
        }
        if (!(item == **slot)) {
            *slot = std::make_shared<const MediaItem>(std::move(item));
            changed = true;
        }
    }
    return changed ? out : state;
}

std::expected<Created<MediaId>, Error> makeSubclip(const ProjectState& state, MediaId masterId, TimeRange range,
                                                   std::string name, bool restricted, BinId bin) {
    const MediaItem* master = state.findMedia(masterId);
    if (!master) {
        return std::unexpected(notFound("media " + std::to_string(masterId.value) + " does not exist"));
    }
    if (master->isGenerator() || master->info.isStillImage) {
        return std::unexpected(invalidArgument("'" + master->name + "' has no time to make a subclip of"));
    }
    if (range.start.value < 0 || range.duration.value <= 0) {
        return std::unexpected(invalidArgument("a subclip needs an In before its Out"));
    }
    // Inside the file, and inside the master's own range when the master is a restricted subclip.
    Ticks first{};
    std::optional<Ticks> last = fileExtent(*master);
    if (master->subclip && master->subclip->restricted) {
        first = master->subclip->range.start;
        last = last ? std::min(*last, master->subclip->range.end()) : master->subclip->range.end();
    }
    if (range.start < first || (last && range.end() > *last)) {
        return std::unexpected(invalidArgument("the subclip lies outside '" + master->name + "'"));
    }
    MediaItem item;
    item.bin = bin;
    item.name = name.empty() ? master->name + ".Subclip" : std::move(name);
    item.locator = master->locator;
    item.info = master->info;
    item.generated = master->generated;
    item.colorIndex = master->colorIndex;
    item.markIn = range.start;
    item.markOut = range.end();
    for (const Marker& m : master->markers) {
        if (range.contains(m.start)) {
            item.markers.push_back(m);
        }
    }
    item.subclip = SubclipSpec{master->id, range, restricted};
    return addMedia(state, std::move(item));
}

std::expected<ProjectState, Error> setSubclip(const ProjectState& state, MediaId id, TimeRange range,
                                              bool restricted) {
    const MediaItem* current = state.findMedia(id);
    if (!current) {
        return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
    }
    if (!current->subclip) {
        return std::unexpected(invalidArgument("'" + current->name + "' is not a subclip"));
    }
    if (range.start.value < 0 || range.duration.value <= 0) {
        return std::unexpected(invalidArgument("a subclip needs an In before its Out"));
    }
    if (const auto extent = fileExtent(*current); extent && range.end() > *extent) {
        return std::unexpected(invalidArgument("the subclip runs past the end of its media"));
    }
    if (current->subclip->range == range && current->subclip->restricted == restricted) {
        return state;
    }
    MediaItem item = *current;
    item.subclip->range = range;
    item.subclip->restricted = restricted;
    // Marks follow the range (Premiere's Edit Subclip resets them to the new boundaries when they fall outside).
    if (item.markIn && !range.contains(*item.markIn)) {
        item.markIn = std::clamp(*item.markIn, range.start, range.end() - Ticks{1});
    }
    if (item.markOut && (*item.markOut <= range.start || *item.markOut > range.end())) {
        item.markOut = std::clamp(*item.markOut, range.start + Ticks{1}, range.end());
    }
    if (item.markIn && item.markOut && *item.markOut <= *item.markIn) {
        item.markIn = range.start;
        item.markOut = range.end();
    }
    ProjectState out = state;
    *findMediaSlot(out, id) = std::make_shared<const MediaItem>(item);
    if (restricted) {
        // Every clip of it, and the transitions they take part in, must still fit the new range.
        for (const SequencePtr& seq : out.sequences) {
            std::optional<std::string> problem;
            seq->forEachTrack([&](const Track& track) {
                for (const Clip& clip : track.clips) {
                    if (!problem && clip.media == id) {
                        problem = clipBoundsProblem(item, clip);
                    }
                }
                for (const Transition& t : track.transitions) {
                    const Clip* a = t.outgoing.isValid() ? track.findClip(t.outgoing) : nullptr;
                    const Clip* b = t.incoming.isValid() ? track.findClip(t.incoming) : nullptr;
                    if (problem || !((a && a->media == id) || (b && b->media == id))) {
                        continue;
                    }
                    if (const auto p = transitionProblem(out, track, t, Validation::Strict)) {
                        problem = "a transition of '" + item.name + "' " + *p;
                    }
                }
            });
            if (problem) {
                return std::unexpected(invalidArgument(*problem + " in sequence '" + seq->name + "'"));
            }
        }
    }
    return out;
}

} // namespace wabcut::edit
