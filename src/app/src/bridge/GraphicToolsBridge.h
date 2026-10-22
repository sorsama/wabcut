#pragma once

// GraphicToolsBridge: the Essential Graphics panel's 0.2 tools for QML (singleton `GraphicTools`, docs/graphics.md):
// Align and Distribute, Roll / Crawl, and Text Styles. Every change is a typed command (graphic.align, graphic.setRoll,
// textStyle.*) on ProjectController's session - one undo step, the same as the CLI, MCP and the assistant get. The
// panel passes the clip and layer it shows (Graphics.clip, Graphics.selectedLayer).

#include <nlohmann/json.hpp>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <string>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class GraphicToolsBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(GraphicTools)
    QML_SINGLETON

    /// The project's text styles by name (Premiere's Text Style items), in the order they were made.
    Q_PROPERTY(QStringList textStyles READ textStyles NOTIFY changed FINAL)
    /// What the last action said ("" when it worked).
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL)

public:
    explicit GraphicToolsBridge(QObject* parent = nullptr);
    static GraphicToolsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    QStringList textStyles() const;
    QString message() const { return m_message; }

    // ---- Align and Distribute ----
    /// "left", "horizontalCenter", "right", "top", "verticalCenter", "bottom", "distributeHorizontal",
    /// "distributeVertical" for `layers` (ids) of the graphic clip `clip`, to the video frame or the selection.
    Q_INVOKABLE QString align(const QString& clip, const QVariantList& layers, const QString& how, bool toSelection);
    /// The ids of the clip's top-level layers (what Distribute spaces when no layers are chosen).
    Q_INVOKABLE QVariantList topLayers(const QString& clip) const;

    // ---- Roll / Crawl ----
    /// {enabled, kind, startOffScreen, endOffScreen, preroll, postroll, easeIn, easeOut (seconds)}.
    Q_INVOKABLE QVariantMap roll(const QString& clip) const;
    /// Sets the options (`enabled` false clears them).
    Q_INVOKABLE QString setRoll(const QString& clip, const QVariantMap& roll);

    // ---- Text Styles ----
    /// The text style the layer follows ("" = none).
    Q_INVOKABLE QString layerTextStyle(const QString& clip, int layer) const;
    Q_INVOKABLE QString createTextStyle(const QString& clip, int layer, const QString& name);
    Q_INVOKABLE QString applyTextStyle(const QString& clip, int layer, const QString& name);
    /// Push to Text Style: the layer's look becomes its style's, and every layer that follows it changes.
    Q_INVOKABLE QString pushTextStyle(const QString& clip, int layer);
    Q_INVOKABLE QString renameTextStyle(const QString& from, const QString& to);
    Q_INVOKABLE QString removeTextStyle(const QString& name);

    // ---- Text Styles as project items (0.2: Premiere's Text Style items in the Project panel) ----
    /// The project's text styles with their look for a preview: [{ name, fontFamily, fontSize, fontWeight, italic,
    /// color ("#RRGGBB"), uses (text layers that follow it) }].
    Q_INVOKABLE QVariantList textStyleItems() const;
    /// A copy of a style under a free name ("<name> copy", "<name> copy 2"...); returns its name ("" when refused).
    Q_INVOKABLE QString duplicateTextStyle(const QString& name);
    /// Applies a style to every text layer of the selected graphic clips on the timeline (Premiere: a Text Style item
    /// dropped on a graphic); one undo step. "" or why not.
    Q_INVOKABLE QString applyTextStyleToSelection(const QString& name);

signals:
    void changed();

private:
    /// Runs a command on the session (the current sequence); "" or the error, which is also `message`.
    QString run(const std::string& name, nlohmann::json params);

    QPointer<ProjectController> m_controller;
    QString m_message;
};

} // namespace wabcut::app
