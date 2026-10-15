// Source edits (insert/overwrite, three-point editing), lift/extract, add edit, delete, ripple delete,
// marks, markers and selection queries.

#include <wabcut/model/EditOps.h>

#include <wabcut/model/Nesting.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace wabcut::edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

// --- Queries ------------------------------------------------------------------------------------------

std::vector<TrackId> targetedTracks(const Sequence& sequence) {
    std::vector<TrackId> out;
    sequence.forEachTrack([&](const Track& t) {
        if (t.targeted) {
            out.push_back(t.id);
        }
    });
    return out;
}

std::vector<TrackId> allTracks(const Sequence& sequence) {
    std::vector<TrackId> out;
    sequence.forEachTrack([&](const Track& t) { out.push_back(t.id); });
    return out;
}

std::vector<ClipId> withLinkedClips(const Sequence& sequence, std::span<const ClipId> clips) {
    std::vector<ClipId> out(clips.begin(), clips.end());
    std::unordered_set<ClipId> seen(clips.begin(), clips.end());
    std::unordered_set<LinkGroupId> groups;
    sequence.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.linkGroup.isValid() && seen.contains(c.id)) {
                groups.insert(c.linkGroup);
            }
        }
    });
    if (groups.empty()) {
        return out;
    }
    sequence.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.linkGroup.isValid() && groups.contains(c.linkGroup) && seen.insert(c.id).second) {
                out.push_back(c.id);
            }
        }
    });
    return out;
}

// --- Multi-track recordings (A.10) --------------------------------------------------------------------

namespace {

using Origin = SourceTrackName::Origin;

char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && isAsciiSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isAsciiSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// At most `maxBytes` of `text`, cut before a character that would not fit whole (UTF-8), trimmed again.
std::string_view truncatedUtf8(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return text;
    }
    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
        --end; // text[end] continues a character
    }
    return trimmed(text.substr(0, end));
}

/// What recognition compares: ASCII letters (lowered) and digits. Other ASCII characters (spaces, '/', '_', '-', ...)
/// are dropped, so "Mic/Aux", "mic aux" and "MIC_AUX" are one key; bytes of non-ASCII characters are kept as they are.
std::string recognitionKey(std::string_view text) {
    std::string key;
    key.reserve(text.size());
    for (const char c : text) {
        const bool other = static_cast<unsigned char>(c) >= 0x80u;
        if (other || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            key.push_back(lowerAscii(c));
        }
    }
    return key;
}

struct KnownSource {
    std::string_view key; ///< recognitionKey of a title
    std::string_view name;
    TrackRole role;
};

/// The titles OBS users, ShadowPlay and Game Bar give their audio tracks, by key, with the canonical track name and the
/// role it implies (Essential Sound: Game and Alerts are sound effects, voice chat is dialogue like the mic).
constexpr KnownSource kKnownSources[] = {
    {"game", "Game", TrackRole::Effects},
    {"gameaudio", "Game", TrackRole::Effects},
    {"gameplay", "Game", TrackRole::Effects},
    {"desktop", "Game", TrackRole::Effects},
    {"desktopaudio", "Game", TrackRole::Effects},
    {"system", "Game", TrackRole::Effects},
    {"systemaudio", "Game", TrackRole::Effects},
    {"appaudio", "Game", TrackRole::Effects},
    {"applicationaudio", "Game", TrackRole::Effects},
    {"mic", "Mic", TrackRole::Dialogue},
    {"microphone", "Mic", TrackRole::Dialogue},
    {"micaux", "Mic", TrackRole::Dialogue},
    {"micaudio", "Mic", TrackRole::Dialogue},
    {"voice", "Mic", TrackRole::Dialogue},
    {"commentary", "Mic", TrackRole::Dialogue},
    {"discord", "Discord", TrackRole::Dialogue},
    {"voicechat", "Discord", TrackRole::Dialogue},
    {"chat", "Discord", TrackRole::Dialogue},
    {"partychat", "Discord", TrackRole::Dialogue},
    {"teamspeak", "Discord", TrackRole::Dialogue},
    {"teamspeak3", "Discord", TrackRole::Dialogue},
    {"ts3", "Discord", TrackRole::Dialogue},
    {"music", "Music", TrackRole::Music},
    {"spotify", "Music", TrackRole::Music},
    {"bgm", "Music", TrackRole::Music},
    {"alerts", "Alerts", TrackRole::Effects},
    {"streamalerts", "Alerts", TrackRole::Effects},
};

const KnownSource* knownSource(std::string_view key) {
    for (const KnownSource& source : kKnownSources) {
        if (source.key == key) {
            return &source;
        }
    }
    return nullptr;
}

