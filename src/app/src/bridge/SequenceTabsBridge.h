#pragma once

// SequenceTabsBridge: Premiere's sequence tabs above the timeline, for QML (singleton `SequenceTabs`, M5i).
//
//   * Open sequences are tabs; one is current (ProjectController's current sequence). Tabs open from the Library
//     (double-click a sequence), the sequence menu and New Sequence; close with the tab's ×, a middle click or
//     close(); reorder by dragging (move()); Ctrl+Tab / Ctrl+Shift+Tab cycle them in the Timeline (cycle()). The last
//     open tab cannot be closed (the editor always shows a sequence).
//   * Each tab keeps its own playhead (here) and its zoom and scroll (TimelineView keeps those per sequence). Track
//     targets and source patching are per sequence already (the model, SourceMonitorBridge).
//   * `sequences` lists every sequence of the project for the Library (with whether it is open).
//   * Nested sequences (later, in the model): a clip that is a sequence opens its tab through open(); that is the
//     seam for "double-click a nested sequence".
//
// Principle 9: QML sees opaque keys ("s3") and text.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <unordered_map>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class SequenceTabsBridge : public QObject {
    Q_OBJECT
    /// [{ id: "s…", name, current, label (0.2: the label colour index, 0..15) }] in tab order.
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged FINAL)
    Q_PROPERTY(QString current READ current NOTIFY tabsChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY tabsChanged FINAL)
    /// Every sequence of the project: [{ id, name, durationText, open, label }] in project order.
    Q_PROPERTY(QVariantList sequences READ sequences NOTIFY tabsChanged FINAL)

public:
    explicit SequenceTabsBridge(QObject* parent = nullptr);
    static SequenceTabsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    QVariantList tabs() const;
    QString current() const;
    int count() const { return static_cast<int>(m_tabs.size()); }
    QVariantList sequences() const;

    /// Opens the sequence's tab (after the current one) or focuses it; it becomes the current sequence.
    Q_INVOKABLE bool open(const QString& sequenceId);
    /// Focuses an open tab.
    Q_INVOKABLE bool activate(const QString& sequenceId);
    /// Closes a tab (not the last one); closing the current tab focuses its neighbour.
    Q_INVOKABLE bool close(const QString& sequenceId);
    /// Moves the tab at `from` to `to` (tab indices).
    Q_INVOKABLE void move(int from, int to);
    /// Focuses the next (+1) or previous (-1) tab, wrapping around.
    Q_INVOKABLE void cycle(int delta);
    /// 0.2, Premiere's Label on a sequence tab: the sequence's label colour (0..15), one undo step. False when the
    /// sequence or the index is not valid.
    Q_INVOKABLE bool setLabel(const QString& sequenceId, int colorIndex);

signals:
    void tabsChanged();

private:
    void onSequencesChanged();
    void onSequenceChanged();
    void onPlayheadChanged();
    bool switchTo(SequenceId id);
    int tabIndex(SequenceId id) const;

    QPointer<ProjectController> m_controller;
    std::vector<SequenceId> m_tabs;
    SequenceId m_current;
    std::unordered_map<std::uint64_t, Ticks> m_playheads;
};

/// QML singleton registration through the factory.
struct SequenceTabsSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::SequenceTabsBridge)
    QML_NAMED_ELEMENT(SequenceTabs)
    QML_SINGLETON
public:
    static SequenceTabsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return SequenceTabsBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
