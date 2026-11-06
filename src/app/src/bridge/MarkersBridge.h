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
//   * 0.7 (card 119) Several markers at once: Ctrl/Shift-click rows (or flags on the ruler, or a Ctrl-drag marquee over
//     the ruler) select a group; Delete, Cut, Copy, Paste at the playhead and a group move (a flag drag, or Alt+arrows)
//     act on all of them, each as ONE undo step. The selection is a list of indices into the current scope's list; it
//     follows its own edits and is dropped when anything else changes the markers (their indices would mean something
//     else). The marker clipboard is kept by this singleton, so it survives a change of sequence or project.
//   * Navigation: Go to Next / Previous Marker (Shift+M, Ctrl+Shift+M).
//   * Export: the sequence's markers as CSV or YouTube chapters (the sequence.exportMarkers command; YouTube's rules
//     come back as warnings), to text or to a file the user chose.
//
// Principle 9: QML sees marker indices, text and colour indices; times go in as timecode text or the ruler's time keys.

#include <wabcut/model/Ids.h>
#include <wabcut/model/MarkerGroupOps.h>
#include <wabcut/model/Sequence.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <expected>
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
    /// 0.7 (card 119): the selected markers, as indices (the `index` of `items`), ascending; only of the current scope.
    Q_PROPERTY(QVariantList selection READ selection NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged FINAL)
    /// How many markers the marker clipboard holds (Cut / Copy); Paste is available while it is more than 0.
    Q_PROPERTY(int clipboardCount READ clipboardCount NOTIFY clipboardChanged FINAL)

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
    QVariantList selection() const;
    int selectionCount() const { return static_cast<int>(m_selected.size()); }
    int clipboardCount() const { return static_cast<int>(m_clipboard.size()); }

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
    /// The sequence's markers as "csv" or "youtube": { text, count, warnings: [...] }. P1 (#24): `range` "inOut" keeps
    /// the markers inside the In/Out marks (YouTube times from the In mark; "" / "sequence" = all), `chaptersOnly` uses
    /// chapter markers only.
    Q_INVOKABLE QVariantMap exportText(const QString& format, const QString& range = QString(), bool chaptersOnly = false) const;
    /// Writes exportText(format, range, chaptersOnly) to `file` (UTF-8); returns the path written, or "" (see `message`).
    Q_INVOKABLE QString exportToFile(const QString& format, const QUrl& file, const QString& range = QString(),
                                     bool chaptersOnly = false);
    /// Runs a keymap command this bridge owns (marker.goToNext / goToPrevious / clearSelected / clearAll); false for
    /// others.
    Q_INVOKABLE bool runCommand(const QString& commandId);

    // ---- 0.6 (P1 round, stream 1A): Edit Marker..., Add Chapter Marker..., M on a marker opens it ----
    /// The marker M would open instead of adding one: a sequence marker starting on the playhead's frame, or (with
    /// `timelineFocused` and a selected video clip under the playhead, where M adds a clip marker) a clip marker on that
    /// clip's source frame there. { scope: "sequence" | "clip", index } or {}.
    Q_INVOKABLE QVariantMap markerAtPlayhead(bool timelineFocused = false) const;
    /// Add Chapter Marker...: a sequence marker of type chapter at the playhead, one undo step. Returns its index (the
    /// sequence scope), or -1 (see `message`).
    Q_INVOKABLE int addChapterMarker();
    /// The keymap commands Main routes here for the editor (marker.edit, marker.addChapter).
    static bool handlesCommand(const QString& commandId);
    /// { handled, enabled, reason } of those commands ("No marker at the playhead").
    Q_INVOKABLE QVariantMap commandAvailability(const QString& commandId) const;

    // ---- 0.6 (P1 round, stream 1C #24): YouTube chapters to the clipboard ----
    /// Puts exportText(format, range, chaptersOnly) on the clipboard and says so in `message` ("Copied 5 chapters",
    /// "Copied with warnings: ..."); false (and why in `message`) when there is nothing to copy.
    Q_INVOKABLE bool copyToClipboard(const QString& format, const QString& range = QString(), bool chaptersOnly = false);

    // ---- 0.7 (card 119): several markers at once ----
    Q_INVOKABLE bool isSelected(int index) const;
    /// Selects marker `index` of the current scope. mode: "replace" (only it; the default), "toggle" (Ctrl-click: it joins
    /// or leaves the selection) or "extend" (Shift-click: every listed marker between the last plain click and it, in the
    /// list's order, joins). An index that does not exist selects nothing.
    Q_INVOKABLE void select(int index, const QString& mode = QStringLiteral("replace"));
    /// Selects every marker the panel lists (the colour and search filters apply), or none.
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    /// The ruler's marquee (sequence scope): selects the markers starting between the two TimelineView time keys
    /// (`add`: they join the selection). Returns how many are selected afterwards.
    Q_INVOKABLE int selectBetween(const QString& fromKey, const QString& toKey, bool add);
    /// Delete: removes the selected markers, one undo step ("Remove Markers"). False without a selection.
    Q_INVOKABLE bool removeSelected();
    /// Moves the selected markers together by a TimelineView time distance (a ruler drag) or by whole frames (Alt+arrows),
    /// one undo step ("Move Markers"). The distance lands on the frame grid and stops where the earliest marker reaches
    /// 0. They stay selected.
    Q_INVOKABLE bool moveSelectedBy(const QString& deltaKey);
    Q_INVOKABLE bool nudgeSelected(int frames);
    /// Copy: the selected markers go on the marker clipboard. Returns how many; cutSelected also removes them.
    Q_INVOKABLE int copySelected();
    Q_INVOKABLE bool cutSelected();
    /// Paste: the clipboard's markers go into the current scope with the earliest on the playhead (clip scope: where the
    /// selected clip shows its media under the playhead), the others keeping their distances; one undo step ("Paste
    /// Markers"); the pasted ones are selected. False when the clipboard is empty.
    Q_INVOKABLE bool paste();

    // ---- 0.7: Import markers ----
    /// Reads a marker list from a CSV, text or JSON file (marker.import for the sequence's markers, source.importMarkers
    /// in the clip scope, the selected clip's media): one undo step. They join the markers there (exact copies are left
    /// out); `replace` clears those first; `atPlayhead` (sequence scope) counts the file's times from the playhead.
    /// Returns "" or why not; `message` says what came in ("Imported 12 markers", with the lines skipped).
    Q_INVOKABLE QString importFromFile(const QUrl& file, bool atPlayhead = false, bool replace = false);