/// Track names match ignoring ASCII case and surrounding spaces ("mic " is the Mic track).
bool sameTrackName(std::string_view a, std::string_view b) {
    a = trimmed(a);
    b = trimmed(b);
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool hasName(const Track& track) { return !trimmed(track.name).empty(); }

} // namespace

SourceTrackName sourceTrackName(std::string_view title) {
    const std::string_view text = trimmed(title);
    if (text.empty()) {
        return {};
    }
    const std::string key = recognitionKey(text);
    if (const KnownSource* source = knownSource(key)) {
        return {std::string(source->name), source->role, Origin::Recognised};
    }
    // A number after a recognised name: OBS's "Track1", a second "Mic/Aux 2". Long numbers are not a source number.
    std::size_t digits = key.size();
    while (digits > 0 && key[digits - 1] >= '0' && key[digits - 1] <= '9') {
        --digits;
    }
    if (digits > 0 && digits < key.size() && key.size() - digits <= 4) {
        const std::string base = key.substr(0, digits);
        const std::string number = key.substr(digits);
        if (base == "track") {
            return {"Track " + number, TrackRole::None, Origin::Recognised};
        }
        if (const KnownSource* source = knownSource(base)) {
            return {std::string(source->name) + " " + number, source->role, Origin::Recognised};
        }
    }
    return {std::string(truncatedUtf8(text, kMaxSourceTrackNameBytes)), TrackRole::None, Origin::Title};
}

std::vector<SourceTrackName> sourceTrackNames(const MediaInfo& media) {
    std::vector<SourceTrackName> out;
    out.reserve(media.audio.size());
    for (const AudioStreamInfo& stream : media.audio) {
        out.push_back(sourceTrackName(stream.title));
    }
    if (out.size() < 2) {
        return out; // a single untitled stream keeps no name: it is patched as before
    }
    const bool allUntitled =
        std::all_of(out.begin(), out.end(), [](const SourceTrackName& n) { return n.origin == Origin::None; });
    // ShadowPlay ("separate both tracks") and Xbox Game Bar (with the microphone) write the game and the mic, in that
    // order, without names.
    const bool gameAndMic = allUntitled && out.size() == 2 && media.hasVideo();
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (out[k].origin != Origin::None) {
            continue;
        }
        const std::string name = gameAndMic ? std::string(k == 0 ? "Game" : "Mic") : "Track " + std::to_string(k + 1);
        out[k] = sourceTrackName(name);
        out[k].origin = Origin::Inferred;
    }
    return out;
}

bool routesAudioByName(const MediaInfo& media) {
    if (media.audio.size() >= 2) {
        return true;
    }
    return media.audio.size() == 1 && sourceTrackName(media.audio.front().title).origin == Origin::Recognised;
}

std::vector<SourcePatch> audioPatchesByName(const Sequence& sequence, const MediaInfo& media, std::size_t firstTrack) {
    const std::vector<SourceTrackName> names = sourceTrackNames(media);
    const std::vector<TrackPtr>& tracks = sequence.audioTracks;
    std::vector<bool> taken(tracks.size(), false);
    std::vector<std::optional<SourcePatch>> chosen(names.size());
    const auto patchTo = [&](std::size_t stream, std::size_t index, bool nameIt) {
        taken[index] = true;
        SourcePatch p;
        p.source = {StreamKind::Audio, static_cast<std::int32_t>(stream)};
        p.destination = tracks[index]->id;
        if (nameIt && !names[stream].name.empty()) {
            p.newTrack = names[stream].name;
        }
        chosen[stream] = std::move(p);
    };

    // 1. The track already named for the source (the previous recording's Mic track, ...).
    for (std::size_t k = 0; k < names.size(); ++k) {
        if (names[k].name.empty()) {
            continue;
        }
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (!taken[i] && !tracks[i]->locked && sameTrackName(tracks[i]->name, names[k].name)) {
                patchTo(k, i, false);
                break;
            }
        }
    }
    // 2. Untitled streams where Premiere's default patching puts them: stream k on the k-th unlocked track from
    // `firstTrack`, unless that track carries another source's name. It is named only when it is empty too: a clip
    // there says the track already holds something else.
    std::vector<std::size_t> unlocked;
    for (std::size_t i = firstTrack; i < tracks.size(); ++i) {
        if (!tracks[i]->locked) {
            unlocked.push_back(i);
        }
    }
    for (std::size_t k = 0; k < names.size() && k < unlocked.size(); ++k) {
        const bool untitled = names[k].origin == Origin::Inferred || names[k].origin == Origin::None;
        const std::size_t i = unlocked[k];
        if (!chosen[k] && untitled && !taken[i] && !hasName(*tracks[i])) {
            patchTo(k, i, tracks[i]->clips.empty());
        }
    }
    // 3. The next free track from `firstTrack` on (named for the stream), 4. else a new track.
    std::vector<SourcePatch> out;
    std::size_t next = firstTrack;
    for (std::size_t k = 0; k < names.size(); ++k) {
        if (!chosen[k]) {
            while (next < tracks.size() &&
                   (taken[next] || tracks[next]->locked || hasName(*tracks[next]) || !tracks[next]->clips.empty())) {
                ++next;
            }
            if (next < tracks.size()) {
                patchTo(k, next, true);
                ++next;
            } else if (!names[k].name.empty()) {
                SourcePatch p;
                p.source = {StreamKind::Audio, static_cast<std::int32_t>(k)};
                p.newTrack = names[k].name;
                chosen[k] = std::move(p);
            }
        }
        if (chosen[k]) {
            out.push_back(std::move(*chosen[k]));
        }
    }
    return out;
}

