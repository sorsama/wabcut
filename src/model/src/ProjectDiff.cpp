// What changed between two revisions of a project (ProjectDiff.h, C.9).

#include <wabcut/model/ProjectDiff.h>

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>

namespace wabcut {

namespace {

std::string labelOf(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
}

struct Placed {
    const Clip* clip = nullptr;
    std::string track;
    std::tuple<int, std::size_t> order; ///< (kind, track index) for sorting
};

/// The clips of the tracks of `s` that `changed` admits (a track that is the same node on both sides holds the same
/// clips), by clip id.
template <class Admit>
std::unordered_map<std::uint64_t, Placed> clipsOf(const Sequence& s, Admit&& changed) {
    std::unordered_map<std::uint64_t, Placed> out;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = s.tracks(kind);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (!changed(*tracks[i])) {
                continue;
            }
            for (const Clip& c : tracks[i]->clips) {
                out.emplace(c.id.value, Placed{&c, labelOf(kind, i), {static_cast<int>(kind), i}});
            }
        }
    }
    return out;
}

const Track* trackById(const Sequence& s, TrackId id) { return s.findTrack(id); }

void diffEffects(const EffectStack& a, const EffectStack& b, ClipDiff& d) {
    for (const EffectInstance& e : a) {
        const auto it = std::find_if(b.begin(), b.end(), [&](const EffectInstance& x) { return x.id == e.id; });
        if (it == b.end()) {
            d.effectsRemoved.push_back(e.effect);
        } else if (!(*it == e)) {
            d.effectsChanged.push_back(e.effect);
        }
    }
    for (const EffectInstance& e : b) {
        if (std::none_of(a.begin(), a.end(), [&](const EffectInstance& x) { return x.id == e.id; })) {
            d.effectsAdded.push_back(e.effect);
        }
    }
}

ClipDiff compareClips(const Placed& a, const Placed& b) {
    const Clip& x = *a.clip;
    const Clip& y = *b.clip;
    ClipDiff d;
    d.kind = ClipDiff::Kind::Changed;
    d.clip = y.id;
    d.name = y.name();
    d.trackBefore = a.track;
    d.trackAfter = b.track;
    d.before = x.timelineRange;
    d.after = y.timelineRange;
    d.moved = x.start() != y.start() || a.track != b.track;
    d.trimmed = x.duration() != y.duration() || x.sourceIn != y.sourceIn;
    d.retimed = x.speed != y.speed || x.reversed != y.reversed || x.content().timeRemap != y.content().timeRemap;
    d.source = x.media != y.media || x.stream != y.stream || x.sequence != y.sequence || x.angles != y.angles ||
               !(x.graphic() == y.graphic() || (x.graphic() && y.graphic() && *x.graphic() == *y.graphic()));
    d.enabled = x.enabled != y.enabled;
    d.renamed = x.name() != y.name();
    d.relinked = x.linkGroup.isValid() != y.linkGroup.isValid();
    if (!x.sharesContentWith(y)) {
        diffEffects(x.effects(), y.effects(), d);
        for (std::size_t i = 0; i < kParamCount; ++i) {
            const auto id = static_cast<ParamId>(i);
            const ParamValue& p = x.properties()[id];
            const ParamValue& q = y.properties()[id];
            if (p == q) {
                continue;
            }
            (p.isCurve() || q.isCurve() ? d.keyframes : d.parameters).emplace_back(parameterInfo(id).displayName);
        }
        if (x.blendMode() != y.blendMode()) {
            d.parameters.emplace_back("Blend Mode"); // C.15
        }
    }
    return d;
}

bool anyChange(const ClipDiff& d) {
    return d.moved || d.trimmed || d.retimed || d.source || d.enabled || d.renamed || d.relinked ||
           !d.effectsAdded.empty() || !d.effectsRemoved.empty() || !d.effectsChanged.empty() || !d.keyframes.empty() ||
           !d.parameters.empty();
}

