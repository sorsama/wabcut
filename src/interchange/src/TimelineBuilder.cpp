#include "TimelineBuilder.h"

#include "TextUtil.h"
#include "TimeMath.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/TitleOps.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace wabcut::interchange::detail {

namespace {

std::string trackLabel(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
}

std::string_view transitionNoun(TransitionKind kind) {
    switch (kind) {
    case TransitionKind::Dissolve:
        return "dissolve";
    case TransitionKind::DipToBlack:
        return "dip to black";
    case TransitionKind::DipToWhite:
        return "dip to white";
    case TransitionKind::Wipe:
        return "wipe";
    case TransitionKind::AudioCrossfade:
        return "audio crossfade";
    case TransitionKind::Other:
        return "transition";
    }
    return "transition";
}

std::string inQuotes(std::string_view text) {
    return "'" + std::string(text) + "'";
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// The model transition for an imported one, and what was approximated (empty: exact).
struct MappedTransition {
    std::string effect;
    std::string approximation;
};

MappedTransition mapTransition(TrackKind kind, const IrTransition& t) {
    const std::string lower = toLowerAscii(t.name);
    if (kind == TrackKind::Audio) {
        // FCP7 "Cross Fade (0dB)" / Premiere "Constant Gain" fade linearly; "Cross Fade (+3dB)", "Constant
        // Power" and plain crossfades/dissolves are equal-power.
        if (contains(lower, "0db") || contains(lower, "0 db") || contains(lower, "gain")) {
            return {"constantGain", {}};
        }
        if (contains(lower, "exponential")) {
            return {"exponentialFade", {}}; // 0.2
        }
        return {"constantPower", {}};
    }
    // 0.2: a transition of Premiere's library by its name ("Iris Round", "Additive Dissolve (Legacy)"), with its
    // default options; a plain "Wipe" keeps its note below (its direction is not read).
    std::string name = lower;
    if (const auto legacy = name.find(" (legacy)"); legacy != std::string::npos) {
        name.erase(legacy);
    }
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (d.kind == EffectKind::VideoTransition && d.id != "wipe" && toLowerAscii(std::string(d.displayName)) == name) {
            return {std::string(d.id), {}};
        }
    }
    switch (t.kind) {
    case TransitionKind::Dissolve:
        if (contains(lower, "additive") || contains(lower, "film") || contains(lower, "morph") ||
            contains(lower, "dither")) {
            return {"crossDissolve", "it became a cross dissolve"};
        }
        return {"crossDissolve", {}};
    case TransitionKind::DipToBlack:
        return {"dipToBlack", contains(lower, "colo") ? "the dip to colour became a dip to black" : std::string()};
    case TransitionKind::DipToWhite:
        return {"dipToWhite", {}};
    case TransitionKind::Wipe:
        return {"wipe", "its pattern and direction became a straight left-to-right wipe"};
    case TransitionKind::AudioCrossfade:
    case TransitionKind::Other:
        break;
    }
    return {"crossDissolve", "it became a cross dissolve"};
}

/// How much of each media item the clips use, so the media item gets the streams and extent they need.
struct MediaUsage {
    bool video = false;
    std::int32_t audioStreams = 0;
    Ticks maxSourceOut;
    std::size_t clips = 0;
};

class Builder {
public:
    Builder(IrDocument& document, const BuildContext& context)
        : m_doc(document), m_ctx(context), m_relinker(relinkOptions(context)) {}

    std::expected<ImportResult, Error> run();

private:
    static RelinkOptions relinkOptions(const BuildContext& context) {
        RelinkOptions options;
        options.projectDirectory = context.baseDirectory;
        if (context.options) {
            options.searchRoots = context.options->mediaSearchRoots;
            options.inspector = context.options->mediaInspector;
            options.fileExists = context.options->fileExists;
        }
        return options;
    }

    void report(ReportOutcome outcome, ReportCategory category, std::string reason,
                const SourceLocation& location, const std::string& sequence = {}) {
        m_doc.report.add(outcome, category, std::move(reason), location, sequence);
    }

    SourceLocation fileLocation() const { return {m_ctx.fileLabel, {}, 0}; }

    std::vector<MediaUsage> scanUsage() const;
    std::expected<void, Error> addMediaItems();
    Ticks sourceOffsetFor(const IrMedia& media, const std::string& name, const RelinkResult& link);
    MediaItem makeMediaItem(const IrMedia& media, const MediaUsage& usage, RelinkResult& link);
    std::expected<void, Error> buildSequence(const IrSequence& ir);
    std::optional<std::size_t> nestedIndex(const IrClip& clip) const;
    std::vector<std::size_t> buildOrder();
    void applyMulticam(const IrSequence& ir);
    bool placeNested(const IrClip& c, TrackKind kind, TrackId trackId, const std::string& label, const std::string& name);

    // Per-sequence state.
    struct Placed {
        ClipId id;
        const IrClip* ir = nullptr;
    };
    /// A media clip waiting for the track's one placeClips edit (X.1).
    struct Pending {
        const IrClip* ir = nullptr;
        edit::ClipPlacement placement;
        std::string name;
    };
    std::expected<void, Error> flushPending();
    /// The bin for a source's bin path under the import bin (X.1), made on first use; the import bin on failure.
    BinId binFor(const std::vector<std::string>& path, const SourceLocation& where);
    std::map<std::vector<std::string>, BinId> m_bins;

    std::expected<void, Error> applyTrackSettings(const IrSequence& ir);
    std::expected<void, Error> placeClips(const IrTrack& track, TrackId trackId);
    std::optional<ClipId> placeTitle(const IrClip& clip, TrackId destination, Ticks start, Ticks duration);
    void applySpeed(const IrClip& clip, ClipId id, const std::string& name, const std::string& label);
    void applyRemap(const IrClip& clip, ClipId id, const std::string& name, const std::string& label);
    std::expected<TrackId, Error> overflowTrack(TrackKind kind, TimeRange range, const std::string& from);
    std::expected<void, Error> applyClipAttributes();
    std::expected<void, Error> applyLinks();
    void importTransition(const IrTrack& track, std::size_t trackIndex, TrackId trackId,
                          const IrTransition& transition);
    void reportGaps(const IrTrack& track, TrackId trackId, std::size_t trackIndex);
    std::expected<void, Error> applyMarkers(const IrSequence& ir);
    std::expected<void, Error> applyMarks(const IrSequence& ir);
    void applySequenceDuration(const IrSequence& ir);
    std::expected<void, Error> applyLocks(const IrSequence& ir);

    const Sequence& sequence() const { return *m_state.findSequence(m_seq); }
    bool fits(TrackId trackId, TimeRange range) const;
    std::string labelOf(TrackId trackId) const;
    std::string at(Ticks t) const { return timecodeLabel(t, m_rate, m_dropFrame); }
    std::string length(Ticks d) const { return framesLabel(d, m_rate); }

    IrDocument& m_doc;
    const BuildContext& m_ctx;
    MediaRelinker m_relinker;
    ProjectState m_state;
    ImportResult m_result;
    std::vector<MediaId> m_mediaIds;
    /// Per media: subtracted from clips' source In (the start timecode of absolute-timecode media).
    std::vector<Ticks> m_sourceOffset;

