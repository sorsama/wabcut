#pragma once

// ReviewBridge (M5m, QML singleton `Review`): frame-accurate review notes (C.9, ReviewNotes.h) in the Review panel and
// on the Program Monitor.
//
//   * The notes of the current sequence (or of every sequence), filtered by status and text, in time order; the note
//     at the playhead (`currentNote`) and the mark to draw on the monitor (`shownMark`: the selected note's, else the
//     one at the playhead).
//   * Add a note at the playhead or over In-Out (review.add), with a point or rectangle drawn on the frame
//     (`drawing`, `pendingMark`, set by ReviewMarkOverlay), by `author` (Preferences.reviewAuthor; "Editor" when
//     empty). Reply, resolve or reopen, delete: one undo step each (review.*).
//   * Notes to markers (review.toMarkers), review packages: export (review.exportPackage, thumbnails rendered by
//     ReviewFrames on a worker) and import (review.importPackage: added, updated, unchanged and conflicts).
//
// Principle 9: QML sees note ids (numbers), keys ("s3"), timecodes and fractions of the frame.

#include "CommandJob.h"

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;

class ReviewBridge : public QObject {
    Q_OBJECT
    /// The notes shown, in time order: [{ id, sequenceKey, sequenceName, startText, endText ("" for a frame), range,
    /// author, text, createdText, status ("open" | "resolved"), resolved, hasMark, mark: { x, y, width, height },
    /// replies: [{ author, text, createdText }], replyCount }].
    Q_PROPERTY(QVariantList notes READ notes NOTIFY notesChanged FINAL)
    /// "all" | "open" | "resolved".
    Q_PROPERTY(QString statusFilter READ statusFilter WRITE setStatusFilter NOTIFY filterChanged FINAL)
    /// Every sequence's notes, not only the current sequence's.
    Q_PROPERTY(bool allSequences READ allSequences WRITE setAllSequences NOTIFY filterChanged FINAL)
    /// Text the notes (and their replies or authors) contain.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY filterChanged FINAL)
    /// Counts in the scope (current sequence or all), whatever the status filter.
    Q_PROPERTY(int openCount READ openCount NOTIFY notesChanged FINAL)
    Q_PROPERTY(int resolvedCount READ resolvedCount NOTIFY notesChanged FINAL)
    /// The note under the playhead in the current sequence (0: none).
    Q_PROPERTY(int currentNote READ currentNote NOTIFY currentChanged FINAL)
    /// The note chosen in the panel (0: none).
    Q_PROPERTY(int selectedNote READ selectedNote WRITE setSelectedNote NOTIFY currentChanged FINAL)
    /// Drawing a mark on the monitor for the next note.
    Q_PROPERTY(bool drawing READ drawing WRITE setDrawing NOTIFY markChanged FINAL)
    /// The mark the next note gets: { x, y, width, height } in frame fractions, or empty.
    Q_PROPERTY(QVariantMap pendingMark READ pendingMark NOTIFY markChanged FINAL)
    /// What the monitor draws: { x, y, width, height, resolved, pending } or empty.
    Q_PROPERTY(QVariantMap shownMark READ shownMark NOTIFY markChanged FINAL)
    /// Who writes the notes (Preferences.reviewAuthor); `authorName` is it or "Editor".
    Q_PROPERTY(QString author READ author WRITE setAuthor NOTIFY authorChanged FINAL)
    Q_PROPERTY(QString authorName READ authorName NOTIFY authorChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    /// The last package: { kind ("export" | "import"), ok, path, notes, frames, added, updated, unchanged,
    /// conflicts: [text], warnings: [text], error }; empty before.
    Q_PROPERTY(QVariantMap packageResult READ packageResult NOTIFY packageChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit ReviewBridge(QObject* parent = nullptr);
    ~ReviewBridge() override;
    static ReviewBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    /// Package exports on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_job.setSynchronous(synchronous); }
    /// The thumbnails' services (tests may pass placeholders; by default ReviewFrames over the export renderer).
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }

    QVariantList notes() const { return m_notes; }
    QString statusFilter() const { return m_status; }
    void setStatusFilter(const QString& status);
    bool allSequences() const { return m_allSequences; }
    void setAllSequences(bool all);
    QString query() const { return m_query; }
    void setQuery(const QString& query);
    int openCount() const { return m_openCount; }
    int resolvedCount() const { return m_resolvedCount; }
    int currentNote() const { return m_currentNote; }
    int selectedNote() const { return m_selectedNote; }
    void setSelectedNote(int id);
    bool drawing() const { return m_drawing; }
    void setDrawing(bool drawing);
    QVariantMap pendingMark() const { return m_pendingMark; }
    QVariantMap shownMark() const { return m_shownMark; }
    QString author() const { return m_author; }
    void setAuthor(const QString& author);
    QString authorName() const;
    bool busy() const { return m_job.running(); }
    QVariantMap packageResult() const { return m_package; }
    QString message() const { return m_message; }

    /// A note at the playhead, or over the sequence's In-Out when `overInOut` and both are set, with the pending mark.
    /// "" or why not.
    Q_INVOKABLE QString add(const QString& text, bool overInOut = false);
    Q_INVOKABLE QString reply(int id, const QString& text);
    Q_INVOKABLE QString resolve(int id, bool resolved = true);
    Q_INVOKABLE QString remove(int id);
    /// The current sequence's notes (the shown ones) as markers; `removeNotes` deletes them too.
    Q_INVOKABLE QString toMarkers(bool removeNotes = false);
    /// Moves the playhead to a note (switching to its sequence) and selects it.
    Q_INVOKABLE bool seekToNote(int id);
    /// The mark the next note gets, in frame fractions (width = height = 0: a point); clamped to the frame.
    Q_INVOKABLE void setPendingMark(double x, double y, double width = 0.0, double height = 0.0);
    Q_INVOKABLE void clearPendingMark();
    /// Writes the current sequence's notes (`openOnly`: the open ones) as a package folder. "" when started.
    Q_INVOKABLE QString exportPackage(const QUrl& folder, const QString& title = {}, bool openOnly = false);
    /// Reads a package's notes back (one undo step). "" or why not; the counts are in packageResult.
    Q_INVOKABLE QString importPackage(const QUrl& folder);

signals:
    void notesChanged();
    void filterChanged();
    void currentChanged();
    void markChanged();
    void authorChanged();
    void busyChanged();
    void packageChanged();
    void messageChanged();

private:
    std::shared_ptr<command::CommandServices> services();
    void setMessage(const QString& message);
    void rebuild();
    void updateCurrent();
    QString run(const char* command, command::Json params);

    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::CommandServices> m_services;
    CommandJob m_job;
    QVariantList m_notes;
    QString m_status = QStringLiteral("all");
    bool m_allSequences = false;
    QString m_query;
    int m_openCount = 0;
    int m_resolvedCount = 0;
    int m_currentNote = 0;
    int m_selectedNote = 0;
    bool m_drawing = false;
    QVariantMap m_pendingMark;
    QVariantMap m_shownMark;
    QString m_author;
    QVariantMap m_package;
    QString m_message;
};

struct ReviewBridgeForeign {
    Q_GADGET
    QML_FOREIGN(ReviewBridge)
    QML_NAMED_ELEMENT(Review)
    QML_SINGLETON
public:
    static ReviewBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return ReviewBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
