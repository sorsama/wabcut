#pragma once

// SourcesBridge: sources and citations for video essays, for QML (singleton `Sources`, 0.7 card 177).
//
//   * The Sources panel lists the current sequence's Source markers by time (the claim, the reference, whether it still
//     wants a source; the claims that need one stand out and can be shown alone); click to jump to the claim.
//   * Adding: a claim at the playhead, over the In-Out range, or over the selected clips (add*), with its reference; editing
//     and removing go through the sources.* commands, one undo step each (sources.add / update / remove).
//   * Exporting: the list as text (a YouTube description block, a numbered list, Markdown, an APA bibliography:
//     sources.export), copied, written to a file, or as a PDF (Qt's PDF writer over the same text); lower-third citations
//     over the claims (sources.lowerThirds).
//
// Principle 9: QML sees marker indices, text and timecodes.

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;
class TranscriptBridge;

class SourcesBridge : public QObject {
    Q_OBJECT
    /// [{ index, number, claim, note, url, title, author, publisher, date, archive, screenshot, needed, cited,
    ///    atText, atSeconds, duration, atPlayhead }] in time order; only the claims that need a source with `onlyNeeded`.
    Q_PROPERTY(QVariantList items READ items NOTIFY sourcesChanged FINAL)
    /// Every source of the sequence, and how many still want a source.
    Q_PROPERTY(int count READ count NOTIFY sourcesChanged FINAL)
    Q_PROPERTY(int neededCount READ neededCount NOTIFY sourcesChanged FINAL)
    Q_PROPERTY(bool onlyNeeded READ onlyNeeded WRITE setOnlyNeeded NOTIFY sourcesChanged FINAL)
    /// The last problem or result, for a toast.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit SourcesBridge(QObject* parent = nullptr);
    static SourcesBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    QVariantList items() const;
    int count() const { return m_count; }
    int neededCount() const { return m_needed; }
    bool onlyNeeded() const { return m_onlyNeeded; }
    void setOnlyNeeded(bool only);
    QString message() const { return m_message; }

    /// Cites a source for `claim` at the playhead, over the In-Out range (`range` "marks"), over the selected clips
    /// (`range` "selection") or over the words selected in the Transcript tab (`range` "transcript"); `fields` as sources.add (url, title, author, publisher, date, archive, screenshot, note,
    /// needed). Returns "" or the problem.
    Q_INVOKABLE QString add(const QString& claim, const QVariantMap& fields = {}, const QString& range = QStringLiteral("playhead"));
    /// Changes the source at marker `index` (any of the sources.update fields). Returns "" or the problem.
    Q_INVOKABLE QString update(int index, const QVariantMap& fields);
    Q_INVOKABLE QString remove(int index);
    /// Moves the playhead to the claim.
    Q_INVOKABLE void seekTo(int index);
    /// The list as text in `format` ("youtube", "plain", "markdown", "apa"); `includeUncited` lists the claims that still
    /// need a source too. { text, cited, uncited, warnings }.
    Q_INVOKABLE QVariantMap exportText(const QString& format, bool includeUncited = false) const;
    /// Writes that text to `file` (UTF-8), or a PDF of it when the file ends in .pdf. Returns "" or the problem.
    Q_INVOKABLE QString exportFile(const QUrl& file, const QString& format, bool includeUncited = false);
    /// Lower-third graphics over the cited claims (sources.lowerThirds). Returns "" or the problem.
    Q_INVOKABLE QString addLowerThirds(double seconds = 4.0);

signals:
    void sourcesChanged();
    void messageChanged();

private:
    QString runCommand(const std::string& name, const QVariantMap& params);
    void setMessage(const QString& message);
    void refresh();
    QVariantList list(bool filtered) const;

    QPointer<ProjectController> m_controller;
    QPointer<TranscriptBridge> m_transcripts;
    bool m_onlyNeeded = false;
    int m_count = 0;
    int m_needed = 0;
    QString m_message;
};

struct SourcesSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::SourcesBridge)
    QML_NAMED_ELEMENT(Sources)
    QML_SINGLETON
public:
    static SourcesBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return SourcesBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