// --- Source edits -------------------------------------------------------------------------------------

std::vector<SourcePatch> defaultSourcePatches(const Sequence& sequence, const MediaItem& media) {
    std::vector<SourcePatch> out;
    if (media.info.hasVideo() && !sequence.videoTracks.empty()) {
        out.push_back({{StreamKind::Video, 0}, sequence.videoTracks.front()->id});
    }
    if (routesAudioByName(media.info)) {
        // A.10: a multi-track recording's streams go to the tracks named for them.
        for (SourcePatch& p : audioPatchesByName(sequence, media.info, 0)) {
            out.push_back(std::move(p));
        }
        return out;
    }
    const std::size_t audio = std::min(media.info.audio.size(), sequence.audioTracks.size());
    for (std::size_t i = 0; i < audio; ++i) {
        out.push_back({{StreamKind::Audio, static_cast<std::int32_t>(i)}, sequence.audioTracks[i]->id});
    }
    return out;
}

std::vector<SourcePatch> defaultSourcePatches(const Sequence& sequence, const Sequence& source) {
    std::vector<SourcePatch> out;
    if (sequenceHasStream(source, {StreamKind::Video, 0}) && !sequence.videoTracks.empty()) {
        out.push_back({{StreamKind::Video, 0}, sequence.videoTracks.front()->id});
    }
    if (sequenceHasStream(source, {StreamKind::Audio, 0}) && !sequence.audioTracks.empty()) {
        out.push_back({{StreamKind::Audio, 0}, sequence.audioTracks.front()->id});
    }
    return out;
}

std::expected<ThreePointEdit, Error> resolveThreePointEdit(const ProjectState& state, SequenceId sequenceId,
                                                           MediaId mediaId, std::span<const SourcePatch> patches,
                                                           SourceMarks marks, Ticks playhead) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const MediaItem* media = state.findMedia(mediaId);
    if (!media) {
        return std::unexpected(notFound("media " + std::to_string(mediaId.value) + " does not exist"));
    }
    if (patches.empty()) {
        return std::unexpected(invalidArgument("no source tracks are patched"));
    }
    // What the patched streams may use (MediaItem::sourceBounds: a restricted subclip's range).
    std::optional<Ticks> extent;
    Ticks start{};
    for (const SourcePatch& p : patches) {
        if (!media->hasStream(p.source)) {
            return std::unexpected(invalidArgument("the source has no such stream"));
        }
        const SourceBounds bounds = media->sourceBounds(p.source);
        start = std::max(start, bounds.start);
        if (bounds.end) {
            extent = extent ? std::min(*extent, *bounds.end) : *bounds.end;
        }
    }
    // Unmarked, a subclip (restricted or not) offers its range; other media all of it.
    Ticks defaultIn = start;
    std::optional<Ticks> defaultOut = extent;
    if (media->subclip) {
        defaultIn = std::max(start, media->subclip->range.start);
        defaultOut = extent ? std::min(*extent, media->subclip->range.end()) : media->subclip->range.end();
    }

    const Ticks sourceIn = marks.in.value_or(defaultIn);
    Ticks sourceOut{};
    if (marks.out) {
        sourceOut = *marks.out;
    } else if (defaultOut) {
        sourceOut = *defaultOut;
    } else {
        sourceOut = sourceIn + kDefaultStillDuration;
    }
    if (sourceIn < start || sourceOut <= sourceIn || (extent && sourceOut > *extent)) {
        return std::unexpected(invalidArgument("the source In/Out marks are outside the media"));
    }

    const Sequence& s = **seq;
    ThreePointEdit edit;
    if (s.markIn && s.markOut) {
        const Ticks duration = *s.markOut - *s.markIn;
        edit.at = *s.markIn;
        edit.sourceRange = (marks.out && !marks.in) ? TimeRange{sourceOut - duration, duration}
                                                    : TimeRange{sourceIn, duration};
        if (edit.sourceRange.start < start || (extent && edit.sourceRange.end() > *extent)) {
            return std::unexpected(invalidArgument("the source is too short for the marked sequence range"));
        }
    } else if (s.markIn) {
        edit.at = *s.markIn;
        edit.sourceRange = TimeRange::fromStartEnd(sourceIn, sourceOut);
    } else if (s.markOut) {
        // Back-timed: the edit ends at the sequence Out; drop source frames that would land before 0.
        edit.sourceRange = TimeRange::fromStartEnd(sourceIn, sourceOut);
        edit.at = *s.markOut - edit.sourceRange.duration;
        if (edit.at.value < 0) {
            edit.sourceRange = TimeRange::fromStartEnd(sourceIn - edit.at, sourceOut);
            edit.at = Ticks{};
        }
    } else {
        edit.at = playhead;
        edit.sourceRange = TimeRange::fromStartEnd(sourceIn, sourceOut);
    }
    if (edit.at.value < 0) {
        return std::unexpected(invalidArgument("the edit would start before 0"));
    }
    return edit;
}

