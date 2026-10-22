// Three-way project merge (Merge.h, C.9).

#include <wabcut/model/Merge.h>

#include <wabcut/model/Nesting.h>
#include <wabcut/model/SubmixOps.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace wabcut {

namespace {

// --- Renumbering ------------------------------------------------------------------------------------------

/// Calls f(id value) for every id and id reference of an effect stack (asset references in its data).
template <class F>
void visitEffects(EffectStack& stack, F& f) {
    for (EffectInstance& e : stack) {
        for (EffectData& d : e.data) {
            if (const std::string* text = d.text()) {
                std::uint64_t v = assetOfReference(*text).value;
                if (v != 0) {
                    f(v);
                    d.value = assetReference(AssetId{v});
                }
            }
        }
    }
}

/// The media ids a graphic refers to (C.13): image layers and image field values.
template <class F>
void visitGraphicIds(Graphic& g, F&& f) {
    const std::function<void(std::vector<GraphicLayer>&)> layers = [&](std::vector<GraphicLayer>& list) {
        for (GraphicLayer& l : list) {
            if (l.kind == LayerKind::Image) {
                f(l.image.media.value);
            }
            layers(l.children);
        }
    };
    layers(g.layers);
    if (g.instance) {
        for (TemplateField& field : g.instance->fields) {
            if (field.kind == FieldKind::Image) {
                f(field.value.media.value);
            }
        }
    }
}

/// Calls f(std::uint64_t&) on every id and id reference of the project (copying the shared nodes it visits).
template <class F>
void visitIds(ProjectState& s, F&& f) {
    for (Bin& b : s.bins) {
        f(b.id.value);
        f(b.parent.value);
    }
    for (MediaPtr& m : s.media) {
        auto c = std::make_shared<MediaItem>(*m);
        f(c->id.value);
        f(c->bin.value);
        if (c->generated) {
            f(c->generated->source.value);
        }
        if (c->derived) {
            f(c->derived->source.value);
        }
        if (c->recording) {
            f(c->recording->sequence.value);
            f(c->recording->track.value);
        }
        m = std::move(c);
    }
    for (ProjectAsset& a : s.assets) {
        f(a.id.value);
    }
    // 0.2 (P.3): Freeform views name a bin and place bins, sequences and media items (weak references, renumbered too).
    for (FreeformView& v : s.freeform) {
        f(v.bin.value);
        for (FreeformArrangement* a : {&v.current}) {
            for (FreeformPlacement& p : a->items) {
                f(p.id);
            }
        }
        for (FreeformLayout& l : v.layouts) {
            for (FreeformPlacement& p : l.arrangement.items) {
                f(p.id);
            }
        }
    }
    for (Redaction& r : s.redactions) {
        f(r.id.value);
        f(r.media.value);
        for (ClipId& c : r.exceptions) {
            f(c.value);
        }
    }
    for (Speaker& sp : s.speakers) {
        f(sp.id.value);
    }
    for (TranscriptEdits& e : s.transcriptEdits.edit()) {
        f(e.media.value);
        for (SpeakerLabel& l : e.labels) {
            f(l.speaker.value);
        }
        for (SpeakerSpan& span : e.speakerSpans) {
            f(span.speaker.value);
        }
    }
    for (ReviewNote& n : s.reviewNotes.edit()) {
        f(n.id.value);
        f(n.sequence.value);
    }
    if (s.brandKit) {
        for (BrandLogo& logo : s.brandKit->logos) {
            f(logo.media.value);
        }
    }
    for (GraphicTemplate& t : s.templates.edit()) {
        visitGraphicIds(t.graphic, f);
        for (TemplateField& field : t.fields) {
            if (field.kind == FieldKind::Image) {
                f(field.value.media.value);
            }
        }
    }
    for (SequencePtr& sp : s.sequences) {
        auto seq = std::make_shared<Sequence>(*sp);
        f(seq->id.value);
        f(seq->bin.value);
        if (seq->derivedFrom) {
            f(seq->derivedFrom->source.value);
        }
        if (seq->multicam) {
            for (MulticamAngle& a : seq->multicam->angles) {
                f(a.video.value);
                for (TrackId& t : a.audio) {
                    f(t.value);
                }
            }
        }
        for (CaptionTrack& ct : seq->captionTracks) {
            f(ct.id.value);
            for (CaptionCue& cue : ct.cues) {
                f(cue.speaker.value);
            }
        }
        visitEffects(seq->effects, f);
        visitEffects(seq->audioEffects, f);
        for (auto* list : {&seq->videoTracks, &seq->audioTracks, &seq->submixTracks}) {
            for (TrackPtr& tp : *list) {
                auto t = std::make_shared<Track>(*tp);
                f(t->id.value);
                f(t->output.value); // p02 audio: the output assignment and the sends name submix tracks
                for (TrackSend& send : t->sends) {
                    f(send.submix.value);
                }
                visitEffects(t->effects, f);
                for (Transition& tr : t->transitions) {
                    f(tr.id.value);
                    f(tr.outgoing.value);
                    f(tr.incoming.value);
                }
                std::vector<Clip> clips = t->clips.toVector();
                for (Clip& c : clips) {
                    f(c.id.value);
                    f(c.media.value);
                    f(c.linkGroup.value);
                    f(c.group.value); // 0.2 clip groups come from the same id generator
                    f(c.sequence.value);
                    if (!c.effects().empty()) {
                        ClipContent content = c.content();
                        visitEffects(content.effects, f);
                        if (!(content == c.content())) {
                            c.setContent(std::move(content));
                        }
                    }
                    if (const Graphic* graphic = c.graphic()) {
                        Graphic g = *graphic;
                        visitGraphicIds(g, f);
                        if (!(g == *graphic)) {
                            ClipContent content = c.content();
                            content.graphic = std::make_shared<const Graphic>(std::move(g));
                            c.setContent(std::move(content));
                        }
                    }
                }
                t->clips = ClipList(std::move(clips));
                tp = std::move(t);
            }
        }
        sp = std::move(seq);
    }
}

// --- Three-way merging ------------------------------------------------------------------------------------

enum class Origin : std::uint8_t { Base, Mine, Theirs };

template <class T>
struct Three {
    const T* b = nullptr;
    const T* m = nullptr;
    const T* t = nullptr;
};

struct Merger {
    const std::map<std::string, MergeChoice>& choices;
    std::vector<MergeConflict> conflicts;
    std::size_t fromTheirs = 0;
    std::vector<std::string> adjustments;

    MergeChoice choose(std::string id, std::string what, SequenceId sequence = {}) {
        const auto it = choices.find(id);
        const MergeChoice c = it == choices.end() ? MergeChoice::Mine : it->second;
        conflicts.push_back({std::move(id), std::move(what), sequence, c});
        return c;
    }

    /// The three-way pick of one object (null = absent) and where it came from.
    template <class T, class Eq>
    std::pair<const T*, Origin> pick(const Three<T>& x, Eq eq, const std::function<std::pair<std::string, std::string>()>& describe,
                                     SequenceId sequence = {}) {
        const auto same = [&](const T* p, const T* q) { return p == q || (p && q && eq(*p, *q)); };
        if (same(x.m, x.t)) {
            return {x.m, same(x.b, x.m) ? Origin::Base : Origin::Mine};
        }
        if (same(x.b, x.m)) {
            ++fromTheirs;
            return {x.t, Origin::Theirs};
        }
        if (same(x.b, x.t)) {
            return {x.m, Origin::Mine};
        }
        const auto [id, what] = describe();
        return choose(id, what, sequence) == MergeChoice::Mine ? std::pair{x.m, Origin::Mine} : std::pair{x.t, Origin::Theirs};
    }
};

/// Where a clip is: its sequence and track.
struct ClipPlace {
    SequenceId sequence;
    TrackId track;
    const Clip* clip = nullptr;

    bool samePlace(const ClipPlace& o) const { return sequence == o.sequence && track == o.track; }
};

std::unordered_map<std::uint64_t, ClipPlace> clipPlaces(const ProjectState& s) {
    std::unordered_map<std::uint64_t, ClipPlace> out;
    for (const SequencePtr& seq : s.sequences) {
        seq->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                out[c.id.value] = ClipPlace{seq->id, t.id, &c};
            }
        });
    }
    return out;
}