    /// C.8: the built sequence of each IrDocument::sequences index (invalid until built), and sequences by key.
    std::vector<SequenceId> m_sequenceIds;
    std::unordered_map<std::string, std::size_t> m_sequenceKeys;
    SequenceId m_seq;
    std::string m_seqName;
    Rate m_rate = kRate30;
    bool m_dropFrame = false;
    std::vector<TrackId> m_videoIds;
    std::vector<TrackId> m_audioIds;
    std::vector<TrackId> m_overflow;
    std::vector<Placed> m_placed;
    std::vector<Pending> m_pending;
    std::unordered_map<std::uint64_t, Ticks> m_pendingEnd; ///< per destination track: where its pending clips end
};

std::vector<MediaUsage> Builder::scanUsage() const {
    std::vector<MediaUsage> usage(m_doc.media.size());
    for (const IrSequence& s : m_doc.sequences) {
        for (const auto* list : {&s.videoTracks, &s.audioTracks}) {
            for (const IrTrack& t : *list) {
                for (const IrClip& c : t.clips) {
                    if (c.media >= usage.size()) {
                        continue;
                    }
                    MediaUsage& u = usage[c.media];
                    ++u.clips;
                    if (c.stream.kind == StreamKind::Video) {
                        u.video = true;
                    } else if (c.stream.index >= 0) {
                        u.audioStreams = std::max(u.audioStreams, c.stream.index + 1);
                    }
                    // A retimed clip is placed at 100% first, then retimed: it needs both extents.
                    Ticks used = c.duration;
                    if (c.speed && c.speed->ratio.num > 0) {
                        used = std::max(used, toSourceTicks(c.duration, c.speed->ratio));
                    }
                    if (c.remap) {
                        // Every position of the remap must be media (its last frame included).
                        for (const Keyframe& k : c.remap->curve.keys) {
                            const Ticks position{static_cast<std::int64_t>(k.value)};
                            used = std::max(used, position - std::max(c.sourceIn, Ticks{}));
                        }
                    }
                    if (const auto out = checkedAdd(std::max<std::int64_t>(c.sourceIn.value, 0), used.value)) {
                        u.maxSourceOut = std::max(u.maxSourceOut, Ticks{*out});
                    }
                }
            }
        }
    }
    return usage;
}

BinId Builder::binFor(const std::vector<std::string>& path, const SourceLocation& where) {
    BinId parent = m_result.bin;
    std::vector<std::string> prefix;
    for (const std::string& raw : path) {
        if (prefix.size() >= 16) {
            break; // deeper folders flatten into the 16th
        }
        std::string name = raw.substr(0, 256);
        if (name.empty() || !edit::isValidUtf8(name)) {
            name = "Bin";
        }
        prefix.push_back(name);
        if (const auto it = m_bins.find(prefix); it != m_bins.end()) {
            parent = it->second;
            continue;
        }
        auto added = edit::addBin(m_state, name, parent);
        if (!added) {
            report(ReportOutcome::Approximated, ReportCategory::Media,
                   "bin " + inQuotes(name) + " could not be made (" + added.error().message + "); its items go one level up",
                   where);
            return parent;
        }
        m_state = std::move(added->state);
        m_bins.emplace(prefix, added->id);
        parent = added->id;
        std::string label;
        for (const std::string& part : prefix) {
            label += (label.empty() ? "" : "/") + part;
        }
        report(ReportOutcome::Carried, ReportCategory::Media, "bin " + inQuotes(label), where);
    }
    return parent;
}

MediaItem Builder::makeMediaItem(const IrMedia& m, const MediaUsage& usage, RelinkResult& link) {
    MediaItem item;
    item.bin = binFor(m.bin, m.location);
    item.name = m.name;
    // X.1: the source's markers (Premiere's clip markers), in media time.
    for (const IrMarker& k : m.markers) {
        const std::string label = k.marker.name.empty() ? std::string("marker") : inQuotes(k.marker.name);
        if (const auto problem = markerProblem(k.marker); problem || k.marker.start.value < 0) {
            report(ReportOutcome::Dropped, ReportCategory::Marker,
                   "clip marker " + label + " of " + inQuotes(m.name) + " is not valid: " + problem.value_or("it starts before the media"),
                   k.location);
            continue;
        }
        item.markers.push_back(k.marker);
        report(ReportOutcome::Carried, ReportCategory::Marker,
               "clip marker " + label + " of " + inQuotes(m.name) + " (on its source: it moves with its clips)", k.location);
    }
    std::stable_sort(item.markers.begin(), item.markers.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
    if (item.name.empty()) {
        item.name = std::string(fileNameOf(m.reference.path));
    }
    if (item.name.empty()) {
        item.name = "Media " + std::to_string(m_mediaIds.size() + 1);
    }
    link = m_relinker.resolve(m.reference);
    // The locator keeps the absolute path (the file found, or the original one) and the path relative to
    // the project file, so a project moved together with its media relinks itself. The fingerprint stays
    // unknown: computing it would read every media file; the media probe fills it in.
    MediaLocator& locator = item.locator;
    if (link.found()) {
        locator.path = utf8FromPath(link.resolved);
    } else if (!m.reference.path.empty() && !isAbsoluteOnAnyPlatform(m.reference.path) &&
               !m_ctx.baseDirectory.empty()) {
        locator.path =
            utf8FromPath((m_ctx.baseDirectory / pathFromUtf8(m.reference.path)).lexically_normal());
    } else {
        locator.path = m.reference.path;
    }
    if (!m_ctx.projectFile.empty() && !locator.path.empty()) {
        if (const std::filesystem::path absolute = pathFromUtf8(locator.path); absolute.is_absolute()) {
            locator.relativePath = relativeMediaPath(m_ctx.projectFile, absolute);
        }
    }
    locator.offline = !link.found();

    MediaInfo& info = item.info;
    info.path = locator.path;
    info.isStillImage = m.stillImage;
    info.fileSize = m.reference.fileSize.value_or(0);
    Ticks duration{};
    if (m.duration && m.duration->value > 0 && !m.stillImage) {
        if (usage.maxSourceOut <= *m.duration) {
            duration = *m.duration;
        } else {
            report(ReportOutcome::Approximated, ReportCategory::Media,
                   inQuotes(item.name) + " declares a duration of " +
                       std::to_string(m.duration->toSeconds()) + " s but clips use it up to " +
                       std::to_string(usage.maxSourceOut.toSeconds()) +
                       " s; its length is left for the media probe to fill in",
                   m.location);
        }
    }
    info.duration = duration;
    if (m.hasVideo || usage.video) {
        VideoStreamInfo video;
        video.index = 0;
        video.width = std::max(m.width, 0);
        video.height = std::max(m.height, 0);
        if (m.frameRate) {
            video.frameRate = *m.frameRate;
        }
        video.duration = duration;
        info.video.push_back(video);
    }
    const auto declared = static_cast<std::int32_t>(m.audioChannels.size());
    const std::int32_t streams = std::max(declared, usage.audioStreams);
    for (std::int32_t k = 0; k < streams; ++k) {
        AudioStreamInfo audio;
        audio.index = static_cast<int>(info.video.size()) + k;
        audio.sampleRate = m.sampleRate;
        audio.channels = k < declared ? std::max(m.audioChannels[static_cast<std::size_t>(k)], 0) : 0;
        audio.duration = duration;
        info.audio.push_back(audio);
    }
    return item;
}

std::expected<void, Error> Builder::addMediaItems() {
    const auto usage = scanUsage();
    m_mediaIds.reserve(m_doc.media.size());
    for (std::size_t i = 0; i < m_doc.media.size(); ++i) {
        const IrMedia& m = m_doc.media[i];
        RelinkResult link;
        MediaItem item = makeMediaItem(m, usage[i], link);
        const std::string name = item.name;
        auto added = edit::addMedia(m_state, std::move(item));
        if (!added) {
            return std::unexpected(added.error());
        }
        m_state = std::move(added->state);
        m_mediaIds.push_back(added->id);
        m_result.media.push_back(added->id);
        m_sourceOffset.push_back(sourceOffsetFor(m, name, link));

        if (link.found()) {
            const std::string where = utf8FromPath(link.resolved);
            std::string reason;
            switch (link.method) {
            case RelinkMethod::ExactPath:
                reason = inQuotes(name) + " is at its original path " + where;
                break;
            case RelinkMethod::ProjectRelative:
                reason = inQuotes(name) + " found next to the imported file at " + where;
                break;
            default:
                reason = inQuotes(name) + " found under a search folder at " + where;
                break;
            }
            if (!link.note.empty()) {
                reason += " (" + link.note + ")";
            }
            report(link.note.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated,
                   ReportCategory::Media, std::move(reason), m.location);
        } else {
            std::string reason = inQuotes(name) + " is offline: not found";
            reason +=
                m.reference.path.empty() ? " (the file only names the clip)" : " at " + m.reference.path;
            if (!link.note.empty()) {
                reason += "; " + link.note;
            }
            report(ReportOutcome::Approximated, ReportCategory::Media, std::move(reason), m.location);
            m_result.unresolvedMedia.push_back(
                {added->id, name, m.reference.original, m.reference.path, link.tried, link.note});
        }
    }
    return {};
}

Ticks Builder::sourceOffsetFor(const IrMedia& m, const std::string& name, const RelinkResult& link) {
    if (!m.absoluteSourceTimecode) {
        return {};
    }
    if (link.found() && m_ctx.options && m_ctx.options->mediaInspector) {
        if (const auto facts = m_ctx.options->mediaInspector(link.resolved); facts && facts->startTimecode) {
            report(ReportOutcome::Carried, ReportCategory::Media,
                   "source timecodes of " + inQuotes(name) + " converted with its start timecode (" +
                       std::to_string(facts->startTimecode->toSeconds()) + " s)",
                   m.location);
            return *facts->startTimecode;
        }
    }
    report(ReportOutcome::Approximated, ReportCategory::Media,
           inQuotes(name) +
               " is referenced by source timecode, but its start timecode is unknown: its clips use "
               "the absolute source timecode as source In, which is only right for media whose "
               "timecode starts at 00:00:00:00",
           m.location);
    return {};
}

bool Builder::fits(TrackId trackId, TimeRange range) const {
    const Track* t = sequence().findTrack(trackId);
    if (!t) {
        return false;
    }
    // Clips waiting for the batch's edit take their time too (they come in start order).
    if (const auto it = m_pendingEnd.find(trackId.value); it != m_pendingEnd.end() && it->second > range.start) {
        return false;
    }
    const std::size_t next = t->firstClipAtOrAfter(range.start);
    if (next < t->clips.size() && t->clips[next].start() < range.end()) {
        return false;
    }
    return next == 0 || t->clips[next - 1].end() <= range.start;
}

std::string Builder::labelOf(TrackId trackId) const {
    if (const auto pos = sequence().trackPosition(trackId)) {
        return trackLabel(pos->kind, pos->index);
    }
    return "?";
}

std::expected<void, Error> Builder::applyTrackSettings(const IrSequence& ir) {
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = kind == TrackKind::Video ? ir.videoTracks : ir.audioTracks;
        const auto& ids = kind == TrackKind::Video ? m_videoIds : m_audioIds;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const IrTrack& t = tracks[i];
            edit::TrackStateChange change;
            if (!t.name.empty()) {
                change.name = t.name;
            }
            if (!t.enabled) {
                change.enabled = false;
            }
            if (t.targeted) {
                change.targeted = *t.targeted;
            }
            if (t.muted) { // X.1: formats that say (OTIO's Wabcut metadata)
                change.muted = true;
            }
            if (t.role != TrackRole::None && kind == TrackKind::Audio) {
                change.role = t.role;
            }
            if (!change.name && !change.enabled && !change.targeted && !change.muted && !change.role) {
                continue;
            }
            auto changed = edit::setTrackState(m_state, m_seq, ids[i], change);
            if (!changed) {
                return std::unexpected(changed.error());
            }
            m_state = std::move(changed->state);
        }
    }
    return {};
}

