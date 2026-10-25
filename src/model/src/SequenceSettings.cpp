// Sequence settings, duplicates and New Sequence From Clip (P.2, E5): SequenceSettings.h; the name tables of the sequence
// settings enums (Sequence.h).

#include <wabcut/model/SequenceSettings.h>

#include <wabcut/model/FieldOptions.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/Vfr.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

namespace wabcut {

namespace {

template <class E, std::size_t N>
std::string_view nameOf(const std::array<std::pair<E, std::string_view>, N>& table, E value) {
    for (const auto& [v, name] : table) {
        if (v == value) {
            return name;
        }
    }
    return table[0].second;
}

template <class E, std::size_t N>
std::optional<E> valueOf(const std::array<std::pair<E, std::string_view>, N>& table, std::string_view text) {
    for (const auto& [v, name] : table) {
        if (name == text) {
            return v;
        }
    }
    return std::nullopt;
}

constexpr std::array<std::pair<FieldOrder, std::string_view>, 3> kFieldOrders{{
    {FieldOrder::Progressive, "progressive"},
    {FieldOrder::UpperFirst, "upperFirst"},
    {FieldOrder::LowerFirst, "lowerFirst"},
}};
constexpr std::array<std::pair<VideoTimeDisplay, std::string_view>, 4> kVideoDisplays{{
    {VideoTimeDisplay::Timecode, "timecode"},
    {VideoTimeDisplay::Frames, "frames"},
    {VideoTimeDisplay::Feet16mm, "feet16mm"},
    {VideoTimeDisplay::Feet35mm, "feet35mm"},
}};
constexpr std::array<std::pair<AudioTimeDisplay, std::string_view>, 2> kAudioDisplays{{
    {AudioTimeDisplay::AudioSamples, "audioSamples"},
    {AudioTimeDisplay::Milliseconds, "milliseconds"},
}};

} // namespace

std::string_view toString(FieldOrder order) { return nameOf(kFieldOrders, order); }
std::optional<FieldOrder> fieldOrderFromString(std::string_view text) { return valueOf(kFieldOrders, text); }
std::string_view toString(VideoTimeDisplay display) { return nameOf(kVideoDisplays, display); }
std::optional<VideoTimeDisplay> videoTimeDisplayFromString(std::string_view text) { return valueOf(kVideoDisplays, text); }
std::string_view toString(AudioTimeDisplay display) { return nameOf(kAudioDisplays, display); }
std::optional<AudioTimeDisplay> audioTimeDisplayFromString(std::string_view text) { return valueOf(kAudioDisplays, text); }

} // namespace wabcut