/// Where the merge puts a clip that moved on a side (nullopt: nowhere, it was removed) and whose version it is.
struct Placement {
    std::optional<ClipPlace> place;
    Origin origin = Origin::Base;
};

/// Three-way merge of a list of objects keyed by `key`: the merged list in mine's order, then theirs' additions.
template <class T, class Key, class KeyOf, class Eq>
std::vector<T> mergeList(Merger& merger, const std::vector<T>& b, const std::vector<T>& m, const std::vector<T>& t,
                         KeyOf keyOf, Eq eq, const std::function<std::pair<std::string, std::string>(const Key&)>& describe) {
    std::map<Key, Three<T>> all;
    for (const T& x : b) {
        all[keyOf(x)].b = &x;
    }
    for (const T& x : m) {
        all[keyOf(x)].m = &x;
    }
    for (const T& x : t) {
        all[keyOf(x)].t = &x;
    }
    std::map<Key, const T*> chosen;
    for (const auto& [key, three] : all) {
        const auto [item, origin] = merger.pick<T>(three, eq, [&] { return describe(key); });
        if (item) {
            chosen.emplace(key, item);
        }
    }
    std::vector<T> out;
    std::set<Key> placed;
    for (const T& x : m) {
        if (const auto it = chosen.find(keyOf(x)); it != chosen.end()) {
            out.push_back(*it->second);
            placed.insert(it->first);
        }
    }
    for (const T& x : t) {
        if (const auto it = chosen.find(keyOf(x)); it != chosen.end() && !placed.contains(it->first)) {
            out.push_back(*it->second);
            placed.insert(it->first);
        }
    }
    for (const auto& [key, item] : chosen) {
        if (!placed.contains(key)) {
            out.push_back(*item); // kept from the base (both sides removed nothing... a resolved conflict)
        }
    }
    return out;
}

bool sequenceHeaderEq(const Sequence& a, const Sequence& b) {
    return a.name == b.name && a.bin == b.bin && a.frameRate == b.frameRate && a.width == b.width && a.height == b.height &&
           a.audioRate == b.audioRate && a.dropFrame == b.dropFrame && a.startTimecode == b.startTimecode &&
           a.markIn == b.markIn && a.markOut == b.markOut &&
           a.workArea == b.workArea && a.effects == b.effects && a.audioEffects == b.audioEffects &&
           a.masterVolume == b.masterVolume && a.beatGrid == b.beatGrid && a.derivedFrom == b.derivedFrom &&
           a.multicam == b.multicam &&
           std::equal(a.submixTracks.begin(), a.submixTracks.end(), b.submixTracks.begin(), b.submixTracks.end(),
                      [](const TrackPtr& x, const TrackPtr& y) { return x == y || (x && y && *x == *y); });
}

void copySequenceHeader(const Sequence& from, Sequence& to) {
    to.name = from.name;
    to.bin = from.bin;
    to.frameRate = from.frameRate;
    to.width = from.width;
    to.height = from.height;
    to.audioRate = from.audioRate;
    to.dropFrame = from.dropFrame;
    to.startTimecode = from.startTimecode;
    to.markIn = from.markIn;
    to.markOut = from.markOut;
    to.workArea = from.workArea;
    to.effects = from.effects;
    to.audioEffects = from.audioEffects;
    to.masterVolume = from.masterVolume;
    to.beatGrid = from.beatGrid;
    to.derivedFrom = from.derivedFrom;
    to.multicam = from.multicam;
    to.submixTracks = from.submixTracks; // p02 audio: the submix buses go with the sequence's settings
}

bool trackHeaderEq(const Track& a, const Track& b) {
    return a.kind == b.kind && a.name == b.name && a.enabled == b.enabled && a.locked == b.locked &&
           a.syncLocked == b.syncLocked && a.muted == b.muted && a.solo == b.solo && a.targeted == b.targeted &&
           a.role == b.role && a.effects == b.effects && a.volume == b.volume && a.pan == b.pan && a.output == b.output &&
           a.sends == b.sends;
}

void copyTrackHeader(const Track& from, Track& to) {
    to.name = from.name;
    to.enabled = from.enabled;
    to.locked = from.locked;
    to.syncLocked = from.syncLocked;
    to.muted = from.muted;
    to.solo = from.solo;
    to.targeted = from.targeted;
    to.role = from.role;
    to.effects = from.effects;
    to.volume = from.volume;
    to.pan = from.pan;
    to.output = from.output;
    to.sends = from.sends;
}

bool trackEq(const Track& a, const Track& b) { return &a == &b || a == b; }

struct SequenceMerger {
    Merger& merger;
    const Sequence& seq; ///< mine (names and rate for messages)
    const std::map<std::uint64_t, Placement>& placed; ///< clips that moved: placed once, where this says

    std::string label(TrackKind kind, std::size_t index) const {
        return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
    }
    std::string tc(Ticks t) const { return ticksToTimecode(t, seq.frameRate, seq.dropFrame); }

    std::shared_ptr<Track> mergeTrack(const Track& b, const Track& m, const Track& t, const std::string& where) {
        auto out = std::make_shared<Track>(m);
        // Settings.
        const bool mh = trackHeaderEq(m, b);
        const bool th = trackHeaderEq(t, b);
        if (mh && !th) {
            copyTrackHeader(t, *out);
            ++merger.fromTheirs;
        } else if (!mh && !th && !trackHeaderEq(m, t) &&
                   merger.choose("track:" + std::to_string(m.id.value) + "/settings",
                                 where + " settings (name, flags, effects or faders) changed on both sides", seq.id) ==
                       MergeChoice::Theirs) {
            copyTrackHeader(t, *out);
        }
        // Clips, by id.
        std::map<std::uint64_t, Three<Clip>> all;
        for (const Clip& c : b.clips) {
            all[c.id.value].b = &c;
        }
        for (const Clip& c : m.clips) {
            all[c.id.value].m = &c;
        }
        for (const Clip& c : t.clips) {
            all[c.id.value].t = &c;
        }
        struct Chosen {
            Clip clip;
            Origin origin;
            const Clip* base;
        };
        std::vector<Chosen> chosen;
        for (const auto& [id, three] : all) {
            if (const auto p = placed.find(id); p != placed.end()) {
                const std::optional<ClipPlace>& place = p->second.place;
                if (place && place->sequence == seq.id && place->track == m.id) {
                    chosen.push_back({*place->clip, p->second.origin, three.b});
                }
                continue; // decided across the project (it moved): here or not at all
            }
            const auto [clip, origin] = merger.pick<Clip>(
                three, [](const Clip& x, const Clip& y) { return x == y; },
                [&, cid = id] {
                    const Clip& any = three.m ? *three.m : *three.t;
                    return std::pair{"clip:" + std::to_string(cid),
                                     where + " '" + any.name() + "' at " + tc(any.start()) + " was changed on both sides" +
                                         (three.m && three.t ? "" : " (removed on one)")};
                },
                seq.id);
            if (clip) {
                chosen.push_back({*clip, origin, three.b});
            }
        }
        // Clips both sides placed over the same time: one side's change goes back.
        for (int guard = 0; guard < 100000; ++guard) {
            std::sort(chosen.begin(), chosen.end(), [](const Chosen& x, const Chosen& y) {
                return std::tuple(x.clip.start(), x.clip.id) < std::tuple(y.clip.start(), y.clip.id);
            });
            std::size_t clash = 0;
            for (std::size_t i = 1; i < chosen.size() && clash == 0; ++i) {
                if (chosen[i].clip.start() < chosen[i - 1].clip.end()) {
                    clash = i;
                }
            }
            if (clash == 0) {
                break;
            }
            Chosen& x = chosen[clash - 1];
            Chosen& y = chosen[clash];
            const auto [lo, hi] = std::minmax(x.clip.id.value, y.clip.id.value);
            Chosen* theirs = x.origin == Origin::Theirs ? &x : y.origin == Origin::Theirs ? &y : nullptr;
            if (!theirs) {
                // Only after an undone change of theirs can a clip clash with mine: that one goes.
                Chosen* drop = x.origin == Origin::Base ? &x : &y;
                merger.adjustments.push_back(where + " '" + drop->clip.name() + "' no longer fitted after a conflict; it was left out");
                chosen.erase(chosen.begin() + (drop - chosen.data()));
                continue;
            }
            Chosen* other = theirs == &x ? &y : &x;
            const MergeChoice c = merger.choose(
                "overlap:" + std::to_string(m.id.value) + ":" + std::to_string(lo) + ":" + std::to_string(hi),
                where + " '" + x.clip.name() + "' and '" + y.clip.name() + "' were placed over the same time (" +
                    tc(y.clip.start()) + ") on the two sides",
                seq.id);
            // The side not chosen goes back to the base (or out, when it added the clip).
            Chosen* revert = c == MergeChoice::Mine ? theirs : other;
            if (revert->base && revert->origin != Origin::Base) {
                revert->clip = *revert->base;
                revert->origin = Origin::Base;
            } else {
                chosen.erase(chosen.begin() + (revert - chosen.data()));
            }
        }
        std::vector<Clip> clips;
        clips.reserve(chosen.size());
        for (Chosen& c : chosen) {
            clips.push_back(std::move(c.clip));
        }
        out->clips = ClipList(std::move(clips));
        // Transitions, by id.
        out->transitions = mergeList<Transition, std::uint64_t>(
            merger, b.transitions, m.transitions, t.transitions, [](const Transition& x) { return x.id.value; },
            [](const Transition& x, const Transition& y) { return x == y; },
            [&](const std::uint64_t& id) {
                return std::pair{"transition:" + std::to_string(id), where + " a transition was changed on both sides"};
            });
        return out;
    }

