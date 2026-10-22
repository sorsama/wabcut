#pragma once

// 0.2: Premiere 25.5's Animations as transitions on a clip's edge (the "Animations" bin of the video transitions,
// EffectsLibrary.cpp): dropped on a clip's In or Out point the clip animates in or out; between two clips the incoming
// one animates in over the outgoing one. They are ordinary transitions (a duration handle, one undo step, Delete), so
// they move with their clip. The evaluator (playback/Evaluate.h, transitionAnimationAt) moves the layer itself - its
// Motion, opacity and library effects (Basic 3D for Flip and Fold, Block Dissolve for Block) - so the CPU and GPU paths
// draw the same.

#include <wabcut/model/Effects.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

/// Surprise Me: becomes a random animation when it is added (addTransition, setTransition).
inline constexpr std::string_view kSurpriseAnimation = "animationSurprise";

/// The animations Surprise Me picks from (every Animations transition but Surprise Me), in the bin's order.
std::span<const std::string_view> animationTransitions();
/// An Animations transition (Surprise Me too).
bool isAnimationTransition(std::string_view effect);

/// What Surprise Me becomes for `seed`: one of animationTransitions() and its options (Travel's side, Flip's axis, Fold's
/// hinge, Spin's turn), with `params` (Start and End) kept.
struct PickedAnimation {
    std::string effect;
    std::vector<EffectParam> params;
};
PickedAnimation surpriseAnimation(std::uint64_t seed, std::vector<EffectParam> params = {});

} // namespace wabcut
