#pragma once

// Master Clip Effects (0.6, card #71; docs/effects.md "Master Clip Effects"): Premiere's effects on a master clip in the
// Project panel, which every instance of that media plays before its own effects. They live on the media item
// (MediaItem::masterEffects), so new instances, inserts, overwrites and replaces have them at once; a subclip's clips get
// the master item's first, then the subclip's own. A clip can play without them (ClipContent::masterEffectsOff).
//
// The evaluators (playback video and audio, export) read a clip's played stack through playedEffects(); edits go
// through the effect-stack operations of FinishingOps.h with EffectTarget::ofMedia, or through a clip's operations
// naming a master effect (they act on the stack that holds it). Formats without master clip effects (FCP7 XML, FCPXML,
// OTIO) get them folded onto every instance (flattenMasterEffects), and the export report says so.

#include <wabcut/model/Clip.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>

namespace wabcut {

/// The master clip effects clips of `media` play: a subclip's master item's, then the item's own (every kind).
EffectStack masterEffectsOf(const ProjectState& state, MediaId media);
/// Of one kind (Video or Audio).
EffectStack masterEffectsOf(const ProjectState& state, MediaId media, EffectKind kind);
/// A clip's effects as they play: its media's master clip effects of `kind` (unless the clip turned them off), then
/// its own stack. Clips of nested sequences, multicam sources and generators without master effects: their own stack.
EffectStack playedEffects(const ProjectState& state, const Clip& clip, EffectKind kind);
/// Whether a clip plays master clip effects (its media has some and the clip has not turned them off).
bool playsMasterEffects(const ProjectState& state, const Clip& clip);
/// The media item whose master stack holds `effect` for clips of `media` (the item itself, or a subclip's master item),
/// or null.
const MediaItem* masterEffectHolder(const ProjectState& state, MediaId media, EffectInstanceId effect);
/// The state with every clip's master clip effects folded into its own stack (before its own effects) and the media
/// items' master stacks emptied: what formats without master clip effects can carry. `clips` (optional) counts the clips
/// that took some.
ProjectState flattenMasterEffects(const ProjectState& state, std::size_t* clips = nullptr);

} // namespace wabcut