    std::vector<TrackPtr> mergeTracks(TrackKind kind, const Sequence& b, const Sequence& m, const Sequence& t) {
        const auto index = [](const Sequence& s, TrackKind k) {
            std::map<std::uint64_t, std::pair<const Track*, std::size_t>> out;
            for (std::size_t i = 0; i < s.tracks(k).size(); ++i) {
                out[s.tracks(k)[i]->id.value] = {s.tracks(k)[i].get(), i};
            }
            return out;
        };
        const auto bi = index(b, kind);
        const auto ti = index(t, kind);
        const auto mi = index(m, kind);
        std::vector<TrackPtr> out;
        for (std::size_t i = 0; i < m.tracks(kind).size(); ++i) {
            const TrackPtr& mp = m.tracks(kind)[i];
            const auto bt = bi.find(mp->id.value);
            const auto tt = ti.find(mp->id.value);
            const std::string where = label(kind, i);
            if (tt == ti.end()) {
                if (bt == bi.end()) {
                    out.push_back(mp); // mine added it
                } else if (trackEq(*bt->second.first, *mp)) {
                    ++merger.fromTheirs; // theirs removed it, mine did not touch it
                } else if (merger.choose("track:" + std::to_string(mp->id.value), where + " was removed on their side and changed on mine",
                                         seq.id) == MergeChoice::Mine) {
                    out.push_back(mp);
                }
                continue;
            }
            const Track& tp = *tt->second.first;
            if (bt == bi.end() || trackEq(*mp, tp)) {
                out.push_back(mp);
            } else if (trackEq(*bt->second.first, *mp)) {
                ++merger.fromTheirs;
                out.push_back(t.tracks(kind)[tt->second.second]);
            } else if (trackEq(*bt->second.first, tp)) {
                out.push_back(mp);
            } else {
                out.push_back(mergeTrack(*bt->second.first, *mp, tp, where));
            }
        }
        for (std::size_t i = 0; i < t.tracks(kind).size(); ++i) {
            const TrackPtr& tp = t.tracks(kind)[i];
            if (mi.contains(tp->id.value)) {
                continue;
            }
            const auto bt = bi.find(tp->id.value);
            if (bt == bi.end()) {
                ++merger.fromTheirs;
                out.push_back(tp); // theirs added it
            } else if (!trackEq(*bt->second.first, *tp) &&
                       merger.choose("track:" + std::to_string(tp->id.value),
                                     label(kind, i) + " was removed on my side and changed on theirs", seq.id) ==
                           MergeChoice::Theirs) {
                out.push_back(tp);
            }
        }
        return out;
    }

    SequencePtr merge(const Sequence& b, const Sequence& m, const Sequence& t) {
        auto out = std::make_shared<Sequence>(m);
        const std::string head = "'" + m.name + "'";
        const bool mh = sequenceHeaderEq(m, b);
        const bool th = sequenceHeaderEq(t, b);
        if (mh && !th) {
            copySequenceHeader(t, *out);
            ++merger.fromTheirs;
        } else if (!mh && !th && !sequenceHeaderEq(m, t) &&
                   merger.choose("sequence:" + std::to_string(m.id.value) + "/settings",
                                 head + " settings (name, rate, canvas, marks, master effects) changed on both sides",
                                 m.id) == MergeChoice::Theirs) {
            copySequenceHeader(t, *out);
        }
        const auto [markers, markersFrom] = merger.pick<std::vector<Marker>>(
            {&b.markers, &m.markers, &t.markers}, [](const auto& x, const auto& y) { return x == y; },
            [&] { return std::pair{"sequence:" + std::to_string(m.id.value) + "/markers", head + " markers changed on both sides"}; },
            m.id);
        out->markers = *markers;
        out->captionTracks = mergeList<CaptionTrack, std::uint64_t>(
            merger, b.captionTracks, m.captionTracks, t.captionTracks, [](const CaptionTrack& x) { return x.id.value; },
            [](const CaptionTrack& x, const CaptionTrack& y) { return x == y; },
            [&](const std::uint64_t& id) {
                return std::pair{"captions:" + std::to_string(id), head + " a caption track changed on both sides"};
            });
        out->videoTracks = mergeTracks(TrackKind::Video, b, m, t);
        out->audioTracks = mergeTracks(TrackKind::Audio, b, m, t);
        out->duration = std::max({m.duration, t.duration, out->contentEnd()});
        return out;
    }
};

/// Keeps a merged sequence valid: link groups left with one clip dissolve, transitions whose clips no longer meet go,
/// the duration covers the content.
SequencePtr tidySequence(const ProjectState& state, SequencePtr seq, std::vector<std::string>& adjustments) {
    std::unordered_map<std::uint64_t, int> members;
    seq->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.linkGroup.isValid()) {
                ++members[c.linkGroup.value];
            }
        }
    });
    auto out = std::make_shared<Sequence>(*seq);
    bool changed = false;
    for (auto* list : {&out->videoTracks, &out->audioTracks}) {
        for (TrackPtr& tp : *list) {
            const bool lonely = std::any_of(tp->clips.begin(), tp->clips.end(), [&](const Clip& c) {
                return c.linkGroup.isValid() && members[c.linkGroup.value] < 2;
            });
            std::vector<Transition> transitions;
            for (const Transition& tr : tp->transitions) {
                if (!transitionProblem(state, *tp, tr, Validation::Structure)) {
                    transitions.push_back(tr);
                }
            }
            if (!lonely && transitions.size() == tp->transitions.size()) {
                continue;
            }
            auto t = std::make_shared<Track>(*tp);
            if (lonely) {
                std::vector<Clip> clips = t->clips.toVector();
                for (Clip& c : clips) {
                    if (c.linkGroup.isValid() && members[c.linkGroup.value] < 2) {
                        c.linkGroup = {};
                    }
                }
                t->clips = ClipList(std::move(clips));
                adjustments.push_back("'" + seq->name + "': a link group left with one clip was dissolved");
            }
            if (transitions.size() != tp->transitions.size()) {
                adjustments.push_back("'" + seq->name + "': a transition whose clips no longer meet was dropped");
            }
            std::sort(transitions.begin(), transitions.end(), [&](const Transition& x, const Transition& y) {
                return *transitionEditPoint(*t, x) < *transitionEditPoint(*t, y);
            });
            t->transitions = std::move(transitions);
            tp = std::move(t);
            changed = true;
        }
    }
    // p02 audio: outputs and sends that name a submix the merge left out go to the Mix; a loop opens at the Mix.
    if (audioRoutingProblem(*out)) {
        const auto keep = [&](TrackId submix) { return out->findSubmix(submix) != nullptr; };
        const bool loop = !submixProcessingOrder(*out);
        for (auto* list : {&out->audioTracks, &out->submixTracks}) {
            for (TrackPtr& tp : *list) {
                auto t = std::make_shared<Track>(*tp);
                if (t->output.isValid() && (!keep(t->output) || (loop && list == &out->submixTracks))) {
                    t->output = TrackId{};
                }
                std::erase_if(t->sends, [&](const TrackSend& s) { return !keep(s.submix); });
                if (t->sends.size() > kMaxTrackSends) {
                    t->sends.resize(kMaxTrackSends);
                }
                tp = std::move(t);
            }
        }
        adjustments.push_back("'" + seq->name + "': audio routing to a submix that was removed now goes to the Mix");
        changed = true;
    }
    out->duration = std::max(out->duration, out->contentEnd());
    if (!changed && out->duration == seq->duration) {
        return seq;
    }
    return out;
}

} // namespace

