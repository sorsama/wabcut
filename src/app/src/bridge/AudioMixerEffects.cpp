// r07 (stream D): the Audio Track Mixer's effect settings (AudioMixerBridge.h).
//
//   * effectSettings(): every parameter of a track, submix or master effect in UI units (ParameterUi.h, as Effect
//     Controls shows clip effects): a slider with its unit, a switch or a menu, at the playhead.
//   * setEffectParam(): one parameter, a constant or (animated) the key at the playhead; a slider drag merges into one
//     undo step until endEffectGesture(). resetEffect(): every parameter back to its default, one undo step.
//   * The slots' hover details ("Threshold -20.0 dB · Ratio 4.00 · ..."): mixerEffectDetails().
//   * Clip effects picked in a strip's "+ Effect" menu (Enhance Speech is rendered per clip, so it lives on the track's
//     clips, never on the track): the strip lists them as a slot of their own (mixerClipEffectsOf()) that can be
//     bypassed or removed on every clip of the track in one undo step.

#include "AudioMixerBridge.h"

#include "BridgeIds.h"
#include "EffectControls.h"
#include "ParameterUi.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>

#include <QCoreApplication>
#include <QLocale>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

QString tx(const char* text) {
    return QCoreApplication::translate("wabcut::app::AudioMixerBridge", text);
}

QString tx(const char* text, const char* disambiguation, int n) {
    return QCoreApplication::translate("wabcut::app::AudioMixerBridge", text, disambiguation, n);
}

/// Effect and parameter names, in Effect Controls' translation context (the same words are shown there).
QString fxName(std::string_view text) {
    const std::string s(text);
    return QCoreApplication::translate("wabcut::app::EffectControlsModel", s.c_str());
}

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// The clip effects a strip's "+ Effect" menu puts on the track's clips (AudioMixerBridge::addEffect).
constexpr std::array kTrackClipEffects{std::string_view{"wabcut.audio.enhanceSpeech"}};

edit::EffectTarget siteOf(const QString& target) {
    if (target == QLatin1String("master"))
        return edit::EffectTarget::masterAudio();
    return edit::EffectTarget::ofTrack(parseTrackKey(target).value_or(TrackId{}));
}

bool validTarget(const QString& target) {
    return target == QLatin1String("master") || parseTrackKey(target).has_value();
}

const EffectInstance* findInstance(const EffectStack& stack, EffectInstanceId id) {
    const auto it = std::ranges::find_if(stack, [&](const EffectInstance& e) { return e.id == id; });
    return it == stack.end() ? nullptr : &*it;
}

const EffectParam* findParam(const EffectInstance& e, std::string_view key) {
    const auto it = std::ranges::find_if(e.params, [&](const EffectParam& p) { return p.key == key; });
    return it == e.params.end() ? nullptr : &*it;
}

/// A parameter's value as the slot's details and the popup's tooltips say it ("-20.0 dB", "On", "High").
QString valueText(const EffectParamInfo& p, const UiRange& r, double model) {
    if (p.isChoice()) {
        const std::string_view label = p.optionLabel(std::round(model));
        return label.empty() ? QLocale().toString(model, 'f', 0) : fxName(label);
    }
    if (r.toggle)
        return model >= 0.5 ? tx("On") : tx("Off");
    const QString n = QLocale().toString(r.toUi(model, PropertyFrame{}), 'f', r.decimals);
    const QString unit = unitText(r);
    return unit.isEmpty() ? n : n + QLatin1Char(' ') + unit;
}

/// Several edits as one: each runs on what the one before made; the changes are the whole difference.
edit::EditOutcome chained(const ProjectState& state, const std::vector<std::function<edit::EditOutcome(const ProjectState&)>>& steps) {
    ProjectState now = state;
    for (const auto& step : steps) {
        auto done = step(now);
        if (!done)
            return done;
        now = std::move(done->state);
    }
    edit::EditResult result;
    result.changes = diff(state, now);
    result.state = std::move(now);
    return result;
}

} // namespace

