#pragma once

// MarkersBridge: Markers 2.0, for QML (singleton `Markers`, M5i).
//
//   * Sequence markers and clip markers (a media item's markers in source time; every clip of the media shows them,
//     like Premiere's master-clip markers). The Markers panel lists either the sequence's or the selected clip's
//     (`scope`), filtered by Premiere's 8 colours (`colorMask`) and a search over names and comments, click to jump.
//   * Editing: add (M: a clip marker when a selected clip is under the playhead in the Timeline, else a sequence marker),
//     inline name/comment, colour, type (comment, chapter, segmentation, web link with URL and frame target), moving
//     and Alt-drag duration from the ruler, remove, Clear Selected Marker (Ctrl+Alt+M: the marker at the playhead) and
//     Clear All Markers. Each change is one undo step through the model's marker ops.
//   * Navigation: Go to Next / Previous Marker (Shift+M, Ctrl+Shift+M).
//   * Export: the sequence's markers as CSV or YouTube chapters (the sequence.exportMarkers command; YouTube's rules
//     come back as warnings), to text or to a file the user chose.
//
// Principle 9: QML sees marker indices, text and colour indices; times go in as timecode text or the ruler's time keys.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Sequence.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class MarkersBridge : public QObject {
    Q_OBJECT
    /// "sequence" or "clip" (the primary selected clip's media).
    Q_PROPERTY(QString scope READ scope WRITE setScope NOTIFY markersChanged FINAL)
    /// The listed markers: [{ index, name, comment, colorIndex, type, url, target, inText, outText, durationText,
    /// ranged, atPlayhead }] (index = position in the unfiltered list, for edits).
    Q_PROPERTY(QVariantList items READ items NOTIFY markersChanged FINAL)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY markersChanged FINAL)
    /// Bit i = show colour i (Premiere's 8: green, red, purple, orange, yellow, white, blue, cyan). 0xFF = all.
    Q_PROPERTY(int colorMask READ colorMask WRITE setColorMask NOTIFY markersChanged FINAL)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY markersChanged FINAL)
    /// Clip scope: the clip's name ("" without a selected clip).
    Q_PROPERTY(QString clipName READ clipName NOTIFY markersChanged FINAL)
    /// The last problem or export result, for a toast.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit MarkersBridge(QObject* parent = nullptr);
    static MarkersBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    QString scope() const { return m_clipScope ? QStringLiteral("clip") : QStringLiteral("sequence"); }
    void setScope(const QString& scope);
    QVariantList items() const;
    int totalCount() const;
    int colorMask() const { return m_colorMask; }
    void setColorMask(int mask);
    QString search() const { return m_search; }
    void setSearch(const QString& text);
    QString clipName() const;
    QString message() const { return m_message; }

    /// One marker of the current scope, unfiltered (the same keys as `items`), or an empty map.
    Q_INVOKABLE QVariantMap marker(int index) const;
    /// M: a marker at the playhead; `timelineFocused` lets a selected clip under the playhead take a clip marker.
    /// Returns "sequence", "clip" or "" (nothing added).
    Q_INVOKABLE QString add(bool timelineFocused = false);
    /// A clip marker on the clip's media at the source frame under the playhead.
    Q_INVOKABLE bool addClipMarker(const QString& clipId);
    /// Changes the marker at `index` (in the current scope): any of name, comment, colorIndex, type, url, target, and
    /// start / duration as timecode text.
    Q_INVOKABLE bool update(int index, const QVariantMap& fields);
    /// Ruler drags (sequence markers): move the start, or (Alt-drag) set the end, to a TimelineView time key.
    Q_INVOKABLE bool moveTo(int index, const QString& timeKey);
    Q_INVOKABLE bool setEnd(int index, const QString& timeKey);
    Q_INVOKABLE bool remove(int index);
    /// Clear Selected Marker (Ctrl+Alt+M): the sequence marker at the playhead.
    Q_INVOKABLE bool clearAtPlayhead();
    /// Clear All Markers (Ctrl+Alt+Shift+M), in the current scope.
    Q_INVOKABLE bool clearAll();
    Q_INVOKABLE bool goToNext();
    Q_INVOKABLE bool goToPrevious();
    /// Moves the playhead to the marker (clip scope: where the primary clip shows it).
    Q_INVOKABLE bool seek(int index);
    /// The sequence's markers as "csv" or "youtube": { text, count, warnings: [...] }.
    Q_INVOKABLE QVariantMap exportText(const QString& format) const;
    /// Writes exportText(format) to `file` (UTF-8); returns the path written, or "" (see `message`).
    Q_INVOKABLE QString exportToFile(const QString& format, const QUrl& file);
    /// Runs a keymap command this bridge owns (marker.goToNext / goToPrevious / clearSelected / clearAll); false for
    /// others.
    Q_INVOKABLE bool runCommand(const QString& commandId);

signals:
    void markersChanged();
    void messageChanged();

private:
    struct Scoped {
        const std::vector<Marker>* markers = nullptr;
        MediaId media;  ///< clip scope
        ClipId clip;    ///< clip scope: the clip whose instance times are shown
    };
    Scoped scoped() const;
    QVariantList list(bool filtered) const;
    bool replaceMarkers(const char* label, std::vector<Marker> markers);
    std::optional<Ticks> parseTime(const QVariant& text) const;
    void setMessage(const QString& message);
    /// M5n (P.1): an edit lists the markers again only when it changed them (or the clip-scope clip, its media).
    void onRevision();
    /// The playhead lists them again only when the markers under it change (`atPlayhead`).
    void onPlayhead();
    /// Indexes of the sequence-scope markers under the playhead (the `atPlayhead` items; none in clip scope).
    std::vector<int> markersAtPlayhead() const;

    QPointer<ProjectController> m_controller;
    /// markersAtPlayhead() as of the last markersChanged.
    std::vector<int> m_atPlayhead;
    bool m_clipScope = false;
    int m_colorMask = 0xFF;
    QString m_search;
    QString m_message;
};

struct MarkersSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::MarkersBridge)
    QML_NAMED_ELEMENT(Markers)
    QML_SINGLETON
public:
    static MarkersBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return MarkersBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
