// TimelineView's Show Clip Keyframes (0.7, card 041; Premiere's menu on a clip's fx badge). A click on the fx badge selects the
// clip and opens a menu (QML: ClipKeyframesMenu) of the clip's properties that can show their keyframes on the clip: Motion
// and Opacity of a video clip, Volume and Pan of an audio clip, then each effect's animatable parameters; the ones that have
// keyframes are marked. Choosing one draws its value line and keys over the clip (TimelineView.cpp, addKeyframeProperty),
// ranged to the keys' values; a click on the same item (or Hide Clip Keyframes) hides it. One property per clip. The choice
// is view state: nothing is saved, nothing is edited, so it is no undo step. Keys are edited in Effect Controls; this
// view lets you see where they are and jump between them (seekClipKeyframe, the menu's Previous / Next Keyframe).

#include "TimelineView.h"

#include "BridgeIds.h"
#include "ParameterUi.h"
#include "TimelineKeyframes.h"

#include <wabcut/model/Effects.h>

#include <QCoreApplication>

#include <algorithm>

namespace wabcut::app {

namespace {

/// The effect instance `id` of the clip's stack, or null.
const EffectInstance* effectOf(const Clip& clip, std::uint64_t id) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.id.value == id)
            return &e;
    }
    return nullptr;
}

QString effectText(const char* text) {
    return QCoreApplication::translate("wabcut::app::EffectControlsModel", text);
}

} // namespace

std::optional<ParamValue> shownKeyframeValue(const Clip& clip, const ShownKeyframeProperty& property) {
    if (property.param >= 0) {
        if (static_cast<std::size_t>(property.param) >= kParamCount)
            return std::nullopt;
        return clip.properties()[static_cast<ParamId>(property.param)];
    }
    const EffectInstance* effect = effectOf(clip, property.effect);
    if (!effect)
        return std::nullopt;
    if (const ParamValue* value = effect->find(property.key))
        return *value;
    // An effect lists only the parameters that are off their default.
    const EffectDescriptor* descriptor = EffectRegistry::builtin().find(effect->effect);
    const EffectParamInfo* info = descriptor ? descriptor->param(property.key) : nullptr;
    if (!info || !info->animatable)
        return std::nullopt;
    return ParamValue(info->defaultValue);
}

QString keyframePropertyId(const ShownKeyframeProperty& property) {
    if (property.param >= 0)
        return QStringLiteral("p:%1").arg(property.param);
    return QStringLiteral("e:%1:%2").arg(property.effect).arg(QString::fromStdString(property.key));
}

std::optional<ShownKeyframeProperty> parseKeyframePropertyId(const QString& id) {
    ShownKeyframeProperty out;
    if (id.startsWith(QLatin1String("p:"))) {
        bool ok = false;
        const int param = id.mid(2).toInt(&ok);
        if (!ok || param < 0 || static_cast<std::size_t>(param) >= kParamCount)
            return std::nullopt;
        out.param = param;
        return out;
    }
    if (id.startsWith(QLatin1String("e:"))) {
        const qsizetype colon = id.indexOf(QLatin1Char(':'), 2);
        if (colon < 0)
            return std::nullopt;
        bool ok = false;
        const quint64 effect = id.mid(2, colon - 2).toULongLong(&ok);
        const QString key = id.mid(colon + 1);
        if (!ok || key.isEmpty())
            return std::nullopt;
        out.effect = effect;
        out.key = key.toStdString();
        return out;
    }
    return std::nullopt;
}

