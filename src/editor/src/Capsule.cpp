#include <wabcut/editor/Capsule.h>

#include "PathSafety.h"

#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/TrimmedCopy.h>
#include <wabcut/model/Auditions.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/Plural.h>
#include <wabcut/model/SpannedClip.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Plural.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/SpectralRepair.h>
#include <wabcut/model/TimeRemap.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <istream>
#include <map>
#include <ostream>
#include <random>
#include <set>
#include <system_error>
#include <utility>

namespace wabcut::editor {

namespace {

constexpr std::int64_t kSpaceMargin = 64ll * 1024 * 1024;

bool isFile(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

std::int64_t sizeOf(const std::filesystem::path& p) {
    std::error_code ec;
    const auto s = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(s);
}

/// The keys of `remap` that bound the segments curve times [from, to) cross: the first and last index (C.6 segments
/// between keys, and the holds before the first and after the last key).
std::pair<std::size_t, std::size_t> keysAround(const Curve& remap, Ticks from, Ticks to) {
    std::size_t first = remap.keys.size() - 1;
    std::size_t last = 0;
    for (std::size_t i = 0; i < remap.keys.size(); ++i) {
        const Ticks t = remap.keys[i].time;
        // Key i bounds the segment that ends at it (if that one starts before `to`) and the one that starts at it
        // (if that one ends after `from`).
        const bool before = t > from && (i == 0 || remap.keys[i - 1].time < to);
        const bool after = t < to && (i + 1 == remap.keys.size() || remap.keys[i + 1].time > from);
        if (before || after || (t >= from && t < to)) {
            first = std::min(first, i);
            last = std::max(last, i);
        }
    }
    if (first > last) { // [from, to) lies in a hold before the first or after the last key
        first = last = remap.keys.front().time >= to ? 0 : remap.keys.size() - 1;
    }
    return {first, last};
}

/// The media time a clip shows: [sourceIn, sourceOut) at its speed (a freeze frame holds one `frame`). A remapped clip
/// (C.6: sourceIn and sourceOut are curve times) shows what its curve maps them to, so the range is the lowest to the
/// highest source time shown, widened to the positions of the keys bounding those segments: the trimmed media then
/// holds every position the shifted curve interpolates between (Codex wave-4 P1-7).
TimeRange shownMediaRange(const Clip& clip, Ticks frame) {
    if (const Curve* remap = clip.timeRemap(); remap && !remap->keys.empty()) {
        const Ticks to = std::max(clip.sourceOut(), clip.sourceIn + Ticks{1});
        auto [lo, hi] = remapShownRange(*remap, clip.sourceIn, to);
        const auto [first, last] = keysAround(*remap, clip.sourceIn, to);
        for (std::size_t i = first; i <= last; ++i) {
            const Ticks position{static_cast<std::int64_t>(remap->keys[i].value)};
            lo = std::min(lo, position);
            hi = std::max(hi, position);
        }
        return TimeRange::fromStartEnd(std::max(lo, Ticks{}), std::max(hi, lo) + frame); // hi starts the last frame
    }
    return TimeRange::fromStartEnd(clip.sourceIn, std::max(clip.sourceOut(), clip.sourceIn + frame));
}

/// The source range every clip of every sequence uses, per media item (a hull: one range per file).
std::map<std::uint64_t, TimeRange> usedRanges(const ProjectState& state) {
    std::map<std::uint64_t, TimeRange> out;
    for (const SequencePtr& seq : state.sequences) {
        if (!seq) {
            continue;
        }
        const Rate rate = seq->frameRate.isValid() ? seq->frameRate : Rate{30, 1};
        const Ticks frame = framesToTicks(1, rate, Rounding::Floor).value_or(Ticks{1});
        const auto use = [&](const Clip& clip) {
            if (!clip.media.isValid()) {
                return;
            }
            const TimeRange r = shownMediaRange(clip, frame);
            auto [it, inserted] = out.emplace(clip.media.value, r);
            if (!inserted) {
                it->second = TimeRange::fromStartEnd(std::min(it->second.start, r.start), std::max(it->second.end(), r.end()));
            }
        };
        seq->forEachTrack([&](const Track& track) {
            for (const Clip& clip : track.clips) {
                use(clip);
                // 0.6 (card #74): an audition's other takes are collected (and trimmed) with what they would play.
                if (const Audition* a = clip.audition()) {
                    for (const AuditionTake& take : a->others) {
                        use(takeClip(take, auditionRange(clip)));
                    }
                }
            }
        });
    }
    return out;
}

/// Moves the key times of a keyframed value `shift` earlier (a constant is left alone).
void shiftValueTime(ParamValue& v, Ticks shift) {
    if (auto* curve = std::get_if<Curve>(&v.data)) {
        for (Keyframe& k : curve->keys) {
            k.time = k.time - shift;
        }
    }
}

/// Moves a mask's media-time keys `shift` earlier: its parameter curves and path keys (Masks.h: a mask's curve time is
/// the clip's curve time = media time). A Matte mask is not shifted here: its per-frame mattes are looked up by media
/// time in the analysis cache, so media with one is never trimmed (usesMattes).
void shiftMaskTime(Mask& mask, Ticks shift) {
    for (MaskParam& p : mask.params) {
        shiftValueTime(p.value, shift);
    }
    for (MaskPathKey& k : mask.path) {
        k.time = k.time - shift;
    }
}

/// 0.7 (card 047): Spectral Repair keeps its edits as seconds of the source, in the effect's data. A trimmed copy starts
/// `shift` later in the original, so every edit moves `shift` earlier; an edit that ends before the copy begins goes, one
/// that starts before it is cut at the copy's start.
void shiftSpectralEdits(EffectInstance& effect, Ticks shift) {
    const double seconds = shift.toSeconds();
    std::vector<SpectralEdit> kept;
    for (SpectralEdit edit : unpackSpectralEdits(effect)) {
        edit.start -= seconds;
        edit.end -= seconds;
        for (SpectralPoint& point : edit.path) {
            point.time -= seconds;
        }
        if (spectralBounds(edit).end <= 0.0) {
            continue;
        }
        edit.start = std::max(edit.start, 0.0);
        for (SpectralPoint& point : edit.path) {
            point.time = std::max(point.time, 0.0);
        }
        edit = clampedSpectralEdit(std::move(edit));
        if (!spectralEditProblem(edit)) {
            kept.push_back(std::move(edit));
        }
    }
    std::erase_if(effect.data, [](const EffectData& d) { return isSpectralEditKey(d.key); });
    for (EffectData& entry : packSpectralEdits(kept)) {
        effect.data.push_back(std::move(entry));
    }
    std::sort(effect.data.begin(), effect.data.end(), [](const EffectData& a, const EffectData& b) { return a.key < b.key; });
}

/// Card #89: the keys of an effect stack's parameters and masks, `shift` earlier.
void shiftEffectsTime(EffectStack& effects, Ticks shift) {
    for (EffectInstance& e : effects) {
        if (e.effect == kSpectralRepairEffect && !e.data.empty()) {
            shiftSpectralEdits(e, shift);
        }
        for (EffectParam& p : e.params) {
            shiftValueTime(p.value, shift);
        }
        for (Mask& m : e.masks) {
            shiftMaskTime(m, shift);
        }
    }
}

/// Moves every media-time value of `clip` `shift` earlier. A plain clip: its In point, keyframe curves and effect
/// curves and masks (all media time). A remapped clip keeps its In point and its curves, which are curve times; only the
/// remap's key positions are media time (Codex wave-4 P1-7). Keys bounding no segment the clip shows are dropped first:
/// their positions may lie before the trimmed media, and they change nothing the clip plays.
void shiftMediaTime(Clip& clip, Ticks shift) {
    if (clip.renderedNest()) {
        // 0.7 (card 069): a rendered-and-replaced clip: the file now starts `shift` later in the nest's time, so Restore
        // Unrendered still puts the nest back exactly where it was.
        ClipContent block = clip.content();
        block.renderedNest->origin = block.renderedNest->origin + shift;
        clip.setContent(std::move(block));
    }
    if (const Curve* remap = clip.timeRemap(); remap && !remap->keys.empty()) {
        const auto [first, last] = keysAround(*remap, clip.sourceIn, std::max(clip.sourceOut(), clip.sourceIn + Ticks{1}));
        ClipContent content = clip.content();
        std::vector<Keyframe> keys(content.timeRemap->keys.begin() + static_cast<std::ptrdiff_t>(first),
                                   content.timeRemap->keys.begin() + static_cast<std::ptrdiff_t>(last) + 1);
        for (Keyframe& k : keys) {
            k.value -= static_cast<double>(shift.value);
        }
        content.timeRemap->keys = std::move(keys);
        clip.setContent(std::move(content));
        return;
    }
    clip.sourceIn = clip.sourceIn - shift;
    ClipProperties props = clip.properties();
    for (ParamValue& v : props.values) {
        shiftValueTime(v, shift);
    }
    clip.setProperties(props);
    EffectStack effects = clip.effects();
    shiftEffectsTime(effects, shift);
    clip.setEffects(std::move(effects));
}

/// A media-time range `shift` earlier, cut at the start of the trimmed file; nothing when it ends before that.
std::optional<TimeRange> shiftedRange(const TimeRange& range, Ticks shift) {
    const Ticks end = range.end() - shift;
    if (!(Ticks{} < end)) {
        return std::nullopt;
    }
    return TimeRange::fromStartEnd(std::max(range.start - shift, Ticks{}), end);
}

/// Card #89: a redaction (marked once in the media's own time) `shift` earlier, to follow the trimmed copy: its source
/// range and audio ranges (cut at the file's start) and its region's keys. Nothing when it lies wholly before the
/// trimmed file (that part of the footage is not in the capsule). When all its audio ranges lie there, it no longer
/// bleeps or mutes (an empty list would mean "the whole source range").
std::optional<Redaction> shiftedRedaction(Redaction r, Ticks shift) {
    const auto source = shiftedRange(r.source, shift);
    if (!source) {
        return std::nullopt;
    }
    r.source = *source;
    std::vector<TimeRange> audio;
    for (const TimeRange& range : r.audioRanges) {
        if (const auto moved = shiftedRange(range, shift)) {
            audio.push_back(*moved);
        }
    }
    if (!r.audioRanges.empty() && audio.empty()) {
        r.audio = RedactionAudio::None;
    }
    r.audioRanges = std::move(audio);
    shiftMaskTime(r.region, shift);
    return r;
}

/// Card #89: markers of a media item `shift` earlier; one that ends before the trimmed file goes, one that starts
/// before it is cut there.
std::vector<Marker> shiftedMarkers(const std::vector<Marker>& markers, Ticks shift) {
    std::vector<Marker> out;
    for (Marker m : markers) {
        const Ticks end = m.start + m.duration - shift;
        m.start = m.start - shift;
        if (m.start < Ticks{}) {
            if (!(Ticks{} < end)) {
                continue;
            }
            m.start = Ticks{};
            m.duration = end;
        }
        out.push_back(std::move(m));
    }
    return out;
}

/// The timecode of a file's first frame once `shift` of its start was cut away: `shift` later, wrapping at 24 hours like
/// a timecode does (the project refuses a start timecode past kMaxStartTimecode).
Ticks laterTimecode(Ticks start, Ticks shift) {
    Ticks later = start + shift;
    if (later > kMaxStartTimecode) {
        later = Ticks{later.value % (kMaxStartTimecode.value + 1)};
    }
    return later;
}

/// Card #89: everything on a media item that is keyed to its own time, `shift` earlier: markers, the Source Monitor
/// marks, the poster frame, a subclip's range and its Master Clip Effects. The first frame of the trimmed copy is
/// `shift` later than the original's: so is the item's start timecode (the file's, and a Clip > Modify > Timecode one).
void shiftMediaItemTime(MediaItem& item, Ticks shift) {
    if (item.info.startTimecode) {
        item.info.startTimecode = laterTimecode(*item.info.startTimecode, shift);
    }
    if (item.timecode && item.timecode->start) {
        item.timecode->start = laterTimecode(*item.timecode->start, shift);
    }
    item.markers = shiftedMarkers(item.markers, shift);
    if (item.markIn) {
        item.markIn = std::max(*item.markIn - shift, Ticks{});
    }
    if (item.markOut) {
        item.markOut = *item.markOut - shift;
    }
    if (item.markOut && (!(Ticks{} < *item.markOut) || (item.markIn && !(*item.markIn < *item.markOut)))) {
        item.markIn.reset(); // the marked part lies before the trimmed file
        item.markOut.reset();
    }
    if (item.posterFrame) {
        item.posterFrame = *item.posterFrame - shift;
        if (*item.posterFrame < Ticks{}) {
            item.posterFrame.reset(); // the middle of the trimmed file
        }
    }
    if (item.subclip) {
        const Ticks start = std::max(item.subclip->range.start - shift, Ticks{});
        const Ticks end = std::max(item.subclip->range.end() - shift, start + Ticks{1});
        item.subclip->range = TimeRange::fromStartEnd(start, end);
    }
    shiftEffectsTime(item.masterEffects, shift);
}

/// Card #89: a transcript's edits (speaker spans and word corrections, in the media's time) `shift` earlier; nothing
/// when no edit is left.
std::optional<TranscriptEdits> shiftedTranscriptEdits(TranscriptEdits edits, Ticks shift) {
    std::vector<SpeakerSpan> spans;
    for (SpeakerSpan s : edits.speakerSpans) {
        if (const auto moved = shiftedRange(s.source, shift)) {
            s.source = *moved;
            spans.push_back(s);
        }
    }
    edits.speakerSpans = std::move(spans);
    std::vector<WordCorrection> corrections;
    for (WordCorrection c : edits.corrections) {
        if (const auto moved = shiftedRange(c.source, shift)) {
            c.source = *moved;
            corrections.push_back(std::move(c));
        }
    }
    edits.corrections = std::move(corrections);
    if (edits.empty()) {
        return std::nullopt;
    }
    return edits;
}

bool stackHasMatte(const EffectStack& effects) {
    return std::ranges::any_of(effects, [](const EffectInstance& e) {
        return std::ranges::any_of(e.masks, [](const Mask& m) { return m.shape == MaskShape::Matte || m.matte.has_value(); });
    });
}

/// Card #89: whether a mask of `media`'s footage is a Matte mask (an AI object mask: its per-frame mattes are kept in
/// the analysis cache by media time, which a trimmed copy no longer has) - on a clip of that media, in its Master Clip
/// Effects or in a redaction covering it. Such media is copied whole.
bool usesMattes(const ProjectState& state, const MediaItem& item) {
    if (stackHasMatte(item.masterEffects)) {
        return true;
    }
    if (item.subclip) {
        if (const MediaItem* master = state.findMedia(item.subclip->master); master && stackHasMatte(master->masterEffects)) {
            return true;
        }
    }
    for (const Redaction* r : redactionsCovering(state, item.id)) {
        if (r->region.shape == MaskShape::Matte || r->region.matte.has_value()) {
            return true;
        }
    }
    for (const SequencePtr& seq : state.sequences) {
        if (!seq) {
            continue;
        }
        bool found = false;
        seq->forEachTrack([&](const Track& track) {
            for (const Clip& clip : track.clips) {
                if (clip.media == item.id && stackHasMatte(clip.effects())) {
                    found = true;
                }
            }
        });
        if (found) {
            return true;
        }
    }
    return false;
}

/// 0.6 (card #74): moves the media time of an audition's other takes whose media was trimmed (`shifts`: media id ->
/// shift), each as shiftMediaTime moves a clip. True when a take changed.
bool shiftTakesMediaTime(Clip& clip, const std::map<std::uint64_t, Ticks>& shifts) {
    const Audition* a = clip.audition();
    if (!a) {
        return false;
    }
    Audition next = *a;
    bool changed = false;
    for (AuditionTake& take : next.others) {
        const auto s = shifts.find(take.media.value);
        if (!take.media.isValid() || s == shifts.end() || s->second.value == 0) {
            continue;
        }
        Clip v = takeClip(take, auditionRange(clip));
        shiftMediaTime(v, s->second);
        take = takeFromClip(v);
        changed = true;
    }
    if (changed) {
        ClipContent content = clip.content();
        content.audition = std::make_shared<const Audition>(std::move(next));
        clip.setContent(std::move(content));
    }
    return changed;
}

/// Every path the capsule holds, case-folded (Codex wave-3 P1-4): on Windows and macOS "A.wav" and "a.wav" are one
/// file, so names that differ only by case must never be handed out twice, in any folder of the capsule.
struct CapsuleNames {
    std::set<std::string> taken;

    /// A capsule moves between computers: names must differ on the strictest file system it may land on (I.7).
    static std::string keyOf(const std::filesystem::path& relative) {
        std::string k = pathToUtf8(relative.lexically_normal());
        std::ranges::replace(k, '\\', '/');
        return paths::portableNameKey(k);
    }
    bool claim(const std::filesystem::path& relative) { return taken.insert(keyOf(relative)).second; }
    /// `folder / name` with the name made valid on every OS (paths::portableName), or "<stem> (n)<ext>" in that folder
    /// when the name (in any case or Unicode normalisation) is taken.
    std::filesystem::path unique(const std::filesystem::path& folder, const std::filesystem::path& sourceName) {
        const std::filesystem::path name = pathFromUtf8(paths::portableName(pathToUtf8(sourceName)));
        const std::string stem = pathToUtf8(name.stem());
        const std::string ext = pathToUtf8(name.extension());
        std::filesystem::path candidate = folder / name;
        for (int n = 2; !claim(candidate); ++n) {
            candidate = folder / pathFromUtf8(stem + " (" + std::to_string(n) + ")" + ext);
        }
        return candidate;
    }
};

std::string randomSuffix() {
    std::random_device rd;
    return std::to_string(rd() % 1'000'000);
}

/// Removes a folder unless committed (cancel / error: no partial capsule).
struct FolderGuard {
    std::filesystem::path path;
    bool keep = false;
    ~FolderGuard() {
        if (!keep && !path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

struct MediaPlan {
    MediaPtr item;
    std::filesystem::path source;
    std::optional<TimeRange> keep; ///< trimmed copy of this range (source time); nullopt = the whole file
    /// 0.2: an item whose path names several files (an image sequence's frames, a spanned clip's parts): they are
    /// copied together into a folder of their own, keeping their names. Empty for one-file items (a layered file's
    /// item copies its PSD like any file).
    std::vector<std::filesystem::path> files;
    /// Card #89: why this item is copied whole although "trim" is on and its format could be cut (listed in the notes).
    std::string untrimmedNote;
};

/// 0.2: the files of an image sequence (its frames from the first, up to the first gap) or a spanned clip (its
/// parts), or nothing for an item of one file.
std::vector<std::filesystem::path> filesOfMedia(const std::string& path) {
    std::vector<std::filesystem::path> out;
    if (const auto sequence = parseImageSequencePath(path)) {
        if (auto found = findImageSequence(sequence->firstFramePath(), sequence->frameRate)) {
            for (std::int64_t n = 0; n < found->count; ++n) {
                out.push_back(found->framePath(found->first + n));
            }
        }
    } else if (const auto spanned = parseSpannedClipPath(path)) {
        for (std::size_t i = 0; i < spanned->parts.size(); ++i) {
            out.push_back(spanned->partPath(i));
        }
    }
    return out;
}

std::optional<TimeRange> keepRange(const MediaItem& item, const TimeRange& used, Ticks handles) {
    const Ticks duration = item.info.duration;
    const Ticks start = std::max(Ticks{}, used.start - handles);
    Ticks end = used.end() + handles;
    if (duration.value > 0) {
        end = std::min(end, duration);
    }
    if (!(start < end)) {
        return std::nullopt;
    }
    return TimeRange::fromStartEnd(start, end);
}

/// Card #89: whether the media-time data of `item` is shared with other items of one footage: a redaction covers them
/// all (subclips and time-preserving derived items share their master's redactions), or a Master Clip Effects stack is
/// inherited by subclips. Their media times must stay equal, so they are cut at the same time or not at all.
bool sharesMediaTime(const ProjectState& state, const MediaItem& item) {
    if (!item.masterEffects.empty() || !redactionsCovering(state, item.id).empty()) {
        return true;
    }
    if (item.subclip) {
        const MediaItem* master = state.findMedia(item.subclip->master);
        return master && !master->masterEffects.empty();
    }
    return false;
}

std::expected<std::vector<MediaPlan>, Error> planMedia(const ProjectState& state, const std::filesystem::path& projectFile,
                                                       const CapsuleOptions& options) {
    const auto used = usedRanges(state);
    // Card #89 (and #169): a subclip plays its master's Master Clip Effects, so a master that has some and a used subclip
    // goes into the capsule too (cut to what its subclips use) - its project would otherwise play the subclip without
    // them.
    std::map<std::uint64_t, TimeRange> masterRanges;
    for (const auto& [id, range] : used) {
        const MediaItem* item = state.findMedia(MediaId{id});
        const MediaItem* master = item && item->subclip ? state.findMedia(item->subclip->master) : nullptr;
        if (!master || master->masterEffects.empty() || used.contains(master->id.value)) {
            continue;
        }
        auto [it, inserted] = masterRanges.emplace(master->id.value, range);
        if (!inserted) {
            it->second = TimeRange::fromStartEnd(std::min(it->second.start, range.start), std::max(it->second.end(), range.end()));
        }
    }
    std::vector<MediaPlan> plan;
    for (const MediaPtr& m : state.media) {
        if (!m || m->isGenerator()) {
            continue;
        }
        const TimeRange* range = nullptr;
        if (const auto u = used.find(m->id.value); u != used.end()) {
            range = &u->second;
        } else if (const auto mr = masterRanges.find(m->id.value); mr != masterRanges.end()) {
            range = &mr->second;
        }
        if (!range && options.excludeUnused) {
            continue;
        }
        const auto found = resolveMediaPath(m->locator, projectFile);
        if (!found || m->locator.offline) {
            return std::unexpected(Error{ErrorCode::NotFound, m->name + " is offline: relink it before collecting"});
        }
        MediaPlan p;
        p.item = m;
        p.source = *found;
        p.files = filesOfMedia(pathToUtf8(*found)); // 0.2: an image sequence or a spanned clip
        if (p.files.empty() && pathToUtf8(*found).find('?') != std::string::npos) {
            p.source = mediaFileToCheck(*found); // a layered file's item: its PSD
        }
        if (options.trim && p.files.empty() && range && !m->info.isStillImage && exporter::canTrimCopy(p.source)) {
            if (usesMattes(state, *m)) {
                p.untrimmedNote = "its object masks follow the whole file";
            } else {
                p.keep = keepRange(*m, *range, options.handles);
            }
        }
        plan.push_back(std::move(p));
    }
    if (options.trim) {
        // Items that share redactions or Master Clip Effects (one footage, several library items) must keep one media
        // time: the same cut of the same file, or every one of them whole.
        std::map<std::uint64_t, std::vector<MediaPlan*>> groups;
        for (MediaPlan& p : plan) {
            if (sharesMediaTime(state, *p.item)) {
                groups[redactionRoot(state, p.item->id).value].push_back(&p);
            }
        }
        for (auto& [root, members] : groups) {
            const bool anyCut = std::ranges::any_of(members, [](const MediaPlan* p) { return p->keep.has_value(); });
            if (!anyCut || members.size() < 2) {
                continue;
            }
            const bool sameCut = std::ranges::all_of(members, [&](const MediaPlan* p) {
                std::error_code ec;
                return p->keep && p->files.empty() && std::filesystem::equivalent(p->source, members.front()->source, ec) && !ec;
            });
            if (!sameCut) {
                for (MediaPlan* p : members) {
                    if (p->keep) {
                        p->keep.reset();
                        p->untrimmedNote = "it shares redactions or Master Clip Effects with other items of the same footage";
                    }
                }
                continue;
            }
            Ticks start = members.front()->keep->start;
            Ticks end = members.front()->keep->end();
            for (const MediaPlan* p : members) {
                start = std::min(start, p->keep->start);
                end = std::max(end, p->keep->end());
            }
            for (MediaPlan* p : members) {
                p->keep = TimeRange::fromStartEnd(start, end);
            }
        }
    }
    return plan;
}

/// Card 003: `chosen` and every sequence they play (the closure), by id.
std::set<std::uint64_t> sequenceClosure(const ProjectState& state, const std::vector<SequenceId>& chosen) {
    std::set<std::uint64_t> keep;
    std::vector<std::uint64_t> todo;
    for (const SequenceId id : chosen) {
        if (state.findSequence(id) && keep.insert(id.value).second) {
            todo.push_back(id.value);
        }
    }
    while (!todo.empty()) {
        const Sequence* seq = state.findSequence(SequenceId{todo.back()});
        todo.pop_back();
        seq->forEachTrack([&](const Track& track) {
            for (const Clip& clip : track.clips) {
                forEachTakeSource(clip, [&](MediaId, SequenceId played) {
                    if (played.isValid() && state.findSequence(played) && keep.insert(played.value).second) {
                        todo.push_back(played.value);
                    }
                });
            }
        });
    }
    return keep;
}

/// Card 003: `state` with only the sequences in `keep` (and the review notes on them).
ProjectState restrictedToSequences(const ProjectState& state, const std::set<std::uint64_t>& keep) {
    ProjectState out = state;
    std::erase_if(out.sequences, [&](const SequencePtr& s) { return !s || !keep.contains(s->id.value); });
    const auto dropped = [&](const ReviewNote& n) { return !keep.contains(n.sequence.value); };
    if (std::ranges::any_of(std::as_const(out.reviewNotes).get(), dropped)) {
        std::erase_if(out.reviewNotes.edit(), dropped);
    }
    return out;
}

std::int64_t estimate(const MediaPlan& p) {
    if (!p.files.empty()) {
        std::int64_t total = 0;
        for (const std::filesystem::path& f : p.files) {
            total += sizeOf(f);
        }
        return total;
    }
    const std::int64_t size = sizeOf(p.source);
    if (!p.keep || p.item->info.duration.value <= 0) {
        return size;
    }
    // A share of the file, plus a GOP or two on each side.
    const double share = std::clamp(p.keep->duration.toSeconds() / p.item->info.duration.toSeconds(), 0.0, 1.0);
    return std::min(size, static_cast<std::int64_t>(size * share * 1.1) + 4 * 1024 * 1024);
}

/// The asset references ("asset:<id>") effects in the project use.
std::set<std::string> referencedAssets(const ProjectState& state) {
    std::set<std::string> out;
    const auto add = [&](const EffectStack& stack) {
        for (const EffectInstance& e : stack) {
            for (const EffectData& d : e.data) {
                if (d.text()) {
                    out.insert(*d.text());
                }
            }
        }
    };
    for (const SequencePtr& seq : state.sequences) {
        if (!seq) {
            continue;
        }
        add(seq->effects);
        add(seq->audioEffects);
        seq->forEachTrack([&](const Track& t) {
            add(t.effects);
            for (const Clip& c : t.clips) {
                add(c.effects());
                if (const Audition* a = c.audition()) { // 0.6 (card #74): the other takes' effects
                    for (const AuditionTake& take : a->others) {
                        add(take.contentOrDefault().effects);
                    }
                }
            }
        });
    }
    for (const MediaPtr& m : state.media) { // 0.6 (card #71): Master Clip Effects
        if (m) {
            add(m->masterEffects);
        }
    }
    for (const Redaction& r : state.redactions) { // 0.7 (card 102): the sound that replaces a bleep
        if (r.sound.isValid()) {
            out.insert(assetReference(r.sound));
        }
    }
    return out;
}

/// The assets the capsule carries: those effects use (or all of them unless unused files are left out).
std::vector<const ProjectAsset*> assetsToCollect(const ProjectState& state, const CapsuleOptions& options) {
    const std::set<std::string> used = referencedAssets(state);
    std::vector<const ProjectAsset*> out;
    for (const ProjectAsset& a : state.assets) {
        if (!options.excludeUnused || used.contains(assetReference(a.id))) {
            out.push_back(&a);
        }
    }
    return out;
}

/// Card #89: the project-level data keyed to a trimmed item's media time, `shift` earlier (`shifts`: media id -> shift):
/// redactions (shifted by what their footage was cut by: the shift of any trimmed item that shares their redaction root,
/// which planMedia keeps equal) and transcript edits.
void shiftProjectMediaTime(ProjectState& out, const ProjectState& original, const std::map<std::uint64_t, Ticks>& shifts) {
    std::map<std::uint64_t, Ticks> rootShift;
    for (const auto& [id, shift] : shifts) {
        if (shift.value != 0) {
            rootShift[redactionRoot(original, MediaId{id}).value] = shift;
        }
    }
    if (!rootShift.empty() && !out.redactions.empty()) {
        std::vector<Redaction> next;
        for (const Redaction& r : out.redactions) {
            const auto it = rootShift.find(redactionRoot(original, r.media).value);
            if (it == rootShift.end()) {
                next.push_back(r);
            } else if (auto moved = shiftedRedaction(r, it->second)) {
                next.push_back(std::move(*moved));
            }
        }
        out.redactions = std::move(next);
    }
    if (!out.transcriptEdits.empty()) {
        std::vector<TranscriptEdits> next;
        for (const TranscriptEdits& e : out.transcriptEdits) {
            const auto it = shifts.find(e.media.value);
            if (it == shifts.end() || it->second.value == 0) {
                next.push_back(e);
            } else if (auto moved = shiftedTranscriptEdits(e, it->second)) {
                next.push_back(std::move(*moved));
            }
        }
        out.transcriptEdits = std::move(next);
    }
}

// ---- Layout of Media/ (0.7, card 076) -------------------------------------------------------------------------------

/// The folder of Media/ each collected file goes in, from the layout chosen. Folder names are bin names or the names of
/// the folders the files were in: untrusted text. Each is made valid on every OS (paths::portableName: no separator,
/// "." or "..", reserved name, trailing dot), names that differ only by case or Unicode form share the spelling used
/// first (the capsule travels to systems that tell them apart), and a path longer than kMaxLayoutDepth folders or
/// kMaxLayoutChars characters keeps its last folders.
class MediaFolders {
public:
    static constexpr std::size_t kMaxLayoutDepth = 8;
    static constexpr std::size_t kMaxLayoutChars = 120;

    MediaFolders(const ProjectState& state, CapsuleLayout layout, const std::vector<MediaPlan>& plan)
        : m_state(state), m_layout(layout) {
        if (layout == CapsuleLayout::SourceFolders) {
            prepareSourceTree(plan);
        }
    }

    /// The folder below Media/ for `plan` (empty = Media/ itself).
    std::filesystem::path folderFor(const MediaPlan& plan) {
        std::vector<std::string> segments;
        switch (m_layout) {
        case CapsuleLayout::Flat:
            return {};
        case CapsuleLayout::Bins:
            segments = binSegments(plan.item->bin);
            break;
        case CapsuleLayout::SourceFolders:
            segments = sourceSegments(plan.source);
            break;
        }
        return canonical(std::move(segments));
    }

    /// Files whose folder path was shortened.
    int shortened() const { return m_shortened; }

private:
    using Segments = std::vector<std::string>;

    /// "D:\Footage\Day 1\a.mp4" -> {"Footage", "Day 1"} (no drive, no file name); the volume is separate.
    static Segments directoryNames(const std::filesystem::path& file) {
        Segments out;
        const std::filesystem::path parent = mediaFileToCheck(file).lexically_normal().parent_path();
        const std::filesystem::path relative = parent.relative_path();
        for (const std::filesystem::path& part : relative) {
            const std::string name = pathToUtf8(part);
            if (!name.empty() && name != "." && name != "..") {
                out.push_back(name);
            }
        }
        return out;
    }
    /// "D:" -> "D"; a network share "\\server\share" -> "server share". Empty on a system without volume names.
    static std::string volumeName(const std::filesystem::path& file) {
        std::string root = pathToUtf8(mediaFileToCheck(file).lexically_normal().root_name());
        std::string out;
        for (const char c : root) {
            if (c == ':' || c == '\\' || c == '/') {
                if (!out.empty() && out.back() != ' ' && c != ':') {
                    out += ' ';
                }
                continue;
            }
            out += c;
        }
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        return out;
    }

    void prepareSourceTree(const std::vector<MediaPlan>& plan) {
        // The common folder of each volume's files; with more than one volume each keeps a folder of its own name.
        std::map<std::string, Segments> common;
        for (const MediaPlan& p : plan) {
            const Segments names = directoryNames(p.source);
            const std::string volume = volumeName(p.source);
            const auto [it, added] = common.try_emplace(volume, names);
            if (!added) {
                Segments& prefix = it->second;
                std::size_t n = 0;
                while (n < prefix.size() && n < names.size() && paths::portableNameKey(prefix[n]) == paths::portableNameKey(names[n])) {
                    ++n;
                }
                prefix.resize(n);
            }
        }
        m_common = std::move(common);
        m_volumes = m_common.size();
    }

    Segments sourceSegments(const std::filesystem::path& source) const {
        Segments names = directoryNames(source);
        const std::string volume = volumeName(source);
        const auto it = m_common.find(volume);
        const std::size_t skip = it == m_common.end() ? 0 : std::min(it->second.size(), names.size());
        Segments out;
        if (m_volumes > 1) {
            out.push_back(volume.empty() ? std::string("Files") : volume);
        }
        out.insert(out.end(), names.begin() + static_cast<std::ptrdiff_t>(skip), names.end());
        return out;
    }

    /// The bins from the root down to `bin`, by name. A damaged project may have a loop or a very deep chain: bounded.
    Segments binSegments(BinId bin) const {
        Segments reversed;
        std::set<std::uint64_t> seen;
        for (BinId at = bin; at.isValid() && reversed.size() < 64 && seen.insert(at.value).second;) {
            const Bin* b = m_state.findBin(at);
            if (!b) {
                break;
            }
            reversed.push_back(b->name);
            at = b->parent;
        }
        return Segments(reversed.rbegin(), reversed.rend());
    }

    std::filesystem::path canonical(Segments segments) {
        std::filesystem::path out;
        std::string parentKey;
        Segments valid;
        for (const std::string& raw : segments) {
            valid.push_back(paths::portableName(raw));
        }
        // Keep the deepest folders when the path is too long for the strictest system (Windows' 260 characters
        // include the capsule's own location).
        bool cut = false;
        while (valid.size() > kMaxLayoutDepth) {
            valid.erase(valid.begin());
            cut = true;
        }
        const auto chars = [&] {
            std::size_t n = 0;
            for (const std::string& s : valid) {
                n += s.size() + 1;
            }
            return n;
        };
        while (valid.size() > 1 && chars() > kMaxLayoutChars) {
            valid.erase(valid.begin());
            cut = true;
        }
        if (cut) {
            ++m_shortened;
        }
        for (std::string& name : valid) {
            if (name.size() > 60) { // one folder name is never allowed to eat the budget
                std::size_t end = 60;
                while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0U) == 0x80U) {
                    --end;
                }
                name.resize(end);
                name = paths::portableName(name);
            }
            const std::string key = parentKey + "/" + paths::portableNameKey(name);
            const auto [it, added] = m_spelling.try_emplace(key, name);
            out /= pathFromUtf8(it->second);
            parentKey = key;
        }
        return out;
    }

    const ProjectState& m_state;
    CapsuleLayout m_layout;
    std::map<std::string, Segments> m_common;
    std::size_t m_volumes = 0;
    std::map<std::string, std::string> m_spelling; // folded path -> the spelling of its first use
    int m_shortened = 0;
};

} // namespace

const char* capsuleLayoutName(CapsuleLayout layout) {
    switch (layout) {
    case CapsuleLayout::Flat:
        return "flat";
    case CapsuleLayout::Bins:
        return "bins";
    case CapsuleLayout::SourceFolders:
        return "folders";
    }
    return "flat";
}

CapsuleLayout capsuleLayoutFromName(const std::string& name) {
    if (name == "bins") {
        return CapsuleLayout::Bins;
    }
    if (name == "folders") {
        return CapsuleLayout::SourceFolders;
    }
    return CapsuleLayout::Flat;
}

namespace detail {

std::expected<std::int64_t, Error> copyChecked(std::istream& in, std::ostream& out, std::int64_t expectedBytes,
                                               std::stop_token stop, const std::function<void(std::int64_t)>& onChunk) {
    std::vector<char> buffer(4 * 1024 * 1024);
    std::int64_t written = 0;
    while (in) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Collect Files cancelled"});
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) {
            out.write(buffer.data(), n);
            written += n;
            if (onChunk) {
                onChunk(n);
            }
        }
    }
    if (in.bad() || !in.eof()) {
        return std::unexpected(Error{ErrorCode::IoError, "the file could not be read to its end (a read error after " +
                                                             std::to_string(written) + " of " + std::to_string(expectedBytes) +
                                                             " bytes)"});
    }
    if (written != expectedBytes) {
        return std::unexpected(Error{ErrorCode::IoError, "only " + std::to_string(written) + " of " + std::to_string(expectedBytes) +
                                                             " bytes could be read (the file changed or the drive dropped)"});
    }
    out.flush();
    if (!out) {
        return std::unexpected(Error{ErrorCode::IoError, "the copy could not be written"});
    }
    return written;
}

} // namespace detail

std::vector<SequenceId> capsuleSequences(const ProjectState& state, const std::vector<SequenceId>& chosen) {
    std::vector<SequenceId> out;
    for (const std::uint64_t id : sequenceClosure(state, chosen)) {
        out.push_back(SequenceId{id});
    }
    return out;
}

std::vector<MediaId> capsuleMedia(const ProjectState& fullState, const std::filesystem::path& projectFile,
                                  const CapsuleOptions& options) {
    ProjectState restricted;
    if (!options.sequences.empty()) {
        restricted = restrictedToSequences(fullState, sequenceClosure(fullState, options.sequences));
    }
    const ProjectState& state = options.sequences.empty() ? fullState : restricted;
    std::vector<MediaId> out;
    if (auto plan = planMedia(state, projectFile, options)) {
        for (const MediaPlan& p : *plan) {
            out.push_back(p.item->id);
        }
    }
    std::sort(out.begin(), out.end(), [](MediaId a, MediaId b) { return a.value < b.value; });
    return out;
}

std::int64_t capsuleBytesNeeded(const ProjectState& fullState, const std::filesystem::path& projectFile,
                                const CapsuleOptions& options) {
    // Card 003: only the chosen sequences (and what they play) count.
    ProjectState restricted;
    if (!options.sequences.empty()) {
        restricted = restrictedToSequences(fullState, sequenceClosure(fullState, options.sequences));
    }
    const ProjectState& state = options.sequences.empty() ? fullState : restricted;
    std::int64_t total = 1024 * 1024; // project + manifest
    if (auto plan = planMedia(state, projectFile, options)) {
        for (const MediaPlan& p : *plan) {
            total += estimate(p);
            if (options.includeProxies && !options.trim && p.item->locator.proxy) {
                total += sizeOf(pathFromUtf8(p.item->locator.proxy->path));
            }
        }
    }
    for (const CapsuleExtraFile& e : options.extras) {
        total += sizeOf(e.source);
    }
    for (const ProjectAsset* a : assetsToCollect(state, options)) {
        if (const auto found = resolveMediaPath(a->locator, projectFile)) {
            total += sizeOf(*found);
        }
    }
    return total;
}

std::expected<CapsuleResult, Error> collectProject(const ProjectState& fullState, const std::filesystem::path& projectFile,
                                                   const CapsuleOptions& options, std::stop_token stop,
                                                   const std::function<void(const CapsuleProgress&)>& onProgress) {
    const auto cancelled = [] { return std::unexpected(Error{ErrorCode::Cancelled, "Collect Files cancelled"}); };
    // Card 003: only the chosen sequences and what they play go in; everything below works on that state.
    ProjectState restricted;
    std::vector<std::string> collectedNames;
    if (!options.sequences.empty()) {
        const std::set<std::uint64_t> keep = sequenceClosure(fullState, options.sequences);
        if (keep.empty()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "choose at least one sequence to collect"});
        }
        restricted = restrictedToSequences(fullState, keep);
    }
    const ProjectState& state = options.sequences.empty() ? fullState : restricted;
    for (const SequencePtr& s : state.sequences) {
        collectedNames.push_back(s->name);
    }
    const std::string requested =
        !options.name.empty() ? options.name : !projectFile.empty() ? pathToUtf8(projectFile.stem()) : std::string("Untitled");
    // The capsule's own folder: one plain name, valid on every OS (no "../", no separators; I.7 portability audit).
    if (!paths::isPlainName(requested)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "\"" + requested + "\" is not a folder name"});
    }
    const std::string name = paths::portableName(requested);
    const std::filesystem::path folder = options.destination / pathFromUtf8(name);
    const std::filesystem::path finalProject = folder / pathFromUtf8(name + std::string(io::kProjectExtension));
    std::error_code ec;
    if (std::filesystem::exists(folder, ec) && !std::filesystem::is_empty(folder, ec)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, pathToUtf8(folder) + " already exists"});
    }
    CapsuleProgress progress;
    const auto report = [&](std::string phase, std::string current = {}) {
        progress.phase = std::move(phase);
        progress.current = std::move(current);
        if (onProgress) {
            onProgress(progress);
        }
    };
    report("checking");

    auto plan = planMedia(state, projectFile, options);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    const std::int64_t needed = capsuleBytesNeeded(state, projectFile, options);
    std::filesystem::create_directories(options.destination, ec);
    const auto space = std::filesystem::space(options.destination, ec);
    if (!ec && static_cast<std::int64_t>(space.available) < needed + kSpaceMargin) {
        return std::unexpected(Error{ErrorCode::IoError,
                                     std::format("Not enough disk space in {}: the capsule needs about {} MB, {} MB are free",
                                                 pathToUtf8(options.destination), (needed + kSpaceMargin) >> 20,
                                                 static_cast<std::int64_t>(space.available >> 20))});
    }
    progress.bytesTotal = needed;
    const std::vector<const ProjectAsset*> assets = assetsToCollect(state, options);
    progress.filesTotal = static_cast<int>(plan->size() + options.extras.size() + assets.size());

    FolderGuard temp{options.destination / pathFromUtf8(name + ".wabcut-partial-" + randomSuffix())};
    if (std::filesystem::exists(temp.path, ec)) {
        temp.keep = true; // not ours: never removed
        return std::unexpected(Error{ErrorCode::IoError, pathToUtf8(temp.path) + " already exists"});
    }
    std::filesystem::create_directories(temp.path / "Media", ec);
    if (ec || paths::isLink(temp.path)) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot create " + pathToUtf8(temp.path) + ": " + ec.message()});
    }
    // Names inside the capsule: the project and the manifest first, then every copied file (case-folded).
    CapsuleNames names;
    names.claim(finalProject.filename());
    names.claim("manifest.json");

    // Copies a file in 4 MiB chunks (progress, cancel) and keeps its modification time (caches keyed by it stay
    // valid for the copy). The copy is created exclusively: nothing in the capsule is ever overwritten. A copy that is
    // not the whole file (a read error, a file that changed while it was copied) fails and is deleted (card #90).
    const auto copyFile = [&](const std::filesystem::path& from, const std::filesystem::path& to) -> std::expected<void, Error> {
        std::filesystem::create_directories(to.parent_path(), ec);
        std::ifstream in(from, std::ios::binary);
        std::ofstream out(to, std::ios::binary | std::ios::noreplace);
        if (!in || !out) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot copy " + pathToUtf8(from) + " to " + pathToUtf8(to)});
        }
        std::error_code sizeEc;
        const auto size = std::filesystem::file_size(from, sizeEc);
        if (sizeEc) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot read the size of " + pathToUtf8(from)});
        }
        const auto copied = detail::copyChecked(in, out, static_cast<std::int64_t>(size), stop, [&](std::int64_t n) {
            progress.bytesDone += n;
            report(progress.phase, progress.current);
        });
        if (copied) {
            out.close();
            if (!out) {
                std::filesystem::remove(to, ec);
                return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + pathToUtf8(to)});
            }
            std::error_code timeEc;
            std::filesystem::last_write_time(to, std::filesystem::last_write_time(from, timeEc), timeEc);
            return {};
        }
        out.close();
        std::filesystem::remove(to, ec); // never leave a truncated copy behind
        Error error = copied.error();
        if (error.code != ErrorCode::Cancelled) {
            error.message = "Cannot copy " + pathToUtf8(from) + ": " + error.message;
        }
        return std::unexpected(std::move(error));
    };

    CapsuleResult result;
    ProjectState out = state;
    std::map<std::uint64_t, MediaPtr> replaced;
    std::map<std::uint64_t, Ticks> shifts;
    const std::string proxyFolderName = name + " Proxies";
    MediaFolders folders(state, options.layout, *plan); // card 076: where below Media/ each file goes
    for (const MediaPlan& p : *plan) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        const std::filesystem::path mediaFolder = std::filesystem::path("Media") / folders.folderFor(p);
        if (!p.files.empty()) {
            // 0.2: an image sequence's frames or a spanned clip's parts, together in a folder of their own.
            const std::filesystem::path relativeFolder = names.unique(mediaFolder, mediaFileToCheck(p.source).stem());
            for (const std::filesystem::path& f : p.files) {
                report("copying", pathToUtf8(f.filename()));
                const std::filesystem::path relativeFile = relativeFolder / f.filename();
                if (auto c = copyFile(f, temp.path / relativeFile); !c) {
                    return std::unexpected(c.error());
                }
                result.files.push_back({relativeFile, "media", sizeOf(temp.path / relativeFile),
                                        fingerprintFile(temp.path / relativeFile).value_or(SourceFingerprint{}).sampleHash,
                                        pathToUtf8(f), false});
            }
            const std::filesystem::path firstFinal = folder / relativeFolder / p.files.front().filename();
            const std::string finalPath = relinkedMediaPath(pathToUtf8(p.source), firstFinal);
            const auto fingerprint = fingerprintFile(temp.path / relativeFolder / p.files.front().filename());
            if (!fingerprint) {
                return std::unexpected(fingerprint.error());
            }
            auto item = std::make_shared<MediaItem>(*p.item);
            item->locator.path = finalPath;
            item->locator.relativePath = relativeMediaPath(finalProject, pathFromUtf8(finalPath));
            item->locator.fingerprint = *fingerprint;
            item->locator.offline = false;
            item->locator.proxy.reset();
            if (p.item->locator.proxy && options.includeProxies) {
                result.notes.push_back(p.item->name + ": proxy left out (make it again from the collected files)");
            }
            ++result.mediaFiles;
            ++progress.filesDone;
            replaced[p.item->id.value] = std::move(item);
            continue;
        }
        const std::filesystem::path relative = names.unique(mediaFolder, p.source.filename());
        const std::filesystem::path staged = temp.path / relative;
        if (std::filesystem::exists(staged, ec)) {
            return std::unexpected(Error{ErrorCode::IoError, pathToUtf8(staged) + " already exists"});
        }
        std::filesystem::create_directories(staged.parent_path(), ec); // a trimmed copy is written by the exporter
        auto item = std::make_shared<MediaItem>(*p.item);
        bool trimmed = false;
        if (p.keep) {
            report("trimming", pathToUtf8(p.source.filename()));
            const std::int64_t before = progress.bytesDone;
            const std::int64_t share = estimate(p);
            auto copy = exporter::trimmedCopy(pathToUtf8(p.source), staged, *p.keep, stop, [&](double f) {
                progress.bytesDone = before + static_cast<std::int64_t>(f * static_cast<double>(share));
                report("trimming", pathToUtf8(p.source.filename()));
            });
            if (!copy) {
                if (copy.error().code == ErrorCode::Cancelled) {
                    return cancelled();
                }
                result.notes.push_back(p.item->name + ": copied whole (" + copy.error().message + ")");
            } else {
                progress.bytesDone = before + share;
                auto described = describeMediaFile(staged, {});
                if (!described) {
                    return std::unexpected(described.error());
                }
                item->info = described->info;
                item->info.startTimecode = p.item->info.startTimecode; // the probe does not read it; shifted below
                shifts[p.item->id.value] = copy->shift;
                if (copy->shift.value != 0) {
                    shiftMediaItemTime(*item, copy->shift); // card #89: markers, marks, subclip range, Master Clip Effects
                }
                trimmed = true;
                ++result.trimmedFiles;
            }
        }
        if (!trimmed && options.trim && !p.untrimmedNote.empty()) {
            result.notes.push_back(p.item->name + ": copied whole (" + p.untrimmedNote + ")");
        }
        if (!trimmed) {
            report("copying", pathToUtf8(p.source.filename()));
            if (auto c = copyFile(p.source, staged); !c) {
                return std::unexpected(c.error());
            }
        }
        const auto fingerprint = fingerprintFile(staged);
        if (!fingerprint) {
            return std::unexpected(fingerprint.error());
        }
        const std::filesystem::path finalPath = folder / relative;
        // 0.2: a layered file's item keeps its layers (its path names the PSD and them).
        item->locator.path = relinkedMediaPath(p.item->locator.path, finalPath);
        item->locator.relativePath = relativeMediaPath(finalProject, pathFromUtf8(item->locator.path));
        item->locator.fingerprint = *fingerprint;
        item->locator.offline = false;
        item->locator.proxy.reset();
        if (p.item->locator.proxy && options.includeProxies) {
            const std::filesystem::path proxySource = pathFromUtf8(p.item->locator.proxy->path);
            if (trimmed) {
                result.notes.push_back(p.item->name + ": proxy left out (a trimmed copy needs its own)");
            } else if (isFile(proxySource)) {
                const std::filesystem::path proxyRelative = names.unique(pathFromUtf8(proxyFolderName), proxySource.filename());
                report("copying", pathToUtf8(proxySource.filename()));
                if (auto c = copyFile(proxySource, temp.path / proxyRelative); !c) {
                    return std::unexpected(c.error());
                }
                ProxyMedia proxy = *p.item->locator.proxy;
                proxy.path = pathToUtf8(folder / proxyRelative);
                item->locator.proxy = std::move(proxy);
                result.files.push_back({proxyRelative, "proxy", sizeOf(temp.path / proxyRelative),
                                        fingerprintFile(temp.path / proxyRelative).value_or(SourceFingerprint{}).sampleHash,
                                        pathToUtf8(proxySource), false});
            }
        }
        result.files.push_back({relative, "media", sizeOf(staged), fingerprint->sampleHash, pathToUtf8(p.source), trimmed});
        ++result.mediaFiles;
        ++progress.filesDone;
        replaced[p.item->id.value] = std::move(item);
    }
    if (folders.shortened() > 0) {
        result.notes.push_back(counted(folders.shortened(), "file") + std::string(" went") +
                               " into a shorter folder path than " + (options.layout == CapsuleLayout::Bins ? "its bins" : "its folders") +
                               " (more than " + std::to_string(MediaFolders::kMaxLayoutDepth) + " folders deep or " +
                               std::to_string(MediaFolders::kMaxLayoutChars) + " characters): only the last folders were kept.");
    }
    for (const CapsuleExtraFile& e : options.extras) {
        if (!paths::isContainedRelative(e.relative)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         pathToUtf8(e.relative) + " is not a place inside the capsule"});
        }
        if (!isFile(e.source)) {
            result.notes.push_back(pathToUtf8(e.source.filename()) + ": not found, left out");
            continue;
        }
        const std::filesystem::path relative = names.unique(e.relative.lexically_normal().parent_path(), e.relative.filename());
        report("copying", pathToUtf8(e.source.filename()));
        if (auto c = copyFile(e.source, temp.path / relative); !c) {
            return std::unexpected(c.error());
        }
        result.files.push_back({relative, e.role, sizeOf(temp.path / relative),
                                fingerprintFile(temp.path / relative).value_or(SourceFingerprint{}).sampleHash,
                                pathToUtf8(e.source), false});
        ++progress.filesDone;
    }

    // LUT assets (I.5): copied whole into "LUTs/", their locators pointed there; an offline one stays offline (noted).
    std::vector<ProjectAsset> collectedAssets;
    for (const ProjectAsset* a : assets) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        ProjectAsset copy = *a;
        const auto found = a->locator.offline ? std::nullopt : resolveMediaPath(a->locator, projectFile);
        if (!found) {
            result.notes.push_back(a->name + ": the LUT file is offline, left out (relink it and collect again)");
            collectedAssets.push_back(std::move(copy));
            continue;
        }
        const std::filesystem::path relative = names.unique("LUTs", found->filename());
        report("copying", pathToUtf8(found->filename()));
        if (auto c = copyFile(*found, temp.path / relative); !c) {
            return std::unexpected(c.error());
        }
        const auto fingerprint = fingerprintFile(temp.path / relative);
        const std::filesystem::path finalPath = folder / relative;
        copy.locator.path = pathToUtf8(finalPath);
        copy.locator.relativePath = relativeMediaPath(finalProject, finalPath);
        copy.locator.fingerprint = fingerprint.value_or(SourceFingerprint{});
        copy.locator.offline = false;
        result.files.push_back({relative, "lut", sizeOf(temp.path / relative), copy.locator.fingerprint.sampleHash,
                                pathToUtf8(*found), false});
        ++result.lutFiles;
        ++progress.filesDone;
        collectedAssets.push_back(std::move(copy));
    }
    out.assets = std::move(collectedAssets);

    // The project: collected media only, locators into the capsule, media time moved for trimmed copies.
    report("saving");
    std::vector<MediaPtr> media;
    for (const MediaPtr& m : out.media) {
        if (!m) {
            continue;
        }
        if (m->isGenerator()) {
            media.push_back(m); // titles: no file
        } else if (const auto it = replaced.find(m->id.value); it != replaced.end()) {
            media.push_back(it->second);
        }
    }
    out.media = std::move(media);
    if (!shifts.empty()) {
        shiftProjectMediaTime(out, state, shifts);
        for (SequencePtr& seqPtr : out.sequences) {
            if (!seqPtr) {
                continue;
            }
            auto seq = std::make_shared<Sequence>(*seqPtr);
            for (std::vector<TrackPtr>* list : {&seq->videoTracks, &seq->audioTracks}) {
                for (TrackPtr& trackPtr : *list) {
                    if (!trackPtr) {
                        continue;
                    }
                    std::vector<Clip> clips(trackPtr->clips.begin(), trackPtr->clips.end());
                    bool changed = false;
                    for (Clip& clip : clips) {
                        if (const auto s = shifts.find(clip.media.value); s != shifts.end() && s->second.value != 0) {
                            shiftMediaTime(clip, s->second);
                            changed = true;
                        }
                        changed = shiftTakesMediaTime(clip, shifts) || changed; // 0.6 (card #74)
                    }
                    if (changed) {
                        auto track = std::make_shared<Track>(*trackPtr);
                        track->clips = ClipList(std::move(clips));
                        trackPtr = std::move(track);
                    }
                }
            }
            seqPtr = std::move(seq);
        }
    }
    const std::filesystem::path stagedProject = temp.path / finalProject.filename();
    if (auto saved = io::saveProject(out, stagedProject, io::SaveOptions{0}); !saved) {
        return std::unexpected(saved.error());
    }
    result.files.insert(result.files.begin(),
                        CapsuleFile{finalProject.filename(), "project", sizeOf(stagedProject),
                                    fingerprintFile(stagedProject).value_or(SourceFingerprint{}).sampleHash,
                                    pathToUtf8(projectFile), false});

    // The manifest.
    nlohmann::json files = nlohmann::json::array();
    for (const CapsuleFile& f : result.files) {
        result.bytes += f.bytes;
        std::string relative = pathToUtf8(f.relative);
        std::ranges::replace(relative, '\\', '/');
        files.push_back({{"path", relative},
                         {"role", f.role},
                         {"bytes", f.bytes},
                         {"sampleHash", f.sampleHash},
                         {"source", f.source},
                         {"trimmed", f.trimmed}});
    }
    const auto now = std::chrono::system_clock::now();
    const nlohmann::json manifest = {
        {"format", "wabcut-capsule"},
        {"version", 1},
        {"wabcutVersion", options.wabcutVersion},
        {"created", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(now))},
        {"project", pathToUtf8(finalProject.filename())},
        {"sourceProject", pathToUtf8(projectFile)},
        {"trimmed", options.trim},
        {"layout", capsuleLayoutName(options.layout)}, // card 076
        {"handlesSeconds", options.handles.toSeconds()},
        {"sequences", collectedNames}, // card 003: the sequences the capsule holds
        {"files", files},
        {"notes", result.notes},
    };
    {
        std::ofstream m(temp.path / "manifest.json", std::ios::binary | std::ios::trunc);
        m << manifest.dump(2);
        if (!m) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write the manifest"});
        }
    }
    if (stop.stop_requested()) {
        return cancelled();
    }

    if (options.verify) {
        // Before the capsule is committed: copied somewhere else, as if on another machine, only the relative paths
        // can find the media. A capsule that fails is not kept (Codex wave-3 P1-4: never a "successful" damaged one).
        report("verifying");
        std::error_code tempEc;
        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(tempEc);
        FolderGuard check{tempEc ? std::filesystem::path{} : tempRoot / pathFromUtf8("wabcut-capsule-check-" + randomSuffix())};
        ec = tempEc;
        if (!ec) {
            std::filesystem::create_directories(check.path / folder.filename(), ec);
        }
        if (!ec) {
            std::filesystem::copy(temp.path, check.path / folder.filename(), std::filesystem::copy_options::recursive, ec);
        }
        const std::filesystem::path copiedProject = check.path / folder.filename() / finalProject.filename();
        auto loaded = io::loadProject(copiedProject);
        if (ec || !loaded) {
            result.notes.push_back("verification could not open the copy: " +
                                   (ec ? ec.message() : loaded ? std::string() : loaded.error().message));
        } else {
            ProjectState elsewhere = *loaded;
            for (MediaPtr& m : elsewhere.media) {
                if (m && !m->isGenerator()) {
                    auto moved = std::make_shared<MediaItem>(*m);
                    moved->locator.path.clear(); // the capsule's absolute paths would still find the originals here
                    m = std::move(moved);
                }
            }
            for (ProjectAsset& a : elsewhere.assets) {
                if (!a.locator.offline) {
                    a.locator.path.clear(); // the same for LUTs
                }
            }
            const ProjectState checked = withMediaChecked(elsewhere, copiedProject);
            result.offlineAfterReopen = static_cast<int>(std::ranges::count_if(
                checked.media, [](const MediaPtr& m) { return m && !m->isGenerator() && m->locator.offline; }));
            // LUTs that were collected must be found too (ones already offline were noted above).
            for (const ProjectAsset& a : checked.assets) {
                const ProjectAsset* before = out.findAsset(a.id);
                if (a.locator.offline && before && !before->locator.offline) {
                    ++result.offlineAfterReopen;
                }
            }
            for (const CapsuleFile& f : result.files) {
                const auto fp = fingerprintFile(check.path / folder.filename() / f.relative);
                if (!fp || fp->sampleHash != f.sampleHash) {
                    ++result.fingerprintMismatches;
                }
            }
            result.verified = result.offlineAfterReopen == 0 && result.fingerprintMismatches == 0;
        }
        if (!result.verified) {
            std::string why = result.notes.empty() ? std::string() : " (" + result.notes.back() + ")";
            if (result.fingerprintMismatches > 0 || result.offlineAfterReopen > 0) {
                why = std::format(": {} differing from what was copied, {} that would be offline",
                                  counted(static_cast<std::int64_t>(result.fingerprintMismatches), "file"),
                                  counted(static_cast<std::int64_t>(result.offlineAfterReopen), "clip"));
            }
            return std::unexpected(Error{ErrorCode::IoError, "The capsule failed its check and was not kept" + why});
        }
    }

    std::filesystem::remove(folder, ec); // an empty folder of the same name
    std::filesystem::rename(temp.path, folder, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot finish the capsule: " + ec.message()});
    }
    temp.keep = true;
    result.folder = folder;
    result.sequences = collectedNames;
    result.projectFile = finalProject;
    result.manifest = folder / "manifest.json";
    report("done");
    return result;
}

} // namespace wabcut::editor
