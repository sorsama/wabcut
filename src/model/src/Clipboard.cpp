// The clipboard (Clipboard.h, C.11): copy / cut / paste of timeline clips and Paste / Remove Attributes.

#include <wabcut/model/Clipboard.h>

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RedactionMedia.h>

#include "SequenceEditor.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <variant>

namespace wabcut {

using edit::EditResult;
using edit::LinkMode;

namespace {

using json = nlohmann::json;
using edit::detail::invalidArgument;
using edit::detail::notFound;

constexpr std::string_view kFormat = "wabcut-clipboard";

/// Clips (with their tracks) of a sequence named by id, with or without their link partners.
struct Picked {
    TrackKind kind;
    std::size_t trackIndex;
    const Track* track;
    const Clip* clip;
};

std::expected<std::vector<Picked>, Error> pick(const Sequence& seq, std::span<const ClipId> ids, LinkMode links) {
    std::set<std::uint64_t> wanted;
    std::set<std::uint64_t> groups;
    for (const ClipId id : ids) {
        const auto loc = seq.findClip(id);
        if (!loc) {
            return std::unexpected(notFound("clip " + std::to_string(id.value) + " is not in sequence '" + seq.name + "'"));
        }
        wanted.insert(id.value);
        if (links == LinkMode::FollowLinks && loc->clip().linkGroup.isValid()) {
            groups.insert(loc->clip().linkGroup.value);
        }
    }
    std::vector<Picked> out;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = seq.tracks(kind);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            for (const Clip& c : tracks[i]->clips) {
                if (wanted.contains(c.id.value) || (c.linkGroup.isValid() && groups.contains(c.linkGroup.value))) {
                    out.push_back({kind, i, tracks[i].get(), &c});
                }
            }
        }
    }
    if (out.empty()) {
        return std::unexpected(invalidArgument("no clips to copy"));
    }
    if (out.size() > kMaxClipboardClips) {
        return std::unexpected(invalidArgument("too many clips to copy (at most " + std::to_string(kMaxClipboardClips) + ")"));
    }
    return out;
}

/// The media items and sequences `seq`'s clips play, nested sequences followed (the closure).
void collectSources(const ProjectState& state, const Sequence& seq, std::set<std::uint64_t>& media,
                    std::set<std::uint64_t>& sequences) {
    seq.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.isNested()) {
                if (sequences.insert(c.sequence.value).second) {
                    if (const Sequence* nested = state.findSequence(c.sequence)) {
                        collectSources(state, *nested, media, sequences);
                    }
                }
            } else if (c.media.isValid()) {
                media.insert(c.media.value);
            }
        }
    });
}

bool sameFile(const MediaItem& a, const MediaItem& b) {
    if (a.generator || b.generator) {
        return a.generator == b.generator && a.name == b.name;
    }
    if (a.locator.path != b.locator.path) {
        return false;
    }
    return !a.locator.fingerprint.isKnown() || !b.locator.fingerprint.isKnown() ||
           a.locator.fingerprint.sameContentAs(b.locator.fingerprint);
}

// ---- Keyframe times between two clips --------------------------------------------------------------------------------

/// Curve time per timeline tick of a clip: its speed (negative reversed), 1 on a freeze frame or a remapped clip.
std::pair<std::int64_t, std::int64_t> curveRate(const Clip& c) {
    if (c.isFreeze() || c.isRemapped()) {
        return {1, 1};
    }
    return {c.reversed ? -std::int64_t{c.speed.num} : std::int64_t{c.speed.num}, c.speed.den};
}

/// Maps a curve time of `from` to the curve time on `to` at the same time from the clip's start (or the same fraction
/// of its length with `scale`).
class TimeMapper {
public:
    TimeMapper(const Clip& from, const Clip& to, bool scale)
        : m_fromStart(from.curveTimeAt(from.start())), m_toStart(to.curveTimeAt(to.start())), m_fromRate(curveRate(from)),
          m_toRate(curveRate(to)), m_fromDuration(from.duration()), m_toDuration(to.duration()), m_scale(scale) {}

    Ticks operator()(Ticks t) const {
        const bool sameRate = m_fromRate.first * m_toRate.second == m_toRate.first * m_fromRate.second;
        if (sameRate && (!m_scale || m_fromDuration == m_toDuration)) {
            return m_toStart + (t - m_fromStart); // exact
        }
        long double u = static_cast<long double>((t - m_fromStart).value) * m_fromRate.second / m_fromRate.first;
        if (m_scale && m_fromDuration.value > 0) {
            u = u * static_cast<long double>(m_toDuration.value) / static_cast<long double>(m_fromDuration.value);
        }
        const long double v = u * m_toRate.first / m_toRate.second;
        return m_toStart + Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(v)))};
    }

private:
    Ticks m_fromStart;
    Ticks m_toStart;
    std::pair<std::int64_t, std::int64_t> m_fromRate;
    std::pair<std::int64_t, std::int64_t> m_toRate;
    Ticks m_fromDuration;
    Ticks m_toDuration;
    bool m_scale;
};

ParamValue mapValue(const ParamValue& value, const TimeMapper& map) {
    const Curve* curve = std::get_if<Curve>(&value.data);
    if (!curve) {
        return value;
    }
    Curve out = *curve;
    for (Keyframe& k : out.keys) {
        k.time = map(k.time);
    }
    // A reversed mapping (one clip reversed) turns the order around; keys stay sorted and unique.
    std::sort(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    out.keys.erase(std::unique(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time == b.time; }),
                   out.keys.end());
    return ParamValue(std::move(out));
}