std::expected<TrackId, Error> Builder::overflowTrack(TrackKind kind, TimeRange range,
                                                     const std::string& from) {
    for (const TrackId id : m_overflow) {
        const auto pos = sequence().trackPosition(id);
        if (pos && pos->kind == kind && fits(id, range)) {
            return id;
        }
    }
    auto added = edit::addTrack(m_state, m_seq, kind, from + " overlaps");
    if (!added) {
        return std::unexpected(added.error());
    }
    m_state = std::move(added->state);
    m_overflow.push_back(added->id);
    report(ReportOutcome::Approximated, ReportCategory::Track,
           "added track " + labelOf(added->id) + " for clips that overlap on " + from, fileLocation(),
           m_seqName);
    return added->id;
}

std::expected<void, Error> Builder::placeClips(const IrTrack& track, TrackId trackId) {
    const std::string label = labelOf(trackId);
    std::vector<std::size_t> order(track.clips.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return track.clips[a].start < track.clips[b].start;
    });

    for (const std::size_t index : order) {
        const IrClip& c = track.clips[index];
        const std::string name = c.name.empty() ? (c.title ? std::string("title") : std::string("clip")) : c.name;
        if (c.title || c.sequence != kNoSequence || !c.sequenceKey.empty()) {
            // Placed one by one (rare): after the media clips before them.
            if (auto flushed = flushPending(); !flushed) {
                return flushed;
            }
        }
        if (c.title) {
            if (track.kind != TrackKind::Video || c.duration.value <= 0 || c.start.value < 0) {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       "title " + inQuotes(name) + " on " + label + " has no valid place", c.location, m_seqName);
                continue;
            }
            TrackId destination = trackId;
            const TimeRange range{c.start, c.duration};
            if (!fits(trackId, range)) {
                auto spill = overflowTrack(track.kind, range, label);
                if (!spill) {
                    return std::unexpected(spill.error());
                }
                destination = *spill;
            }
            if (const auto id = placeTitle(c, destination, c.start, c.duration)) {
                m_placed.push_back({*id, &c});
            }
            continue;
        }
        if (c.sequence != kNoSequence || !c.sequenceKey.empty()) {
            placeNested(c, track.kind, trackId, label, name);
            continue;
        }
        if (c.media >= m_mediaIds.size()) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   inQuotes(name) + " on " + label + " has no media", c.location, m_seqName);
            continue;
        }
        if (c.stream.kind != streamKindOf(track.kind)) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   inQuotes(name) + " plays a stream of the wrong kind for " + label, c.location, m_seqName);
            continue;
        }
        Ticks start = c.start;
        Ticks duration = c.duration;
        Ticks sourceIn = c.sourceIn - m_sourceOffset[c.media];
        const auto end = checkedAdd(start.value, duration.value);
        if (duration.value <= 0 || !end) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   inQuotes(name) + " on " + label + " has no duration", c.location, m_seqName);
            continue;
        }
        if (start.value < 0) {
            if (*end <= 0) {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       inQuotes(name) + " on " + label + " lies entirely before the start of the sequence",
                       c.location, m_seqName);
                continue;
            }
            report(ReportOutcome::Approximated, ReportCategory::Clip,
                   inQuotes(name) + " on " + label + " starts " + length(-start) +
                       " before the sequence start; that part was trimmed off",
                   c.location, m_seqName);
            sourceIn += -start;
            duration = Ticks{*end};
            start = Ticks{};
        }
        if (sourceIn.value < 0) {
            report(ReportOutcome::Approximated, ReportCategory::Clip,
                   inQuotes(name) + " on " + label +
                       " starts before its media; its source In was moved to the first frame",
                   c.location, m_seqName);
            sourceIn = Ticks{};
        }
        const TimeRange range{start, duration};
        TrackId destination = trackId;
        if (!fits(trackId, range)) {
            auto spill = overflowTrack(track.kind, range, label);
            if (!spill) {
                return std::unexpected(spill.error());
            }
            destination = *spill;
            report(ReportOutcome::Approximated, ReportCategory::Track,
                   inQuotes(name) + " at " + at(start) + " overlaps another clip on " + label +
                       "; moved to " + labelOf(destination),
                   c.location, m_seqName);
        }
        edit::SourceClip source;
        source.media = m_mediaIds[c.media];
        if (c.remap) {
            // Placed at 100% first, then remapped: any media that fits the clip's length will do.
            if (const MediaItem* item = m_state.findMedia(source.media)) {
                if (const auto extent = item->sourceExtent(c.stream); extent && sourceIn + duration > *extent) {
                    sourceIn = std::max(Ticks{}, *extent - duration);
                }
            }
        }
        // Media clips go in with one edit per track (edit::placeClips, X.1): a 50,000-clip timeline is not 50,000
        // edits of an ever longer track.
        const MediaItem* item = m_state.findMedia(source.media);
        const SourceBounds bounds = item ? item->sourceBounds(c.stream) : SourceBounds{};
        if (!item || !item->hasStream(c.stream) || (bounds.end && sourceIn + duration > *bounds.end) || sourceIn < bounds.start) {
            // Not placeable as it is: the single edit says why.
            source.sourceRange = {sourceIn, duration};
            source.patches.push_back({c.stream, destination});
            const auto placed = edit::overwriteEdit(m_state, m_seq, source, start);
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   inQuotes(name) + " on " + label + " could not be placed" +
                       (placed ? std::string() : ": " + placed.error().message),
                   c.location, m_seqName);
            continue;
        }
        m_pending.push_back(Pending{&c, edit::ClipPlacement{destination, source.media, c.stream, start, duration, sourceIn}, name});
        Ticks& pendingEnd = m_pendingEnd[destination.value];
        pendingEnd = std::max(pendingEnd, start + duration);
    }
    return flushPending();
}