signals:
    void markersChanged();
    void messageChanged();
    void selectionChanged();
    void clipboardChanged();

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

    /// 0.7: sets the selection (indices into the scope's list), remembering the list they refer to.
    void setSelection(std::vector<int> indices);
    /// Drops the selection when the markers it points at are not the list it was made on.
    void pruneSelection();
    /// The selected markers, in list order.
    std::vector<Marker> selectedMarkers() const;
    /// The group edit's result stored under `label`; the markers it touched stay selected. False (and `message`) on failure.
    bool applyGroup(const char* label, std::expected<edit::MarkerGroupEdit, Error> group);
    /// Removes the selected markers as one step called `label`.
    bool removeSelectedAs(const char* label);
    /// The frame rate the current scope's times count in.
    std::optional<Rate> scopeRate() const;
    /// Where Paste puts the earliest copied marker.
    std::optional<Ticks> pasteTarget() const;

    QPointer<ProjectController> m_controller;
    /// 0.7: the selection (ascending indices), the list it was made on and what that list belongs to.
    std::vector<int> m_selected;
    std::vector<Marker> m_selectedFor;
    MediaId m_selectedMedia;
    int m_anchor = -1; ///< the last plain or toggled click: where a Shift-click extends from
    std::vector<Marker> m_clipboard;
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