EffectInstance mapEffect(EffectInstance effect, const TimeMapper& map) {
    for (EffectParam& p : effect.params) {
        p.value = mapValue(p.value, map);
    }
    for (Mask& m : effect.masks) {
        for (MaskParam& p : m.params) {
            p.value = mapValue(p.value, map);
        }
        for (MaskPathKey& k : m.path) {
            k.time = map(k.time);
        }
        std::sort(m.path.begin(), m.path.end(), [](const MaskPathKey& a, const MaskPathKey& b) { return a.time < b.time; });
    }
    return effect;
}

std::string effectName(const EffectInstance& e) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
    return d ? std::string(d->displayName) : e.effect;
}

bool effectFits(const EffectInstance& e, const Clip& clip) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
    if (!d || d->intrinsic) {
        return false;
    }
    return d->kind == (clip.stream.kind == StreamKind::Video ? EffectKind::Video : EffectKind::Audio);
}

constexpr std::array<ParamId, 6> kMotion{ParamId::PositionX, ParamId::PositionY, ParamId::Scale,
                                        ParamId::Rotation,  ParamId::AnchorX,   ParamId::AnchorY};

struct TargetRef {
    TrackId track;
    std::size_t index;
};

std::expected<std::vector<TargetRef>, Error> resolveTargets(const Sequence& seq, std::span<const ClipId> targets) {
    if (targets.empty()) {
        return std::unexpected(invalidArgument("no clips to change"));
    }
    std::vector<TargetRef> out;
    std::set<std::uint64_t> seen;
    for (const ClipId id : targets) {
        if (!seen.insert(id.value).second) {
            continue;
        }
        const auto loc = seq.findClip(id);
        if (!loc) {
            return std::unexpected(notFound("clip " + std::to_string(id.value) + " does not exist"));
        }
        if (loc->track->locked) {
            return std::unexpected(invalidArgument("clip " + std::to_string(id.value) + " is on a locked track"));
        }
        out.push_back({loc->track->id, loc->index});
    }
    return out;
}

EditResult composed(const ProjectState& before, ProjectState after) {
    EditResult r;
    r.changes = diff(before, after);
    r.state = std::move(after);
    return r;
}

} // namespace

std::size_t ClipboardDocument::clipCount() const {
    std::size_t n = 0;
    sequence().forEachTrack([&](const Track& t) { n += t.clips.size(); });
    return n;
}

// ---- Copy ------------------------------------------------------------------------------------------------------------