std::expected<void, Error> Builder::flushPending() {
    if (m_pending.empty()) {
        return {};
    }
    std::vector<edit::ClipPlacement> placements;
    placements.reserve(m_pending.size());
    for (const Pending& p : m_pending) {
        placements.push_back(p.placement);
    }
    std::vector<Pending> pending = std::move(m_pending);
    m_pending.clear();
    m_pendingEnd.clear();
    auto placed = edit::placeClips(m_state, m_seq, placements);
    if (!placed || placed->placed.size() != pending.size()) {
        return std::unexpected(Error{ErrorCode::Unknown, "placing the imported clips failed (please report): " +
                                                             (placed ? std::string("a clip is missing") : placed.error().message)});
    }
    m_state = std::move(placed->state);
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const Pending& p = pending[i];
        const IrClip& c = *p.ir;
        const ClipId id = placed->placed[i];
        m_placed.push_back({id, &c});
        const std::string label = labelOf(p.placement.track);
        report(ReportOutcome::Carried, ReportCategory::Clip,
               inQuotes(p.name) + " on " + label + " at " + at(p.placement.start) + " (" + length(p.placement.duration) +
                   ")" + (c.enabled ? "" : ", disabled"),
               c.location, m_seqName);
        if (c.remap) {
            applyRemap(c, id, p.name, label);
        } else if (c.speed) {
            applySpeed(c, id, p.name, label);
        }
    }
    return {};
}

std::optional<std::size_t> Builder::nestedIndex(const IrClip& c) const {
    if (c.sequence != kNoSequence) {
        return c.sequence < m_doc.sequences.size() ? std::optional(c.sequence) : std::nullopt;
    }
    const auto it = m_sequenceKeys.find(c.sequenceKey);
    return it == m_sequenceKeys.end() ? std::nullopt : std::optional(it->second);
}

/// A nested or multicam clip (C.8): edited in from its built sequence like media, then retimed as the source says.
bool Builder::placeNested(const IrClip& c, TrackKind kind, TrackId trackId, const std::string& label,
                          const std::string& name) {
    const auto index = nestedIndex(c);
    const SequenceId nestedId = index ? m_sequenceIds[*index] : SequenceId{};
    const Sequence* nested = nestedId.isValid() ? m_state.findSequence(nestedId) : nullptr;
    if (!nested) {
        report(ReportOutcome::Dropped, ReportCategory::Clip,
               inQuotes(name) + " on " + label + " plays a nested sequence that is missing or contains itself; its "
                                                  "time is left as a gap",
               c.location, m_seqName);
        return false;
    }
    const StreamRef stream{streamKindOf(kind), 0};
    const std::vector<TrackPtr>& inside = nested->tracks(kind);
    if (!sequenceHasStream(*nested, stream) ||
        (!nested->multicam && std::all_of(inside.begin(), inside.end(), [](const TrackPtr& t) { return t->clips.empty(); }))) {
        // A compound clip's sound (or picture) side whose sequence has none: nothing to place, nothing lost.
        return false;
    }
    const auto end = checkedAdd(c.start.value, c.duration.value);
    if (c.duration.value <= 0 || c.start.value < 0 || !end || c.sourceIn.value < 0 || c.sourceIn >= nested->duration) {
        report(ReportOutcome::Dropped, ReportCategory::Clip,
               inQuotes(name) + " on " + label + " has no valid place in its sequence", c.location, m_seqName);
        return false;
    }
    Ticks duration = c.duration;
    if (!c.remap && !c.speed && c.sourceIn + duration > nested->duration) {
        // Longer than its sequence: past its end it shows and plays nothing, so the clip ends there.
        duration = nested->duration - c.sourceIn;
        report(ReportOutcome::Approximated, ReportCategory::Clip,
               inQuotes(name) + " on " + label + " is longer than its sequence (" + length(nested->duration) +
                   "); it ends with it (the rest showed nothing)",
               c.location, m_seqName);
    }
    TrackId destination = trackId;
    const TimeRange range{c.start, duration};
    if (!fits(trackId, range)) {
        auto spill = overflowTrack(kind, range, label);
        if (!spill) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   inQuotes(name) + " on " + label + " could not be placed: " + spill.error().message, c.location,
                   m_seqName);
            return false;
        }
        destination = *spill;
    }
    edit::SourceClip source;
    source.sequence = nestedId;
    // Retimed clips are placed at 100 % from the same In first (any part of the nested sequence that fits will do).
    Ticks sourceIn = c.sourceIn;
    if ((c.remap || c.speed) && sourceIn + duration > nested->duration) {
        sourceIn = std::max(Ticks{}, nested->duration - duration);
    }
    source.sourceRange = {sourceIn, duration};
    source.patches.push_back({stream, destination});
    for (const std::size_t a : c.angles) {
        if (a < kMaxMulticamAngles) {
            (kind == TrackKind::Video ? source.videoAngle : source.audioAngles).bits |= AngleSet::of(a).bits;
        }
    }
    auto placed = edit::overwriteEdit(m_state, m_seq, source, c.start);
    if (!placed || placed->placed.empty()) {
        report(ReportOutcome::Dropped, ReportCategory::Clip,
               inQuotes(name) + " on " + label + " could not be placed" +
                   (placed ? std::string() : ": " + placed.error().message),
               c.location, m_seqName);
        return false;
    }
    m_state = std::move(placed->state);
    m_placed.push_back({placed->placed.front(), &c});
    report(ReportOutcome::Carried, ReportCategory::Clip,
           std::string(nested->multicam ? "multicam clip " : "nested sequence ") + inQuotes(nested->name) + " on " +
               labelOf(destination) + " at " + at(c.start) + " (" + length(duration) + ")",
           c.location, m_seqName);
    if (c.remap) {
        applyRemap(c, placed->placed.front(), name, labelOf(destination));
    } else if (c.speed) {
        applySpeed(c, placed->placed.front(), name, labelOf(destination));
    }
    return true;
}

