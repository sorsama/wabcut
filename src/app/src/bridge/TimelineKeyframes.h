#pragma once

// 0.7 (card 041), Show Clip Keyframes: what the fx badge's menu shows and the timeline draws on a clip - one property of the
// clip (an intrinsic parameter or a parameter of one of its effects) as a value line with its keyframes. The ids are the
// view's own ("p:6" for ParamId 6, "e:12:radius" for the parameter "radius" of effect instance 12).

#include "TimelineView.h"

#include <wabcut/model/Clip.h>
#include <wabcut/model/Parameters.h>

#include <QString>

#include <optional>

namespace wabcut::app {

/// The property's value on the clip (a copy): an effect parameter the instance does not list has the descriptor's default.
/// Empty when the clip does not have it (an effect that was removed, a parameter the effect has no more).
std::optional<ParamValue> shownKeyframeValue(const Clip& clip, const ShownKeyframeProperty& property);

/// "p:6" / "e:12:radius", and back (nullopt for text that is no property id).
QString keyframePropertyId(const ShownKeyframeProperty& property);
std::optional<ShownKeyframeProperty> parseKeyframePropertyId(const QString& id);

} // namespace wabcut::app
