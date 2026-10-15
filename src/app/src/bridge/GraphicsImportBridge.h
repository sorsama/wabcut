#pragma once

// GraphicsImportBridge: Lottie import for QML (singleton `GraphicsImport`, C.13 / M5o; docs/graphics.md "Lottie").
//
//   * isLottie(url): a .json or .lottie file whose first bytes look like Bodymovin JSON (looksLikeLottie: an object with
//     the animation's v, fr, ip, op, w and h keys); Main's importUrls sends those here instead of the media importer. A
//     .lottie archive (dotLottie, a zip) is claimed too, and refused with a clear message: the importer reads JSON.
//   * importLottie(url): lottie.import of the file as a graphic clip at the playhead on the targeted video track, on a
//     worker (CommandJob: a big animation does not stall the window), one undo step "Import Lottie". Then `imported`
//     carries the report in ImportReportDialog's shape: { file, format, summary, carried, approximated, dropped,
//     entries: [{ outcome, category, reason, location }] } (what was approximated or dropped, with its JSON path), and
//     the new clip is selected. With options.saveAsTemplate the animation is also kept as a template of the project's
//     library as "lottie.<file name>" (the Callouts panel's Stickers group shows those).
//   * Several files (a drop of many): imported one after another, each placed after the one before it.
//   * busy while an import runs; message: the last error ("" after a clean import), for a toast.

#include "CommandJob.h"

#include <wabcut/model/Time.h>

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class GraphicsImportBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// The file dialog's filters for File > Import Lottie….
    Q_PROPERTY(QStringList nameFilters READ nameFilters CONSTANT FINAL)

public:
    explicit GraphicsImportBridge(QObject* parent = nullptr);
    static GraphicsImportBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    /// Runs imports on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_job.setSynchronous(synchronous); }

    bool busy() const { return m_job.running(); }
    QString message() const { return m_message; }
    QStringList nameFilters() const;

    Q_INVOKABLE bool isLottie(const QUrl& url) const;
    /// "" when the import started (the result comes with `imported` or `message`), else why it did not.
    Q_INVOKABLE QString importLottie(const QUrl& url, const QVariantMap& options = {});
    /// File > Import Lottie…: asks the window for its file dialog; the files chosen are imported with `options`
    /// (Callouts > Stickers passes { saveAsTemplate: true }: the animation is kept as a "lottie.<name>" sticker).
    Q_INVOKABLE void openImport(const QVariantMap& options = {}) { emit importRequested(options); }

    /// The report map ImportReportDialog shows, from lottie.import's result data and the file's name.
    static QVariantMap reportMap(const command::Json& data, const QString& fileName);

signals:
    void busyChanged();
    void messageChanged();
    void imported(const QVariantMap& report);
    void importRequested(const QVariantMap& options);

private:
    void setMessage(const QString& message);
    QString start(const QUrl& url, const QVariantMap& options, Ticks at);

    QPointer<ProjectController> m_controller;
    CommandJob m_job;
    QString m_message;
    QList<QPair<QUrl, QVariantMap>> m_queue; ///< files dropped together, waiting for the one importing
    Ticks m_nextAt{};                        ///< where a queued file goes: the end of the clip imported last
};

struct GraphicsImportSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::GraphicsImportBridge)
    QML_NAMED_ELEMENT(GraphicsImport)
    QML_SINGLETON
public:
    static GraphicsImportBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return GraphicsImportBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