/// The main values of an effect for a slot's hover tooltip: its first four parameters at `at` (AudioMixerBridge.cpp).
QString mixerEffectDetails(const EffectInstance& e, Ticks at) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
    if (!d)
        return {};
    constexpr std::size_t kShown = 4;
    QStringList parts;
    for (std::size_t i = 0; i < d->params.size() && i < kShown; ++i) {
        const EffectParamInfo& p = d->params[i];
        parts << QStringLiteral("%1 %2").arg(fxName(p.displayName), valueText(p, uiRange(p), effectParameterAt(e, p.key, at)));
    }
    if (d->params.size() > kShown)
        parts << tx("%n more", "", static_cast<int>(d->params.size() - kShown));
    return parts.join(QStringLiteral(" · "));
}

/// A track's clip effects for its strip: [{ effect, name, enabled (on for at least one clip), count, total (clips),
/// clips ("c…" keys that have it), summary ("2 of 3 clips"), details }] (AudioMixerBridge.cpp).
QVariantList mixerClipEffectsOf(const Track& track) {
    QVariantList out;
    for (const std::string_view id : kTrackClipEffects) {
        QStringList clips;
        int enabled = 0;
        double strength = 0.0;
        for (const Clip& c : track.clips) {
            const auto it = std::ranges::find_if(c.effects(), [&](const EffectInstance& e) { return e.effect == id; });
            if (it == c.effects().end())
                continue;
            clips << clipKey(c.id);
            if (it->enabled)
                ++enabled;
            strength = std::max(strength, effectParameterAt(*it, "strength", Ticks{}, 1.0));
        }
        if (clips.isEmpty())
            continue;
        const EffectDescriptor* d = EffectRegistry::builtin().find(id);
        const int total = static_cast<int>(track.clips.size());
        out.append(QVariantMap{
            {QStringLiteral("effect"), fromView(id)},
            {QStringLiteral("name"), d ? fxName(d->displayName) : fromView(id)},
            {QStringLiteral("enabled"), enabled > 0},
            {QStringLiteral("count"), static_cast<int>(clips.size())},
            {QStringLiteral("total"), total},
            {QStringLiteral("clips"), clips},
            {QStringLiteral("summary"), tx("%1 of %n clip(s)", "", total).arg(clips.size())},
            {QStringLiteral("details"), tx("On the track's clips (rendered per clip) · Strength up to %1 %")
                                            .arg(std::lround(strength * 100.0))},
        });
    }
    return out;
}

QVariantMap AudioMixerBridge::effectSettings(const QString& target, const QString& effect) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto id = parseEffectKey(effect);
    if (!seq || !id || !validTarget(target))
        return {};
    const EffectStack* stack = edit::effectStackOf(*seq, siteOf(target));
    const EffectInstance* e = stack ? findInstance(*stack, *id) : nullptr;
    if (!e)
        return {};
    const EffectDescriptor* d = EffectRegistry::builtin().find(e->effect);
    const Ticks at = m_controller->playhead();
    const PropertyFrame frame;
    QVariantList params;
    if (d) {
        for (const EffectParamInfo& p : d->params) {
            const UiRange r = uiRange(p);
            const EffectParam* set = findParam(*e, p.key);
            const double model = effectParameterAt(*e, p.key, at);
            QVariantMap row{
                {QStringLiteral("key"), fromView(p.key)},
                {QStringLiteral("name"), fxName(p.displayName)},
                {QStringLiteral("kind"), p.isChoice() ? QStringLiteral("choice") : r.toggle ? QStringLiteral("toggle") : QStringLiteral("slider")},
                {QStringLiteral("value"), r.toUi(model, frame)},
                {QStringLiteral("defaultValue"), r.toUi(p.defaultValue, frame)},
                {QStringLiteral("from"), r.from},
                {QStringLiteral("to"), r.to},
                {QStringLiteral("step"), r.step},
                {QStringLiteral("decimals"), r.decimals},
                {QStringLiteral("origin"), r.bipolar ? 0.0 : r.from},
                {QStringLiteral("unit"), unitText(r)},
                {QStringLiteral("animated"), set && set->value.isCurve()},
                {QStringLiteral("animatable"), p.animatable},
                {QStringLiteral("valueText"), valueText(p, r, model)},
            };
            if (p.isChoice()) {
                QVariantList options;
                const std::vector<std::string_view> labels = p.optionLabels();
                for (std::size_t i = 0; i < labels.size(); ++i)
                    options.append(QVariantMap{{QStringLiteral("id"), QString::number(r.toUi(p.minValue + static_cast<double>(i), frame))},
                                               {QStringLiteral("name"), fxName(labels[i])}});
                row.insert(QStringLiteral("options"), options);
                row.insert(QStringLiteral("current"), QString::number(r.toUi(std::round(model), frame)));
            }
            params.append(row);
        }
    }
    const bool plugin = e->effect.starts_with(kPluginEffectPrefix);
    return {
        {QStringLiteral("key"), effect},
        {QStringLiteral("target"), target},
        {QStringLiteral("effect"), QString::fromStdString(e->effect)},
        {QStringLiteral("name"), d ? fxName(d->displayName) : QString::fromStdString(e->effect)},
        {QStringLiteral("description"), d ? fromView(d->description) : QString()},
        {QStringLiteral("enabled"), e->enabled},
        {QStringLiteral("known"), d != nullptr && isEvaluable(*e, EffectKind::Audio)},
        {QStringLiteral("plugin"), plugin},
        {QStringLiteral("details"), mixerEffectDetails(*e, at)},
        {QStringLiteral("params"), params},
    };
}