std::expected<ClipboardDocument, Error> copyClips(const ProjectState& state, SequenceId sequenceId,
                                                  std::span<const ClipId> ids, const CopyOptions& options) {
    const auto found = edit::detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& seq = **found;
    auto picked = pick(seq, ids, options.links);
    if (!picked) {
        return std::unexpected(picked.error());
    }
    ClipboardDocument doc;
    Ticks start = Ticks::max();
    Ticks end = Ticks::min();
    int base[2] = {-1, -1};
    int top[2] = {-1, -1};
    for (const Picked& p : *picked) {
        start = std::min(start, p.clip->start());
        end = std::max(end, p.clip->end());
        const int k = p.kind == TrackKind::Video ? 0 : 1;
        const int i = static_cast<int>(p.trackIndex);
        base[k] = base[k] < 0 ? i : std::min(base[k], i);
        top[k] = std::max(top[k], i);
    }
    doc.videoBase = base[0];
    doc.audioBase = base[1];
    doc.duration = end - start;
    doc.noCloud = state.aiNoCloud; // the project's no-cloud lock follows its clips (S5-P2-12)

    ProjectState& content = doc.content;
    content.ids = state.ids;
    // The clipboard's sequence: the source's settings, no tracks yet, nothing else carried over.
    auto clipSeq = std::make_shared<Sequence>();
    clipSeq->id = seq.id;
    clipSeq->name = seq.name;
    clipSeq->frameRate = seq.frameRate;
    clipSeq->audioRate = seq.audioRate;
    clipSeq->dropFrame = seq.dropFrame;
    clipSeq->width = seq.width;
    clipSeq->height = seq.height;
    clipSeq->duration = doc.duration;
    std::set<std::uint64_t> copied;
    for (const Picked& p : *picked) {
        copied.insert(p.clip->id.value);
    }
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const int k = kind == TrackKind::Video ? 0 : 1;
        for (int i = base[k]; base[k] >= 0 && i <= top[k]; ++i) {
            const Track& from = *seq.tracks(kind)[static_cast<std::size_t>(i)];
            auto t = std::make_shared<Track>();
            t->id = content.ids.allocate<TrackId>();
            t->kind = kind;
            t->name = from.name;
            t->role = from.role;
            for (const Picked& p : *picked) {
                if (p.track == &from) {
                    Clip c = *p.clip;
                    c.timelineRange.start = c.start() - start;
                    // Links survive only among copied clips.
                    if (c.linkGroup.isValid()) {
                        const auto members = std::count_if(picked->begin(), picked->end(), [&](const Picked& o) {
                            return o.clip->linkGroup == c.linkGroup;
                        });
                        if (members < 2) {
                            c.linkGroup = {};
                        }
                    }
                    t->clips.push_back(std::move(c));
                }
            }
            for (const Transition& tr : from.transitions) {
                const bool out = !tr.outgoing.isValid() || copied.contains(tr.outgoing.value);
                const bool in = !tr.incoming.isValid() || copied.contains(tr.incoming.value);
                if (out && in) {
                    t->transitions.push_back(tr);
                }
            }
            (kind == TrackKind::Video ? clipSeq->videoTracks : clipSeq->audioTracks).push_back(std::move(t));
        }
    }
    if (options.markers) {
        for (const Marker& m : seq.markers) {
            if (m.start >= start && m.start < end) {
                Marker moved = m;
                moved.start = m.start - start;
                clipSeq->markers.push_back(std::move(moved));
            }
        }
    }
    std::set<std::uint64_t> speakers;
    if (options.captions) {
        for (const CaptionTrack& ct : seq.captionTracks) {
            CaptionTrack copy = ct;
            copy.cues.clear();
            for (const CaptionCue& cue : ct.cues) {
                if (cue.end() > start && cue.start < end) {
                    CaptionCue moved = cue;
                    const Ticks s = std::max(cue.start, start);
                    const Ticks e = std::min(cue.end(), end);
                    moved.start = s - start;
                    moved.duration = e - s;
                    if (cue.speaker.isValid()) {
                        speakers.insert(cue.speaker.value);
                    }
                    copy.cues.push_back(std::move(moved));
                }
            }
            clipSeq->captionTracks.push_back(std::move(copy));
            doc.hasCaptions = true;
        }
    }
    // What the clips play.
    std::set<std::uint64_t> media;
    std::set<std::uint64_t> nested;
    collectSources(state, *clipSeq, media, nested);
    for (const std::uint64_t id : media) {
        if (const MediaItem* m = state.findMedia(MediaId{id})) {
            auto item = std::make_shared<MediaItem>(*m);
            item->bin = {};
            content.media.push_back(std::move(item));
        }
    }
    // Their source-anchored redactions travel with them, keyed to the copied item (a subclip's master's and a stem's
    // source's too: RedactionMedia.h), so a paste - into another project, or after the item was removed here - never
    // plays them unredacted (review wave 5 E5-P2-4). Exceptions name clips of this project: left behind.
    for (const MediaPtr& item : content.media) {
        for (const Redaction* r : redactionsCovering(state, item->id)) {
            Redaction copy = *r;
            copy.id = content.ids.allocate<RedactionId>();
            copy.media = item->id;
            copy.exceptions.clear();
            content.redactions.push_back(std::move(copy));
        }
    }
    for (const std::uint64_t id : nested) {
        if (const Sequence* s = state.findSequence(SequenceId{id})) {
            auto copy = std::make_shared<Sequence>(*s);
            copy->bin = {};
            copy->derivedFrom.reset();
            content.sequences.push_back(std::move(copy));
        }
    }
    for (const Speaker& s : state.speakers) {
        if (speakers.contains(s.id.value)) {
            content.speakers.push_back(s);
        }
    }
    doc.clips = clipSeq->id;
    content.sequences.push_back(std::move(clipSeq));
    std::sort(content.sequences.begin(), content.sequences.end(),
              [](const SequencePtr& a, const SequencePtr& b) { return a->id < b->id; });
    if (auto valid = validate(content, Validation::Structure); !valid) {
        return std::unexpected(invalidArgument("cannot copy these clips: " + valid.error().message));
    }
    return doc;
}

// ---- JSON ------------------------------------------------------------------------------------------------------------

std::expected<std::string, Error> clipboardToJson(const ClipboardDocument& document) {
    auto project = io::toJson(document.content);
    if (!project) {
        return std::unexpected(project.error());
    }
    json doc{{"format", kFormat},
             {"version", kClipboardVersion},
             {"sequence", document.clips.value},
             {"videoBase", document.videoBase},
             {"audioBase", document.audioBase},
             {"duration", document.duration.value},
             {"captions", document.hasCaptions},
             {"noCloud", document.noCloud},
             {"project", json::parse(*project)}};
    try {
        return doc.dump();
    } catch (const json::exception& e) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::string("cannot serialize the clipboard: ") + e.what()});
    }
}

std::expected<ClipboardDocument, Error> clipboardFromJson(std::string_view text) {
    const auto bad = [](const std::string& why) {
        return std::unexpected(Error{ErrorCode::ParseError, "not a usable Wabcut clipboard: " + why});
    };
    if (text.size() > kMaxClipboardBytes) {
        return bad("larger than allowed");
    }
    if (!io::jsonNestingWithin(text)) {
        return bad("nested too deep");
    }
    const json doc = json::parse(text, nullptr, false);
    // Every field read may meet the wrong type (value() and get() throw then): one try for the whole header.
    ClipboardDocument out;
    try {
        if (!doc.is_object() || doc.value("format", std::string()) != kFormat || !doc.contains("project") ||
            !doc["project"].is_object()) {
            return bad("not a clipboard");
        }
        if (doc.value("version", 0) != kClipboardVersion) {
            return std::unexpected(Error{ErrorCode::VersionMismatch, "the clipboard comes from a newer version of Wabcut"});
        }
        out.clips = SequenceId{doc.at("sequence").get<std::uint64_t>()};
        out.videoBase = doc.value("videoBase", -1);
        out.audioBase = doc.value("audioBase", -1);
        out.duration = Ticks{doc.at("duration").get<std::int64_t>()};
        out.hasCaptions = doc.value("captions", false);
        out.noCloud = doc.value("noCloud", false);
    } catch (const json::exception& e) {
        return bad(e.what());
    }
    auto content = io::fromJson(doc["project"].dump());
    if (!content) {
        return std::unexpected(Error{ErrorCode::ParseError, "the clipboard's clips: " + content.error().message});
    }
    out.content = std::move(*content);
    const Sequence* seq = out.content.findSequence(out.clips);
    if (!seq) {
        return bad("it names no sequence of clips");
    }
    if (out.videoBase < -1 || out.audioBase < -1 || out.videoBase > 1000 || out.audioBase > 1000) {
        return bad("track numbers out of range");
    }
    if (out.clipCount() == 0 || out.clipCount() > kMaxClipboardClips) {
        return bad("no clips, or more than allowed");
    }
    if (out.duration.value <= 0 || out.duration != seq->contentEnd() || seq->duration != out.duration) {
        return bad("its length does not match its clips");
    }
    // Clips start at 0 (the first copied clip's start).
    Ticks first = Ticks::max();
    seq->forEachTrack([&](const Track& t) {
        if (!t.clips.empty()) {
            first = std::min(first, t.clips.begin()->start());
        }
    });
    if (first != Ticks{}) {
        return bad("its clips do not start at 0");
    }
    return out;
}