namespace {

EditOutcome sourceEdit(const ProjectState& state, SequenceId sequenceId, const SourceClip& source, Ticks at,
                       bool insert) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    // The source: media, or a sequence (C.8: nested and multicam clips).
    const MediaItem* media = nullptr;
    const Sequence* nested = nullptr;
    if (source.sequence.isValid()) {
        nested = state.findSequence(source.sequence);
        if (!nested) {
            return std::unexpected(notFound("sequence " + std::to_string(source.sequence.value) + " does not exist"));
        }
        if (sequenceContains(state, source.sequence, sequenceId)) {
            return std::unexpected(invalidArgument("sequence '" + nested->name + "' cannot go into '" + (*seq)->name +
                                                   "': a sequence never contains itself"));
        }
    } else {
        media = state.findMedia(source.media);
        if (!media) {
            return std::unexpected(notFound("media " + std::to_string(source.media.value) + " does not exist"));
        }
    }
    const auto hasStream = [&](StreamRef stream) {
        return nested ? sequenceHasStream(*nested, stream) : media->hasStream(stream);
    };
    // What edits may use of a stream: a nested sequence's length, or the media (a restricted subclip's range, M5k).
    const auto boundsOf = [&](StreamRef stream) -> SourceBounds {
        return nested ? SourceBounds{Ticks{}, nested->duration} : media->sourceBounds(stream);
    };
    if (at.value < 0) {
        return std::unexpected(invalidArgument("cannot edit before the start of the sequence"));
    }
    if (source.sourceRange.isEmpty() || source.sourceRange.start.value < 0) {
        return std::unexpected(invalidArgument("the source range is empty"));
    }
    if (source.patches.empty()) {
        return std::unexpected(invalidArgument("no source tracks are patched"));
    }

    std::vector<SourcePatch> active;
    std::unordered_set<TrackId> destinations;
    for (const SourcePatch& patch : source.patches) {
        if (!hasStream(patch.source)) {
            return std::unexpected(invalidArgument("the source has no such stream"));
        }
        if (!patch.newTrack.empty() && patch.source.kind != StreamKind::Audio) {
            return std::unexpected(invalidArgument("only an audio stream can name or add a track"));
        }
        // A.10: no destination and a track name: the stream goes to a new track of that name.
        const bool toNewTrack = !patch.destination && !patch.newTrack.empty();
        const Track* track = toNewTrack ? nullptr : (*seq)->findTrack(patch.destination);
        if (!toNewTrack) {
            if (!track) {
                return std::unexpected(notFound("destination track does not exist"));
            }
            if (streamKindOf(track->kind) != patch.source.kind) {
                return std::unexpected(invalidArgument("a source stream is patched to a track of the other kind"));
            }
            if (!destinations.insert(patch.destination).second) {
                return std::unexpected(invalidArgument("two source streams are patched to the same track"));
            }
        }
        const SourceBounds bounds = boundsOf(patch.source);
        if (bounds.end && source.sourceRange.end() > *bounds.end) {
            return std::unexpected(invalidArgument("the source range runs past the end of the media"));
        }
        if (source.sourceRange.start < bounds.start) {
            return std::unexpected(invalidArgument("the source range starts before the subclip"));
        }
        if (toNewTrack || !track->locked) {
            active.push_back(patch);
        }
    }
    if (active.empty()) {
        return std::unexpected(invalidArgument("every destination track is locked"));
    }

