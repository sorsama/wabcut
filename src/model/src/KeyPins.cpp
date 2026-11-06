// Keyframes pinned to a clip's start or end follow edits (KeyPins.h).

#include <wabcut/model/KeyPins.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace wabcut::edit {

using detail::SequenceEditor;

namespace {

/// The curves a clip's keyframes live in: its Motion / Opacity / Volume / Panner parameters and its effects' parameters.
template <class F>
void forEachCurve(ClipContent& content, F&& visit) {
    for (ParamValue& value : content.properties.values) {
        if (auto* curve = std::get_if<Curve>(&value.data)) {
            visit(*curve);
        }
    }
    for (EffectInstance& effect : content.effects) {
        for (EffectParam& param : effect.params) {
            if (auto* curve = std::get_if<Curve>(&param.value.data)) {
                visit(*curve);
            }
        }
    }
}

bool curveHasPins(const Curve& curve) {
    return std::any_of(curve.keys.begin(), curve.keys.end(), [](const Keyframe& k) { return k.pin != KeyPin::None; });
}

bool hasPins(const ClipContent& content) {
    for (const ParamValue& value : content.properties.values) {
        if (const Curve* curve = value.curve(); curve && curveHasPins(*curve)) {
            return true;
        }
    }
    for (const EffectInstance& effect : content.effects) {
        for (const EffectParam& param : effect.params) {
            if (const Curve* curve = param.value.curve(); curve && curveHasPins(*curve)) {
                return true;
            }
        }
    }
    return false;
}

/// The curve times of a clip's two ends.
struct Ends {
    Ticks start;
    Ticks end;
};

Ends endsOf(const Clip& clip) { return {clip.startCurveTime(), clip.endCurveTime()}; }

/// Moves the keys of `curve` pinned to an end that moved, from where they sat on it to where it is now. A moved key
/// replaces the key it lands on; the keys stay sorted with distinct times. True when anything moved.
bool followEnds(Curve& curve, const Ends& was, const Ends& now, bool followEnd) {
    struct Item {
        Keyframe key;
        bool moved = false;
    };
    std::vector<Item> items;
    items.reserve(curve.keys.size());
    bool changed = false;
    for (const Keyframe& k : curve.keys) {
        Item item{k, false};
        if (k.pin == KeyPin::ClipStart && k.time == was.start && now.start != was.start) {
            item.key.time = now.start;
            item.moved = true;
        } else if (followEnd && k.pin == KeyPin::ClipEnd && k.time == was.end && now.end != was.end) {
            item.key.time = now.end;
            item.moved = true;
        }
        changed = changed || item.moved;
        items.push_back(std::move(item));
    }
    if (!changed) {
        return false;
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.key.time < b.key.time; });
    std::vector<Keyframe> keys;
    keys.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i + 1 < items.size() && items[i + 1].key.time == items[i].key.time) {
            // Two keys on one tick: the moved one wins (the later of two moved ones).
            if (items[i + 1].moved || !items[i].moved) {
                continue;
            }
            std::swap(items[i], items[i + 1]);
            continue;
        }
        keys.push_back(items[i].key);
    }
    curve.keys = std::move(keys);
    return true;
}

/// A clip that appeared in this edit and shares `clip`'s content with the old end of `clip` as its own end: the other
/// half of a razor cut. The old end (and the keys pinned to it) went to that clip, not with the trimmed one.
bool endHandedOver(const Sequence& now, const Sequence& was, const Clip& clip, Ticks oldEnd) {
    bool handed = false;
    now.forEachTrack([&](const Track& track) {
        if (handed) {
            return;
        }
        for (const Clip& other : track.clips) {
            if (other.id != clip.id && other.sharesContentWith(clip) && !was.findClip(other.id) &&
                other.endCurveTime() == oldEnd) {
                handed = true;
                return;
            }
        }
    });
    return handed;
}

} // namespace

std::optional<ProjectState> followPinnedKeyframes(const ProjectState& before, const ProjectState& after) {
    std::optional<ProjectState> working;
    for (const SequencePtr& now : after.sequences) {
        const Sequence* was = now ? before.findSequence(now->id) : nullptr;
        if (!was || was == now.get()) {
            continue; // new, or shared: nothing in it changed
        }
        struct Fix {
            TrackId track;
            std::size_t index = 0;
            ClipContent content;
        };
        std::vector<Fix> fixes;
        now->forEachTrack([&](const Track& track) {
            const Track* wasTrack = was->findTrack(track.id);
            if (wasTrack == &track) {
                return; // the track is shared: none of its clips changed
            }
            std::size_t index = 0;
            for (const Clip& clip : track.clips) {
                const std::size_t at = index++;
                if (!hasPins(clip.content())) {
                    continue;
                }
                const auto old = was->findClip(clip.id);
                if (!old) {
                    continue; // a new clip (a razor cut's right half, a paste): nothing moved under its keys
                }
                const Ends wasEnds = endsOf(old->clip());
                const Ends nowEnds = endsOf(clip);
                if (wasEnds.start == nowEnds.start && wasEnds.end == nowEnds.end) {
                    continue;
                }
                const bool followEnd = nowEnds.end == wasEnds.end || !endHandedOver(*now, *was, clip, wasEnds.end);
                ClipContent content = clip.content();
                bool changed = false;
                forEachCurve(content, [&](Curve& curve) { changed = followEnds(curve, wasEnds, nowEnds, followEnd) || changed; });
                if (changed) {
                    fixes.push_back({track.id, at, std::move(content)});
                }
            }
        });
        if (fixes.empty()) {
            continue;
        }
        if (!working) {
            working = after;
        }
        const Sequence* target = working->findSequence(now->id);
        SequenceEditor ed(*working, *target);
        for (Fix& fix : fixes) {
            ed.mutableClip(fix.track, fix.index).setContent(std::move(fix.content));
        }
        EditResult result = ed.finish();
        working = std::move(result.state);
    }
    return working;
}

} // namespace wabcut::edit