SequenceDiff diffSequence(const Sequence& before, const Sequence& after) {
    SequenceDiff d;
    d.id = after.id;
    d.name = after.name;
    d.frameRate = after.frameRate;
    d.dropFrame = after.dropFrame;
    const auto setting = [&](bool differs, const char* what) {
        if (differs) {
            d.settings.emplace_back(what);
        }
    };
    setting(before.name != after.name, "name");
    setting(before.bin != after.bin, "bin");
    setting(!(before.frameRate == after.frameRate) || before.dropFrame != after.dropFrame, "frame rate");
    setting(before.width != after.width || before.height != after.height, "canvas");
    setting(before.startTimecode != after.startTimecode, "start timecode");
    setting(!(before.audioRate == after.audioRate), "audio rate");
    setting(before.duration != after.duration, "duration");
    setting(before.workArea != after.workArea, "work area");
    setting(before.effects != after.effects || before.audioEffects != after.audioEffects ||
                !(before.masterVolume == after.masterVolume) || before.masterFormat != after.masterFormat,
            "master effects");
    setting(before.beatGrid != after.beatGrid, "beat grid");
    setting(before.multicam != after.multicam, "multicam angles");
    d.marksChanged = before.markIn != after.markIn || before.markOut != after.markOut;
    d.markersChanged = before.markers != after.markers;
    d.captionsChanged = before.captionTracks != after.captionTracks;

    // Tracks by id; a track that is the same node on both sides changed nothing.
    std::unordered_map<std::uint64_t, const Track*> old;
    before.forEachTrack([&](const Track& t) { old.emplace(t.id.value, &t); });
    std::unordered_map<std::uint64_t, bool> changedTrack;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = after.tracks(kind);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const Track& t = *tracks[i];
            const auto it = old.find(t.id.value);
            if (it == old.end()) {
                d.tracksAdded.push_back(labelOf(kind, i));
                changedTrack[t.id.value] = true;
                continue;
            }
            const Track& o = *it->second;
            const bool same = &o == &t;
            changedTrack[t.id.value] = !same;
            if (!same) {
                if (o.name != t.name || o.enabled != t.enabled || o.locked != t.locked || o.syncLocked != t.syncLocked ||
                    o.muted != t.muted || o.solo != t.solo || o.role != t.role || o.recordArmed != t.recordArmed ||
                    o.format != t.format || o.surround != t.surround ||
                    o.effects != t.effects ||
                    !(o.volume == t.volume) || !(o.pan == t.pan)) {
                    d.tracksChanged.push_back(labelOf(kind, i));
                }
                d.transitionsChanged = d.transitionsChanged || o.transitions != t.transitions;
            }
        }
    }
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = before.tracks(kind);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (!trackById(after, tracks[i]->id)) {
                d.tracksRemoved.push_back(labelOf(kind, i));
                changedTrack[tracks[i]->id.value] = true;
            }
        }
    }
    const auto admit = [&](const Track& t) {
        const auto it = changedTrack.find(t.id.value);
        return it == changedTrack.end() || it->second;
    };
    const auto a = clipsOf(before, admit);
    const auto b = clipsOf(after, admit);
    std::vector<std::pair<std::tuple<int, std::size_t, std::int64_t, std::uint64_t>, ClipDiff>> found;
    for (const auto& [id, p] : a) {
        const auto it = b.find(id);
        if (it == b.end()) {
            ClipDiff c;
            c.kind = ClipDiff::Kind::Removed;
            c.clip = p.clip->id;
            c.name = p.clip->name();
            c.trackBefore = p.track;
            c.before = p.clip->timelineRange;
            found.push_back({{std::get<0>(p.order), std::get<1>(p.order), p.clip->start().value, id}, std::move(c)});
        } else if (!(*it->second.clip == *p.clip) || it->second.track != p.track) {
            ClipDiff c = compareClips(p, it->second);
            if (anyChange(c)) {
                const Placed& q = it->second;
                found.push_back({{std::get<0>(q.order), std::get<1>(q.order), q.clip->start().value, id}, std::move(c)});
            }
        }
    }
    for (const auto& [id, p] : b) {
        if (!a.contains(id)) {
            ClipDiff c;
            c.kind = ClipDiff::Kind::Added;
            c.clip = p.clip->id;
            c.name = p.clip->name();
            c.trackAfter = p.track;
            c.after = p.clip->timelineRange;
            found.push_back({{std::get<0>(p.order), std::get<1>(p.order), p.clip->start().value, id}, std::move(c)});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (auto& [key, c] : found) {
        d.clips.push_back(std::move(c));
    }
    return d;
}

bool sequenceDiffEmpty(const SequenceDiff& d) {
    return d.settings.empty() && d.tracksAdded.empty() && d.tracksRemoved.empty() && d.tracksChanged.empty() &&
           !d.marksChanged && !d.markersChanged && !d.captionsChanged && !d.transitionsChanged && d.clips.empty();
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& s : items) {
        out += (out.empty() ? "" : ", ") + s;
    }
    return out;
}

} // namespace