    SequenceEditor ed(state, **seq);
    // A.10: the tracks the patches name. A new track below the last audio track for each patch without a destination;
    // a destination without a name is given it. Either takes the role the name implies when it has none.
    for (SourcePatch& p : active) {
        if (p.newTrack.empty()) {
            continue;
        }
        const TrackRole role = sourceTrackName(p.newTrack).role;
        if (!p.destination) {
            p.destination = ed.addTrack(TrackKind::Audio, p.newTrack, role);
            destinations.insert(p.destination);
        } else if (!hasName(ed.track(p.destination))) {
            Track& renamed = ed.mutableTrack(p.destination);
            renamed.name = p.newTrack;
            if (renamed.role == TrackRole::None) {
                renamed.role = role;
            }
        }
    }
    const Ticks duration = source.sourceRange.duration; // 1/1 speed: timeline length == source length
    if (insert) {
        for (const TrackId id : ed.trackIds()) {
            const Track& t = ed.track(id);
            if (!t.locked && (destinations.contains(id) || t.syncLocked)) {
                ed.rippleInsert(id, at, duration);
            }
        }
        ed.captionsInsert(at, duration);
    } else {
        for (const SourcePatch& p : active) {
            ed.clearRange(p.destination, {at, duration});
        }
    }

    const LinkGroupId group = active.size() > 1 ? ed.newLinkGroup() : LinkGroupId{};
    std::shared_ptr<const ClipContent> content;
    if (nested) {
        ClipContent named;
        named.name = nested->name;
        content = std::make_shared<const ClipContent>(std::move(named));
    } else {
        content = media->defaultClipContent();
    }
    // A multicam clip's angles: the ones asked for, else the first angle with that kind of track.
    const auto firstAngle = [&](StreamKind kind) {
        for (std::size_t i = 0; nested && nested->multicam && i < nested->multicam->angles.size(); ++i) {
            const MulticamAngle& a = nested->multicam->angles[i];
            if (kind == StreamKind::Video ? a.video.isValid() : !a.audio.empty()) {
                return AngleSet::of(i);
            }
        }
        return AngleSet{};
    };
    for (const SourcePatch& p : active) {
        Clip clip;
        clip.id = ed.newClipId();
        clip.stream = p.source;
        if (nested) {
            clip.sequence = nested->id;
            if (nested->multicam) {
                const bool video = p.source.kind == StreamKind::Video;
                const AngleSet asked = video ? source.videoAngle : source.audioAngles;
                clip.angles = asked.empty() ? firstAngle(p.source.kind) : asked;
            }
        } else {
            clip.media = media->id;
        }
        clip.timelineRange = {at, duration};
        clip.sourceIn = source.sourceRange.start;
        clip.setContent(content);
        clip.linkGroup = group;
        if (media) {
            clip.colorIndex = media->colorIndex; // the item's label (M5k)
        }
        if (const auto problem = clipAnglesProblem(state, clip)) {
            return std::unexpected(invalidArgument("the clip " + *problem));
        }
        ed.placeNewClip(p.destination, std::move(clip), true);
    }
    return ed.finish(at + duration);
}

} // namespace

EditOutcome insertEdit(const ProjectState& state, SequenceId sequence, const SourceClip& source, Ticks at) {
    return sourceEdit(state, sequence, source, at, true);
}

EditOutcome overwriteEdit(const ProjectState& state, SequenceId sequence, const SourceClip& source, Ticks at) {
    return sourceEdit(state, sequence, source, at, false);
}