ProjectState renumberIdsFrom(const ProjectState& state, std::uint64_t threshold, std::uint64_t start) {
    ProjectState out = state;
    std::set<std::uint64_t> found;
    visitIds(out, [&](std::uint64_t& v) {
        if (v >= threshold) {
            found.insert(v);
        }
    });
    std::unordered_map<std::uint64_t, std::uint64_t> to;
    std::uint64_t next = start;
    for (const std::uint64_t v : found) {
        to.emplace(v, next++);
    }
    visitIds(out, [&](std::uint64_t& v) {
        if (const auto it = to.find(v); it != to.end()) {
            v = it->second;
        }
    });
    out.ids.next = std::max(next, std::min(state.ids.next, threshold));
    return out;
}

namespace edit {

std::expected<Created<SequenceId>, Error> copySequenceFrom(const ProjectState& state, const ProjectState& source,
                                                           SequenceId sequence, std::string name) {
    const SequencePtr original = source.sequencePtr(sequence);
    if (!original) {
        return std::unexpected(Error{ErrorCode::NotFound, "sequence " + std::to_string(sequence.value) + " is not in that version"});
    }
    // What the copy needs from the project: its media (the project's own when it still has the same file), nested
    // sequences (must be there), speakers and its bin (dropped when gone).
    std::set<std::uint64_t> keep;
    ProjectState mini;
    mini.ids = source.ids;
    auto seq = std::make_shared<Sequence>(*original);
    if (seq->bin.isValid() && state.findBin(seq->bin)) {
        keep.insert(seq->bin.value);
    } else {
        seq->bin = {};
    }
    for (CaptionTrack& ct : seq->captionTracks) {
        for (CaptionCue& cue : ct.cues) {
            if (cue.speaker.isValid() && state.findSpeaker(cue.speaker)) {
                keep.insert(cue.speaker.value);
            } else {
                cue.speaker = {};
            }
        }
    }
    std::set<std::uint64_t> media;
    std::optional<std::string> missing;
    seq->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.media.isValid()) {
                media.insert(c.media.value);
            }
            if (c.isNested()) {
                if (state.findSequence(c.sequence)) {
                    keep.insert(c.sequence.value);
                } else {
                    missing = "it plays a nested sequence the project no longer has";
                }
            }
        }
    });
    if (missing) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "cannot copy sequence '" + original->name + "': " + *missing});
    }
    // The media id is the source's identity, and source-anchored redactions follow it (review wave 5 E5-P1-3): the copy
    // plays the project's own item when it still has the id (renamed or relinked since: the same source), and an item
    // the project no longer has comes back under its own id (ids are never reused: it is free), where the project's
    // redactions of it - kept when it was removed - apply again. A new id would play it unredacted.
    std::vector<std::uint64_t> renumberedMedia; // only for an id the project cannot take back (not from this project)
    for (const std::uint64_t id : media) {
        if (state.findMedia(MediaId{id})) {
            keep.insert(id);
            continue;
        }
        const MediaItem* from = source.findMedia(MediaId{id});
        if (!from) {
            continue;
        }
        auto item = std::make_shared<MediaItem>(*from);
        if (!state.findBin(item->bin)) {
            item->bin = {};
        } else {
            keep.insert(item->bin.value);
        }
        if (id < state.ids.next) {
            keep.insert(id);
            // What it was made from keeps pointing where it did (its redaction root: model/RedactionMedia.h).
            if (item->derived && item->derived->source.isValid()) {
                keep.insert(item->derived->source.value);
            }
            if (item->generated && item->generated->source.isValid()) {
                keep.insert(item->generated->source.value);
            }
        } else {
            renumberedMedia.push_back(id);
        }
        mini.media.push_back(std::move(item));
    }
    mini.sequences = {std::move(seq)};
    // Everything else gets a new id, in id order (deterministic).
    std::set<std::uint64_t> found;
    visitIds(mini, [&](std::uint64_t& v) {
        if (v != 0 && !keep.contains(v)) {
            found.insert(v);
        }
    });
    // Asset references stay as they are when the project has the asset (they are weak otherwise).
    for (const ProjectAsset& a : state.assets) {
        found.erase(a.id.value);
    }
    std::unordered_map<std::uint64_t, std::uint64_t> to;
    std::uint64_t next = state.ids.next;
    for (const std::uint64_t v : found) {
        to.emplace(v, next++);
    }
    visitIds(mini, [&](std::uint64_t& v) {
        if (const auto it = to.find(v); it != to.end()) {
            v = it->second;
        }
    });
    Created<SequenceId> out{state, {}};
    // A renumbered item (an id this project never had) carries the redactions any revision has of its old id: fail
    // closed rather than play it unredacted.
    std::vector<Redaction> carried;
    std::set<std::uint64_t> carriedFrom; // redaction ids already carried (the same one in both revisions)
    for (const std::uint64_t old : renumberedMedia) {
        const MediaId now{to.at(old)};
        for (const ProjectState* revision : {&state, &source}) {
            for (const Redaction& r : revision->redactions) {
                if (r.media.value == old && carriedFrom.insert(r.id.value).second) {
                    Redaction copy = r;
                    copy.id = RedactionId{next++};
                    copy.media = now;
                    copy.exceptions.clear(); // clips of the original id: none here
                    carried.push_back(std::move(copy));
                }
            }
        }
    }
    out.state.ids.next = next;
    for (MediaPtr& m : mini.media) {
        out.state.media.insert(std::lower_bound(out.state.media.begin(), out.state.media.end(), m->id,
                                                [](const MediaPtr& x, MediaId v) { return x->id < v; }),
                               std::move(m));
    }
    for (Redaction& r : carried) {
        out.state.redactions.push_back(std::move(r)); // the newest ids: the list stays sorted
    }
    auto copy = std::make_shared<Sequence>(*mini.sequences.front());
    copy->name = std::move(name);
    out.id = copy->id;
    out.state.sequences.push_back(std::move(copy)); // the newest id: the list stays sorted
    if (auto valid = validate(out.state, Validation::Structure); !valid) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the copy of '" + original->name + "' is not valid here: " +
                                                                     valid.error().message});
    }
    return out;
}

