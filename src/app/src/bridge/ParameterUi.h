#pragma once

// Parameters in UI units: how the clip editor and Effect Controls show model parameter values.
//
// Intrinsic parameters (Motion, Opacity, Volume, Panner; ClipProperties) have a fixed table: position is shown in
// sequence pixels from the frame's top-left and the anchor in source pixels from the media's top-left (like
// Premiere), while the model stores both as offsets from the centre; scale and opacity are percentages, pan is
// -100..100. Effect parameters (EffectRegistry descriptors) are mapped by their unit: fractions and factors become
// percentages, balances -100..100, and very wide model ranges get a practical slider range.
//
//   ui = model * factor + origin        model = (ui - origin) / factor

#include <wabcut/model/Effects.h>
#include <wabcut/model/Parameters.h>

#include <QString>

#include <span>

namespace wabcut::app {

/// Sizes the position/anchor origins depend on.
struct PropertyFrame {
    double sequenceWidth = 1920, sequenceHeight = 1080;
    double mediaWidth = 1920, mediaHeight = 1080;
};

enum class UiOrigin { None, SequenceX, SequenceY, MediaX, MediaY };

double originOf(UiOrigin origin, const PropertyFrame& frame);

/// How one parameter is shown: slider/field range, step and decimals in UI units, the unit suffix and an icon.
struct UiRange {
    double factor = 1.0;
    UiOrigin origin = UiOrigin::None;
    double from = 0.0, to = 100.0; ///< UI units (origin excluded: add originOf())
    double step = 1.0;
    int decimals = 0;
    const char* unit = "";        ///< translated with the "wabcut::app::ParameterUi" context
    bool bipolar = false;         ///< the slider fills from 0 (rotation, pan, offsets)
    bool toggle = false;          ///< a switch (a non-animatable 0..1 parameter)

    double toUi(double model, const PropertyFrame& frame) const { return model * factor + originOf(origin, frame); }
    double toModel(double ui, const PropertyFrame& frame) const { return (ui - originOf(origin, frame)) / factor; }
};

/// An intrinsic parameter of the clip editor.
struct UiParameter {
    const char* key;       ///< clip editor key ("scale", "positionX", ...)
    ParamId id;
    const char* label;     ///< display label ("Position X"; "wabcut::app::ParameterUi" context)
    const char* undoLabel; ///< undo label ("Position"; "wabcut::app::ProjectController" context)
    const char* icon;      ///< icon name
    UiRange range;
};

/// The clip editor's intrinsic parameters, in display order (Transform, Opacity, Volume/Pan).
std::span<const UiParameter> uiParameters();
const UiParameter* uiParameter(const QString& key);
const UiParameter& uiParameter(ParamId id);

/// UI range of an effect (or transition) parameter.
UiRange uiRange(const EffectParamInfo& info);

/// Display text of a unit suffix ("%", "px", "°", "dB", ...), translated.
QString unitText(const UiRange& range);

} // namespace wabcut::app
