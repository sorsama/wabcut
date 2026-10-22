#include <wabcut/model/Animations.h>

#include <algorithm>
#include <array>

namespace wabcut {

namespace {

constexpr std::array<std::string_view, 9> kAnimations{
    "animationBlock", "animationFade", "animationFlip",   "animationFold",   "animationPop",
    "animationPull",  "animationSpin", "animationSpring", "animationTravel",
};

/// splitmix64: neighbouring seeds (ids) give unrelated picks.
std::uint64_t mixed(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

} // namespace

std::span<const std::string_view> animationTransitions() { return kAnimations; }

bool isAnimationTransition(std::string_view effect) {
    return effect == kSurpriseAnimation || std::ranges::find(kAnimations, effect) != kAnimations.end();
}

PickedAnimation surpriseAnimation(std::uint64_t seed, std::vector<EffectParam> params) {
    const std::uint64_t r = mixed(seed);
    PickedAnimation picked;
    picked.effect = std::string(kAnimations[r % kAnimations.size()]);
    // Keep only Start and End; the option (if the animation has one) is picked too.
    std::erase_if(params, [](const EffectParam& p) { return p.key != "start" && p.key != "end"; });
    const std::uint64_t option = (r / kAnimations.size()) % 4;
    if (picked.effect == "animationTravel") {
        params.push_back({"side", ParamValue(static_cast<double>(option))});
    } else if (picked.effect == "animationFold") {
        params.push_back({"hinge", ParamValue(static_cast<double>(option))});
    } else if (picked.effect == "animationFlip") {
        params.push_back({"axis", ParamValue(static_cast<double>(option % 2))});
    } else if (picked.effect == "animationSpin") {
        params.push_back({"turn", ParamValue(static_cast<double>(option % 2))});
    }
    picked.params = std::move(params);
    return picked;
}

} // namespace wabcut
