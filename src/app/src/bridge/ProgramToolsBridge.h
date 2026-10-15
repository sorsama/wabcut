#pragma once

// ProgramToolsBridge: the Program Monitor's editing buttons, for QML (singleton `ProgramTools`, M5h).
//
//   lift() / extract()   Premiere ; and ': the sequence In/Out range off the targeted tracks (sequence.lift /
//                        sequence.extract), one undo step each
//   addMarker()          M: a marker at the playhead (marker.add)
//   exportFrame(folder)  the camera button: the program frame at the playhead, rendered offscreen at the sequence's
//                        size (the export renderer: exporter::renderedVideoSourceFactory), saved as a PNG named
//                        "<sequence> <timecode>.png" in `folder` (QML passes the project's folder or a chosen one)
//
// Every action goes through the typed command catalog on ProjectController's session.

#include <wabcut/command/Engine.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <expected>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class ProgramToolsBridge : public QObject {
    Q_OBJECT

    /// Lift and Extract need a sequence In and Out.
    Q_PROPERTY(bool canLift READ canLift NOTIFY stateChanged FINAL)
    /// The last result: the frame written, or what went wrong.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit ProgramToolsBridge(QObject* parent = nullptr);
    static ProgramToolsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    /// The program frame at `at` in `sequence`, at the sequence's size, from the offscreen export renderer.
    std::expected<QImage, QString> renderFrame(SequenceId sequence, Ticks at) const;
    /// The same from any project state (thread-safe: the Trim view renders on a worker thread), `width` px wide
    /// (0 = the sequence's width).
    static std::expected<QImage, QString> renderStateFrame(const ProjectState& state, SequenceId sequence, Ticks at,
                                                           int width = 0);

    bool canLift() const;
    QString message() const { return m_message; }

    Q_INVOKABLE bool lift();
    Q_INVOKABLE bool extract();
    Q_INVOKABLE bool addMarker();
    /// Writes the frame at the playhead into `folder`; returns the file's path, or "" (see `message`).
    Q_INVOKABLE QString exportFrame(const QUrl& folder);

signals:
    void stateChanged();
    void messageChanged();
    /// A frame was written (the path, for a toast).
    void frameExported(const QString& path);

private:
    command::CommandResult run(const std::string& name, command::Json params);
    void setMessage(const QString& message);

    QPointer<ProjectController> m_controller;
    QString m_message;
};

/// QML: the singleton `ProgramTools`.
struct ProgramToolsBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::ProgramToolsBridge)
    QML_NAMED_ELEMENT(ProgramTools)
    QML_SINGLETON
public:
    static ProgramToolsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return ProgramToolsBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