EditOutcome placeClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipPlacement> clips) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (clips.empty()) {
        return detail::unchanged(state);
    }
    // Everything is checked before anything changes.
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> perTrack;
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const ClipPlacement& c = clips[i];
        const Track* track = (*seq)->findTrack(c.track);
        if (!track) {
            return std::unexpected(notFound("track " + std::to_string(c.track.value) + " is not in the sequence"));
        }
        if (track->locked) {
            return std::unexpected(invalidArgument("track " + detail::trackLabel(**seq, c.track) + " is locked"));
        }
        if (streamKindOf(track->kind) != c.stream.kind) {
            return std::unexpected(invalidArgument("a stream is placed on a track of the other kind"));
        }
        const MediaItem* media = state.findMedia(c.media);
        if (!media) {
            return std::unexpected(notFound("media " + std::to_string(c.media.value) + " does not exist"));
        }
        if (!media->hasStream(c.stream)) {
            return std::unexpected(invalidArgument("media '" + media->name + "' has no such stream"));
        }
        if (c.duration.value <= 0 || c.start.value < 0 || c.sourceIn.value < 0 || !checkedAdd(c.start, c.duration) ||
            !checkedAdd(c.sourceIn, c.duration)) {
            return std::unexpected(invalidArgument("a clip has an empty or out-of-range position"));
        }
        const SourceBounds bounds = media->sourceBounds(c.stream);
        if ((bounds.end && c.sourceIn + c.duration > *bounds.end) || c.sourceIn < bounds.start) {
            return std::unexpected(invalidArgument("the source range of a clip of '" + media->name + "' lies outside the media"));
        }
        perTrack[c.track.value].push_back(i);
    }
    for (auto& [trackId, indices] : perTrack) {
        const Track& track = *(*seq)->findTrack(TrackId{trackId});
        std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) { return clips[a].start < clips[b].start; });
        for (std::size_t k = 0; k < indices.size(); ++k) {
            const ClipPlacement& c = clips[indices[k]];
            const Ticks end = c.start + c.duration;
            if (k + 1 < indices.size() && clips[indices[k + 1]].start < end) {
                return std::unexpected(invalidArgument("two placed clips overlap on " + detail::trackLabel(**seq, track.id)));
            }
            // An existing clip under it: the one before its start reaching past it, or one starting inside it.
            const std::size_t at = track.clips.lowerBoundStart(c.start);
            if ((at > 0 && track.clips[at - 1].end() > c.start) || (at < track.clips.size() && track.clips[at].start() < end)) {
                return std::unexpected(invalidArgument("a placed clip overlaps a clip on " + detail::trackLabel(**seq, track.id)));
            }
        }
    }
    SequenceEditor ed(state, **seq);
    for (const ClipPlacement& c : clips) {
        const MediaItem* media = state.findMedia(c.media);
        Clip clip;
        clip.id = ed.newClipId();
        clip.media = c.media;
        clip.stream = c.stream;
        clip.timelineRange = {c.start, c.duration};
        clip.sourceIn = c.sourceIn;
        clip.setContent(media->defaultClipContent());
        clip.colorIndex = media->colorIndex;
        ed.placeNewClip(c.track, std::move(clip), true);
    }
    return ed.finish();
}

// --- Lift / extract -----------------------------------------------------------------------------------

namespace {

EditOutcome removeMarkedRange(const ProjectState& state, SequenceId sequenceId, bool ripple) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto range = (*seq)->markedRange();
    if (!range) {
        return std::unexpected(invalidArgument("set an In and an Out mark first"));
    }
    const std::vector<TrackId> targets = detail::unlockedTargets(**seq);
    if (targets.empty()) {
        return std::unexpected(invalidArgument("no unlocked track is targeted"));
    }
    SequenceEditor ed(state, **seq);
    if (ripple) {
        for (const TrackId id : ed.trackIds()) {
            const Track& t = ed.track(id);
            if (!t.locked && (t.targeted || t.syncLocked)) {
                ed.rippleRemove(id, *range);
            }
        }
        ed.captionsRemove(*range);
    } else {
        for (const TrackId id : targets) {
            ed.clearRange(id, *range);
        }
    }
    ed.header().markIn.reset();
    ed.header().markOut.reset();
    return ed.finish(range->start);
}

} // namespace

EditOutcome lift(const ProjectState& state, SequenceId sequence) { return removeMarkedRange(state, sequence, false); }

EditOutcome extract(const ProjectState& state, SequenceId sequence) {
    return removeMarkedRange(state, sequence, true);
}

// --- Add edit -----------------------------------------------------------------------------------------

EditOutcome addEdit(const ProjectState& state, SequenceId sequenceId, Ticks at, std::span<const TrackId> tracks,
                    LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    const auto strictlyInside = [&](const Track& t) -> const Clip* {
        const auto index = t.clipIndexAt(at);
        return index && t.clips[*index].start() < at ? &t.clips[*index] : nullptr;
    };

    std::vector<TrackId> cut;
    std::unordered_set<LinkGroupId> groups;
    for (const TrackId id : tracks) {
        const Track* t = s.findTrack(id);
        if (!t) {
            return std::unexpected(notFound("track " + std::to_string(id.value) + " does not exist"));
        }
        if (t->locked || std::find(cut.begin(), cut.end(), id) != cut.end()) {
            continue;
        }
        cut.push_back(id);
        if (const Clip* c = strictlyInside(*t); c && c->linkGroup.isValid()) {
            groups.insert(c->linkGroup);
        }
    }
    if (links == LinkMode::FollowLinks && !groups.empty()) {
        s.forEachTrack([&](const Track& t) {
            if (t.locked || std::find(cut.begin(), cut.end(), t.id) != cut.end()) {
                return;
            }
            if (const Clip* c = strictlyInside(t); c && c->linkGroup.isValid() && groups.contains(c->linkGroup)) {
                cut.push_back(t.id);
            }
        });
    }

    SequenceEditor ed(state, s);
    for (const TrackId id : cut) {
        ed.splitAt(id, at);
    }
    return ed.finish();
}