QVariantMap AudioMixerBridge::dynamicsMeters(const QString& effect) {
    const auto id = parseEffectKey(effect);
    const std::uint64_t instance = id ? id->value : 0;
    return dynamicsMeterReading(instance, m_dynamicsBlocks[instance]);
}

QVariantList AudioMixerBridge::dynamicsCurve(int mode, double threshold, double ratio, double range, double knee, double makeup,
                                             double fromDb, double toDb, int points) const {
    return dynamicsCurvePoints(mode, threshold, ratio, range, knee, makeup, fromDb, toDb, points);
}

QString AudioMixerBridge::setEffectParam(const QString& target, const QString& effect, const QString& param, double value,
                                         bool gesture) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto id = parseEffectKey(effect);
    if (!seq || !id || !validTarget(target))
        return tx("No such effect");
    const edit::EffectTarget site = siteOf(target);
    const EffectStack* stack = edit::effectStackOf(*seq, site);
    const EffectInstance* e = stack ? findInstance(*stack, *id) : nullptr;
    const EffectDescriptor* d = e ? EffectRegistry::builtin().find(e->effect) : nullptr;
    const std::string key = param.toStdString();
    const EffectParamInfo* p = d ? d->param(key) : nullptr;
    if (!p)
        return tx("No such setting");
    if (!std::isfinite(value))
        return tx("Not a number");
    const UiRange r = uiRange(*p);
    double model = std::clamp(r.toModel(value, PropertyFrame{}), p->minValue, p->maxValue);
    if (p->isChoice() || r.toggle)
        model = std::round(model);
    const SequenceId sequence = m_controller->currentSequenceId();
    const Ticks at = m_controller->playhead();
    const edit::ParamRef ref = edit::ParamRef::ofEffect(*id, key);
    // "Compressor: Threshold", as Effect Controls names a clip effect's parameter step.
    const std::string label = std::string(d->displayName) + ": " + std::string(p->displayName);
    const std::string merge = gesture ? "mixerEffect:" + target.toStdString() + ":" + effect.toStdString() + ":" + key : std::string();
    const bool ok = m_controller->applyEditFn(
        label.c_str(),
        [&](const ProjectState& state) -> edit::EditOutcome {
            const Sequence* s = state.findSequence(sequence);
            const EffectStack* st = s ? edit::effectStackOf(*s, site) : nullptr;
            const EffectInstance* now = st ? findInstance(*st, *id) : nullptr;
            if (!now)
                return std::unexpected(Error(ErrorCode::NotFound, "the effect is gone"));
            ParamValue next(model);
            const EffectParam* current = findParam(*now, key);
            if (const Curve* curve = current ? current->value.curve() : nullptr) {
                // Animated: the key at the playhead (track and master curves run in timeline time).
                Curve c = *curve;
                if (const auto index = c.keyAt(at))
                    c.keys[*index].value = model;
                else
                    c.keys.insert(c.keys.begin() + static_cast<std::ptrdiff_t>(c.insertionIndex(at)), Keyframe{at, model});
                next = ParamValue(std::move(c));
            }
            return edit::setParameter(state, sequence, site, ref, std::move(next));
        },
        merge);
    return ok ? QString() : m_controller->lastError();
}