namespace wabcut::edit {

using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

namespace {

/// `t` on the frame grid of `rate` (an exact rate), rounded as asked.
Ticks onGrid(Ticks t, Rate rate, Rounding rounding) {
    const auto frames = ticksToFrames(t, rate, rounding);
    const auto ticks = frames ? framesToTicks(*frames, rate, Rounding::Floor) : std::nullopt;
    return ticks ? *ticks : t;
}

double ratio(Rate r) { return static_cast<double>(r.num) / static_cast<double>(r.den); }

/// The clips of every unlocked track on the frame grid of `rate` (SequenceSettings.h), with their transitions.
void conformClips(SequenceEditor& ed, Rate rate) {
    const Ticks frame = framesToTicks(1, rate);
    for (const TrackId id : ed.trackIds()) {
        if (ed.track(id).locked) {
            continue;
        }
        // Where each transition's edges were, before the clips move.
        struct Edges {
            Ticks start;
            Ticks end;
        };
        std::map<std::uint64_t, Edges> edges;
        for (const Transition& tr : ed.track(id).transitions) {
            if (const auto cut = transitionEditPoint(ed.track(id), tr)) {
                edges[tr.id.value] = {*cut - tr.before, *cut + tr.after};
            }
        }
        // Decide every clip's new edges first (from the clips as they are), then apply them. An edit point two clips
        // share stays shared: the nearest frame both can reach (the clip that grows needs media for it), else the other
        // neighbouring frame; only when neither works do they part by a frame.
        const Track& before = ed.track(id);
        const std::size_t n = before.clips.size();
        struct Plan {
            Ticks start;
            Ticks end;
        };
        std::vector<Plan> plan(n);
        std::vector<std::optional<Ticks>> headRoom(n);
        std::vector<std::optional<Ticks>> tailRoom(n);
        for (std::size_t i = 0; i < n; ++i) {
            const Clip& c = before.clips[i];
            headRoom[i] = ed.headRoom(c);
            const auto most = ed.maxDuration(c);
            tailRoom[i] = most ? std::optional<Ticks>(std::max(Ticks{}, *most - c.duration())) : std::nullopt;
        }
        const auto canStart = [&](std::size_t i, Ticks p) {
            const Ticks s = before.clips[i].start();
            return p >= s || !headRoom[i] || *headRoom[i] >= s - p;
        };
        const auto canEnd = [&](std::size_t i, Ticks p) {
            const Ticks e = before.clips[i].end();
            return p <= e || !tailRoom[i] || *tailRoom[i] >= p - e;
        };
        for (std::size_t i = 0; i < n; ++i) {
            const Clip& c = before.clips[i];
            const Ticks s = c.start();
            const Ticks e = c.end();
            const bool afterNeighbour = i > 0 && before.clips[i - 1].end() == s;
            if (!afterNeighbour) {
                const Ticks nearest = onGrid(s, rate, Rounding::Nearest);
                plan[i].start = canStart(i, nearest) ? nearest : onGrid(s, rate, Rounding::Ceil);
            }
            const Ticks nearest = onGrid(e, rate, Rounding::Nearest);
            const Ticks other = nearest > e ? onGrid(e, rate, Rounding::Floor) : onGrid(e, rate, Rounding::Ceil);
            if (i + 1 < n && before.clips[i + 1].start() == e) {
                if (canEnd(i, nearest) && canStart(i + 1, nearest)) {
                    plan[i].end = plan[i + 1].start = nearest;
                } else if (canEnd(i, other) && canStart(i + 1, other)) {
                    plan[i].end = plan[i + 1].start = other;
                } else {
                    plan[i].end = onGrid(e, rate, Rounding::Floor); // neither reaches the other's frame
                    plan[i + 1].start = onGrid(e, rate, Rounding::Ceil);
                }
            } else {
                plan[i].end = canEnd(i, nearest) ? nearest : onGrid(e, rate, Rounding::Floor);
            }
        }
        std::vector<ClipId> gone;
        for (std::size_t i = 0; i < n; ++i) {
            const Clip& c = ed.track(id).clips[i];
            const Plan p = plan[i];
            if (p.end <= p.start) {
                gone.push_back(c.id);
                continue;
            }
            if (p.start == c.start() && p.end == c.end()) {
                continue;
            }
            Clip& m = ed.mutableClip(id, i);
            if (p.start != m.start()) {
                m.moveHead(p.start - m.start());
            }
            if (p.end != m.end()) {
                m.moveTail(p.end - m.end());
            }
        }
        for (const ClipId g : gone) {
            ed.removeClip(id, g);
        }
        Track& t = ed.mutableTrack(id);
        for (Transition& tr : t.transitions) {
            const auto it = edges.find(tr.id.value);
            const auto cut = transitionEditPoint(t, tr);
            if (it == edges.end() || !cut) {
                continue; // its clips are gone: finish() drops it
            }
            const Ticks start = std::min(*cut, onGrid(it->second.start, rate, Rounding::Nearest));
            const Ticks end = std::max(*cut, onGrid(it->second.end, rate, Rounding::Nearest));
            tr.before = tr.alignment == TransitionAlignment::StartAtCut ? Ticks{} : *cut - start;
            tr.after = tr.alignment == TransitionAlignment::EndAtCut ? Ticks{} : end - *cut;
            if (!alignmentFits(tr.alignment, tr.before, tr.after, frame)) {
                tr.alignment = alignmentOf(tr.before, tr.after, frame);
            }
        }
    }
    // Markers, marks, the work area, the duration and caption cues on the grid.
    Sequence& h = ed.header();
    for (Marker& m : h.markers) {
        const Ticks start = onGrid(m.start, rate, Rounding::Nearest);
        const Ticks end = onGrid(m.end(), rate, Rounding::Nearest);
        m.start = start;
        m.duration = std::max(Ticks{}, end - start);
        if (m.highlight) {
            m.highlight->peak = std::clamp(m.highlight->peak, m.start, m.end());
        }
    }
    std::stable_sort(h.markers.begin(), h.markers.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
    if (h.markIn) {
        h.markIn = onGrid(*h.markIn, rate, Rounding::Nearest);
    }
    if (h.markOut) {
        h.markOut = onGrid(*h.markOut, rate, Rounding::Nearest);
        if (h.markIn && *h.markOut <= *h.markIn) {
            h.markOut = *h.markIn + frame;
        }
    }
    h.duration = onGrid(h.duration, rate, Rounding::Ceil);
    if (h.workArea) {
        const Ticks start = onGrid(h.workArea->start, rate, Rounding::Nearest);
        const Ticks end = std::min(h.duration, onGrid(h.workArea->end(), rate, Rounding::Nearest));
        h.workArea = end > start ? std::optional<TimeRange>(TimeRange::fromStartEnd(start, end)) : std::nullopt;
    }
    for (CaptionTrack& ct : h.captionTracks) {
        std::vector<CaptionCue> cues;
        for (CaptionCue cue : ct.cues) {
            const Ticks start = onGrid(cue.start, rate, Rounding::Nearest);
            const Ticks end = onGrid(cue.end(), rate, Rounding::Nearest);
            if (end <= start) {
                continue;
            }
            cue.start = start;
            cue.duration = end - start;
            cues.push_back(std::move(cue));
        }
        ct.cues = std::move(cues);
    }
}

bool nameTaken(const std::vector<std::string>& taken, const std::string& name) {
    return std::find(taken.begin(), taken.end(), name) != taken.end();
}

} // namespace

EditOutcome setSequenceSettings(const ProjectState& state, SequenceId sequenceId, const SequenceSettingsChange& change) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (change.frameRate &&
        (!change.frameRate->isValid() || !change.frameRate->isExact() || ratio(*change.frameRate) < 1.0 ||
         ratio(*change.frameRate) > 1000.0)) {
        return std::unexpected(invalidArgument("the timebase must be an exact rate from 1 to 1000 fps"));
    }
    if ((change.width && (*change.width < 16 || *change.width > 16384)) ||
        (change.height && (*change.height < 16 || *change.height > 16384))) {
        return std::unexpected(invalidArgument("the frame size must be 16 to 16384 pixels each way"));
    }
    if (change.pixelAspect && (!change.pixelAspect->isValid() || ratio(*change.pixelAspect) < 0.1 ||
                               ratio(*change.pixelAspect) > 10.0)) {
        return std::unexpected(invalidArgument("the pixel aspect ratio must be 0.1 to 10"));
    }
    if (change.audioRate && (!change.audioRate->isValid() || !change.audioRate->isExact() ||
                             ratio(*change.audioRate) < 8000.0 || ratio(*change.audioRate) > 384000.0)) {
        return std::unexpected(invalidArgument("the audio sample rate must be an exact rate from 8 to 384 kHz"));
    }
    if (change.previewFormat && change.previewFormat->size() > 32) {
        return std::unexpected(invalidArgument("the preview format name is longer than 32 bytes"));
    }
    if (change.startTimecode && (change.startTimecode->value < 0 || *change.startTimecode > kMaxStartTimecode)) {
        return std::unexpected(invalidArgument("the start timecode must be 00:00:00:00 to 24 hours"));
    }
    if (change.name && change.name->empty()) {
        return std::unexpected(invalidArgument("the sequence name may not be empty"));
    }
    if (change.colorIndex && (*change.colorIndex < 0 || *change.colorIndex > 15)) {
        return std::unexpected(invalidArgument("the label must be 0 to 15"));
    }
    SequenceEditor ed(state, **seq);
    const Rate oldRate = (*seq)->frameRate;
    if (change.frameRate && !(*change.frameRate == oldRate)) {
        conformClips(ed, *change.frameRate);
    }
    Sequence& h = ed.header();
    if (change.name) {
        h.name = *change.name;
    }
    if (change.frameRate) {
        h.frameRate = *change.frameRate;
    }
    if (change.width) {
        h.width = *change.width;
    }
    if (change.height) {
        h.height = *change.height;
    }
    if (change.pixelAspect) {
        h.pixelAspect = change.pixelAspect->normalized();
    }
    if (change.fields) {
        h.fields = *change.fields;
    }
    if (change.audioRate) {
        h.audioRate = *change.audioRate;
    }
    if (change.dropFrame) {
        h.dropFrame = *change.dropFrame;
    }
    h.dropFrame = h.dropFrame && supportsDropFrame(h.frameRate);
    if (change.previewFormat) {
        h.previewFormat = *change.previewFormat;
    }
    if (change.videoDisplay) {
        h.videoDisplay = *change.videoDisplay;
    }
    if (change.audioDisplay) {
        h.audioDisplay = *change.audioDisplay;
    }
    if (change.startTimecode) {
        h.startTimecode = *change.startTimecode;
    }
    if (change.colorIndex) {
        h.colorIndex = *change.colorIndex;
    }
    if (change.rippleMarkers) { // 0.4: Markers > Ripple Sequence Markers
        h.rippleMarkers = *change.rippleMarkers;
    }
    return ed.finish();
}

std::string formatSequenceTime(const Sequence& sequence, Ticks t) {
    const Rate rate = sequence.frameRate.isValid() ? sequence.frameRate : kRate30;
    if (sequence.videoDisplay == VideoTimeDisplay::Timecode) {
        return ticksToTimecode(t, rate, sequence.dropFrame && supportsDropFrame(rate));
    }
    const std::int64_t frames = ticksToFrames(t, rate); // the frame that contains t
    const std::string sign = frames < 0 ? "-" : "";
    const std::uint64_t count = frames < 0 ? static_cast<std::uint64_t>(-(frames + 1)) + 1 : static_cast<std::uint64_t>(frames);
    if (sequence.videoDisplay == VideoTimeDisplay::Frames) {
        return sign + std::to_string(count);
    }
    const std::uint64_t perFoot = sequence.videoDisplay == VideoTimeDisplay::Feet16mm ? 40 : 16;
    std::string feet = std::to_string(count / perFoot);
    if (feet.size() < 4) {
        feet.insert(0, 4 - feet.size(), '0');
    }
    std::string rest = std::to_string(count % perFoot);
    if (rest.size() < 2) {
        rest.insert(0, 1, '0');
    }
    return sign + feet + "+" + rest;
}

std::string formatSequenceAudioTime(const Sequence& sequence, Ticks t) {
    const Rate rate = sequence.audioRate.isValid() ? sequence.audioRate : kRate48kHz;
    const std::string sign = t.value < 0 ? "-" : "";
    const std::int64_t value = t.value < 0 ? -t.value : t.value;
    const std::int64_t seconds = value / kTicksPerSecond;
    const Ticks within{value % kTicksPerSecond};
    const auto two = [](std::int64_t v) {
        std::string out = std::to_string(v);
        return out.size() < 2 ? "0" + out : out;
    };
    std::string out = sign + two(seconds / 3600) + ":" + two((seconds / 60) % 60) + ":" + two(seconds % 60);
    if (sequence.audioDisplay == AudioTimeDisplay::Milliseconds) {
        std::string ms = std::to_string(within.value * 1000 / kTicksPerSecond);
        ms.insert(0, 3 - std::min<std::size_t>(3, ms.size()), '0');
        return out + "." + ms;
    }
    // The sample within the second, as wide as the largest one (47999 at 48 kHz: five digits).
    const std::int64_t perSecond = rate.num / std::max<std::int64_t>(1, rate.den);
    const std::size_t digits = std::to_string(std::max<std::int64_t>(1, perSecond - 1)).size();
    std::string samples = std::to_string(ticksToSamples(within, rate));
    samples.insert(0, digits - std::min(digits, samples.size()), '0');
    return out + ":" + samples;
}

std::string copyName(const std::string& name, const std::vector<std::string>& taken) {
    const std::string base = name + " Copy";
    if (!nameTaken(taken, base)) {
        return base;
    }
    for (int n = 2; n < 10000; ++n) {
        std::string candidate = base + " " + std::to_string(n);
        if (!nameTaken(taken, candidate)) {
            return candidate;
        }
    }
    return base;
}

std::expected<Created<SequenceId>, Error> duplicateSequence(const ProjectState& state, SequenceId sequence,
                                                            std::string name) {
    const Sequence* original = state.findSequence(sequence);
    if (!original) {
        return std::unexpected(notFound("sequence " + std::to_string(sequence.value) + " does not exist"));
    }
    if (name.empty()) {
        std::vector<std::string> taken;
        for (const SequencePtr& s : state.sequences) {
            taken.push_back(s->name);
        }
        name = copyName(original->name, taken);
    }
    return copySequenceFrom(state, state, sequence, std::move(name));
}

std::expected<Created<MediaId>, Error> duplicateMedia(const ProjectState& state, MediaId media, std::string name) {
    const MediaItem* original = state.findMedia(media);
    if (!original) {
        return std::unexpected(notFound("media " + std::to_string(media.value) + " does not exist"));
    }
    MediaItem copy = *original;
    if (name.empty()) {
        std::vector<std::string> taken;
        for (const MediaPtr& m : state.media) {
            taken.push_back(m->name);
        }
        name = copyName(original->name, taken);
    }
    copy.name = std::move(name);
    copy.clipDefaults.reset();
    return addMedia(state, std::move(copy));
}

std::expected<Created<SequenceId>, Error> newSequenceFromClip(const ProjectState& state, MediaId mediaId,
                                                              std::string name) {
    const MediaItem* item = state.findMedia(mediaId);
    if (!item) {
        return std::unexpected(notFound("media " + std::to_string(mediaId.value) + " does not exist"));
    }
    if (!item->info.hasVideo() && !item->info.hasAudio()) {
        return std::unexpected(invalidArgument("'" + item->name + "' has no picture or sound to make a sequence from"));
    }
    SequenceSettings settings;
    settings.name = name.empty() ? item->name : std::move(name);
    settings.bin = item->bin;
    settings.width = 1920;
    settings.height = 1080;
    settings.frameRate = kRate30;
    Rate pixelAspect{1, 1};
    if (item->info.hasVideo()) {
        const VideoStreamInfo& v = item->info.video.front();
        if (v.width > 0 && v.height > 0) {
            const int quarter = ((v.rotationDegrees / 90) % 4 + 4) % 4;
            settings.width = quarter % 2 == 1 ? v.height : v.width;
            settings.height = quarter % 2 == 1 ? v.width : v.height;
        }
        if (!item->info.isStillImage && v.frameRate.isValid()) {
            const Rate interpreted =
                item->interpretation && item->interpretation->frameRate ? *item->interpretation->frameRate : v.frameRate;
            const Rate nominal = v.variableFrameRate ? nominalRate(interpreted) : interpreted.normalized();
            if (nominal.isValid() && nominal.isExact() && ratio(nominal) >= 1.0 && ratio(nominal) <= 1000.0) {
                settings.frameRate = nominal;
            }
        }
        const Rate par = item->interpretation && item->interpretation->pixelAspect ? *item->interpretation->pixelAspect
                                                                                    : v.sampleAspect;
        if (par.isValid() && ratio(par) >= 0.1 && ratio(par) <= 10.0) {
            pixelAspect = par.normalized();
        }
    }
    settings.width = std::clamp(settings.width, 16, 16384);
    settings.height = std::clamp(settings.height, 16, 16384);
    if (item->info.hasAudio() && item->info.audio.front().sampleRate.isValid() &&
        item->info.audio.front().sampleRate.isExact()) {
        settings.audioRate = item->info.audio.front().sampleRate;
    }
    settings.dropFrame = false;
    settings.videoTracks = 3;
    settings.audioTracks = std::max<std::int32_t>(3, static_cast<std::int32_t>(item->info.audio.size()));
    auto created = addSequence(state, settings);
    if (!created) {
        return std::unexpected(created.error());
    }
    // The rest of its settings, then the item at the start.
    {
        auto seq = std::make_shared<Sequence>(*created->state.findSequence(created->id));
        seq->pixelAspect = pixelAspect;
        seq->fields = effectiveFieldOrder(*item, 0); // Interpret Footage's, else the file's (0.2 field options)
        created->state = created->state.withSequence(std::move(seq));
    }
    const Sequence& seq = *created->state.findSequence(created->id);
    SourceClip source;
    source.media = mediaId;
    source.patches = defaultSourcePatches(seq, *item);
    if (source.patches.empty()) {
        return created; // nothing to place: an empty sequence with its settings
    }
    const SourceMarks marks = storedSourceMarks(*item);
    auto placement = resolveThreePointEdit(created->state, created->id, mediaId, source.patches, marks, Ticks{});
    if (!placement) {
        return std::unexpected(placement.error());
    }
    source.sourceRange = placement->sourceRange;
    auto edited = overwriteEdit(created->state, created->id, source, Ticks{});
    if (!edited) {
        return std::unexpected(edited.error());
    }
    return Created<SequenceId>{std::move(edited->state), created->id};
}

std::expected<Created<TrackId>, Error> insertTrack(const ProjectState& state, SequenceId sequenceId, TrackKind kind,
                                                   std::size_t index, std::string name) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    auto next = std::make_shared<Sequence>(**seq);
    Created<TrackId> out{state, {}};
    auto track = std::make_shared<Track>();
    track->id = out.state.ids.allocate<TrackId>();
    track->kind = kind;
    track->name = std::move(name);
    out.id = track->id;
    auto& tracks = kind == TrackKind::Video ? next->videoTracks : next->audioTracks;
    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(std::min(index, tracks.size())), std::move(track));
    out.state = out.state.withSequence(std::move(next));
    return out;
}

} // namespace wabcut::edit