// ---- Paste -----------------------------------------------------------------------------------------------------------

std::expected<PasteResult, Error> pasteClips(const ProjectState& state, SequenceId sequenceId,
                                             const ClipboardDocument& document, Ticks at, const PasteOptions& options) {
    if (at.value < 0) {
        return std::unexpected(invalidArgument("cannot paste before the start of the sequence"));
    }
    const Sequence* clipSeq = document.content.findSequence(document.clips);
    if (!clipSeq) {
        return std::unexpected(invalidArgument("the clipboard is empty"));
    }
    const auto found = edit::detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    ProjectState out = state;
    PasteReport report;

    // Media: the same file by id, else by fingerprint, else by path, else added.
    std::unordered_map<std::uint64_t, MediaId> mediaMap;
    for (const MediaPtr& m : document.content.media) {
        if (const MediaItem* same = out.findMedia(m->id); same && sameFile(*same, *m)) {
            mediaMap.emplace(m->id.value, m->id);
            continue;
        }
        const MediaItem* match = nullptr;
        std::string how;
        if (m->locator.fingerprint.isKnown()) {
            for (const MediaPtr& o : out.media) {
                if (o->locator.fingerprint.sameContentAs(m->locator.fingerprint)) {
                    match = o.get();
                    how = "matched by fingerprint";
                    break;
                }
            }
        }
        if (!match && !m->locator.path.empty()) {
            for (const MediaPtr& o : out.media) {
                if (o->locator.path == m->locator.path && !o->generator) {
                    match = o.get();
                    how = "matched by path";
                    break;
                }
            }
        }
        if (match) {
            mediaMap.emplace(m->id.value, match->id);
            report.notes.push_back("'" + m->name + "': " + how + " ('" + match->name + "')");
            continue;
        }
        MediaItem item = *m;
        item.id = {};
        item.bin = {};
        auto added = edit::addMedia(out, std::move(item));
        if (!added) {
            return std::unexpected(Error{added.error().code, "cannot add the media '" + m->name + "': " + added.error().message});
        }
        out = std::move(added->state);
        mediaMap.emplace(m->id.value, added->id);
        report.mediaAdded.push_back(added->id);
        report.notes.push_back("'" + m->name + "': added to the project");
    }
    // The copied media's redactions (copyClips carries them), on the item the paste plays: added unless the same marks
    // already cover it here (same project, same item), so nothing pasted plays unredacted (review wave 5 E5-P2-4).
    for (const Redaction& r : document.content.redactions) {
        const auto mapped = mediaMap.find(r.media.value);
        if (mapped == mediaMap.end()) {
            continue;
        }
        Redaction copy = r;
        copy.media = mapped->second;
        copy.exceptions.clear();
        const auto same = [&](const Redaction* o) {
            Redaction a = *o;
            a.id = copy.id;
            a.media = copy.media;
            a.exceptions = copy.exceptions;
            return a == copy;
        };
        if (std::ranges::any_of(redactionsCovering(out, copy.media), same)) {
            continue;
        }
        const std::string name = copy.name;
        auto added = edit::addRedaction(out, std::move(copy));
        if (!added) {
            return std::unexpected(Error{added.error().code, "cannot carry the redaction '" + name + "': " + added.error().message});
        }
        out = std::move(added->state);
        report.notes.push_back("redaction '" + name + "' came with its media");
    }
    // Nested sequences bind to the destination's sequence of the same id only when it is the very sequence that was
    // copied (E5-P2-3): the same name and content, its media the same files under the same ids. Projects made from one
    // template number their sequences alike, so another project's "Sequence 01" must never stand in for this one's.
    std::set<std::uint64_t> nestedOk;
    std::set<std::uint64_t> nestedOther; ///< same id and name, other content
    for (const SequencePtr& s : document.content.sequences) {
        if (s->id == document.clips) {
            continue;
        }
        const Sequence* mine = out.findSequence(s->id);
        if (!mine || mine->name != s->name) {
            continue;
        }
        Sequence normalized = *mine; // as copyClips copies it
        normalized.bin = {};
        normalized.derivedFrom.reset();
        std::set<std::uint64_t> media;
        std::set<std::uint64_t> inner;
        collectSources(document.content, *s, media, inner);
        const bool sameMedia = std::all_of(media.begin(), media.end(), [&](std::uint64_t id) {
            const auto it = mediaMap.find(id);
            return it != mediaMap.end() && it->second.value == id;
        });
        (normalized == *s && sameMedia ? nestedOk : nestedOther).insert(s->id.value);
    }
    // A sequence is only the copied one when the ones it plays are too.
    for (auto it = nestedOk.begin(); it != nestedOk.end();) {
        std::set<std::uint64_t> media;
        std::set<std::uint64_t> inner;
        collectSources(document.content, *document.content.findSequence(SequenceId{*it}), media, inner);
        if (std::all_of(inner.begin(), inner.end(), [&](std::uint64_t id) { return nestedOk.contains(id); })) {
            ++it;
        } else {
            nestedOther.insert(*it);
            it = nestedOk.erase(it);
        }
    }

    // Destination tracks.
    const Sequence* seq = out.findSequence(sequenceId);
    int destBase[2] = {0, 0};
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const int k = kind == TrackKind::Video ? 0 : 1;
        const std::optional<TrackId>& asked = kind == TrackKind::Video ? options.videoTrack : options.audioTrack;
        const auto& tracks = seq->tracks(kind);
        int base = -1;
        if (asked) {
            const auto pos = seq->trackPosition(*asked);
            if (!pos || pos->kind != kind) {
                return std::unexpected(notFound(std::string("the destination ") + (k == 0 ? "video" : "audio") +
                                                " track is not in the sequence"));
            }
            base = static_cast<int>(pos->index);
        } else {
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                if (tracks[i]->targeted && !tracks[i]->locked) {
                    base = static_cast<int>(i);
                    break;
                }
            }
            if (base < 0) {
                base = std::max(0, k == 0 ? document.videoBase : document.audioBase);
            }
        }
        destBase[k] = base;
        const auto& from = clipSeq->tracks(kind);
        int needed = 0;
        for (std::size_t i = 0; i < from.size(); ++i) {
            if (!from[i]->clips.empty()) {
                needed = base + static_cast<int>(i) + 1;
            }
        }
        while (static_cast<int>(out.findSequence(sequenceId)->tracks(kind).size()) < needed) {
            if (!options.addTracks) {
                return std::unexpected(invalidArgument(std::string("the sequence has no ") + (k == 0 ? "video" : "audio") +
                                                       " track to paste into"));
            }
            auto added = edit::addTrack(out, sequenceId, kind);
            if (!added) {
                return std::unexpected(added.error());
            }
            out = std::move(added->state);
            report.tracksAdded.push_back(added->id);
        }
    }
    seq = out.findSequence(sequenceId);
    const auto destination = [&](TrackKind kind, std::size_t index) {
        return seq->tracks(kind)[static_cast<std::size_t>(destBase[kind == TrackKind::Video ? 0 : 1]) + index]->id;
    };
    std::set<std::uint64_t> destinations;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& from = clipSeq->tracks(kind);
        for (std::size_t i = 0; i < from.size(); ++i) {
            if (from[i]->clips.empty()) {
                continue;
            }
            const TrackId dest = destination(kind, i);
            if (seq->findTrack(dest)->locked) {
                return std::unexpected(invalidArgument("the destination track " + edit::detail::trackLabel(*seq, dest) +
                                                       " is locked"));
            }
            destinations.insert(dest.value);
        }
    }

    edit::detail::SequenceEditor ed(out, *seq);
    const Ticks duration = document.duration;
    if (options.mode == PasteMode::Insert) {
        for (const TrackId id : ed.trackIds()) {
            const Track& t = ed.track(id);
            if (!t.locked && (destinations.contains(id.value) || t.syncLocked)) {
                ed.rippleInsert(id, at, duration);
            }
        }
        ed.captionsInsert(at, duration);
    }
    std::unordered_map<std::uint64_t, LinkGroupId> groups;
    std::unordered_map<std::uint64_t, ClipId> clipMap;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& from = clipSeq->tracks(kind);
        for (std::size_t i = 0; i < from.size(); ++i) {
            if (from[i]->clips.empty()) {
                continue;
            }
            const TrackId dest = destination(kind, i);
            for (const Clip& c : from[i]->clips) {
                if (c.isNested()) {
                    const Sequence* s = document.content.findSequence(c.sequence);
                    const std::string name = s ? s->name : std::string("?");
                    if (!nestedOk.contains(c.sequence.value)) {
                        report.notes.push_back(
                            "a clip of sequence '" + name + "' was left out: " +
                            (nestedOther.contains(c.sequence.value)
                                 ? "this project's '" + name + "' is not the sequence that was copied (another project's, "
                                                               "or changed since the copy)"
                                 : std::string("that sequence is not in this project")));
                        continue;
                    }
                    if (sequenceContains(out, c.sequence, sequenceId)) {
                        report.notes.push_back("a clip of sequence '" + name + "' was left out: it would play '" + seq->name +
                                               "' inside itself");
                        continue;
                    }
                }
                Clip n = c;
                n.id = ed.newClipId();
                n.timelineRange.start = at + c.start();
                if (c.media.isValid()) {
                    const auto m = mediaMap.find(c.media.value);
                    if (m == mediaMap.end()) {
                        return std::unexpected(invalidArgument("the clipboard's clip " + std::to_string(c.id.value) +
                                                               " plays media it does not hold"));
                    }
                    n.media = m->second;
                }
                if (c.linkGroup.isValid()) {
                    auto g = groups.find(c.linkGroup.value);
                    if (g == groups.end()) {
                        g = groups.emplace(c.linkGroup.value, ed.newLinkGroup()).first;
                    }
                    n.linkGroup = g->second;
                    ed.touchGroup(n.linkGroup);
                }
                if (!n.effects().empty()) {
                    EffectStack effects = n.effects();
                    for (EffectInstance& e : effects) {
                        e.id = ed.newEffectId();
                    }
                    n.setEffects(std::move(effects));
                }
                if (options.mode == PasteMode::Overwrite) {
                    ed.clearRange(dest, n.timelineRange);
                }
                clipMap.emplace(c.id.value, n.id);
                report.pasted.push_back(n.id);
                ed.placeNewClip(dest, std::move(n), false);
            }
            // Transitions between pasted clips.
            for (const Transition& tr : from[i]->transitions) {
                const auto mapped = [&](ClipId id) -> std::optional<ClipId> {
                    if (!id.isValid()) {
                        return ClipId{};
                    }
                    const auto it = clipMap.find(id.value);
                    return it == clipMap.end() ? std::nullopt : std::optional<ClipId>(it->second);
                };
                const auto outgoing = mapped(tr.outgoing);
                const auto incoming = mapped(tr.incoming);
                if (!outgoing || !incoming) {
                    continue;
                }
                Transition t = tr;
                t.id = ed.newTransitionId();
                t.outgoing = *outgoing;
                t.incoming = *incoming;
                Track& track = ed.mutableTrack(dest);
                const Ticks cut = transitionEditPoint(track, t).value_or(Ticks{});
                const auto pos = std::find_if(track.transitions.begin(), track.transitions.end(), [&](const Transition& o) {
                    return transitionEditPoint(track, o).value_or(Ticks{}) > cut;
                });
                track.transitions.insert(pos, std::move(t));
            }
        }
    }
    if (report.pasted.empty()) {
        return std::unexpected(invalidArgument("nothing on the clipboard can be pasted here"));
    }
    if (options.markers && !clipSeq->markers.empty()) {
        Sequence& header = ed.header();
        for (const Marker& m : clipSeq->markers) {
            Marker moved = m;
            moved.start = at + m.start;
            header.markers.push_back(std::move(moved));
        }
        std::stable_sort(header.markers.begin(), header.markers.end(),
                         [](const Marker& a, const Marker& b) { return a.start < b.start; });
    }
    if (options.captions && document.hasCaptions) {
        Sequence& header = ed.header();
        for (std::size_t k = 0; k < clipSeq->captionTracks.size(); ++k) {
            const CaptionTrack& from = clipSeq->captionTracks[k];
            if (from.cues.empty()) {
                continue;
            }
            // Caption track k of the clipboard goes to the sequence's k-th; missing ones are added (like the copied ones).
            while (header.captionTracks.size() <= k) {
                CaptionTrack fresh = clipSeq->captionTracks[header.captionTracks.size()];
                fresh.id = ed.newCaptionTrackId();
                fresh.cues.clear();
                header.captionTracks.push_back(std::move(fresh));
            }
            CaptionTrack& to = header.captionTracks[k];
            const TimeRange range{at, duration};
            // The pasted cues replace what was there (insert opened the room already).
            std::erase_if(to.cues, [&](const CaptionCue& cue) { return cue.end() > range.start && cue.start < range.end(); });
            for (const CaptionCue& cue : from.cues) {
                CaptionCue moved = cue;
                moved.start = at + cue.start;
                const Speaker* sp = cue.speaker.isValid() ? out.findSpeaker(cue.speaker) : nullptr;
                const Speaker* was = cue.speaker.isValid() ? document.content.findSpeaker(cue.speaker) : nullptr;
                if (!sp || !was || sp->name != was->name) {
                    moved.speaker = {};
                }
                to.cues.push_back(std::move(moved));
            }
            std::stable_sort(to.cues.begin(), to.cues.end(), [](const CaptionCue& a, const CaptionCue& b) { return a.start < b.start; });
        }
    }
    EditResult edited = ed.finish(at + duration);
    if (auto valid = validate(edited.state, Validation::Structure); !valid) {
        return std::unexpected(invalidArgument("cannot paste here: " + valid.error().message));
    }
    // Strict for what the paste brings only (E5-P2-2): a clip elsewhere that plays past the end of a file relinked to
    // a shorter one, or a transition elsewhere without handles, is no reason to refuse a paste.
    {
        const std::set<std::uint64_t> pasted = [&] {
            std::set<std::uint64_t> ids;
            for (const ClipId id : report.pasted) {
                ids.insert(id.value);
            }
            return ids;
        }();
        std::optional<std::string> problem;
        edited.state.findSequence(sequenceId)->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                if (!problem && pasted.contains(c.id.value)) {
                    if (const auto p = clipBoundsProblem(edited.state, c)) {
                        problem = "clip '" + c.name() + "' " + *p;
                    }
                }
            }
            for (const Transition& tr : t.transitions) {
                if (!problem && (pasted.contains(tr.outgoing.value) || pasted.contains(tr.incoming.value))) {
                    problem = transitionProblem(edited.state, t, tr, Validation::Strict);
                }
            }
        });
        if (problem) {
            return std::unexpected(invalidArgument("cannot paste here: " + *problem));
        }
    }
    ProjectState pastedState = std::move(edited.state);
    if (document.noCloud && !pastedState.aiNoCloud) {
        // Clips of a no-cloud project keep their lock where they go (review wave 5, S5-P2-12).
        pastedState.aiNoCloud = true;
        report.notes.push_back("this project is now locked to no-cloud AI: the clips came from a no-cloud project");
    }
    PasteResult result;
    result.edit = composed(state, std::move(pastedState));
    result.edit.playhead = at + duration;
    result.edit.created = edited.created; // the pasted clips and pieces an insert cut off
    result.edit.placed = report.pasted;
    result.edit.removed = edited.removed;
    result.edit.createdTransitions = edited.createdTransitions;
    result.edit.createdEffects = edited.createdEffects;
    result.report = std::move(report);
    return result;
}

