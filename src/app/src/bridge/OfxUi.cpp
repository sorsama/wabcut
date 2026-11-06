#include "OfxUi.h"

#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>

#if defined(WABCUT_WITH_OPENFX)
#include <wabcut/ofxhost/OfxSystem.h>
#endif

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>

#include <algorithm>
#include <filesystem>
#include <span>
#include <vector>

namespace wabcut::app {

namespace {

[[maybe_unused]] QString text(const std::string& s) { return QString::fromStdString(s); }
[[maybe_unused]] QString pathText(const std::filesystem::path& p) { return QDir::toNativeSeparators(QString::fromStdWString(p.wstring())); }

} // namespace

OfxUi::OfxUi() = default;

OfxUi& OfxUi::instance() {
    static OfxUi ui;
    return ui;
}

#if defined(WABCUT_WITH_OPENFX)

OfxUi::Sources OfxUi::sources() const {
    if (m_catalog && m_effects) {
        return {m_catalog, m_effects};
    }
    if (auto* system = ofxhost::OfxSystem::current()) {
        return {system->catalog(), system->effects()};
    }
    return {};
}

void OfxUi::setSources(std::shared_ptr<ofxhost::OfxCatalog> catalog, std::shared_ptr<ofxhost::OfxEffects> effects) {
    if (m_effects) {
        m_effects->setChangeListener({});
    }
    m_catalog = std::move(catalog);
    m_effects = std::move(effects);
    m_listening = false;
    emit changed();
}

void OfxUi::listen() const {
    const Sources s = sources();
    if (m_listening || !s.effects) {
        return;
    }
    m_listening = true;
    // A scan finishing or a status changing on a worker thread: told to the GUI thread.
    s.effects->setChangeListener([self = const_cast<OfxUi*>(this)] { QMetaObject::invokeMethod(self, &OfxUi::changed, Qt::QueuedConnection); });
}

bool OfxUi::available() const {
    listen();
    return sources().catalog != nullptr;
}

QVariantList OfxUi::plugins() const {
    QVariantList out;
    const Sources system = sources();
    if (!available() || !system.catalog) {
        return out;
    }
    std::vector<ofxhost::OfxPluginInfo> list = system.catalog->plugins();
    std::sort(list.begin(), list.end(), [](const ofxhost::OfxPluginInfo& a, const ofxhost::OfxPluginInfo& b) {
        return QString::compare(text(a.name), text(b.name), Qt::CaseInsensitive) < 0;
    });
    for (const ofxhost::OfxPluginInfo& p : list) {
        if (!p.problem.empty()) {
            continue;
        }
        out.append(QVariantMap{{QStringLiteral("id"), text(p.effectId())},
                               {QStringLiteral("name"), text(p.name)},
                               {QStringLiteral("vendor"), text(p.group)},
                               {QStringLiteral("category"), text(p.group)},
                               {QStringLiteral("format"), QStringLiteral("OpenFX")},
                               {QStringLiteral("version"), text(p.version)},
                               {QStringLiteral("latencyText"), QString()},
                               {QStringLiteral("hasEditor"), false},
                               {QStringLiteral("problem"), QString()}});
    }
    return out;
}

QVariantList OfxUi::unusable() const {
    QVariantList out;
    const Sources system = sources();
    if (!system.catalog) {
        return out;
    }
    for (const ofxhost::OfxPluginInfo& p : system.catalog->plugins()) {
        if (!p.problem.empty()) {
            out.append(QVariantMap{{QStringLiteral("name"), text(p.name)}, {QStringLiteral("problem"), text(p.problem)}});
        }
    }
    return out;
}

QVariantList OfxUi::blocked() const {
    QVariantList out;
    const Sources system = sources();
    if (!system.catalog) {
        return out;
    }
    for (const ofxhost::OfxBlocked& b : system.catalog->blocked()) {
        out.append(QVariantMap{{QStringLiteral("file"), pathText(b.path)},
                               {QStringLiteral("name"), QString::fromStdWString(b.path.filename().wstring())},
                               {QStringLiteral("reason"), text(b.reason)}});
    }
    return out;
}

QVariantMap OfxUi::effectInfo(const EffectInstance& effect) const {
    if (!effect.effect.starts_with(ofxhost::kOfxEffectPrefix)) {
        return {{QStringLiteral("plugin"), false}};
    }
    QVariantMap out{{QStringLiteral("plugin"), true},     {QStringLiteral("openfx"), true},
                    {QStringLiteral("hasEditor"), false}, {QStringLiteral("format"), QStringLiteral("OpenFX")},
                    {QStringLiteral("kind"), QString()},  {QStringLiteral("text"), QString()},
                    {QStringLiteral("missedBlocks"), 0}};
    const Sources system = available() ? sources() : Sources{};
    if (system.catalog && system.effects) {
        if (const auto plugin = system.catalog->find(effect.effect)) {
            out.insert(QStringLiteral("name"), text(plugin->name));
            out.insert(QStringLiteral("vendor"), text(plugin->group));
        }
        const ofxhost::OfxEffectReport report = system.effects->status(effect);
        QString kind;
        switch (report.status) {
        case ofxhost::OfxEffectStatus::Missing: kind = QStringLiteral("missing"); break;
        case ofxhost::OfxEffectStatus::Crashed: kind = QStringLiteral("crashed"); break;
        case ofxhost::OfxEffectStatus::Failed: kind = QStringLiteral("failed"); break;
        case ofxhost::OfxEffectStatus::Hung: kind = QStringLiteral("hung"); break;
        default: break;
        }
        out.insert(QStringLiteral("kind"), kind);
        out.insert(QStringLiteral("text"), text(report.message));
    }
    if (out.value(QStringLiteral("name")).toString().isEmpty()) {
        if (const EffectData* saved = effect.findData(ofxhost::kOfxNameKey); saved && saved->text()) {
            out.insert(QStringLiteral("name"), text(*saved->text()));
        } else {
            out.insert(QStringLiteral("name"), text(effect.effect.substr(ofxhost::kOfxEffectPrefix.size())));
        }
    }
    return out;
}

void OfxUi::retry(const EffectInstance& effect) const {
    if (const Sources system = sources(); system.effects && effect.effect.starts_with(ofxhost::kOfxEffectPrefix)) {
        system.effects->retry(effect.id);
    }
}

QString OfxUi::rescan(bool all) {
    const Sources system = sources();
    if (!system.catalog) {
        return tr("OpenFX plug-ins are not available here");
    }
    if (system.catalog->scanning()) {
        return tr("A scan is running");
    }
    system.catalog->startScan({}, all);
    return {};
}

std::uint64_t OfxUi::revision() const {
    const Sources system = sources();
    return system.effects ? system.effects->statusRevision() : 0;
}

namespace {

QVariantMap failure(const QString& error) {
    return QVariantMap{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), error},
                       {QStringLiteral("message"), error},
                       {QStringLiteral("clips"), 0}};
}