/// Sequences in an order that builds every nested sequence before the sequences that play it (a cycle is broken:
/// its clips are dropped when they are placed).
std::vector<std::size_t> Builder::buildOrder() {
    for (std::size_t i = 0; i < m_doc.sequences.size(); ++i) {
        if (!m_doc.sequences[i].key.empty()) {
            m_sequenceKeys.emplace(m_doc.sequences[i].key, i);
        }
    }
    std::vector<std::size_t> order;
    std::vector<int> mark(m_doc.sequences.size(), 0); // 0 new, 1 open, 2 done
    for (std::size_t root = 0; root < m_doc.sequences.size(); ++root) {
        if (mark[root] != 0) {
            continue;
        }
        std::vector<std::pair<std::size_t, std::vector<std::size_t>>> stack;
        const auto deps = [&](std::size_t s) {
            std::vector<std::size_t> out;
            for (const auto* tracks : {&m_doc.sequences[s].videoTracks, &m_doc.sequences[s].audioTracks}) {
                for (const IrTrack& t : *tracks) {
                    for (const IrClip& c : t.clips) {
                        if (const auto n = nestedIndex(c); n && mark[*n] == 0) {
                            out.push_back(*n);
                        }
                    }
                }
            }
            return out;
        };
        mark[root] = 1;
        stack.emplace_back(root, deps(root));
        while (!stack.empty()) {
            auto& [s, pending] = stack.back();
            if (pending.empty()) {
                mark[s] = 2;
                order.push_back(s);
                stack.pop_back();
                continue;
            }
            const std::size_t next = pending.back();
            pending.pop_back();
            if (mark[next] == 0) {
                mark[next] = 1;
                stack.emplace_back(next, deps(next));
            }
        }
    }
    return order;
}

/// A multicam source's angles (C.8), once its tracks exist.
void Builder::applyMulticam(const IrSequence& ir) {
    if (!ir.multicam) {
        return;
    }
    MulticamInfo info;
    info.sync = ir.multicam->sync;
    for (const IrMulticamAngle& a : ir.multicam->angles) {
        MulticamAngle angle;
        angle.name = a.name;
        if (a.videoTrack && *a.videoTrack < m_videoIds.size()) {
            angle.video = m_videoIds[*a.videoTrack];
        }
        for (const std::size_t t : a.audioTracks) {
            if (t < m_audioIds.size()) {
                angle.audio.push_back(m_audioIds[t]);
            }
        }
        angle.offset = a.offset;
        info.angles.push_back(std::move(angle));
    }
    auto seq = std::make_shared<Sequence>(sequence());
    seq->multicam = std::move(info);
    if (const auto problem = multicamProblem(*seq)) {
        report(ReportOutcome::Dropped, ReportCategory::SequenceSetting,
               "the multicam angles could not be carried (" + *problem + "); the source is an ordinary sequence",
               ir.location, m_seqName);
        return;
    }
    report(ReportOutcome::Carried, ReportCategory::SequenceSetting,
           "multicam source with " + std::to_string(seq->multicam->angles.size()) + " angles (" +
               std::string(toString(seq->multicam->sync)) + " sync)",
           ir.location, m_seqName);
    m_state = m_state.withSequence(std::move(seq));
}

std::optional<ClipId> Builder::placeTitle(const IrClip& c, TrackId destination, Ticks start, Ticks duration) {
    const IrTitle& t = *c.title;
    edit::TitleFont font;
    if (!t.fontFamily.empty()) {
        font.family = t.fontFamily;
    }
    if (!t.fontStyle.empty()) {
        font.style = t.fontStyle;
    }
    const std::string label = labelOf(destination);
    std::string text = t.text;
    if (!edit::isValidUtf8(text)) {
        text = "Title";
    }
    auto added = edit::addTitle(m_state, m_seq, destination, start, duration, text, t.style, font, m_result.bin);
    if (!added) {
        // A style value out of range: keep the text and font, drop the styling.
        added = edit::addTitle(m_state, m_seq, destination, start, duration, text, {}, font, m_result.bin);
        if (!added) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "title " + inQuotes(c.name) + " on " + label + " could not be placed: " + added.error().message,
                   c.location, m_seqName);
            return std::nullopt;
        }
        report(ReportOutcome::Approximated, ReportCategory::Effect,
               "the styling of title " + inQuotes(c.name) + " was out of range and was not imported", c.location,
               m_seqName);
    }
    m_state = std::move(added->state);
    m_result.media.push_back(added->createdMedia);
    std::string firstLine = text.substr(0, text.find('\n'));
    std::string reason = "title " + inQuotes(firstLine) + " on " + label + " at " + at(start) + " (" + length(duration) +
                         ", " + font.family + " " + font.style + ")";
    if (t.approximation.empty() && text == t.text) {
        report(ReportOutcome::Carried, ReportCategory::Clip, std::move(reason), c.location, m_seqName);
    } else {
        reason += text == t.text ? "; " + t.approximation : "; its text was not UTF-8";
        report(ReportOutcome::Approximated, ReportCategory::Clip, std::move(reason), c.location, m_seqName);
    }
    return added->placed.front();
}

void Builder::applySpeed(const IrClip& c, ClipId id, const std::string& name, const std::string& label) {
    const IrSpeed& sp = *c.speed;
    const SourceLocation& where = sp.location.file.empty() ? c.location : sp.location;
    const std::string who = inQuotes(name) + " on " + label;
    if (sp.ratio.num == 0) {
        // A freeze frame: the whole clip holds the frame at its source In.
        if (c.stream.kind != StreamKind::Video) {
            report(ReportOutcome::Approximated, ReportCategory::Speed,
                   who + " is a freeze frame; audio cannot hold a frame, so it plays at 100%", where, m_seqName);
            return;
        }
        const Clip& clip = sequence().findClip(id)->clip();
        auto held = edit::frameHold(m_state, m_seq, id, clip.start(), std::nullopt);
        if (!held) {
            report(ReportOutcome::Approximated, ReportCategory::Speed,
                   who + " is a freeze frame; imported at 100%: " + held.error().message, where, m_seqName);
            return;
        }
        m_state = std::move(held->state);
        report(ReportOutcome::Carried, ReportCategory::Speed, who + " is a freeze frame (frame hold)", where,
               m_seqName);
        return;
    }
    edit::SpeedChange change;
    change.speed = sp.ratio;
    change.reversed = sp.reversed;
    change.keepDuration = true;
    const ClipId ids[] = {id};
    auto retimed = edit::setSpeed(m_state, m_seq, ids, change, edit::LinkMode::IgnoreLinks);
    const std::string speed = speedLabel(sp.ratio) + (sp.reversed ? " in reverse" : "");
    if (!retimed) {
        report(ReportOutcome::Approximated, ReportCategory::Speed,
               who + " plays at " + speed + "; imported at 100% forward: " + retimed.error().message, where,
               m_seqName);
        return;
    }
    m_state = std::move(retimed->state);
    std::string note = sp.approximation;
    if (sp.reversed) {
        note += std::string(note.empty() ? "" : "; ") +
                "it plays the same media backwards from the clip's end (the first frame may differ by a frame)";
    }
    if (note.empty()) {
        report(ReportOutcome::Carried, ReportCategory::Speed, who + " plays at " + speed, where, m_seqName);
    } else {
        report(ReportOutcome::Approximated, ReportCategory::Speed, who + " plays at " + speed + "; " + note, where,
               m_seqName);
    }
}