// ---- Paste / Remove Attributes ---------------------------------------------------------------------------------------

std::expected<AttributesResult, Error> pasteAttributes(const ProjectState& state, SequenceId sequenceId, const Clip& source,
                                                       std::span<const ClipId> targets, const AttributeSet& attributes,
                                                       bool scaleTimes) {
    if (attributes.empty()) {
        return std::unexpected(invalidArgument("choose at least one attribute to paste"));
    }
    const EffectStack& sourceEffects = source.effects();
    std::vector<std::size_t> effects = attributes.effects;
    if (attributes.allEffects) {
        effects.clear();
        for (std::size_t i = 0; i < sourceEffects.size(); ++i) {
            effects.push_back(i);
        }
    }
    for (const std::size_t i : effects) {
        if (i >= sourceEffects.size()) {
            return std::unexpected(invalidArgument("the source clip has no effect #" + std::to_string(i + 1)));
        }
    }
    const bool sourceVideo = source.stream.kind == StreamKind::Video;
    ProjectState current = state;
    std::vector<AttributeChange> changes;
    // Time remapping first: keyframes then land in the target's new timing.
    if (attributes.timeRemap) {
        const Sequence* seq = current.findSequence(sequenceId);
        if (!seq) {
            return std::unexpected(notFound("sequence " + std::to_string(sequenceId.value) + " does not exist"));
        }
        if (auto refs = resolveTargets(*seq, targets); !refs) {
            return std::unexpected(refs.error());
        }
        for (const ClipId id : targets) {
            AttributeChange change{id, {}, {}};
            if (!source.isRemapped()) {
                change.skipped.push_back("Time Remapping: the source clip is not remapped");
            } else {
                const Clip& target = current.findSequence(sequenceId)->findClip(id)->clip();
                const Curve& from = *source.timeRemap();
                const double startValue = from.valueAt(source.sourceIn);
                const Ticks shown = target.sourceTimeAt(target.start());
                const Ticks curveIn = std::max(Ticks{}, target.curveTimeAt(target.start()));
                Curve to = from;
                for (Keyframe& k : to.keys) {
                    Ticks offset = k.time - source.sourceIn;
                    if (scaleTimes && source.duration().value > 0) {
                        offset = Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(offset.value) *
                                                                              static_cast<double>(target.duration().value) /
                                                                              static_cast<double>(source.duration().value)))};
                    }
                    k.time = curveIn + offset;
                    k.value = k.value - startValue + static_cast<double>(shown.value);
                }
                auto remapped = edit::setTimeRemap(current, sequenceId, id, std::move(to), curveIn);
                if (remapped) {
                    current = std::move(remapped->state);
                    change.applied.push_back("Time Remapping");
                } else {
                    change.skipped.push_back("Time Remapping: " + remapped.error().message);
                }
            }
            changes.push_back(std::move(change));
        }
    }
    const Sequence* seq = current.findSequence(sequenceId);
    if (!seq) {
        return std::unexpected(notFound("sequence " + std::to_string(sequenceId.value) + " does not exist"));
    }
    auto refs = resolveTargets(*seq, targets);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    edit::detail::SequenceEditor ed(current, *seq);
    for (const TargetRef& ref : *refs) {
        Clip& target = ed.mutableClip(ref.track, ref.index);
        auto it = std::find_if(changes.begin(), changes.end(), [&](const AttributeChange& c) { return c.clip == target.id; });
        if (it == changes.end()) {
            changes.push_back({target.id, {}, {}});
            it = changes.end() - 1;
        }
        AttributeChange& change = *it;
        const bool targetVideo = target.stream.kind == StreamKind::Video;
        const TimeMapper map(source, target, scaleTimes);
        ClipContent content = target.content();
        bool touched = false;
        const auto paramGroup = [&](bool wanted, bool video, std::span<const ParamId> params, const char* name) {
            if (!wanted) {
                return;
            }
            if (targetVideo != video || sourceVideo != video) {
                change.skipped.push_back(std::string(name) + (video ? ": not a video clip" : ": not an audio clip"));
                return;
            }
            for (const ParamId p : params) {
                content.properties[p] = mapValue(source.properties()[p], map);
            }
            change.applied.push_back(name);
            touched = true;
        };
        paramGroup(attributes.motion, true, kMotion, "Motion");
        constexpr std::array<ParamId, 1> kOpacity{ParamId::Opacity};
        constexpr std::array<ParamId, 1> kVolume{ParamId::VolumeDb};
        constexpr std::array<ParamId, 1> kPan{ParamId::Pan};
        paramGroup(attributes.opacity, true, kOpacity, "Opacity");
        if (attributes.opacity && targetVideo && sourceVideo) {
            content.blendMode = source.blendMode(); // C.15: part of the Opacity section
        }
        paramGroup(attributes.volume, false, kVolume, "Volume");
        paramGroup(attributes.panner, false, kPan, "Panner");
        for (const std::size_t i : effects) {
            const EffectInstance& e = sourceEffects[i];
            if (!effectFits(e, target)) {
                change.skipped.push_back(effectName(e) + (targetVideo ? ": not a video effect" : ": not an audio effect"));
                continue;
            }
            EffectInstance copy = mapEffect(e, map);
            copy.id = ed.newEffectId();
            content.effects.push_back(std::move(copy));
            change.applied.push_back(effectName(e));
            touched = true;
        }
        if (touched) {
            target.setContent(std::move(content));
        }
    }
    const bool any = std::any_of(changes.begin(), changes.end(), [](const AttributeChange& c) { return !c.applied.empty(); });
    if (!any) {
        std::string why = "nothing to paste onto these clips";
        if (!changes.empty() && !changes.front().skipped.empty()) {
            why += " (" + changes.front().skipped.front() + ")";
        }
        return std::unexpected(invalidArgument(why));
    }
    EditResult edited = ed.finish();
    AttributesResult result;
    result.edit = composed(state, std::move(edited.state));
    result.edit.createdEffects = edited.createdEffects;
    result.changes = std::move(changes);
    return result;
}

