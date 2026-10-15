#pragma once

// CommandBridge: the typed edit commands (src/command catalog, wabcut::command) for the command palette.
//
// The same catalog drives wabcut-cli, scripting and AI agents; here every mutating command becomes a palette action.
// Its JSON-schema parameters are turned into simple prompts (timecode, text, number, choice, track, colour); the
// clip(s) and sequence come from the current selection and sequence, so QML never handles ids or ticks
// (principle 9). preview() is the engine's dry run (what would change, nothing committed); apply() runs the same
// request through CommandEngine on ProjectController's EditSession: one undo step labelled with the command's title.
//
// Catalog commands that the keymap already offers (Link, Ripple Delete, Add Edit, Mark In, ...) are left out
// (`covered`), so the palette lists each action once.

#include <wabcut/command/Command.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <expected>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class CommandBridge : public QObject {
    Q_OBJECT

    /// The catalog's edit commands: [{ name, title, description, group, enabled, reason,
    ///   params: [{ key, label, kind: "timecode"|"text"|"integer"|"number"|"boolean"|"choice"|"track"|"color",
    ///              required, options, minimum, maximum, description }] }].
    Q_PROPERTY(QVariantList commands READ commands NOTIFY commandsChanged FINAL)

public:
    explicit CommandBridge(QObject* parent = nullptr);
    static CommandBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    ProjectController* controller() const { return m_controller; }

    QVariantList commands() const;
    /// Prompt values to start from: the playhead for times, the clip's current name and colour, "V1", ...
    Q_INVOKABLE QVariantMap defaults(const QString& name) const;
    /// Dry run: { ok, summary, error, detail } (nothing changes).
    Q_INVOKABLE QVariantMap preview(const QString& name, const QVariantMap& params);
    /// Runs the command: { ok, summary, error, detail }; one undo step.
    Q_INVOKABLE QVariantMap apply(const QString& name, const QVariantMap& params);

    /// The request for `name` with prompt values `params` (ids and the sequence filled in from the selection).
    std::expected<command::CommandRequest, QString> request(const QString& name, const QVariantMap& params,
                                                            bool dryRun) const;
    /// Catalog commands the keymap already runs (the palette lists the keymap command instead).
    static bool covered(const QString& name);
    /// Commands this bridge lists: mutating catalog commands that are not covered. A keymap command with the same id
    /// that the app cannot run yet (Speed/Duration, Apply Default Transitions) is listed here instead.
    static bool offers(const QString& name);

signals:
    void commandsChanged();

private:
    QVariantMap run(const QString& name, const QVariantMap& params, bool dryRun);

    QPointer<ProjectController> m_controller;
};

/// QML registration through the factory. Qt prefers a default constructor over create() when both exist, and the
/// class stays default-constructible for C++ and tests, so the singleton is registered by this foreign wrapper.
struct CommandBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::CommandBridge)
    QML_NAMED_ELEMENT(CommandBridge)
    QML_SINGLETON
public:
    static CommandBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return CommandBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