EditOutcome addEditToAllTracks(const ProjectState& state, SequenceId sequence, Ticks at) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const std::vector<TrackId> tracks = allTracks(**seq);
    return addEdit(state, sequence, at, tracks, LinkMode::IgnoreLinks);
}

// --- Delete -------------------------------------------------------------------------------------------

EditOutcome deleteClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips,
                        LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto selection = detail::resolveClips(**seq, clips, links);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    SequenceEditor ed(state, **seq);
    ed.removeClips(*selection);
    return ed.finish();
}

EditOutcome rippleDeleteClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips,
                              LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    const auto selection = detail::resolveClips(s, clips, links);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    if (selection->empty()) {
        return detail::unchanged(state);
    }

    // The removed time: union of the selected clips' ranges.
    std::vector<TimeRange> ranges;
    std::unordered_set<ClipId> selected;
    std::unordered_set<TrackId> edited;
    for (const ClipRef& ref : *selection) {
        ranges.push_back(s.findTrack(ref.track)->clips[ref.index].timelineRange);
        selected.insert(ref.clip);
        edited.insert(ref.track);
    }
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> removed;
    for (const TimeRange& r : ranges) {
        if (!removed.empty() && r.start <= removed.back().end()) {
            removed.back() = TimeRange::fromStartEnd(removed.back().start, std::max(removed.back().end(), r.end()));
        } else {
            removed.push_back(r);
        }
    }

    // Closing that time must not take unselected clips off a track the selection is on.
    for (const TrackId id : edited) {
        const Track& t = *s.findTrack(id);
        for (const TimeRange& r : removed) {
            for (std::size_t i = t.clipIndexAt(r.start).value_or(t.firstClipAtOrAfter(r.start));
                 i < t.clips.size() && t.clips[i].start() < r.end(); ++i) {
                if (!selected.contains(t.clips[i].id)) {
                    return std::unexpected(invalidArgument(
                        "ripple delete would also remove unselected clip '" + t.clips[i].name() + "' on " +
                        detail::trackLabel(s, id)));
                }
            }
        }
    }

    SequenceEditor ed(state, s);
    ed.removeClips(*selection);
    const std::vector<TrackId> ids = ed.trackIds();
    for (auto r = removed.rbegin(); r != removed.rend(); ++r) {
        for (const TrackId id : ids) {
            const Track& t = ed.track(id);
            if (!t.locked && (edited.contains(id) || t.syncLocked)) {
                ed.rippleRemove(id, *r);
            }
        }
        ed.captionsRemove(*r);
    }
    return ed.finish();
}

EditOutcome rippleDeleteGap(const ProjectState& state, SequenceId sequenceId, TrackId trackId, Ticks at) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    const Track* track = s.findTrack(trackId);
    if (!track) {
        return std::unexpected(notFound("track " + std::to_string(trackId.value) + " does not exist"));
    }
    if (track->locked) {
        return std::unexpected(invalidArgument("track " + detail::trackLabel(s, trackId) + " is locked"));
    }
    if (at.value < 0 || track->clipIndexAt(at)) {
        return std::unexpected(invalidArgument("there is no gap at this position"));
    }
    const std::size_t next = track->firstClipAtOrAfter(at);
    if (next == track->clips.size()) {
        return std::unexpected(invalidArgument("there is no gap at this position: no clip follows it"));
    }
    const Ticks gapStart = next > 0 ? track->clips[next - 1].end() : Ticks{};
    const TimeRange gap = TimeRange::fromStartEnd(gapStart, track->clips[next].start());

    // Premiere only closes a gap when every other sync-locked track is empty there too.
    for (const auto* list : {&s.videoTracks, &s.audioTracks}) {
        for (const TrackPtr& other : *list) {
            if (other->id == trackId || other->locked || !other->syncLocked) {
                continue;
            }
            const std::size_t i = other->clipIndexAt(gap.start).value_or(other->firstClipAtOrAfter(gap.start));
            if (i < other->clips.size() && other->clips[i].timelineRange.overlaps(gap)) {
                return std::unexpected(invalidArgument("cannot close the gap: sync-locked track " +
                                                       detail::trackLabel(s, other->id) +
                                                       " has clips in that time"));
            }
        }
    }

    SequenceEditor ed(state, s);
    for (const TrackId id : ed.trackIds()) {
        const Track& t = ed.track(id);
        if (!t.locked && (id == trackId || t.syncLocked)) {
            ed.rippleRemove(id, gap);
        }
    }
    ed.captionsRemove(gap);
    return ed.finish();
}