namespace {

/// Whether `bin` is `ancestor` or lies inside it, in `state`.
bool binInside(const ProjectState& state, BinId bin, BinId ancestor) {
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

/// An item of `state` playing the same file as `item` (same path, content not known to differ): the copy can play it
/// instead. Generated items and subclips are always copied.
const MediaItem* sameFileIn(const ProjectState& state, const MediaItem& item) {
    if (item.isGenerator() || item.isSubclip() || item.locator.path.empty()) {
        return nullptr;
    }
    // The same file: the same normal path (on Windows whatever the case: its file systems ignore case).
    const auto key = [](const std::string& path) {
        std::string out = pathToUtf8(pathFromUtf8(path).lexically_normal());
#ifdef _WIN32
        std::replace(out.begin(), out.end(), '\\', '/');
        std::transform(out.begin(), out.end(), out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
#endif
        return out;
    };
    const std::string wanted = key(item.locator.path);
    for (const MediaPtr& m : state.media) {
        if (m->isGenerator() || m->isSubclip() || m->locator.path.empty()) {
            continue;
        }
        if (key(m->locator.path) != wanted) {
            continue;
        }
        const SourceFingerprint& a = m->locator.fingerprint;
        const SourceFingerprint& b = item.locator.fingerprint;
        if (a.isKnown() && b.isKnown() && !a.sameContentAs(b)) {
            continue;
        }
        return m.get();
    }
    return nullptr;
}

} // namespace

std::expected<ProjectImportResult, Error> importProjectItems(const ProjectState& state, const ProjectState& source,
                                                             const ProjectImportSelection& selection,
                                                             const ProjectImportOptions& options) {
    if (options.into.isValid()) {
        const Bin* into = state.findBin(options.into);
        if (!into) {
            return std::unexpected(Error{ErrorCode::NotFound, "bin " + std::to_string(options.into.value) + " does not exist"});
        }
        if (into->isSearch()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "a search bin cannot hold imported items"});
        }
    }
    // ---- What comes: the selection, what is inside the selected bins, and (to a fixed point) what that plays. ----
    std::set<std::uint64_t> bins;
    std::set<std::uint64_t> sequences;
    std::set<std::uint64_t> media;
    const auto addBin = [&](BinId root) {
        for (const Bin& b : source.bins) {
            if (binInside(source, b.id, root)) {
                bins.insert(b.id.value);
            }
        }
    };
    if (selection.everything) {
        for (const Bin& b : source.bins) {
            bins.insert(b.id.value);
        }
        for (const SequencePtr& s : source.sequences) {
            sequences.insert(s->id.value);
        }
        for (const MediaPtr& m : source.media) {
            media.insert(m->id.value);
        }
    }
    for (const BinId b : selection.bins) {
        if (!source.findBin(b)) {
            return std::unexpected(Error{ErrorCode::NotFound, "bin " + std::to_string(b.value) + " is not in that project"});
        }
        addBin(b);
    }
    for (const SequenceId s : selection.sequences) {
        if (!source.findSequence(s)) {
            return std::unexpected(Error{ErrorCode::NotFound, "sequence " + std::to_string(s.value) + " is not in that project"});
        }
        sequences.insert(s.value);
    }
    for (const MediaId m : selection.media) {
        if (!source.findMedia(m)) {
            return std::unexpected(Error{ErrorCode::NotFound, "media " + std::to_string(m.value) + " is not in that project"});
        }
        media.insert(m.value);
    }
    for (const SequencePtr& s : source.sequences) {
        if (s->bin.isValid() && bins.contains(s->bin.value)) {
            sequences.insert(s->id.value);
        }
    }
    for (const MediaPtr& m : source.media) {
        if (m->bin.isValid() && bins.contains(m->bin.value)) {
            media.insert(m->id.value);
        }
    }
    if (bins.empty() && sequences.empty() && media.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "nothing to import"});
    }
    std::set<std::uint64_t> assets;
    std::set<std::uint64_t> speakers;
    for (bool grew = true; grew;) {
        grew = false;
        ProjectState probe;
        for (const std::uint64_t id : sequences) {
            probe.sequences.push_back(source.sequencePtr(SequenceId{id}));
        }
        std::set<std::uint64_t> seen;
        visitIds(probe, [&](std::uint64_t& v) {
            if (v != 0) {
                seen.insert(v);
            }
        });
        for (const std::uint64_t v : seen) {
            if (source.findSequence(SequenceId{v}) && sequences.insert(v).second) {
                grew = true; // a nested or multicam source: look into it too
            } else if (source.findMedia(MediaId{v})) {
                media.insert(v);
            } else if (source.findAsset(AssetId{v})) {
                assets.insert(v);
            } else if (source.findSpeaker(SpeakerId{v})) {
                speakers.insert(v);
            }
        }
    }

    ProjectImportResult out{state, {}, {}, {}, {}, 0, {}};
    // ---- Media the project already has: the copies play its item. ----
    std::unordered_map<std::uint64_t, std::uint64_t> to; // source id -> id here
    std::set<std::uint64_t> copiedMedia;
    for (const std::uint64_t id : media) {
        const MediaItem* item = source.findMedia(MediaId{id});
        const MediaItem* here = options.reuseMedia ? sameFileIn(state, *item) : nullptr;
        if (here) {
            to.emplace(id, here->id.value);
            ++out.reusedMedia;
        } else {
            copiedMedia.insert(id);
        }
    }

    // ---- The copy, still with the other project's ids. ----
    ProjectState mini;
    for (const Bin& b : source.bins) {
        if (bins.contains(b.id.value)) {
            mini.bins.push_back(b);
        }
    }
    for (const std::uint64_t id : copiedMedia) {
        mini.media.push_back(std::make_shared<const MediaItem>(*source.findMedia(MediaId{id})));
    }
    for (const std::uint64_t id : sequences) {
        mini.sequences.push_back(source.sequencePtr(SequenceId{id}));
    }
    for (const std::uint64_t id : assets) {
        mini.assets.push_back(*source.findAsset(AssetId{id}));
    }
    for (const Redaction& r : source.redactions) {
        if (copiedMedia.contains(r.media.value)) {
            Redaction copy = r;
            std::erase_if(copy.exceptions, [&](ClipId c) {
                bool present = false;
                for (const SequencePtr& s : mini.sequences) {
                    s->forEachTrack([&](const Track& t) { present = present || t.findClip(c) != nullptr; });
                }
                return !present;
            });
            mini.redactions.push_back(std::move(copy));
        }
    }
    for (const TranscriptEdits& e : source.transcriptEdits) {
        if (copiedMedia.contains(e.media.value)) {
            mini.transcriptEdits.push_back(e);
            for (const SpeakerLabel& l : e.labels) {
                speakers.insert(l.speaker.value);
            }
            for (const SpeakerSpan& span : e.speakerSpans) {
                speakers.insert(span.speaker.value);
            }
        }
    }
    for (const Speaker& sp : source.speakers) {
        if (speakers.contains(sp.id.value)) {
            mini.speakers.push_back(sp);
        }
    }
    for (const ReviewNote& n : source.reviewNotes) {
        if (sequences.contains(n.sequence.value)) {
            mini.reviewNotes.push_back(n);
        }
    }
    for (const FreeformView& v : source.freeform) {
        if (v.bin.isValid() && bins.contains(v.bin.value)) {
            mini.freeform.push_back(v);
        }
    }
    // 0.2 (P.3): the copies remember where they came from (ids of the other project: the id walk does not visit them).
    if (!options.sourceProject.empty() && options.sourceProject.size() <= ImportSource::kMaxPathBytes) {
        for (MediaPtr& m : mini.media) {
            MediaItem item = *m;
            item.importSource = ImportSource{options.sourceProject, item.id.value};
            m = std::make_shared<const MediaItem>(std::move(item));
        }
        for (SequencePtr& s : mini.sequences) {
            auto seq = std::make_shared<Sequence>(*s);
            seq->importSource = ImportSource{options.sourceProject, seq->id.value};
            s = std::move(seq);
        }
    }
    // A subclip's master is a weak reference the id walk does not follow: kept when the master comes too.
    std::vector<std::pair<std::size_t, std::uint64_t>> masters;
    for (std::size_t i = 0; i < mini.media.size(); ++i) {
        if (mini.media[i]->subclip && mini.media[i]->subclip->master.isValid()) {
            masters.emplace_back(i, mini.media[i]->subclip->master.value);
        }
    }

    // ---- New ids, in the other project's id order; reused media keep this project's. ----
    std::set<std::uint64_t> found;
    visitIds(mini, [&](std::uint64_t& v) {
        if (v != 0 && !to.contains(v)) {
            found.insert(v);
        }
    });
    std::uint64_t next = state.ids.next;
    for (const std::uint64_t v : found) {
        to.emplace(v, next++);
    }
    visitIds(mini, [&](std::uint64_t& v) {
        if (const auto it = to.find(v); it != to.end()) {
            v = it->second;
        }
    });
    // References that did not come along (weak ones: a stem's source, a recording's sequence, a reframe's source)
    // now name fresh ids nothing has: they dangle, as weak references may.
    for (const auto& [index, master] : masters) {
        MediaItem item = *mini.media[index];
        const auto it = to.find(master);
        item.subclip->master = it != to.end() && copiedMedia.contains(master) ? MediaId{it->second} : MediaId{};
        mini.media[index] = std::make_shared<const MediaItem>(std::move(item));
    }

    // ---- Where it goes. ----
    BinId into = options.into;
    if (!options.binName.empty()) {
        out.bin = BinId{next++};
        out.state.bins.push_back(Bin{out.bin, into, options.binName, std::nullopt});
        into = out.bin;
    }
    std::set<std::uint64_t> copiedBins;
    for (const Bin& b : mini.bins) {
        copiedBins.insert(b.id.value);
    }
    for (Bin b : mini.bins) {
        if (!b.parent.isValid() || !copiedBins.contains(b.parent.value)) {
            b.parent = into;
        }
        out.bins.push_back(b.id);
        out.state.bins.push_back(std::move(b));
    }
    for (MediaPtr& m : mini.media) {
        if (!m->bin.isValid() || !copiedBins.contains(m->bin.value)) {
            MediaItem item = *m;
            item.bin = into;
            m = std::make_shared<const MediaItem>(std::move(item));
        }
        // Content new clips start with (not saved; addMedia and loading make it).
        if (!m->clipDefaults) {
            MediaItem item = *m;
            item.clipDefaults = std::make_shared<const ClipContent>(ClipContent{item.name, {}});
            m = std::make_shared<const MediaItem>(std::move(item));
        }
        out.media.push_back(m->id);
        out.state.media.push_back(m); // new ids are above every id here: the list stays sorted
    }
    for (SequencePtr& s : mini.sequences) {
        if (!s->bin.isValid() || !copiedBins.contains(s->bin.value)) {
            auto seq = std::make_shared<Sequence>(*s);
            seq->bin = into;
            s = std::move(seq);
        }
        out.sequences.push_back(s->id);
        out.state.sequences.push_back(s);
    }
    for (ProjectAsset& a : mini.assets) {
        out.state.assets.push_back(std::move(a));
    }
    for (Redaction& r : mini.redactions.edit()) {
        out.state.redactions.edit().push_back(std::move(r));
    }
    for (Speaker& sp : mini.speakers) {
        out.state.speakers.push_back(std::move(sp));
    }
    std::vector<TranscriptEdits>& edits = out.state.transcriptEdits.edit();
    for (TranscriptEdits& e : mini.transcriptEdits.edit()) {
        edits.push_back(std::move(e));
    }
    std::sort(edits.begin(), edits.end(), [](const TranscriptEdits& a, const TranscriptEdits& b) {
        return std::pair{a.media, a.audioStream} < std::pair{b.media, b.audioStream};
    });
    for (ReviewNote& n : mini.reviewNotes.edit()) {
        out.state.reviewNotes.edit().push_back(std::move(n));
    }
    for (FreeformView& v : mini.freeform) {
        out.state.freeform.push_back(std::move(v));
    }
    std::sort(out.state.freeform.begin(), out.state.freeform.end(),
              [](const FreeformView& a, const FreeformView& b) { return a.bin < b.bin; });

    // ---- Library items graphics and captions name: the ones this project has no item of that name for. ----
    std::set<std::string> templateIds;
    std::set<std::string> textStyles;
    std::set<std::string> captionStyles;
    const std::function<void(const std::vector<GraphicLayer>&)> layers = [&](const std::vector<GraphicLayer>& list) {
        for (const GraphicLayer& l : list) {
            if (!l.textStyle.empty()) {
                textStyles.insert(l.textStyle);
            }
            layers(l.children);
        }
    };
    for (const SequencePtr& s : mini.sequences) {
        for (const CaptionTrack& ct : s->captionTracks) {
            if (!ct.trackStyle.empty()) {
                captionStyles.insert(ct.trackStyle);
            }
        }
        s->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                if (const Graphic* g = c.graphic()) {
                    layers(g->layers);
                    if (g->instance) {
                        templateIds.insert(g->instance->templateId);
                    }
                }
            }
        });
    }
    for (const std::string& id : templateIds) {
        const GraphicTemplate* t = source.findTemplate(id);
        if (t && !out.state.findTemplate(id)) {
            std::vector<GraphicTemplate>& list = out.state.templates.edit();
            list.insert(std::lower_bound(list.begin(), list.end(), id,
                                         [](const GraphicTemplate& x, const std::string& v) { return x.id < v; }),
                        *t);
        } else if (t && !(*out.state.findTemplate(id) == *t)) {
            out.notes.push_back("graphic template '" + id + "' keeps this project's version");
        }
    }
    for (const std::string& name : textStyles) {
        const ProjectTextStyle* s = source.findTextStyle(name);
        if (s && !out.state.findTextStyle(name)) {
            out.state.textStyles.edit().push_back(*s);
        } else if (s && !(*out.state.findTextStyle(name) == *s)) {
            out.notes.push_back("text style '" + name + "' keeps this project's version");
        }
    }
    for (const std::string& name : captionStyles) {
        const CaptionPreset* p = source.findCaptionPreset(name);
        if (p && !out.state.findCaptionPreset(name)) {
            out.state.captionPresets.edit().push_back(*p);
        } else if (p && !(*out.state.findCaptionPreset(name) == *p)) {
            out.notes.push_back("caption preset '" + name + "' keeps this project's version");
        }
    }
    if (source.brandKit && !(source.brandKit == state.brandKit)) {
        out.notes.push_back("the other project's brand kit was not imported (graphics use this project's)");
    }
    out.state.ids.next = next;
    // Merged as wholes elsewhere: tidy what a partial copy can leave (a link group with one member, routing).
    for (SequencePtr& s : out.state.sequences) {
        if (std::find(out.sequences.begin(), out.sequences.end(), s->id) != out.sequences.end()) {
            s = tidySequence(out.state, s, out.notes);
        }
    }
    if (auto valid = validate(out.state, Validation::Structure); !valid) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the imported items are not valid here: " + valid.error().message});
    }
    return out;
}

} // namespace edit

