#include "ParameterUi.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::app {

namespace {

#define UI_LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ParameterUi", text)
#define UNDO_LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

// from/to exclude the origin: position X shows -3840..5760 px on a 1920 px frame (origin 960).
constexpr std::array<UiParameter, kParamCount> kUiParameters{{
    {"scale", ParamId::Scale, UI_LABEL("Scale"), UNDO_LABEL("Scale"), "scaling",
     {100.0, UiOrigin::None, 0.0, 400.0, 0.1, 1, UI_LABEL("%")}},
    {"positionX", ParamId::PositionX, UI_LABEL("Position X"), UNDO_LABEL("Position"), "move-horizontal",
     {1.0, UiOrigin::SequenceX, -4800.0, 4800.0, 1.0, 0, UI_LABEL("px"), true}},
    {"positionY", ParamId::PositionY, UI_LABEL("Position Y"), UNDO_LABEL("Position"), "move-vertical",
     {1.0, UiOrigin::SequenceY, -2700.0, 2700.0, 1.0, 0, UI_LABEL("px"), true}},
    {"rotation", ParamId::Rotation, UI_LABEL("Rotation"), UNDO_LABEL("Rotation"), "rotate-cw",
     {1.0, UiOrigin::None, -360.0, 360.0, 0.1, 1, UI_LABEL("°"), true}},
    {"anchorX", ParamId::AnchorX, UI_LABEL("Anchor X"), UNDO_LABEL("Anchor Point"), "crosshair",
     {1.0, UiOrigin::MediaX, -4800.0, 6720.0, 1.0, 0, UI_LABEL("px")}},
    {"anchorY", ParamId::AnchorY, UI_LABEL("Anchor Y"), UNDO_LABEL("Anchor Point"), "crosshair",
     {1.0, UiOrigin::MediaY, -2700.0, 3780.0, 1.0, 0, UI_LABEL("px")}},
    {"opacity", ParamId::Opacity, UI_LABEL("Opacity"), UNDO_LABEL("Opacity"), "blend",
     {100.0, UiOrigin::None, 0.0, 100.0, 0.1, 1, UI_LABEL("%")}},
    {"volume", ParamId::VolumeDb, UI_LABEL("Volume"), UNDO_LABEL("Volume"), "volume-2",
     {1.0, UiOrigin::None, -60.0, 15.0, 0.1, 1, UI_LABEL("dB"), true}},
    {"pan", ParamId::Pan, UI_LABEL("Pan"), UNDO_LABEL("Pan"), "arrow-left-right",
     {100.0, UiOrigin::None, -100.0, 100.0, 1.0, 0, UI_LABEL("L/R"), true}},
}};

} // namespace

double originOf(UiOrigin origin, const PropertyFrame& f) {
    switch (origin) {
    case UiOrigin::None:
        return 0.0;
    case UiOrigin::SequenceX:
        return f.sequenceWidth / 2.0;
    case UiOrigin::SequenceY:
        return f.sequenceHeight / 2.0;
    case UiOrigin::MediaX:
        return f.mediaWidth / 2.0;
    case UiOrigin::MediaY:
        return f.mediaHeight / 2.0;
    }
    return 0.0;
}

std::span<const UiParameter> uiParameters() {
    return kUiParameters;
}

const UiParameter* uiParameter(const QString& key) {
    for (const UiParameter& p : kUiParameters) {
        if (key == QLatin1String(p.key))
            return &p;
    }
    return nullptr;
}

const UiParameter& uiParameter(ParamId id) {
    for (const UiParameter& p : kUiParameters) {
        if (p.id == id)
            return p;
    }
    return kUiParameters.front();
}

UiRange uiRange(const EffectParamInfo& info) {
    UiRange r;
    r.from = info.minValue;
    r.to = info.maxValue;
    const auto span = [&] { return r.to - r.from; };
    switch (info.unit) {
    case ParamUnit::Fraction:
    case ParamUnit::Factor:
        r.factor = 100.0;
        r.unit = UI_LABEL("%");
        r.step = 0.1;
        r.decimals = 1;
        break;
    case ParamUnit::Balance:
        r.factor = 100.0;
        r.unit = UI_LABEL("L/R");
        break;
    case ParamUnit::Degrees:
        r.unit = UI_LABEL("°");
        r.step = 0.1;
        r.decimals = 1;
        break;
    case ParamUnit::Pixels:
        r.unit = UI_LABEL("px");
        if (span() <= 20.0) {
            r.step = 0.1;
            r.decimals = 1;
        }
        break;
    case ParamUnit::Decibels:
        r.unit = UI_LABEL("dB");
        r.step = 0.1;
        r.decimals = 1;
        break;
    case ParamUnit::Stops:
        r.unit = UI_LABEL("EV");
        r.step = 0.01;
        r.decimals = 2;
        break;
    case ParamUnit::Hertz:
        r.unit = UI_LABEL("Hz");
        break;
    case ParamUnit::Milliseconds:
        r.unit = UI_LABEL("ms");
        r.step = 0.1;
        r.decimals = 1;
        break;
    case ParamUnit::Ratio:
        r.step = 0.01;
        r.decimals = 2;
        break;
    case ParamUnit::Amount:
        if (span() <= 20.0) {
            r.step = 0.01;
            r.decimals = 2;
        }
        break;
    }
    r.from *= r.factor;
    r.to *= r.factor;
    // Effect ranges that are "unbounded" in the model get a practical slider range.
    constexpr double kPractical = 4000.0;
    r.from = std::max(r.from, -kPractical);
    r.to = std::min(r.to, kPractical);
    r.bipolar = r.from < 0.0 && r.to > 0.0;
    r.toggle = !info.animatable && info.unit == ParamUnit::Amount && info.minValue == 0.0 && info.maxValue == 1.0;
    return r;
}

QString unitText(const UiRange& range) {
    return range.unit && *range.unit ? QCoreApplication::translate("wabcut::app::ParameterUi", range.unit) : QString();
}

} // namespace wabcut::app
