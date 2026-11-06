#include "TrackEffectsBridge.h"

#include "BridgeIds.h"
#include "ParameterUi.h"
#include "ProjectController.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>

#include <QCoreApplication>
#include <QLocale>
#include <QtQml/qqmlengine.h>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

QString tx(const char* text) {
    return QCoreApplication::translate("wabcut::app::TrackEffectsBridge", text);
}

/// Effect and parameter names, in Effect Controls' translation context (the same words are shown there).
QString fxName(std::string_view text) {
    const std::string s(text);
    return QCoreApplication::translate("wabcut::app::EffectControlsModel", s.c_str());
}

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

const Track* videoTrack(const SequencePtr& seq, const QString& key) {
    const auto id = parseTrackKey(key);
    const Track* t = seq && id ? seq->findTrack(*id) : nullptr;
    return t != nullptr && t->kind == TrackKind::Video ? t : nullptr;
}

const EffectInstance* findInstance(const EffectStack& stack, EffectInstanceId id) {
    const auto it = std::ranges::find_if(stack, [&](const EffectInstance& e) { return e.id == id; });
    return it == stack.end() ? nullptr : &*it;
}

const EffectParam* findParam(const EffectInstance& e, std::string_view key) {
    const auto it = std::ranges::find_if(e.params, [&](const EffectParam& p) { return p.key == key; });
    return it == e.params.end() ? nullptr : &*it;
}

/// A setting's value as a summary says it ("12 px", "On", "High").
QString valueText(const EffectParamInfo& p, const UiRange& r, double model) {
    if (p.isChoice()) {
        const std::string_view label = p.optionLabel(std::round(model));
        return label.empty() ? QLocale().toString(model, 'f', 0) : fxName(label);
    }
    if (r.toggle) {
        return model >= 0.5 ? tx("On") : tx("Off");
    }
    const QString n = QLocale().toString(r.toUi(model, PropertyFrame{}), 'f', r.decimals);
    const QString unit = unitText(r);
    return unit.isEmpty() ? n : n + QLatin1Char(' ') + unit;
}

QString summaryOf(const EffectInstance& e, Ticks at) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
    if (!d) {
        return {};
    }
    constexpr std::size_t kShown = 2;
    QStringList parts;
    for (std::size_t i = 0; i < d->params.size() && i < kShown; ++i) {
        const EffectParamInfo& p = d->params[i];
        parts << QStringLiteral("%1 %2").arg(fxName(p.displayName), valueText(p, uiRange(p), effectParameterAt(e, p.key, at)));
    }
    return parts.join(QStringLiteral(" · "));
}

bool animatedAny(const EffectInstance& e) {
    return std::ranges::any_of(e.params, [](const EffectParam& p) { return p.value.isCurve(); });
}

} // namespace

TrackEffectsBridge::TrackEffectsBridge(QObject* parent) : QObject(parent) {}

TrackEffectsBridge* TrackEffectsBridge::create(QQmlEngine*, QJSEngine*) { return new TrackEffectsBridge; }

ProjectController* TrackEffectsBridge::controller() const {
    if (m_controller) {
        return m_controller;
    }
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController") : nullptr;
}

int TrackEffectsBridge::count(const QString& track) const {
    ProjectController* c = controller();
    const Track* t = c ? videoTrack(c->currentSequence(), track) : nullptr;
    return t ? static_cast<int>(t->effects.size()) : 0;
}

QVariantList TrackEffectsBridge::effectsOf(const QString& track) const {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    QVariantList out;
    if (!t) {
        return out;
    }
    const Ticks at = c->playhead();
    for (const EffectInstance& e : t->effects) {
        const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
        out.append(QVariantMap{
            {QStringLiteral("key"), effectKey(e.id)},
            {QStringLiteral("effect"), QString::fromStdString(e.effect)},
            {QStringLiteral("name"), d ? fxName(d->displayName) : QString::fromStdString(e.effect)},
            {QStringLiteral("enabled"), e.enabled},
            {QStringLiteral("known"), d != nullptr && isEvaluable(e, EffectKind::Video)},
            {QStringLiteral("animated"), animatedAny(e)},
            {QStringLiteral("summary"), summaryOf(e, at)},
        });
    }
    return out;
}

QVariantList TrackEffectsBridge::available(const QString& filter) const {
    const QString needle = filter.trimmed();
    QVariantList out;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (d.kind != EffectKind::Video || d.intrinsic) {
            continue;
        }
        const QString name = fxName(d.displayName);
        const QString category = fromView(d.category);
        if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive) && !category.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        out.append(QVariantMap{{QStringLiteral("id"), fromView(d.id)},
                               {QStringLiteral("name"), name},
                               {QStringLiteral("category"), category},
                               {QStringLiteral("description"), fromView(d.description)}});
    }
    std::ranges::sort(out, [](const QVariant& a, const QVariant& b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();
        const int byCategory = QString::localeAwareCompare(x.value(QStringLiteral("category")).toString(), y.value(QStringLiteral("category")).toString());
        return byCategory != 0 ? byCategory < 0
                               : QString::localeAwareCompare(x.value(QStringLiteral("name")).toString(), y.value(QStringLiteral("name")).toString()) < 0;
    });
    return out;
}