std::expected<MergeResult, Error> mergeProjects(const ProjectState& base, const ProjectState& mine,
                                                const ProjectState& theirsIn, const std::map<std::string, MergeChoice>& choices) {
    for (const auto& [name, s] : {std::pair{"base", &base}, std::pair{"mine", &mine}, std::pair{"theirs", &theirsIn}}) {
        if (auto valid = validate(*s, Validation::Structure); !valid) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         std::string("the ") + name + " project is not valid: " + valid.error().message});
        }
    }
    // Their new ids above every id mine uses.
    const ProjectState theirs = renumberIdsFrom(theirsIn, base.ids.next, std::max(mine.ids.next, base.ids.next));
    Merger merger{choices, {}, 0, {}};
    MergeResult out;
    ProjectState& merged = out.merged;
    merged = mine;
    merged.ids.next = std::max(mine.ids.next, theirs.ids.next);

    const auto same = [](const auto& x, const auto& y) { return x == y; };
    merged.bins = mergeList<Bin, std::uint64_t>(merger, base.bins, mine.bins, theirs.bins, [](const Bin& x) { return x.id.value; },
                                                same, [](const std::uint64_t& id) {
                                                    return std::pair{"bin:" + std::to_string(id), std::string("a bin changed on both sides")};
                                                });
    merged.media = mergeList<MediaPtr, std::uint64_t>(
        merger, base.media, mine.media, theirs.media, [](const MediaPtr& x) { return x->id.value; },
        [](const MediaPtr& x, const MediaPtr& y) { return x == y || *x == *y; },
        [&](const std::uint64_t& id) {
            const MediaItem* any = mine.findMedia(MediaId{id});
            return std::pair{"media:" + std::to_string(id),
                             "media '" + (any ? any->name : std::to_string(id)) + "' changed on both sides"};
        });
    merged.assets = mergeList<ProjectAsset, std::uint64_t>(
        merger, base.assets, mine.assets, theirs.assets, [](const ProjectAsset& x) { return x.id.value; }, same,
        [](const std::uint64_t& id) { return std::pair{"asset:" + std::to_string(id), std::string("a LUT changed on both sides")}; });
    merged.redactions = mergeList<Redaction, std::uint64_t>(
        merger, base.redactions, mine.redactions, theirs.redactions, [](const Redaction& x) { return x.id.value; }, same,
        [](const std::uint64_t& id) {
            return std::pair{"redaction:" + std::to_string(id), std::string("a redaction changed on both sides")};
        });
    merged.speakers = mergeList<Speaker, std::uint64_t>(
        merger, base.speakers, mine.speakers, theirs.speakers, [](const Speaker& x) { return x.id.value; }, same,
        [](const std::uint64_t& id) { return std::pair{"speaker:" + std::to_string(id), std::string("a speaker changed on both sides")}; });
    using EditsKey = std::pair<std::uint64_t, std::int32_t>;
    merged.transcriptEdits = mergeList<TranscriptEdits, EditsKey>(
        merger, base.transcriptEdits, mine.transcriptEdits, theirs.transcriptEdits,
        [](const TranscriptEdits& x) { return EditsKey{x.media.value, x.audioStream}; }, same, [](const EditsKey& k) {
            return std::pair{"transcript:" + std::to_string(k.first) + ":" + std::to_string(k.second),
                             std::string("transcript corrections or speakers of a medium changed on both sides")};
        });
    merged.captionPresets = mergeList<CaptionPreset, std::string>(
        merger, base.captionPresets, mine.captionPresets, theirs.captionPresets,
        [](const CaptionPreset& x) {
            std::string k = x.name;
            std::transform(k.begin(), k.end(), k.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
            return k;
        },
        same,
        [](const std::string& k) { return std::pair{"captionPreset:" + k, "caption preset '" + k + "' changed on both sides"}; });
    merged.reviewNotes = mergeList<ReviewNote, std::uint64_t>(
        merger, base.reviewNotes, mine.reviewNotes, theirs.reviewNotes, [](const ReviewNote& x) { return x.id.value; }, same,
        [](const std::uint64_t& id) {
            return std::pair{"note:" + std::to_string(id), std::string("a review note changed on both sides")};
        });
    std::vector<ReviewNote>& notes = merged.reviewNotes.edit();
    std::sort(notes.begin(), notes.end(), [](const ReviewNote& x, const ReviewNote& y) { return x.id < y.id; });
    if (mine.aiNoCloud != theirs.aiNoCloud) {
        merged.aiNoCloud = mine.aiNoCloud == base.aiNoCloud ? theirs.aiNoCloud : mine.aiNoCloud;
    }
    // C.13: graphic templates by id, the brand kit as one object.
    merged.templates = mergeList<GraphicTemplate, std::string>(
        merger, base.templates, mine.templates, theirs.templates, [](const GraphicTemplate& x) { return x.id; }, same,
        [](const std::string& id) { return std::pair{"template:" + id, "graphic template '" + id + "' changed on both sides"}; });
    std::vector<GraphicTemplate>& templates = merged.templates.edit();
    std::sort(templates.begin(), templates.end(), [](const GraphicTemplate& x, const GraphicTemplate& y) { return x.id < y.id; });
    // 0.2: text styles by name.
    merged.textStyles = mergeList<ProjectTextStyle, std::string>(
        merger, base.textStyles, mine.textStyles, theirs.textStyles, [](const ProjectTextStyle& x) { return x.name; }, same,
        [](const std::string& name) { return std::pair{"textStyle:" + name, "text style '" + name + "' changed on both sides"}; });
    // 0.2 (P.3): each bin's Freeform view as a whole; views of bins the merge removed go.
    merged.freeform = mergeList<FreeformView, std::uint64_t>(
        merger, base.freeform, mine.freeform, theirs.freeform, [](const FreeformView& x) { return x.bin.value; }, same,
        [](const std::uint64_t& bin) {
            return std::pair{"freeform:" + std::to_string(bin), std::string("a bin's Freeform view changed on both sides")};
        });
    std::sort(merged.freeform.begin(), merged.freeform.end(),
              [](const FreeformView& x, const FreeformView& y) { return x.bin < y.bin; });
    std::erase_if(merged.freeform, [&](const FreeformView& v) {
        return v.bin.isValid() && (!merged.findBin(v.bin) || merged.findBin(v.bin)->isSearch());
    });
    {
        const auto ptr = [](const ProjectState& s) { return s.brandKit ? &*s.brandKit : nullptr; };
        const Three<BrandKit> kit{ptr(base), ptr(mine), ptr(theirs)};
        const auto [chosen, origin] = merger.pick<BrandKit>(kit, same, [] {
            return std::pair{std::string("brandKit"), std::string("the brand kit changed on both sides")};
        });
        merged.brandKit = chosen ? std::optional<BrandKit>(*chosen) : std::nullopt;
    }
    {
        const Three<ColorManagement> cm{&base.colorManagement, &mine.colorManagement, &theirs.colorManagement};
        merged.colorManagement = *merger.pick<ColorManagement>(cm, std::equal_to<>{}, [] {
            return std::pair{std::string("colorManagement"), std::string("the colour management changed on both sides")};
        }).first;
    }

    // Clips that moved to another track or sequence on a side are matched across the project and placed once
    // (E5-P2-5): merged track by track, a clip moved on one side and trimmed on the other would end up in both places.
    const auto inBase = clipPlaces(base);
    const auto inMine = clipPlaces(mine);
    const auto inTheirs = clipPlaces(theirs);
    std::map<std::uint64_t, Placement> placed;
    for (const auto& [id, b] : inBase) {
        const auto mi = inMine.find(id);
        const auto ti = inTheirs.find(id);
        const ClipPlace* mp = mi == inMine.end() ? nullptr : &mi->second;
        const ClipPlace* tp = ti == inTheirs.end() ? nullptr : &ti->second;
        if (!(mp && !mp->samePlace(b)) && !(tp && !tp->samePlace(b))) {
            continue; // not moved: merged on its track
        }
        const auto unchanged = [&](const ClipPlace* p) { return p && p->samePlace(b) && *p->clip == *b.clip; };
        const auto at = [](const ClipPlace* p) { return p ? std::optional<ClipPlace>(*p) : std::nullopt; };
        Placement placement;
        if (mp && tp && mp->samePlace(*tp) && *mp->clip == *tp->clip) {
            placement = {at(mp), Origin::Mine};
        } else if (unchanged(mp)) {
            placement = {at(tp), Origin::Theirs};
            ++merger.fromTheirs;
        } else if (unchanged(tp)) {
            placement = {at(mp), Origin::Mine};
        } else {
            const Clip& any = mp ? *mp->clip : tp ? *tp->clip : *b.clip;
            const MergeChoice c = merger.choose("clip:" + std::to_string(id),
                                                "'" + any.name() + "' was moved on one side and changed, moved or removed "
                                                                   "on the other",
                                                b.sequence);
            placement = c == MergeChoice::Mine ? Placement{at(mp), Origin::Mine} : Placement{at(tp), Origin::Theirs};
        }
        placed.emplace(id, placement);
    }

    // Sequences.
    std::map<std::uint64_t, Three<Sequence>> seqs;
    for (const SequencePtr& s : base.sequences) {
        seqs[s->id.value].b = s.get();
    }
    for (const SequencePtr& s : mine.sequences) {
        seqs[s->id.value].m = s.get();
    }
    for (const SequencePtr& s : theirs.sequences) {
        seqs[s->id.value].t = s.get();
    }
    const auto ptrOf = [](const ProjectState& s, const Sequence* raw) {
        for (const SequencePtr& p : s.sequences) {
            if (p.get() == raw) {
                return p;
            }
        }
        return SequencePtr{};
    };
    merged.sequences.clear();
    for (const auto& [id, three] : seqs) {
        const auto eq = [](const Sequence& x, const Sequence& y) { return x == y; };
        const bool bothChanged = three.b && three.m && three.t && !eq(*three.b, *three.m) && !eq(*three.b, *three.t) &&
                                 !eq(*three.m, *three.t);
        if (bothChanged) {
            SequenceMerger sm{merger, *three.m, placed};
            merged.sequences.push_back(sm.merge(*three.b, *three.m, *three.t));
            continue;
        }
        const auto [chosen, origin] = merger.pick<Sequence>(three, eq, [&, sid = id] {
            const Sequence& any = three.m ? *three.m : *three.t;
            return std::pair{"sequence:" + std::to_string(sid), "sequence '" + any.name + "' was removed on one side and changed on the other"};
        });
        if (chosen) {
            merged.sequences.push_back(origin == Origin::Theirs ? ptrOf(theirs, chosen) : ptrOf(mine, chosen));
            if (!merged.sequences.back()) {
                merged.sequences.back() = ptrOf(base, chosen);
            }
        }
    }

    // What the merged clips need: their media, nested sequences and speakers stay (restored from any side).
    const auto restoreMedia = [&](MediaId id) {
        if (!id.isValid() || merged.findMedia(id)) {
            return;
        }
        for (const ProjectState* s : {&theirs, &mine, &base}) {
            for (const MediaPtr& m : s->media) {
                if (m->id == id) {
                    merged.media.insert(std::lower_bound(merged.media.begin(), merged.media.end(), id,
                                                         [](const MediaPtr& x, MediaId v) { return x->id < v; }),
                                        m);
                    out.adjustments.push_back("media '" + m->name + "' was kept: a merged clip uses it");
                    return;
                }
            }
        }
    };
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < merged.sequences.size(); ++i) {
            const SequencePtr seq = merged.sequences[i];
            seq->forEachTrack([&](const Track& t) {
                for (const Clip& c : t.clips) {
                    restoreMedia(c.media);
                    if (const Graphic* graphic = c.graphic()) { // C.13: the pictures its image layers show
                        Graphic g = *graphic;
                        visitGraphicIds(g, [&](std::uint64_t& v) { restoreMedia(MediaId{v}); });
                    }
                    if (c.isNested() && !merged.findSequence(c.sequence)) {
                        for (const ProjectState* s : {&theirs, &mine, &base}) {
                            if (const SequencePtr p = s->sequencePtr(c.sequence)) {
                                merged.sequences.insert(std::lower_bound(merged.sequences.begin(), merged.sequences.end(), p->id,
                                                                         [](const SequencePtr& x, SequenceId v) { return x->id < v; }),
                                                        p);
                                out.adjustments.push_back("sequence '" + p->name + "' was kept: a merged clip plays it");
                                break;
                            }
                        }
                    }
                }
            });
        }
    }
    std::sort(merged.sequences.begin(), merged.sequences.end(), [](const SequencePtr& x, const SequencePtr& y) { return x->id < y->id; });
    if (merged.brandKit) { // C.13: the brand kit's logos
        for (const BrandLogo& logo : merged.brandKit->logos) {
            restoreMedia(logo.media);
        }
    }
    for (const SequencePtr& seq : merged.sequences) {
        for (const CaptionTrack& ct : seq->captionTracks) {
            for (const CaptionCue& cue : ct.cues) {
                if (cue.speaker.isValid() && !merged.findSpeaker(cue.speaker)) {
                    for (const ProjectState* s : {&theirs, &mine, &base}) {
                        if (const Speaker* sp = s->findSpeaker(cue.speaker)) {
                            merged.speakers.insert(std::lower_bound(merged.speakers.begin(), merged.speakers.end(), sp->id,
                                                                    [](const Speaker& x, SpeakerId v) { return x.id < v; }),
                                                   *sp);
                            break;
                        }
                    }
                }
            }
        }
    }
    // A whole sequence or track taken from one side may still hold a moved clip where the merge did not place it.
    if (!placed.empty()) {
        const auto belongs = [&](const Clip& c, SequenceId s, TrackId t) {
            const auto p = placed.find(c.id.value);
            return p == placed.end() || (p->second.place && p->second.place->sequence == s && p->second.place->track == t);
        };
        for (SequencePtr& sp : merged.sequences) {
            bool stray = false;
            sp->forEachTrack([&](const Track& t) {
                for (const Clip& c : t.clips) {
                    stray = stray || !belongs(c, sp->id, t.id);
                }
            });
            if (!stray) {
                continue;
            }
            auto seq = std::make_shared<Sequence>(*sp);
            for (auto* list : {&seq->videoTracks, &seq->audioTracks}) {
                for (TrackPtr& tp : *list) {
                    if (std::all_of(tp->clips.begin(), tp->clips.end(), [&](const Clip& c) { return belongs(c, sp->id, tp->id); })) {
                        continue;
                    }
                    auto t = std::make_shared<Track>(*tp);
                    std::vector<Clip> clips;
                    for (const Clip& c : tp->clips) {
                        if (belongs(c, sp->id, tp->id)) {
                            clips.push_back(c);
                        } else {
                            out.adjustments.push_back("'" + seq->name + "': '" + c.name() +
                                                      "' moved on one side; it stays only where the merge placed it");
                        }
                    }
                    t->clips = ClipList(std::move(clips));
                    tp = std::move(t);
                }
            }
            sp = std::move(seq);
        }
    }
    // Nested clips the two sides added that make sequences play each other (mine nests Q in P, theirs P in Q): one
    // conflict per cycle; the side not chosen loses its nested clips that close it.
    for (int round = 0; round < 64 && nestingCycle(merged); ++round) {
        struct Cyclic {
            SequenceId sequence;
            ClipId clip;
            Origin origin;
        };
        std::vector<Cyclic> cyclic;
        std::uint64_t lowest = 0;
        std::vector<std::string> names;
        for (const SequencePtr& sp : merged.sequences) {
            sp->forEachTrack([&](const Track& t) {
                for (const Clip& c : t.clips) {
                    if (!c.isNested() || !sequenceContains(merged, c.sequence, sp->id)) {
                        continue;
                    }
                    const auto mi = inMine.find(c.id.value);
                    const auto ti = inTheirs.find(c.id.value);
                    const bool m = mi != inMine.end() && mi->second.sequence == sp->id;
                    const bool th = ti != inTheirs.end() && ti->second.sequence == sp->id;
                    cyclic.push_back({sp->id, c.id, m && th ? Origin::Base : m ? Origin::Mine : Origin::Theirs});
                    if (lowest == 0 || sp->id.value < lowest) {
                        lowest = sp->id.value;
                    }
                    if (std::find(names.begin(), names.end(), sp->name) == names.end()) {
                        names.push_back(sp->name);
                    }
                }
            });
        }
        std::string list;
        for (const std::string& n : names) {
            list += (list.empty() ? "'" : ", '") + n + "'";
        }
        const MergeChoice c = merger.choose("nesting:" + std::to_string(lowest),
                                            list + " would play each other: each side nested one into the other",
                                            SequenceId{lowest});
        const Origin drop = c == MergeChoice::Mine ? Origin::Theirs : Origin::Mine;
        std::set<std::uint64_t> dropped;
        for (const Cyclic& k : cyclic) {
            if (k.origin == drop) {
                dropped.insert(k.clip.value);
            }
        }
        if (dropped.empty()) {
            break; // not one side's doing: the validation below reports it
        }
        for (SequencePtr& sp : merged.sequences) {
            bool has = false;
            sp->forEachTrack([&](const Track& t) {
                for (const Clip& clip : t.clips) {
                    has = has || dropped.contains(clip.id.value);
                }
            });
            if (!has) {
                continue;
            }
            auto seq = std::make_shared<Sequence>(*sp);
            for (auto* tracks : {&seq->videoTracks, &seq->audioTracks}) {
                for (TrackPtr& tp : *tracks) {
                    if (std::none_of(tp->clips.begin(), tp->clips.end(), [&](const Clip& clip) { return dropped.contains(clip.id.value); })) {
                        continue;
                    }
                    auto t = std::make_shared<Track>(*tp);
                    std::vector<Clip> clips;
                    for (const Clip& clip : tp->clips) {
                        if (!dropped.contains(clip.id.value)) {
                            clips.push_back(clip);
                        }
                    }
                    t->clips = ClipList(std::move(clips));
                    tp = std::move(t);
                }
            }
            out.adjustments.push_back("'" + seq->name + "': a nested clip that made sequences play each other was left out");
            sp = std::move(seq);
        }
    }
    std::erase_if(merged.reviewNotes.edit(), [&](const ReviewNote& n) {
        const bool gone = !merged.findSequence(n.sequence);
        if (gone) {
            out.adjustments.push_back("a review note on a removed sequence was dropped");
        }
        return gone;
    });
    for (SequencePtr& seq : merged.sequences) {
        seq = tidySequence(merged, seq, out.adjustments);
    }
    std::sort(merger.conflicts.begin(), merger.conflicts.end(),
              [](const MergeConflict& x, const MergeConflict& y) { return x.id < y.id; });
    out.conflicts = std::move(merger.conflicts);
    out.fromTheirs = merger.fromTheirs;
    for (std::string& a : merger.adjustments) {
        out.adjustments.push_back(std::move(a));
    }
    if (auto valid = validate(merged, Validation::Structure); !valid) {
        // The conflicts still go back (E5-P2-5): other choices may resolve it, and the caller stages them.
        out.invalid = "the merge cannot produce a valid project with these choices (choose otherwise, or merge by hand): " +
                      valid.error().message;
        merged = mine;
    }
    return out;
}

} // namespace wabcut