/// Puts `plugin` on `clips` (video clips of `seq`), one instance each, as one undo step named "Add Plug-in".
QVariantMap addTo(ProjectController& controller, SequenceId sequence, const ofxhost::OfxPluginInfo& plugin, const std::vector<ClipId>& clips) {
    const std::string effect = plugin.effectId();
    // The name it was added with, for the "missing plug-in" badge on a computer that does not have it.
    const std::string savedName = plugin.group.empty() ? plugin.name : plugin.name + " (" + plugin.group + ")";
    const bool ok = controller.applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::OfxUi", "Add Plug-in"), [&](const ProjectState& state) -> edit::EditOutcome {
            auto added = edit::addEffect(state, sequence, std::span<const ClipId>(clips), effect);
            if (!added || added->createdEffects.size() != clips.size()) {
                return added;
            }
            ProjectState current = added->state;
            for (std::size_t i = 0; i < clips.size(); ++i) {
                edit::EffectChange change;
                change.data.push_back(EffectData{std::string(ofxhost::kOfxNameKey), savedName});
                auto named = edit::setEffect(current, sequence, edit::EffectTarget::ofClip(clips[i]), added->createdEffects[i], change);
                if (!named) {
                    return added;
                }
                current = std::move(named->state);
            }
            edit::EditResult result = std::move(*added);
            result.changes = diff(state, current);
            result.state = std::move(current);
            return result;
        });
    if (!ok) {
        return failure(controller.lastError());
    }
    const int n = static_cast<int>(clips.size());
    const QString name = text(plugin.name);
    const QString message = n == 1 ? OfxUi::tr("Added %1").arg(name) : OfxUi::tr("Added %1 to %n clip(s)", "", n).arg(name);
    return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("error"), QString()}, {QStringLiteral("message"), message}, {QStringLiteral("clips"), n}};
}

} // namespace

