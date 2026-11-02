// Master Clip Effects (0.6, card #71): MasterClipEffects.h.

#include <wabcut/model/MasterClipEffects.h>

#include <wabcut/model/Auditions.h>

#include <algorithm>
#include <array>

namespace wabcut {

namespace {

/// The items whose master stacks clips of `media` play, in order: a subclip's master item, then the item.
std::array<const MediaItem*, 2> holders(const ProjectState& state, MediaId media) {
    const MediaItem* item = media.isValid() ? state.findMedia(media) : nullptr;
    if (!item) {
        return {nullptr, nullptr};
    }
    const MediaItem* master = item->subclip && item->subclip->master != item->id ? state.findMedia(item->subclip->master) : nullptr;
    return {master, item};
}

bool ofKind(const EffectInstance& e, EffectKind kind) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
    return d ? d->kind == kind : kind == EffectKind::Video; // an unknown effect is kept with the picture (bypassed there)
}

} // namespace

EffectStack masterEffectsOf(const ProjectState& state, MediaId media) {
    EffectStack out;
    for (const MediaItem* item : holders(state, media)) {
        if (item) {
            out.insert(out.end(), item->masterEffects.begin(), item->masterEffects.end());
        }
    }
    return out;
}

EffectStack masterEffectsOf(const ProjectState& state, MediaId media, EffectKind kind) {
    EffectStack out;
    for (const MediaItem* item : holders(state, media)) {
        if (!item) {
            continue;
        }
        for (const EffectInstance& e : item->masterEffects) {
            if (ofKind(e, kind)) {
                out.push_back(e);
            }
        }
    }
    return out;
}

bool playsMasterEffects(const ProjectState& state, const Clip& clip) {
    if (clip.content().masterEffectsOff) {
        return false;
    }
    const auto items = holders(state, clip.media);
    return std::ranges::any_of(items, [](const MediaItem* item) { return item && !item->masterEffects.empty(); });
}

EffectStack playedEffects(const ProjectState& state, const Clip& clip, EffectKind kind) {
    if (!playsMasterEffects(state, clip)) {
        return clip.effects();
    }
    EffectStack out = masterEffectsOf(state, clip.media, kind);
    out.insert(out.end(), clip.effects().begin(), clip.effects().end());
    return out;
}

const MediaItem* masterEffectHolder(const ProjectState& state, MediaId media, EffectInstanceId effect) {
    for (const MediaItem* item : holders(state, media)) {
        if (item && std::ranges::any_of(item->masterEffects, [&](const EffectInstance& e) { return e.id == effect; })) {
            return item;
        }
    }
    return nullptr;
}

ProjectState flattenMasterEffects(const ProjectState& state, std::size_t* clips) {
    std::size_t changed = 0;
    const bool any = std::ranges::any_of(state.media, [](const MediaPtr& m) { return m && !m->masterEffects.empty(); });
    if (!any) {
        if (clips) {
            *clips = 0;
        }
        return state;
    }
    ProjectState out = state;
    // Fix F1A (ARGON-06): an audition's other takes play their media's master clip effects once active, and the formats
    // that carry auditions (FCPXML, OTIO) write those takes: they take theirs too (unless a take turned them off).
    const auto takePlaysMaster = [&](const AuditionTake& take) {
        return !take.isNested() && take.media.isValid() && !take.contentOrDefault().masterEffectsOff &&
               !masterEffectsOf(state, take.media).empty();
    };
    const auto foldTakes = [&](Clip& clip, EffectKind kind) {
        const Audition* audition = clip.audition();
        if (!audition || std::ranges::none_of(audition->others, takePlaysMaster)) {
            return false;
        }
        Audition takes = *audition;
        bool foldedAny = false;
        for (AuditionTake& take : takes.others) {
            EffectStack folded = takePlaysMaster(take) ? masterEffectsOf(state, take.media, kind) : EffectStack{};
            if (folded.empty()) {
                continue;
            }
            ClipContent content = take.contentOrDefault();
            folded.insert(folded.end(), content.effects.begin(), content.effects.end());
            content.effects = std::move(folded);
            take.content = std::make_shared<const ClipContent>(std::move(content));
            foldedAny = true;
        }
        if (foldedAny) {
            ClipContent content = clip.content();
            content.audition = std::make_shared<const Audition>(std::move(takes));
            clip.setContent(std::move(content));
        }
        return foldedAny;
    };
    const auto foldTracks = [&](std::vector<TrackPtr>& tracks, EffectKind kind) {
        for (TrackPtr& trackPtr : tracks) {
            if (!trackPtr) {
                continue;
            }
            std::vector<std::size_t> touched;
            for (std::size_t i = 0; i < trackPtr->clips.size(); ++i) {
                const Clip& c = trackPtr->clips[i];
                const Audition* audition = c.audition();
                if (playsMasterEffects(state, c) || (audition && std::ranges::any_of(audition->others, takePlaysMaster))) {
                    touched.push_back(i);
                }
            }
            if (touched.empty()) {
                continue;
            }
            Track track = *trackPtr;
            for (const std::size_t i : touched) {
                Clip& clip = track.clips.mutableAt(i);
                bool took = foldTakes(clip, kind);
                EffectStack folded = playsMasterEffects(state, clip) ? masterEffectsOf(state, clip.media, kind) : EffectStack{};
                if (!folded.empty()) {
                    folded.insert(folded.end(), clip.effects().begin(), clip.effects().end());
                    clip.setEffects(std::move(folded));
                    took = true;
                }
                if (took) {
                    ++changed;
                }
            }
            trackPtr = std::make_shared<const Track>(std::move(track));
        }
    };
    for (SequencePtr& seqPtr : out.sequences) {
        if (!seqPtr) {
            continue;
        }
        Sequence seq = *seqPtr;
        foldTracks(seq.videoTracks, EffectKind::Video);
        foldTracks(seq.audioTracks, EffectKind::Audio);
        seqPtr = std::make_shared<const Sequence>(std::move(seq));
    }
    for (MediaPtr& m : out.media) {
        if (m && !m->masterEffects.empty()) {
            MediaItem item = *m;
            item.masterEffects.clear();
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    if (clips) {
        *clips = changed;
    }
    return out;
}

} // namespace wabcut