bool ProjectDiff::empty() const {
    return sequences.empty() && mediaAdded.empty() && mediaRemoved.empty() && mediaChanged.empty() &&
           reviewNotesAdded == 0 && reviewNotesRemoved == 0 && reviewNotesChanged == 0 && !binsChanged &&
           !assetsChanged && !redactionsChanged && !transcriptsChanged && !graphicsLibraryChanged;
}

ProjectDiff diffProjects(const ProjectState& before, const ProjectState& after) {
    ProjectDiff out;
    // Sequences by id.
    std::map<std::uint64_t, std::pair<const Sequence*, const Sequence*>> seqs;
    for (const SequencePtr& s : before.sequences) {
        seqs[s->id.value].first = s.get();
    }
    for (const SequencePtr& s : after.sequences) {
        seqs[s->id.value].second = s.get();
    }
    for (const auto& [id, pair] : seqs) {
        const auto [a, b] = pair;
        if (a == b) {
            continue; // the same node: unchanged
        }
        if (!a || !b) {
            const Sequence& s = a ? *a : *b;
            SequenceDiff d;
            d.kind = a ? SequenceDiff::Kind::Removed : SequenceDiff::Kind::Added;
            d.id = s.id;
            d.name = s.name;
            d.frameRate = s.frameRate;
            d.dropFrame = s.dropFrame;
            out.sequences.push_back(std::move(d));
            continue;
        }
        SequenceDiff d = diffSequence(*a, *b);
        if (!sequenceDiffEmpty(d)) {
            out.sequences.push_back(std::move(d));
        }
    }
    // Media by id.
    std::map<std::uint64_t, std::pair<const MediaItem*, const MediaItem*>> media;
    for (const MediaPtr& m : before.media) {
        media[m->id.value].first = m.get();
    }
    for (const MediaPtr& m : after.media) {
        media[m->id.value].second = m.get();
    }
    for (const auto& [id, pair] : media) {
        const auto [a, b] = pair;
        if (a == b) {
            continue;
        }
        if (!a) {
            out.mediaAdded.push_back(b->name);
        } else if (!b) {
            out.mediaRemoved.push_back(a->name);
        } else if (!(*a == *b)) {
            out.mediaChanged.push_back(b->name);
        }
    }
    // Review notes by id.
    std::map<std::uint64_t, std::pair<const ReviewNote*, const ReviewNote*>> notes;
    for (const ReviewNote& n : before.reviewNotes) {
        notes[n.id.value].first = &n;
    }
    for (const ReviewNote& n : after.reviewNotes) {
        notes[n.id.value].second = &n;
    }
    for (const auto& [id, pair] : notes) {
        if (!pair.first) {
            ++out.reviewNotesAdded;
        } else if (!pair.second) {
            ++out.reviewNotesRemoved;
        } else if (!(*pair.first == *pair.second)) {
            ++out.reviewNotesChanged;
        }
    }
    out.binsChanged = before.bins != after.bins;
    out.assetsChanged = before.assets != after.assets;
    out.redactionsChanged = before.redactions != after.redactions;
    out.transcriptsChanged = before.speakers != after.speakers || before.transcriptEdits != after.transcriptEdits;
    out.graphicsLibraryChanged = before.brandKit != after.brandKit || !(before.templates == after.templates);
    return out;
}