void AudioMixerBridge::endEffectGesture() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

QString AudioMixerBridge::resetEffect(const QString& target, const QString& effect) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto id = parseEffectKey(effect);
    if (!seq || !id || !validTarget(target))
        return tx("No such effect");
    const edit::EffectTarget site = siteOf(target);
    const EffectStack* stack = edit::effectStackOf(*seq, site);
    const EffectInstance* e = stack ? findInstance(*stack, *id) : nullptr;
    const EffectDescriptor* d = e ? EffectRegistry::builtin().find(e->effect) : nullptr;
    if (!d)
        return tx("No such effect");
    endEffectGesture();
    edit::EffectChange change;
    for (const EffectParamInfo& p : d->params)
        change.params.push_back(EffectParam{std::string(p.key), ParamValue(p.defaultValue)});
    const SequenceId sequence = m_controller->currentSequenceId();
    const std::string label = "Reset " + std::string(d->displayName);
    const bool ok = m_controller->applyEditFn(label.c_str(), [&](const ProjectState& state) {
        return edit::setEffect(state, sequence, site, *id, change);
    });
    return ok ? QString() : m_controller->lastError();
}

namespace {

/// The edits that change clip effect `effectId` on every clip of `track` that has it.
std::vector<std::function<edit::EditOutcome(const ProjectState&)>>
clipEffectSteps(const Track& track, std::string_view effectId, SequenceId sequence,
                const std::function<edit::EditOutcome(const ProjectState&, SequenceId, const edit::EffectTarget&, EffectInstanceId)>& change) {
    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
    for (const Clip& c : track.clips) {
        for (const EffectInstance& e : c.effects()) {
            if (e.effect != effectId)
                continue;
            const edit::EffectTarget site = edit::EffectTarget::ofClip(c.id);
            const EffectInstanceId instance = e.id;
            steps.emplace_back([=](const ProjectState& state) { return change(state, sequence, site, instance); });
        }
    }
    return steps;
}

} // namespace

QString AudioMixerBridge::setClipEffectEnabled(const QString& track, const QString& effectId, bool enabled) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto id = parseTrackKey(track);
    const Track* t = seq && id ? seq->findTrack(*id) : nullptr;
    if (!t || t->kind != TrackKind::Audio)
        return tx("Choose an audio track");
    const std::string effect = effectId.toStdString();
    edit::EffectChange change;
    change.enabled = enabled;
    const auto steps = clipEffectSteps(*t, effect, m_controller->currentSequenceId(),
                                       [&](const ProjectState& state, SequenceId s, const edit::EffectTarget& site, EffectInstanceId e) {
                                           return edit::setEffect(state, s, site, e, change);
                                       });
    if (steps.empty())
        return tx("No clip of the track has this effect");
    const EffectDescriptor* d = EffectRegistry::builtin().find(effect);
    const std::string label = (enabled ? "Turn On " : "Bypass ") + std::string(d ? d->displayName : std::string_view(effect));
    const bool ok = m_controller->applyEditFn(label.c_str(), [&](const ProjectState& state) { return chained(state, steps); });
    return ok ? QString() : m_controller->lastError();
}

QString AudioMixerBridge::removeClipEffect(const QString& track, const QString& effectId) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto id = parseTrackKey(track);
    const Track* t = seq && id ? seq->findTrack(*id) : nullptr;
    if (!t || t->kind != TrackKind::Audio)
        return tx("Choose an audio track");
    const std::string effect = effectId.toStdString();
    const auto steps = clipEffectSteps(*t, effect, m_controller->currentSequenceId(),
                                       [](const ProjectState& state, SequenceId s, const edit::EffectTarget& site, EffectInstanceId e) {
                                           return edit::removeEffect(state, s, site, e);
                                       });
    if (steps.empty())
        return tx("No clip of the track has this effect");
    const EffectDescriptor* d = EffectRegistry::builtin().find(effect);
    const std::string label = "Remove " + std::string(d ? d->displayName : std::string_view(effect));
    const bool ok = m_controller->applyEditFn(label.c_str(), [&](const ProjectState& state) { return chained(state, steps); });
    return ok ? QString() : m_controller->lastError();
}

} // namespace wabcut::app