QVariantList TimelineView::clipKeyframeProperties(const QString& clipId) const {
    QVariantList out;
    const auto id = parseClipKey(clipId);
    const auto loc = id && m_sequence ? m_sequence->findClip(*id) : std::nullopt;
    if (!loc)
        return out;
    const Clip& clip = loc->clip();
    const auto shown = m_clipKeyframes.find(clip.id.value);
    const QString shownId = shown == m_clipKeyframes.end() ? QString() : keyframePropertyId(shown->second);
    const auto add = [&](const ShownKeyframeProperty& property, const QString& label, const QString& group) {
        const std::optional<ParamValue> value = shownKeyframeValue(clip, property);
        if (!value)
            return;
        const QString propertyId = keyframePropertyId(property);
        out.push_back(QVariantMap{{QStringLiteral("id"), propertyId},
                                  {QStringLiteral("label"), label},
                                  {QStringLiteral("group"), group},
                                  {QStringLiteral("keyed"), value->isCurve()},
                                  {QStringLiteral("shown"), propertyId == shownId}});
    };
    const bool audio = clip.stream.kind == StreamKind::Audio;
    const auto intrinsic = [&](ParamId param, const QString& group) {
        const UiParameter& ui = uiParameter(param);
        ShownKeyframeProperty property;
        property.param = static_cast<int>(param);
        add(property, QCoreApplication::translate("wabcut::app::ParameterUi", ui.label), group);
    };
    if (audio) {
        intrinsic(ParamId::VolumeDb, TimelineView::tr("Volume"));
        intrinsic(ParamId::Pan, TimelineView::tr("Panner"));
    } else {
        for (const ParamId param : {ParamId::PositionX, ParamId::PositionY, ParamId::Scale, ParamId::Rotation,
                                    ParamId::AnchorX, ParamId::AnchorY})
            intrinsic(param, TimelineView::tr("Motion"));
        intrinsic(ParamId::Opacity, TimelineView::tr("Opacity"));
    }
    for (const EffectInstance& effect : clip.effects()) {
        const EffectDescriptor* descriptor = EffectRegistry::builtin().find(effect.effect);
        if (!descriptor)
            continue; // an effect this build does not know has no parameters to show
        const QString group = effectText(descriptor->displayName.data());
        for (const EffectParamInfo& info : descriptor->params) {
            if (!info.animatable)
                continue;
            ShownKeyframeProperty property;
            property.effect = effect.id.value;
            property.key = std::string(info.key);
            add(property, effectText(info.displayName.data()), group);
        }
    }
    return out;
}

void TimelineView::showClipKeyframes(const QString& clipId, const QString& propertyId) {
    const auto id = parseClipKey(clipId);
    if (!id)
        return;
    if (propertyId.isEmpty()) {
        if (m_clipKeyframes.erase(id->value) > 0)
            bumpDisplay();
        return;
    }
    const auto property = parseKeyframePropertyId(propertyId);
    const auto loc = m_sequence ? m_sequence->findClip(*id) : std::nullopt;
    if (!property || !loc || !shownKeyframeValue(loc->clip(), *property))
        return;
    const auto it = m_clipKeyframes.find(id->value);
    if (it != m_clipKeyframes.end() && it->second == *property)
        return;
    m_clipKeyframes[id->value] = *property;
    bumpDisplay();
}

QString TimelineView::shownClipKeyframes(const QString& clipId) const {
    const auto id = parseClipKey(clipId);
    const auto it = id ? m_clipKeyframes.find(id->value) : m_clipKeyframes.end();
    return it == m_clipKeyframes.end() ? QString() : keyframePropertyId(it->second);
}

bool TimelineView::seekClipKeyframe(const QString& clipId, bool forward) {
    const auto id = parseClipKey(clipId);
    const auto it = id ? m_clipKeyframes.find(id->value) : m_clipKeyframes.end();
    const auto loc = id && m_sequence ? m_sequence->findClip(*id) : std::nullopt;
    if (it == m_clipKeyframes.end() || !loc || !m_controller)
        return false;
    const Clip& clip = loc->clip();
    const std::optional<ParamValue> value = shownKeyframeValue(clip, it->second);
    const Curve* curve = value ? value->curve() : nullptr;
    if (!curve)
        return false;
    const Ticks playhead = m_controller->playhead();
    std::optional<Ticks> best;
    for (const Keyframe& key : curve->keys) {
        const auto at = clip.timelineTimeOfCurveTime(key.time); // keys are in media time; a key the clip does not show is skipped
        if (!at || *at < clip.start() || *at >= clip.end())
            continue;
        if (forward ? *at > playhead && (!best || *at < *best) : *at < playhead && (!best || *at > *best))
            best = *at;
    }
    if (!best)
        return false;
    m_controller->setPlayhead(*best);
    return true;
}

} // namespace wabcut::app