// --- Marks and markers --------------------------------------------------------------------------------

EditOutcome markIn(const ProjectState& state, SequenceId sequenceId, Ticks playhead) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (playhead.value < 0) {
        return std::unexpected(invalidArgument("cannot mark before 0"));
    }
    SequenceEditor ed(state, **seq);
    Sequence& h = ed.header();
    h.markIn = snapToFrame(playhead, h.frameRate);
    if (h.markOut && *h.markOut <= *h.markIn) {
        h.markOut.reset();
    }
    return ed.finish();
}

EditOutcome markOut(const ProjectState& state, SequenceId sequenceId, Ticks playhead) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (playhead.value < 0) {
        return std::unexpected(invalidArgument("cannot mark before 0"));
    }
    SequenceEditor ed(state, **seq);
    Sequence& h = ed.header();
    h.markOut = snapToFrame(playhead, h.frameRate) + ed.oneFrame();
    if (h.markIn && *h.markIn >= *h.markOut) {
        h.markIn.reset();
    }
    return ed.finish();
}

namespace {

EditOutcome setMarks(const ProjectState& state, const Sequence& seq, Ticks in, Ticks out) {
    SequenceEditor ed(state, seq);
    ed.header().markIn = in;
    ed.header().markOut = out;
    return ed.finish();
}

} // namespace

EditOutcome markClip(const ProjectState& state, SequenceId sequenceId, Ticks playhead) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Sequence& s = **seq;
    const bool anyTargeted = !targetedTracks(s).empty();
    std::optional<TimeRange> extent;
    s.forEachTrack([&](const Track& t) {
        if (anyTargeted && !t.targeted) {
            return;
        }
        if (const auto i = t.clipIndexAt(playhead)) {
            const TimeRange r = t.clips[*i].timelineRange;
            extent = extent ? TimeRange::fromStartEnd(std::min(extent->start, r.start), std::max(extent->end(), r.end()))
                            : r;
        }
    });
    if (!extent) {
        return std::unexpected(invalidArgument("there is no clip under the playhead on the targeted tracks"));
    }
    return setMarks(state, s, extent->start, extent->end());
}

EditOutcome markSelection(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (clips.empty()) {
        return std::unexpected(invalidArgument("nothing is selected"));
    }
    std::optional<TimeRange> extent;
    for (const ClipId id : clips) {
        const auto location = (*seq)->findClip(id);
        if (!location) {
            return std::unexpected(notFound("clip " + std::to_string(id.value) + " is not in this sequence"));
        }
        const TimeRange r = location->clip().timelineRange;
        extent = extent ? TimeRange::fromStartEnd(std::min(extent->start, r.start), std::max(extent->end(), r.end()))
                        : r;
    }
    return setMarks(state, **seq, extent->start, extent->end());
}

EditOutcome clearMarks(const ProjectState& state, SequenceId sequenceId, bool clearIn, bool clearOut) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if ((!clearIn || !(*seq)->markIn) && (!clearOut || !(*seq)->markOut)) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    if (clearIn) {
        ed.header().markIn.reset();
    }
    if (clearOut) {
        ed.header().markOut.reset();
    }
    return ed.finish();
}

EditOutcome addMarker(const ProjectState& state, SequenceId sequenceId, Marker marker) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (const auto problem = markerProblem(marker)) {
        return std::unexpected(invalidArgument(*problem));
    }
    if ((*seq)->markers.size() >= kMaxMarkers) {
        return std::unexpected(invalidArgument("a sequence holds at most " + std::to_string(kMaxMarkers) + " markers"));
    }
    SequenceEditor ed(state, **seq);
    auto& markers = ed.header().markers;
    const auto it = std::upper_bound(markers.begin(), markers.end(), marker.start,
                                     [](Ticks t, const Marker& m) { return t < m.start; });
    markers.insert(it, std::move(marker));
    return ed.finish();
}

EditOutcome removeMarker(const ProjectState& state, SequenceId sequenceId, std::size_t index) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (index >= (*seq)->markers.size()) {
        return std::unexpected(notFound("there is no marker " + std::to_string(index)));
    }
    SequenceEditor ed(state, **seq);
    auto& markers = ed.header().markers;
    markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(index));
    return ed.finish();
}

} // namespace wabcut::edit