QVariantMap OfxUi::applyToSelectedClips(ProjectController* controller, const QString& effectId) const {
    const Sources system = available() ? sources() : Sources{};
    if (!system.catalog || controller == nullptr) {
        return failure(tr("OpenFX plug-ins are not available here"));
    }
    const auto plugin = system.catalog->find(effectId.toStdString());
    if (!plugin) {
        return failure(tr("This plug-in is not on this computer: rescan the plug-in folders"));
    }
    if (!plugin->problem.empty()) {
        return failure(tr("%1 cannot be used here: %2").arg(text(plugin->name), text(plugin->problem)));
    }
    const SequencePtr seq = controller->currentSequence();
    if (!seq) {
        return failure(tr("No sequence"));
    }
    // Every selected video clip, once each.
    std::vector<ClipId> clips;
    bool anySelected = false;
    for (const ClipId id : controller->selection()) {
        const auto loc = seq->findClip(id);
        if (!loc) {
            continue;
        }
        anySelected = true;
        if (loc->clip().stream.kind == StreamKind::Video && std::find(clips.begin(), clips.end(), id) == clips.end()) {
            clips.push_back(id);
        }
    }
    if (!anySelected) {
        return failure(tr("Select a clip first"));
    }
    if (clips.empty()) {
        return failure(tr("%1 is a video effect: select a video clip").arg(text(plugin->name)));
    }
    return addTo(*controller, seq->id, *plugin, clips);
}

QString OfxUi::applyToClip(ProjectController* controller, const QString& effectId, ClipId clip) const {
    const Sources system = available() ? sources() : Sources{};
    if (!system.catalog || controller == nullptr) {
        return tr("OpenFX plug-ins are not available here");
    }
    const auto plugin = system.catalog->find(effectId.toStdString());
    if (!plugin) {
        return tr("This plug-in is not on this computer: rescan the plug-in folders");
    }
    if (!plugin->problem.empty()) {
        return tr("%1 cannot be used here: %2").arg(text(plugin->name), text(plugin->problem));
    }
    const SequencePtr seq = controller->currentSequence();
    if (!seq) {
        return tr("No sequence");
    }
    const auto loc = seq->findClip(clip);
    if (!loc || loc->clip().stream.kind != StreamKind::Video) {
        return tr("%1 is a video effect: drop it on a video clip").arg(text(plugin->name));
    }
    const QVariantMap done = addTo(*controller, seq->id, *plugin, {clip});
    return done.value(QStringLiteral("error")).toString();
}

#else // no OpenFX in this build: nothing is available and every action says so

OfxUi::Sources OfxUi::sources() const { return {}; }
void OfxUi::setSources(std::shared_ptr<ofxhost::OfxCatalog>, std::shared_ptr<ofxhost::OfxEffects>) {}
void OfxUi::listen() const {}
bool OfxUi::available() const { return false; }
QVariantList OfxUi::plugins() const { return {}; }
QVariantList OfxUi::unusable() const { return {}; }
QVariantList OfxUi::blocked() const { return {}; }
QVariantMap OfxUi::effectInfo(const EffectInstance& effect) const {
    (void)effect;
    return {{QStringLiteral("plugin"), false}};
}
void OfxUi::retry(const EffectInstance& effect) const { (void)effect; }
QString OfxUi::rescan(bool all) {
    (void)all;
    return tr("OpenFX plug-ins are not available here");
}
std::uint64_t OfxUi::revision() const { return 0; }
QVariantMap OfxUi::applyToSelectedClips(ProjectController* controller, const QString& effectId) const {
    (void)controller;
    (void)effectId;
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error"), tr("OpenFX plug-ins are not available here")},
            {QStringLiteral("message"), tr("OpenFX plug-ins are not available here")},
            {QStringLiteral("clips"), 0}};
}
QString OfxUi::applyToClip(ProjectController* controller, const QString& effectId, ClipId clip) const {
    (void)controller;
    (void)effectId;
    (void)clip;
    return tr("OpenFX plug-ins are not available here");
}

#endif

} // namespace wabcut::app
