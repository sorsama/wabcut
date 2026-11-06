#pragma once

// RightsBridge (0.7, card 179, QML singleton `Rights`): the UI of the rights and clearance record - File > Rights and
// Clearance Log…, the Library's "Rights and Clearance…" row and the Media Usage Report's flags. Documentary makers keep,
// for every third-party clip and track, where it came from, the licence, where and until when it may be shown, what it
// cost, how it must be credited and whether it is cleared; the log lists them, flags what is not cleared or about to
// expire, exports as a spreadsheet and makes the end-credits roll.
//
//   * log(): project.rightsLog (the sources the sequence uses, or every file) as { ok, rows, totals, asOf, text }.
//   * setRights(): media.setRights on Library items (one undo step for all of them).
//   * writeLog(): the log's CSV (or JSON) written to a file, atomically, with a byte order mark so spreadsheets read UTF-8.
//   * makeCreditsRoll(): sequence.creditsRoll, a rolling graphic of the credit lines at the end of the sequence.
//   * openFor(): asks the window to open the dialog (selecting the items named).
//
// Every edit goes through the command layer, so it is one undo step with a clear name and the same on the command line.
// Keys: media "m4".

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class RightsBridge : public QObject {
    Q_OBJECT
    /// Counts up when the project changes: the dialog refreshes its list.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged FINAL)
    /// The last thing that went wrong or was done ("" = nothing to say).
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit RightsBridge(QObject* parent = nullptr);
    static RightsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    int revision() const { return m_revision; }
    QString message() const { return m_message; }

    /// The log: `options` { scope "sequence" | "project", flaggedOnly, includeUnrecorded, asOf "YYYY-MM-DD", soonDays,
    /// range "sequence" | "inOut" }. Result { ok, error, asOf, rows: [{ key ("m4"), media, path, hasRecord, inherited,
    /// status, statusLabel, source, licence, reference, territory, expiry, daysLeft, cost (text), currency, creditLine,
    /// notes, uses, usedSeconds, firstUse, flags, uncleared, expired, expiresSoon }], totals: { items, withRecord,
    /// withoutRecord, needAttention, uncleared, expired, expiresSoon, statuses, costs: [{ currency, total, items }] } }.
    Q_INVOKABLE QVariantMap log(const QVariantMap& options) const;
    /// Sets the fields named in `fields` (status, source, licence, reference, territory, expiry, cost, currency,
    /// creditLine, notes; text, "" clears) on the Library items `mediaKeys`: "" on success, else why not.
    Q_INVOKABLE QString setRights(const QStringList& mediaKeys, const QVariantMap& fields);
    /// Writes the log to `file` (a path or file URL; ".json" writes JSON, anything else CSV): { ok, path, rows, error }.
    Q_INVOKABLE QVariantMap writeLog(const QString& file, const QVariantMap& options);
    /// Makes the credits roll from the credit lines of the sources the sequence uses: `options` { heading, order
    /// "appearance" | "alphabetical", range }. Result { ok, error, clip ("c12"), credits [text], missingCreditLine [name],
    /// warnings [text] }.
    Q_INVOKABLE QVariantMap makeCreditsRoll(const QVariantMap& options);
    /// A cost typed by a person -> its normalised text ("1,250.5" -> "1250.50"), or "" when it is not an amount.
    Q_INVOKABLE QString normalisedCost(const QString& text) const;
    /// Today as YYYY-MM-DD (the day licences are judged on unless the dialog names one).
    Q_INVOKABLE QString today() const;
    /// Asks the window to open the Rights and Clearance Log with `mediaKeys` selected.
    Q_INVOKABLE void openFor(const QStringList& mediaKeys);

signals:
    void revisionChanged();
    void messageChanged();
    /// QML: open the dialog and select these Library items.
    void openRequested(const QStringList& mediaKeys);

private:
    void setMessage(const QString& message);

    QPointer<ProjectController> m_controller;
    int m_revision = 0;
    QString m_message;
};

struct RightsBridgeForeign {
    Q_GADGET
    QML_FOREIGN(RightsBridge)
    QML_NAMED_ELEMENT(Rights)
    QML_SINGLETON
public:
    static RightsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return RightsBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