void Builder::applyRemap(const IrClip& c, ClipId id, const std::string& name, const std::string& label) {
    const IrRemap& r = *c.remap;
    const SourceLocation& where = r.location.file.empty() ? c.location : r.location;
    const std::string who = inQuotes(name) + " on " + label;
    // Positions are relative to the media file, the media item may start later in it (sourceOffsetFor), and a
    // clip that began before the sequence start lost its head.
    Curve curve = r.curve;
    const Ticks offset = m_sourceOffset[c.media];
    bool clamped = false; // positions before the media item's start play its first frame there
    for (Keyframe& k : curve.keys) {
        const double position = k.value - static_cast<double>(offset.value);
        clamped = clamped || position < 0.0;
        k.value = std::max(0.0, position);
    }
    const Ticks curveIn = r.curveIn + (sequence().findClip(id)->clip().start() - c.start);
    auto remapped = edit::setTimeRemap(m_state, m_seq, id, std::move(curve), curveIn);
    const std::string keys = std::to_string(r.curve.keys.size()) + " keys";
    if (!remapped) {
        report(ReportOutcome::Approximated, ReportCategory::Speed,
               who + " has variable speed (" + keys + "); imported at 100% from the same source In: " +
                   remapped.error().message,
               where, m_seqName);
        return;
    }
    m_state = std::move(remapped->state);
    std::string approximation = r.approximation;
    if (clamped) {
        approximation += std::string(approximation.empty() ? "" : "; ") +
                         "keys before the start of the media were moved to its first frame";
    }
    if (approximation.empty()) {
        report(ReportOutcome::Carried, ReportCategory::Speed, who + " has variable speed: a time remap with " + keys,
               where, m_seqName);
    } else {
        report(ReportOutcome::Approximated, ReportCategory::Speed,
               who + " has variable speed: a time remap with " + keys + "; " + approximation, where, m_seqName);
    }
}

std::expected<void, Error> Builder::applyClipAttributes() {
    // Grouped so that a 1000-clip timeline costs a handful of edits, not 1000.
    std::map<std::string, std::vector<ClipId>> renames;
    std::vector<ClipId> disabled;
    std::vector<std::pair<ClipProperties, std::vector<ClipId>>> properties;
    std::vector<std::pair<AudioChannelMap, std::vector<ClipId>>> channelMaps; // A.9
    // Every clip by id once (X.1: Sequence::findClip per clip is linear in the track: quadratic for 50,000 clips).
    std::unordered_map<ClipId, const Clip*> byId;
    sequence().forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            byId.emplace(c.id, &c);
        }
    });
    for (const Placed& p : m_placed) {
        const auto found = byId.find(p.id);
        if (found == byId.end()) {
            continue;
        }
        const Clip& clip = *found->second;
        if (!p.ir->name.empty() && p.ir->name != clip.name()) {
            renames[p.ir->name].push_back(p.id);
        }
        if (!p.ir->enabled) {
            disabled.push_back(p.id);
        }
        if (p.ir->channels && clip.stream.kind == StreamKind::Audio) {
            auto it = std::find_if(channelMaps.begin(), channelMaps.end(),
                                   [&](const auto& entry) { return entry.first == *p.ir->channels; });
            if (it == channelMaps.end()) {
                channelMaps.push_back({*p.ir->channels, {p.id}});
            } else {
                it->second.push_back(p.id);
            }
        }
        if (p.ir->properties && !(*p.ir->properties == ClipProperties{})) {
            auto it = std::find_if(properties.begin(), properties.end(),
                                   [&](const auto& entry) { return entry.first == *p.ir->properties; });
            if (it == properties.end()) {
                properties.push_back({*p.ir->properties, {p.id}});
            } else {
                it->second.push_back(p.id);
            }
        }
    }
    auto apply = [&](std::span<const ClipId> clips,
                     const edit::ClipAttributeChange& change) -> std::expected<void, Error> {
        auto changed = edit::setClipAttributes(m_state, m_seq, clips, change);
        if (!changed) {
            return std::unexpected(changed.error());
        }
        m_state = std::move(changed->state);
        return {};
    };
    for (const auto& [name, clips] : renames) {
        edit::ClipAttributeChange change;
        change.name = name;
        if (auto r = apply(clips, change); !r) {
            return r;
        }
    }
    if (!disabled.empty()) {
        edit::ClipAttributeChange change;
        change.enabled = false;
        if (auto r = apply(disabled, change); !r) {
            return r;
        }
    }
    for (const auto& [values, clips] : properties) {
        edit::ClipAttributeChange change;
        change.properties = values;
        if (auto r = apply(clips, change); !r) {
            // Out-of-range values were filtered by the parsers; report rather than fail the import.
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   "clip property values were rejected: " + r.error().message, fileLocation(), m_seqName);
        }
    }
    for (const auto& [map, clips] : channelMaps) { // A.9
        auto changed = edit::setClipChannels(m_state, m_seq, clips, map);
        if (!changed) {
            report(ReportOutcome::Dropped, ReportCategory::AudioMapping,
                   "a channel map was rejected: " + changed.error().message, fileLocation(), m_seqName);
            continue;
        }
        m_state = std::move(changed->state);
    }
    // X.1: effect stacks, in order, each instance with its parameters (and switched off when it was).
    for (const Placed& p : m_placed) {
        for (const EffectInstance& e : p.ir->effects) {
            auto added = edit::addEffect(m_state, m_seq, std::span<const ClipId>(&p.id, 1), e.effect, e.params);
            if (!added) {
                report(ReportOutcome::Dropped, ReportCategory::Effect,
                       "effect '" + e.effect + "' on '" + p.ir->name + "' was rejected: " + added.error().message,
                       p.ir->location, m_seqName);
                continue;
            }
            m_state = std::move(added->state);
            if (!e.enabled && !added->createdEffects.empty()) {
                edit::EffectChange off;
                off.enabled = false;
                if (auto changed = edit::setEffect(m_state, m_seq, p.id, added->createdEffects.front(), off)) {
                    m_state = std::move(changed->state);
                }
            }
        }
    }
    return {};
}

std::expected<void, Error> Builder::applyLinks() {
    std::map<std::int64_t, std::vector<ClipId>> groups;
    for (const Placed& p : m_placed) {
        if (p.ir->linkGroup >= 0) {
            groups[p.ir->linkGroup].push_back(p.id);
        }
    }
    // One edit for every group (X.1: edit::linkClipGroups).
    std::vector<std::vector<ClipId>> linked;
    for (auto& [group, clips] : groups) {
        if (clips.size() >= 2) {
            linked.push_back(std::move(clips));
        }
    }
    if (!linked.empty()) {
        auto done = edit::linkClipGroups(m_state, m_seq, linked);
        if (!done) {
            return std::unexpected(done.error());
        }
        m_state = std::move(done->state);
    }
    return {};
}

