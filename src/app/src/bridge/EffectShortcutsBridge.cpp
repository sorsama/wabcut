#include "EffectShortcutsBridge.h"

#include "EffectControls.h"
#include "EffectPresetsBridge.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EffectPresets.h>
#include <wabcut/model/Effects.h>

#include <QCoreApplication>
#include <QQmlEngine>

#include <algorithm>

namespace wabcut::app {

namespace {

QString tx(const char* text) { return QCoreApplication::translate("wabcut::app::EffectControlsModel", text); }

constexpr QLatin1String kEffectPrefix("fx:");
constexpr QLatin1String kPresetPrefix("preset:");

/// An effect the shortcuts cover: a video or audio effect the Effects panel lists (not a transition, not an audio plug-in).
bool shortcutEffect(const EffectDescriptor& d) {
    return (d.kind == EffectKind::Video || d.kind == EffectKind::Audio) && isListedEffect(d) &&
           !d.id.starts_with(kPluginEffectPrefix);
}

} // namespace

EffectShortcutsBridge::EffectShortcutsBridge(QObject* parent) : QObject(parent) {}

EffectShortcutsBridge* EffectShortcutsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new EffectShortcutsBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setPresets(qmlEngine->singletonInstance<EffectPresetsBridge*>("Wabcut", "EffectPresets"));
        bridge->setDispatcher(qmlEngine->singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher"));
    }
    return bridge;
}

void EffectShortcutsBridge::setController(ProjectController* controller) { m_controller = controller; }

void EffectShortcutsBridge::setPresets(EffectPresetsBridge* presets) {
    if (m_presets == presets)
        return;
    if (m_presets)
        disconnect(m_presets, nullptr, this, nullptr);
    m_presets = presets;
    if (m_presets)
        connect(m_presets, &EffectPresetsBridge::changed, this, &EffectShortcutsBridge::refresh);
}

void EffectShortcutsBridge::setDispatcher(KeymapDispatcher* dispatcher) {
    if (m_dispatcher == dispatcher)
        return;
    if (m_dispatcher)
        disconnect(m_dispatcher, nullptr, this, nullptr);
    m_dispatcher = dispatcher;
    if (m_dispatcher) {
        // A key (or KeymapDispatcher.trigger) on one of the commands applies it.
        connect(m_dispatcher, &KeymapDispatcher::commandTriggered, this, &EffectShortcutsBridge::onCommandTriggered);
        refresh();
    }
}

QString EffectShortcutsBridge::commandFor(const QString& itemId) const {
    QString id = itemId.trimmed();
    if (id.startsWith(QLatin1String("preset:")))
        return id.size() > kPresetPrefix.size() ? id : QString();
    if (id.startsWith(QLatin1String("effect:")))
        id = id.mid(7);
    const EffectDescriptor* d = EffectRegistry::builtin().find(id.toStdString());
    return d && shortcutEffect(*d) ? QString(kEffectPrefix + id) : QString();
}

QString EffectShortcutsBridge::shortcutOf(const QString& itemId) const {
    const QString command = commandFor(itemId);
    return command.isEmpty() || !m_dispatcher ? QString() : m_dispatcher->shortcutText(command);
}

void EffectShortcutsBridge::refresh() {
    if (!m_dispatcher)
        return;
    QList<KeymapDispatcher::ExtraCommand> commands;
    const QString effectGroup = QStringLiteral("Apply Effect");
    const QString presetGroup = QStringLiteral("Apply Preset");
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (!shortcutEffect(d))
            continue;
        const QString name = tx(d.displayName.data());
        const QString id = QString::fromStdString(std::string(d.id));
        //: A command of the Keyboard Shortcuts editor: puts the effect on the selected clips, e.g. "Apply Gaussian Blur"
        QString title = tr("Apply %1").arg(name);
        if (d.kind == EffectKind::Audio)
            //: The same for an audio effect, e.g. "Apply Parametric EQ (audio)"
            title = tr("Apply %1 (audio)").arg(name);
        commands.append({QString(kEffectPrefix + id), title, effectGroup});
    }
    std::vector<std::string> names;
    for (const EffectPreset& p : builtinEffectPresets())
        names.push_back(p.name);
    if (m_presets) {
        for (const QString& name : m_presets->presetNames())
            names.push_back(name.toStdString());
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string& name : names) {
        //: A command of the Keyboard Shortcuts editor: puts the preset on the selected clips, e.g. "Apply Preset: Zoom In"
        const QString label = QString::fromStdString(name);
        commands.append({QString(kPresetPrefix + label), tr("Apply Preset: %1").arg(label), presetGroup});
    }
    const int next = static_cast<int>(commands.size());
    m_dispatcher->setExtraCommandsFor(QStringLiteral("effects"), commands);
    if (next != m_count) {
        m_count = next;
        emit commandsChanged();
    }
}

QVariantMap EffectShortcutsBridge::run(const QString& commandId) {
    EffectControlsModel* controls = m_controller ? m_controller->effectControls() : nullptr;
    QVariantMap result;
    if (!controls) {
        result = {{QStringLiteral("ok"), false}, {QStringLiteral("message"), tr("No project")}, {QStringLiteral("clips"), 0}};
    } else if (commandId.startsWith(kPresetPrefix) || commandId.startsWith(kEffectPrefix)) {
        // The Effects panel's own apply: every selected clip of the item's kind, one undo step.
        const bool preset = commandId.startsWith(kPresetPrefix);
        const QString item = preset ? commandId : commandId.mid(kEffectPrefix.size());
        if (!preset && !EffectRegistry::builtin().find(item.toStdString())) {
            result = {{QStringLiteral("ok"), false}, {QStringLiteral("message"), tr("This effect is not available")}, {QStringLiteral("clips"), 0}};
        } else {
            result = controls->applyToSelectedClips(item);
            if (result.value(QStringLiteral("message")).toString().isEmpty())
                result.insert(QStringLiteral("message"), result.value(QStringLiteral("ok")).toBool()
                                                             ? tr("Applied")
                                                             : result.value(QStringLiteral("error")).toString());
        }
    } else {
        return {};
    }
    emit applied(result.value(QStringLiteral("message")).toString(), result.value(QStringLiteral("ok")).toBool());
    return result;
}

void EffectShortcutsBridge::onCommandTriggered(const QString& commandId) {
    if (commandId.startsWith(kPresetPrefix) || commandId.startsWith(kEffectPrefix))
        run(commandId);
}

} // namespace wabcut::app