QString TrackEffectsBridge::addEffect(const QString& track, const QString& effectId) {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    const EffectDescriptor* d = EffectRegistry::builtin().find(effectId.toStdString());
    if (!t) {
        return tx("Choose a video track");
    }
    if (!d || d->kind != EffectKind::Video || d->intrinsic) {
        return tx("No such effect");
    }
    const SequenceId sequence = c->currentSequenceId();
    const edit::EffectTarget site = edit::EffectTarget::ofTrack(t->id);
    const std::string id(d->id);
    const std::string label = "Add " + std::string(d->displayName) + " to Track";
    const bool ok = c->applyEditFn(label.c_str(), [&](const ProjectState& state) {
        return edit::addEffect(state, sequence, site, id);
    });
    return ok ? QString() : c->lastError();
}

QString TrackEffectsBridge::removeEffect(const QString& track, const QString& effect) {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    const auto id = parseEffectKey(effect);
    const EffectInstance* e = t && id ? findInstance(t->effects, *id) : nullptr;
    if (!e) {
        return tx("No such effect");
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(e->effect);
    const SequenceId sequence = c->currentSequenceId();
    const edit::EffectTarget site = edit::EffectTarget::ofTrack(t->id);
    const std::string label = "Remove " + std::string(d ? d->displayName : std::string_view(e->effect)) + " from Track";
    const bool ok = c->applyEditFn(label.c_str(), [&](const ProjectState& state) {
        return edit::removeEffect(state, sequence, site, *id);
    });
    return ok ? QString() : c->lastError();
}

QString TrackEffectsBridge::setEffectEnabled(const QString& track, const QString& effect, bool enabled) {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    const auto id = parseEffectKey(effect);
    const EffectInstance* e = t && id ? findInstance(t->effects, *id) : nullptr;
    if (!e) {
        return tx("No such effect");
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(e->effect);
    edit::EffectChange change;
    change.enabled = enabled;
    const SequenceId sequence = c->currentSequenceId();
    const edit::EffectTarget site = edit::EffectTarget::ofTrack(t->id);
    const std::string label = (enabled ? "Turn On " : "Bypass ") + std::string(d ? d->displayName : std::string_view(e->effect));
    const bool ok = c->applyEditFn(label.c_str(), [&](const ProjectState& state) {
        return edit::setEffect(state, sequence, site, *id, change);
    });
    return ok ? QString() : c->lastError();
}

QString TrackEffectsBridge::moveEffect(const QString& track, const QString& effect, int index) {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    const auto id = parseEffectKey(effect);
    const EffectInstance* e = t && id ? findInstance(t->effects, *id) : nullptr;
    if (!e) {
        return tx("No such effect");
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(e->effect);
    const SequenceId sequence = c->currentSequenceId();
    const edit::EffectTarget site = edit::EffectTarget::ofTrack(t->id);
    const auto to = static_cast<std::size_t>(std::max(0, index));
    const std::string label = "Reorder " + std::string(d ? d->displayName : std::string_view(e->effect));
    const bool ok = c->applyEditFn(label.c_str(), [&](const ProjectState& state) {
        return edit::moveEffect(state, sequence, site, *id, to);
    });
    return ok ? QString() : c->lastError();
}

QVariantMap TrackEffectsBridge::effectSettings(const QString& track, const QString& effect) const {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    const auto id = parseEffectKey(effect);
    const EffectInstance* e = t && id ? findInstance(t->effects, *id) : nullptr;
    if (!e) {
        return {};
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(e->effect);
    const Ticks at = c->playhead();
    const PropertyFrame frame{static_cast<double>(seq->width), static_cast<double>(seq->height), static_cast<double>(seq->width),
                              static_cast<double>(seq->height)};
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
                for (std::size_t i = 0; i < labels.size(); ++i) {
                    options.append(QVariantMap{{QStringLiteral("id"), QString::number(r.toUi(p.minValue + static_cast<double>(i), frame))},
                                               {QStringLiteral("name"), fxName(labels[i])}});
                }
                row.insert(QStringLiteral("options"), options);
                row.insert(QStringLiteral("current"), QString::number(r.toUi(std::round(model), frame)));
            }
            params.append(row);
        }
    }
    return {
        {QStringLiteral("key"), effect},
        {QStringLiteral("track"), track},
        {QStringLiteral("effect"), QString::fromStdString(e->effect)},
        {QStringLiteral("name"), d ? fxName(d->displayName) : QString::fromStdString(e->effect)},
        {QStringLiteral("description"), d ? fromView(d->description) : QString()},
        {QStringLiteral("enabled"), e->enabled},
        {QStringLiteral("known"), d != nullptr && isEvaluable(*e, EffectKind::Video)},
        {QStringLiteral("params"), params},
    };
}

QString TrackEffectsBridge::setEffectParam(const QString& track, const QString& effect, const QString& param, double value,
                                           bool gesture) {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    const auto id = parseEffectKey(effect);
    const EffectInstance* e = t && id ? findInstance(t->effects, *id) : nullptr;
    if (!e) {
        return tx("No such effect");
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(e->effect);
    const std::string key = param.toStdString();
    const EffectParamInfo* p = d ? d->param(key) : nullptr;
    if (!p) {
        return tx("No such setting");
    }
    if (!std::isfinite(value)) {
        return tx("Not a number");
    }
    const PropertyFrame frame{static_cast<double>(seq->width), static_cast<double>(seq->height), static_cast<double>(seq->width),
                              static_cast<double>(seq->height)};
    const UiRange r = uiRange(*p);
    double model = std::clamp(r.toModel(value, frame), p->minValue, p->maxValue);
    if (p->isChoice() || r.toggle) {
        model = std::round(model);
    }
    const SequenceId sequence = c->currentSequenceId();
    const Ticks at = c->playhead();
    const edit::EffectTarget site = edit::EffectTarget::ofTrack(t->id);
    const edit::ParamRef ref = edit::ParamRef::ofEffect(*id, key);
    // "Gaussian Blur: Blurriness", as Effect Controls names a clip effect's parameter step.
    const std::string label = std::string(d->displayName) + ": " + std::string(p->displayName);
    const std::string merge = gesture ? "trackEffect:" + track.toStdString() + ":" + effect.toStdString() + ":" + key : std::string();
    const bool ok = c->applyEditFn(
        label.c_str(),
        [&](const ProjectState& state) -> edit::EditOutcome {
            const Sequence* s = state.findSequence(sequence);
            const EffectStack* st = s ? edit::effectStackOf(*s, site) : nullptr;
            const EffectInstance* now = st ? findInstance(*st, *id) : nullptr;
            if (!now) {
                return std::unexpected(Error(ErrorCode::NotFound, "the effect is gone"));
            }
            ParamValue next(model);
            const EffectParam* current = findParam(*now, key);
            if (const Curve* curve = current ? current->value.curve() : nullptr) {
                // Animated: the key at the playhead (a track's curves run in timeline time).
                Curve edited = *curve;
                if (const auto index = edited.keyAt(at)) {
                    edited.keys[*index].value = model;
                } else {
                    edited.keys.insert(edited.keys.begin() + static_cast<std::ptrdiff_t>(edited.insertionIndex(at)), Keyframe{at, model});
                }
                next = ParamValue(std::move(edited));
            }
            return edit::setParameter(state, sequence, site, ref, std::move(next));
        },
        merge);
    return ok ? QString() : c->lastError();
}

void TrackEffectsBridge::endEffectGesture() {
    if (ProjectController* c = controller()) {
        c->endPropertyGesture();
    }
}

QString TrackEffectsBridge::resetEffect(const QString& track, const QString& effect) {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    const auto id = parseEffectKey(effect);
    const EffectInstance* e = t && id ? findInstance(t->effects, *id) : nullptr;
    const EffectDescriptor* d = e ? EffectRegistry::builtin().find(e->effect) : nullptr;
    if (!d) {
        return tx("No such effect");
    }
    endEffectGesture();
    edit::EffectChange change;
    for (const EffectParamInfo& p : d->params) {
        change.params.push_back(EffectParam{std::string(p.key), ParamValue(p.defaultValue)});
    }
    const SequenceId sequence = c->currentSequenceId();
    const edit::EffectTarget site = edit::EffectTarget::ofTrack(t->id);
    const std::string label = "Reset " + std::string(d->displayName);
    const bool ok = c->applyEditFn(label.c_str(), [&](const ProjectState& state) {
        return edit::setEffect(state, sequence, site, *id, change);
    });
    return ok ? QString() : c->lastError();
}

QString TrackEffectsBridge::setAnimated(const QString& track, const QString& effect, const QString& param, bool animated) {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const Track* t = videoTrack(seq, track);
    const auto id = parseEffectKey(effect);
    const EffectInstance* e = t && id ? findInstance(t->effects, *id) : nullptr;
    const EffectDescriptor* d = e ? EffectRegistry::builtin().find(e->effect) : nullptr;
    const std::string key = param.toStdString();
    const EffectParamInfo* p = d ? d->param(key) : nullptr;
    if (!p) {
        return tx("No such setting");
    }
    if (!p->animatable) {
        return tx("This setting cannot be animated");
    }
    endEffectGesture();
    const SequenceId sequence = c->currentSequenceId();
    const Ticks at = c->playhead();
    const edit::EffectTarget site = edit::EffectTarget::ofTrack(t->id);
    const edit::ParamRef ref = edit::ParamRef::ofEffect(*id, key);
    const std::string label = std::string(animated ? "Animate " : "Stop Animating ") + std::string(d->displayName) + ": " +
                              std::string(p->displayName);
    const bool ok = c->applyEditFn(label.c_str(), [&](const ProjectState& state) {
        return edit::setAnimated(state, sequence, site, ref, animated, at);
    });
    return ok ? QString() : c->lastError();
}

} // namespace wabcut::app