void Builder::importTransition(const IrTrack& track, std::size_t trackIndex, TrackId trackId, const IrTransition& t) {
    const Ticks total = t.before + t.after;
    std::string placement;
    if (t.before.value == 0) {
        placement = "starting at the cut";
    } else if (t.after.value == 0) {
        placement = "ending at the cut";
    } else if (t.before == t.after) {
        placement = "centred on the cut";
    } else {
        placement = "around the cut";
    }
    const std::string name = t.name.empty() ? std::string(transitionNoun(t.kind)) : t.name;
    const std::string what = inQuotes(name) + " (" + length(total) + ", " + placement + " at " + at(t.cut) + ") on " +
                             trackLabel(track.kind, trackIndex);
    const auto asCut = [&](const std::string& why) {
        report(ReportOutcome::Approximated, ReportCategory::Transition, what + " imported as a cut: " + why,
               t.location, m_seqName);
    };
    if (total.value <= 0) {
        asCut("it has no length");
        return;
    }
    // The clips meeting at the cut on this track (one-sided at a clip's head or tail).
    edit::TransitionSpec spec;
    if (const Track* placed = sequence().findTrack(trackId)) {
        for (const Clip& c : placed->clips) {
            if (c.end() == t.cut) {
                spec.outgoing = c.id;
            }
            if (c.start() == t.cut) {
                spec.incoming = c.id;
            }
        }
    }
    std::vector<std::string> notes;
    spec.before = t.before;
    spec.after = t.after;
    if (spec.outgoing.isValid() && !spec.incoming.isValid() && t.after.value > 0) {
        // Nothing after the cut: a fade out at the outgoing clip's tail, ending where the clip ends.
        spec.before = total;
        spec.after = Ticks{};
        notes.push_back("nothing follows the cut, so it ends with the outgoing clip");
    } else if (spec.incoming.isValid() && !spec.outgoing.isValid() && t.before.value > 0) {
        spec.before = Ticks{};
        spec.after = total;
        notes.push_back("nothing precedes the cut, so it starts with the incoming clip");
    }
    if (!spec.outgoing.isValid() && !spec.incoming.isValid()) {
        asCut("no clip starts or ends at its edit point");
        return;
    }
    MappedTransition mapped = mapTransition(track.kind, t);
    if (!t.effect.empty() && EffectRegistry::builtin().find(t.effect)) {
        mapped = {t.effect, {}}; // the source named the model's effect exactly (X.1)
    }
    spec.effect = mapped.effect;
    if (!mapped.approximation.empty()) {
        notes.push_back(mapped.approximation);
    }
    auto added = edit::addTransition(m_state, m_seq, trackId, spec, edit::HandleFit::Exact);
    if (!added) {
        auto shortened = edit::addTransition(m_state, m_seq, trackId, spec, edit::HandleFit::UseAvailable);
        if (!shortened) {
            asCut(added.error().message);
            return;
        }
        notes.push_back("shortened to the media available (" + added.error().message + ")");
        added = std::move(shortened);
    }
    m_state = std::move(added->state);
    std::string reason = what + " -> " + spec.effect;
    for (const std::string& n : notes) {
        reason += "; " + n;
    }
    report(notes.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Transition,
           std::move(reason), t.location, m_seqName);
}

void Builder::reportGaps(const IrTrack& track, TrackId trackId, std::size_t trackIndex) {
    const Track* t = sequence().findTrack(trackId);
    if (!t) {
        return;
    }
    Ticks cursor{};
    for (const Clip& clip : t->clips) {
        if (clip.start() > cursor) {
            const TimeRange gap = TimeRange::fromStartEnd(cursor, clip.start());
            SourceLocation location = track.location;
            for (const IrGap& g : track.gaps) {
                if (g.range.start == gap.start) {
                    location = g.location;
                    break;
                }
            }
            report(ReportOutcome::Carried, ReportCategory::Gap,
                   "gap on " + trackLabel(track.kind, trackIndex) + " at " + at(gap.start) + " (" +
                       length(gap.duration) + ")",
                   location, m_seqName);
        }
        cursor = clip.end();
    }
}

std::expected<void, Error> Builder::applyMarkers(const IrSequence& ir) {
    for (const IrMarker& m : ir.markers) {
        const std::string label = m.marker.name.empty() ? std::string("marker") : inQuotes(m.marker.name);
        if (m.marker.start.value < 0 || m.marker.duration.value < 0) {
            report(ReportOutcome::Dropped, ReportCategory::Marker,
                   label + " lies before the start of the sequence", m.location, m_seqName);
            continue;
        }
        auto added = edit::addMarker(m_state, m_seq, m.marker);
        if (!added) {
            return std::unexpected(added.error());
        }
        m_state = std::move(added->state);
        if (m.approximation.empty()) {
            report(ReportOutcome::Carried, ReportCategory::Marker, label + " at " + at(m.marker.start),
                   m.location, m_seqName);
        } else {
            report(ReportOutcome::Approximated, ReportCategory::Marker,
                   label + " at " + at(m.marker.start) + ": " + m.approximation, m.location, m_seqName);
        }
    }
    return {};
}

std::expected<void, Error> Builder::applyMarks(const IrSequence& ir) {
    if (ir.markIn && ir.markOut && *ir.markIn >= *ir.markOut) {
        report(ReportOutcome::Dropped, ReportCategory::SequenceSetting,
               "In/Out marks are reversed (In " + at(*ir.markIn) + ", Out " + at(*ir.markOut) + ")",
               ir.location, m_seqName);
        return {};
    }
    if (ir.markIn && ir.markIn->value >= 0) {
        auto marked = edit::markIn(m_state, m_seq, *ir.markIn);
        if (!marked) {
            return std::unexpected(marked.error());
        }
        m_state = std::move(marked->state);
    }
    if (ir.markOut && ir.markOut->value > 0) {
        // markOut sets the Out at the end of the frame under the playhead: the frame before the exclusive
        // Out.
        auto marked = edit::markOut(m_state, m_seq, *ir.markOut - Ticks{1});
        if (!marked) {
            return std::unexpected(marked.error());
        }
        m_state = std::move(marked->state);
    }
    if (ir.markIn || ir.markOut) {
        // Marks sit on frame boundaries in Wabcut (as in Premiere's UI); a sub-frame mark is snapped.
        const Sequence& s = sequence();
        const bool snapped = (ir.markIn && s.markIn != ir.markIn) || (ir.markOut && s.markOut != ir.markOut);
        report(snapped ? ReportOutcome::Approximated : ReportOutcome::Carried,
               ReportCategory::SequenceSetting,
               std::string("sequence marks") + (s.markIn ? " In " + at(*s.markIn) : std::string()) +
                   (s.markOut ? " Out " + at(*s.markOut) : std::string()) +
                   (snapped ? " (snapped to frame boundaries)" : ""),
               ir.location, m_seqName);
    }
    return {};
}

void Builder::applySequenceDuration(const IrSequence& ir) {
    // An explicit duration past the last clip is a trailing gap; the model keeps it as the sequence duration.
    if (!ir.duration) {
        return;
    }
    const Ticks end = sequence().contentEnd();
    if (*ir.duration <= end) {
        return;
    }
    auto extended = edit::setSequenceDuration(m_state, m_seq, *ir.duration);
    if (!extended) {
        report(ReportOutcome::Dropped, ReportCategory::Gap,
               "trailing gap of " + length(*ir.duration - end) +
                   " after the last clip could not be kept: " + extended.error().message,
               ir.location, m_seqName);
        return;
    }
    m_state = std::move(extended->state);
    report(ReportOutcome::Carried, ReportCategory::Gap,
           "trailing gap of " + length(*ir.duration - end) + " after the last clip (the sequence runs to " +
               at(*ir.duration) + ")",
           ir.location, m_seqName);
}