std::vector<std::string> ProjectDiff::summary() const {
    std::vector<std::string> out;
    for (const SequenceDiff& s : sequences) {
        const auto tc = [&](Ticks t) { return ticksToTimecode(t, s.frameRate, s.dropFrame); };
        const auto range = [&](const TimeRange& r) { return tc(r.start) + "-" + tc(r.end()); };
        const std::string head = "'" + s.name + "'";
        if (s.kind == SequenceDiff::Kind::Added) {
            out.push_back("Added sequence " + head);
            continue;
        }
        if (s.kind == SequenceDiff::Kind::Removed) {
            out.push_back("Removed sequence " + head);
            continue;
        }
        if (!s.settings.empty()) {
            out.push_back(head + ": changed " + joined(s.settings));
        }
        for (const std::string& t : s.tracksAdded) {
            out.push_back(head + ": added track " + t);
        }
        for (const std::string& t : s.tracksRemoved) {
            out.push_back(head + ": removed track " + t);
        }
        for (const std::string& t : s.tracksChanged) {
            out.push_back(head + ": track " + t + " settings or effects changed");
        }
        for (const ClipDiff& c : s.clips) {
            const std::string clip = "'" + c.name + "'";
            if (c.kind == ClipDiff::Kind::Added) {
                out.push_back(head + ": " + c.trackAfter + " " + clip + " added at " + range(c.after));
                continue;
            }
            if (c.kind == ClipDiff::Kind::Removed) {
                out.push_back(head + ": " + c.trackBefore + " " + clip + " removed from " + range(c.before));
                continue;
            }
            std::vector<std::string> what;
            if (c.trimmed) {
                what.push_back("trimmed " + range(c.before) + " -> " + range(c.after));
            } else if (c.moved) {
                what.push_back("moved " + tc(c.before.start) + " -> " + tc(c.after.start));
            }
            if (c.trackBefore != c.trackAfter) {
                what.push_back("moved to " + c.trackAfter);
            }
            if (c.retimed) {
                what.push_back("retimed");
            }
            if (c.source) {
                what.push_back("plays another source");
            }
            if (c.enabled) {
                what.emplace_back("enabled or disabled");
            }
            if (c.renamed) {
                what.emplace_back("renamed");
            }
            if (c.relinked) {
                what.emplace_back("linked or unlinked");
            }
            for (const std::string& e : c.effectsAdded) {
                what.push_back("+ effect " + e);
            }
            for (const std::string& e : c.effectsRemoved) {
                what.push_back("- effect " + e);
            }
            for (const std::string& e : c.effectsChanged) {
                what.push_back("effect " + e + " changed");
            }
            if (!c.keyframes.empty()) {
                what.push_back("keyframes of " + joined(c.keyframes) + " changed");
            }
            if (!c.parameters.empty()) {
                what.push_back(joined(c.parameters) + " changed");
            }
            out.push_back(head + ": " + c.trackAfter + " " + clip + " " + joined(what));
        }
        if (s.transitionsChanged) {
            out.push_back(head + ": transitions changed");
        }
        if (s.marksChanged) {
            out.push_back(head + ": In/Out marks changed");
        }
        if (s.markersChanged) {
            out.push_back(head + ": markers changed");
        }
        if (s.captionsChanged) {
            out.push_back(head + ": captions changed");
        }
    }
    for (const std::string& m : mediaAdded) {
        out.push_back("Added media '" + m + "'");
    }
    for (const std::string& m : mediaRemoved) {
        out.push_back("Removed media '" + m + "'");
    }
    for (const std::string& m : mediaChanged) {
        out.push_back("Media '" + m + "' changed (relinked, renamed, marks or markers)");
    }
    if (reviewNotesAdded + reviewNotesRemoved + reviewNotesChanged > 0) {
        out.push_back("Review notes: " + std::to_string(reviewNotesAdded) + " added, " +
                      std::to_string(reviewNotesRemoved) + " removed, " + std::to_string(reviewNotesChanged) +
                      " changed");
    }
    if (binsChanged) {
        out.emplace_back("Bins changed");
    }
    if (assetsChanged) {
        out.emplace_back("Project files (LUTs) changed");
    }
    if (redactionsChanged) {
        out.emplace_back("Redactions changed");
    }
    if (transcriptsChanged) {
        out.emplace_back("Speakers or transcript corrections changed");
    }
    if (graphicsLibraryChanged) {
        out.emplace_back("Brand kit or graphic templates changed");
    }
    return out;
}

} // namespace wabcut