std::expected<AttributesResult, Error> removeAttributes(const ProjectState& state, SequenceId sequenceId,
                                                        std::span<const ClipId> targets, const AttributeSet& attributes) {
    if (attributes.empty()) {
        return std::unexpected(invalidArgument("choose at least one attribute to remove"));
    }
    ProjectState current = state;
    std::vector<AttributeChange> changes;
    for (const ClipId id : targets) {
        changes.push_back({id, {}, {}});
    }
    const Sequence* seq = current.findSequence(sequenceId);
    if (!seq) {
        return std::unexpected(notFound("sequence " + std::to_string(sequenceId.value) + " does not exist"));
    }
    if (auto refs = resolveTargets(*seq, targets); !refs) {
        return std::unexpected(refs.error());
    }
    if (attributes.timeRemap) {
        for (AttributeChange& change : changes) {
            const auto loc = current.findSequence(sequenceId)->findClip(change.clip);
            if (!loc || !loc->clip().isRemapped()) {
                continue;
            }
            auto off = edit::setTimeRemapEnabled(current, sequenceId, change.clip, false);
            if (off) {
                current = std::move(off->state);
                change.applied.push_back("Time Remapping");
            } else {
                change.skipped.push_back("Time Remapping: " + off.error().message);
            }
        }
    }
    seq = current.findSequence(sequenceId);
    auto refs = resolveTargets(*seq, targets);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    const ClipProperties defaults;
    edit::detail::SequenceEditor ed(current, *seq);
    for (const TargetRef& ref : *refs) {
        Clip& target = ed.mutableClip(ref.track, ref.index);
        AttributeChange& change = *std::find_if(changes.begin(), changes.end(),
                                                [&](const AttributeChange& c) { return c.clip == target.id; });
        const bool video = target.stream.kind == StreamKind::Video;
        ClipContent content = target.content();
        bool touched = false;
        const auto reset = [&](bool wanted, bool forVideo, std::span<const ParamId> params, const char* name) {
            if (!wanted || video != forVideo) {
                return;
            }
            bool differs = false;
            for (const ParamId p : params) {
                differs = differs || !(content.properties[p] == defaults[p]);
                content.properties[p] = defaults[p];
            }
            if (differs) {
                change.applied.push_back(name);
                touched = true;
            }
        };
        reset(attributes.motion, true, kMotion, "Motion");
        constexpr std::array<ParamId, 1> kOpacity{ParamId::Opacity};
        constexpr std::array<ParamId, 1> kVolume{ParamId::VolumeDb};
        constexpr std::array<ParamId, 1> kPan{ParamId::Pan};
        reset(attributes.opacity, true, kOpacity, "Opacity");
        if (attributes.opacity && video && content.blendMode != BlendMode::Normal) {
            content.blendMode = BlendMode::Normal; // C.15: part of the Opacity section
            if (std::find(change.applied.begin(), change.applied.end(), "Opacity") == change.applied.end()) {
                change.applied.push_back("Opacity");
            }
            touched = true;
        }
        reset(attributes.volume, false, kVolume, "Volume");
        reset(attributes.panner, false, kPan, "Panner");
        if (attributes.allEffects || !attributes.effectIds.empty()) {
            EffectStack kept;
            for (const EffectInstance& e : content.effects) {
                const bool remove = attributes.allEffects ||
                                    std::find(attributes.effectIds.begin(), attributes.effectIds.end(), e.effect) !=
                                        attributes.effectIds.end();
                if (remove) {
                    change.applied.push_back(effectName(e));
                    touched = true;
                } else {
                    kept.push_back(e);
                }
            }
            content.effects = std::move(kept);
        }
        if (touched) {
            target.setContent(std::move(content));
        }
    }
    const bool any = std::any_of(changes.begin(), changes.end(), [](const AttributeChange& c) { return !c.applied.empty(); });
    if (!any) {
        return std::unexpected(invalidArgument("these clips have none of those attributes to remove"));
    }
    EditResult edited = ed.finish();
    AttributesResult result;
    result.edit = composed(state, std::move(edited.state));
    result.changes = std::move(changes);
    return result;
}

} // namespace wabcut