std::expected<void, Error> Builder::applyLocks(const IrSequence& ir) {
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = kind == TrackKind::Video ? ir.videoTracks : ir.audioTracks;
        const auto& ids = kind == TrackKind::Video ? m_videoIds : m_audioIds;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const IrTrack& t = tracks[i];
            if (t.locked) {
                edit::TrackStateChange change;
                change.locked = true;
                auto changed = edit::setTrackState(m_state, m_seq, ids[i], change);
                if (!changed) {
                    return std::unexpected(changed.error());
                }
                m_state = std::move(changed->state);
            }
            std::string state = t.enabled ? "on" : "output off";
            if (t.locked) {
                state += ", locked";
            }
            report(ReportOutcome::Carried, ReportCategory::Track,
                   trackLabel(kind, i) + (t.name.empty() ? std::string() : " " + inQuotes(t.name)) + " (" +
                       state + ", " + std::to_string(t.clips.size()) +
                       (t.clips.size() == 1 ? " clip)" : " clips)"),
                   t.location, m_seqName);
        }
    }
    return {};
}

std::expected<void, Error> Builder::buildSequence(const IrSequence& ir) {
    m_seqName = ir.name.empty() ? std::string("Sequence") : ir.name;
    m_overflow.clear();
    m_placed.clear();

    edit::SequenceSettings settings;
    settings.name = m_seqName;
    settings.frameRate = ir.frameRate.normalized();
    if (!settings.frameRate.isExact()) {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "frame rate " + std::to_string(ir.frameRate.num) + "/" + std::to_string(ir.frameRate.den) +
                   " is not supported; the sequence uses 30 fps",
               ir.location, m_seqName);
        settings.frameRate = kRate30;
    }
    settings.audioRate = ir.audioRate.normalized();
    if (!settings.audioRate.isExact()) {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "audio rate " + std::to_string(ir.audioRate.num) +
                   " Hz is not supported; the sequence uses 48 kHz",
               ir.location, m_seqName);
        settings.audioRate = kRate48kHz;
    }
    settings.width = ir.width;
    settings.height = ir.height;
    if (settings.width <= 0 || settings.height <= 0 || settings.width > 32768 || settings.height > 32768) {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "frame size " + std::to_string(ir.width) + "x" + std::to_string(ir.height) +
                   " is not usable; the sequence uses 1920x1080",
               ir.location, m_seqName);
        settings.width = 1920;
        settings.height = 1080;
    }
    settings.dropFrame = ir.dropFrame && supportsDropFrame(settings.frameRate);
    settings.videoTracks = static_cast<std::int32_t>(ir.videoTracks.size());
    settings.audioTracks = static_cast<std::int32_t>(ir.audioTracks.size());
    settings.bin = binFor(ir.bin, ir.location);
    auto created = edit::addSequence(m_state, settings);
    if (!created) {
        return std::unexpected(created.error());
    }
    m_state = std::move(created->state);
    m_seq = created->id;
    m_rate = settings.frameRate;
    m_dropFrame = settings.dropFrame;
    report(ReportOutcome::Carried, ReportCategory::SequenceSetting,
           rateLabel(settings.frameRate) + " fps" + (settings.dropFrame ? " drop-frame" : "") + ", " +
               std::to_string(settings.width) + "x" + std::to_string(settings.height) + ", " +
               std::to_string(settings.audioRate.num / settings.audioRate.den) + " Hz audio",
           ir.location, m_seqName);

    m_videoIds.clear();
    m_audioIds.clear();
    for (const auto& t : sequence().videoTracks) {
        m_videoIds.push_back(t->id);
    }
    for (const auto& t : sequence().audioTracks) {
        m_audioIds.push_back(t->id);
    }

    if (auto r = applyTrackSettings(ir); !r) {
        return r;
    }
    for (std::size_t i = 0; i < ir.videoTracks.size(); ++i) {
        if (auto r = placeClips(ir.videoTracks[i], m_videoIds[i]); !r) {
            return r;
        }
    }
    for (std::size_t i = 0; i < ir.audioTracks.size(); ++i) {
        if (auto r = placeClips(ir.audioTracks[i], m_audioIds[i]); !r) {
            return r;
        }
    }
    applyMulticam(ir);
    if (auto r = applyClipAttributes(); !r) {
        return r;
    }
    if (auto r = applyLinks(); !r) {
        return r;
    }
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = kind == TrackKind::Video ? ir.videoTracks : ir.audioTracks;
        const auto& ids = kind == TrackKind::Video ? m_videoIds : m_audioIds;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            for (const IrTransition& t : tracks[i].transitions) {
                importTransition(tracks[i], i, ids[i], t);
            }
            reportGaps(tracks[i], ids[i], i);
        }
    }
    if (auto r = applyMarkers(ir); !r) {
        return r;
    }
    if (auto r = applyMarks(ir); !r) {
        return r;
    }
    if (ir.startTimecode && ir.startTimecode->value > 0) { // X.1
        if (auto started = edit::setSequenceStartTimecode(m_state, m_seq, *ir.startTimecode)) {
            m_state = std::move(started->state);
            report(ReportOutcome::Carried, ReportCategory::SequenceSetting,
                   "start timecode " + timecodeLabel(*ir.startTimecode, m_rate, m_dropFrame), ir.location, m_seqName);
        } else {
            report(ReportOutcome::Dropped, ReportCategory::SequenceSetting,
                   "start timecode " + timecodeLabel(*ir.startTimecode, m_rate, m_dropFrame) + " is out of range",
                   ir.location, m_seqName);
        }
    }
    applySequenceDuration(ir);
    return applyLocks(ir);
}

std::expected<ImportResult, Error> Builder::run() {
    if (m_ctx.options && m_ctx.options->targetProject) {
        m_state = *m_ctx.options->targetProject;
        if (auto valid = validate(m_state, Validation::Structure); !valid) {
            return std::unexpected(
                Error{ErrorCode::InvalidArgument, "the target project is invalid: " + valid.error().message});
        }
    }
    auto bin = edit::addBin(m_state, m_ctx.binName.empty() ? std::string("Imported") : m_ctx.binName);
    if (!bin) {
        return std::unexpected(bin.error());
    }
    m_state = std::move(bin->state);
    m_result.bin = bin->id;
    m_result.format = m_ctx.format;

    if (auto r = addMediaItems(); !r) {
        return std::unexpected(r.error());
    }
    // Nested sequences first (C.8); the result lists the sequences in the document's order.
    m_sequenceIds.assign(m_doc.sequences.size(), SequenceId{});
    for (const std::size_t i : buildOrder()) {
        const IrSequence& s = m_doc.sequences[i];
        if (auto r = buildSequence(s); !r) {
            return std::unexpected(
                Error{r.error().code, "building sequence '" + s.name + "': " + r.error().message});
        }
        m_sequenceIds[i] = m_seq;
    }
    m_result.sequences = m_sequenceIds;
    if (auto valid = validate(m_state, Validation::Strict); !valid) {
        return std::unexpected(
            Error{ErrorCode::Unknown,
                  "import produced an invalid project (please report): " + valid.error().message});
    }
    m_result.project = std::move(m_state);
    m_result.report = std::move(m_doc.report);
    return std::move(m_result);
}

} // namespace

std::expected<ImportResult, Error> buildProject(IrDocument document, const BuildContext& context) {
    Builder builder(document, context);
    return builder.run();
}

} // namespace wabcut::interchange::detail
