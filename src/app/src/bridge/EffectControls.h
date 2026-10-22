#pragma once

// EffectControlsModel: Premiere's Effect Controls (M5d) for the selected clip or transition, as one flat list
// model for QML (ProjectController.effectControls).
//
// Rows, in order (a clip with linked audio shows the video clip's sections, then the audio partner's):
//   header "Transform" + Scale, Position X/Y, Rotation, Anchor X/Y     (intrinsic, pinned)
//   header "Opacity"   + Opacity                                       (intrinsic, pinned)
//   header <effect> + its parameters, for each effect in the video clip's stack (reorderable, removable)
//   header "Audio"     + Volume, Pan                                   (intrinsic, pinned)
//   header <effect> + its parameters, for each effect in the audio clip's stack
// or, for a selected transition: its header, Duration, Alignment (two-sided), Transition and its parameters.
//
// Every row has a stable key ("h:motion", "p:scale", "h:e12", "e12:radius", "t:duration"), so the model updates in
// place: rows whose data changed get dataChanged, removed/added rows are removed/inserted and moved effect sections
// are moved (ListView animates them). Values are in UI units (ParameterUi.h) evaluated at the playhead; keyframes
// are positions 0..1 in the clip's range (the keyframe lane), never ticks (principle 9).
//
// Edits are model edits through ProjectController (one undo step each; slider drags and keyframe drags merge into
// one step until endEdit()/endKeyDrag()). A value change on an animated parameter sets the key at the playhead.
// The keyframe lane keeps a selection of keys (any rows); dragging moves them together (Alt duplicates), the
// interpolation menu and Delete act on them.
// Value graph (M5g): an animated parameter's lane can open into a graph of its curve (setGraphOpen), with the keys at
// their values and, on Bezier keys, their in/out handles. Dragging a handle shapes that side's tangent (influence and
// slope, edit::setKeyframe); the two handles of a key stay linked (same slope) unless Alt breaks them.

#include "BridgeIds.h"
#include "ParameterUi.h"

#include <wabcut/model/Clip.h>
#include <wabcut/model/FinishingOps.h>

#include <QAbstractListModel>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace wabcut::app {

class ProjectController;

class EffectControlsModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by ProjectController.effectControls")

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
    /// "clip" (a clip is selected), "transition", or "" (nothing to show).
    Q_PROPERTY(QString mode READ mode NOTIFY contextChanged FINAL)
    /// Clip name or transition name, the clip's range or the transition's edit point, its track and label colour.
    Q_PROPERTY(QString title READ title NOTIFY contextChanged FINAL)
    Q_PROPERTY(QString subtitle READ subtitle NOTIFY contextChanged FINAL)
    Q_PROPERTY(QString trackName READ trackName NOTIFY contextChanged FINAL)
    Q_PROPERTY(int colorIndex READ colorIndex NOTIFY contextChanged FINAL)
    /// The keyframe lane covers the clip: its first and last frame as timecode.
    Q_PROPERTY(QString laneStartText READ laneStartText NOTIFY laneChanged FINAL)
    Q_PROPERTY(QString laneEndText READ laneEndText NOTIFY laneChanged FINAL)
    /// The playhead in the lane, 0..1; -1 when it is outside the clip.
    Q_PROPERTY(double playheadPosition READ playheadPosition NOTIFY laneChanged FINAL)
    Q_PROPERTY(int selectedKeyCount READ selectedKeyCount NOTIFY keySelectionChanged FINAL)
    /// The interpolation every selected key shares ("linear", "hold", "bezier", "easeIn", "easeOut", "easeInOut"),
    /// or "" when they differ or none is selected.
    Q_PROPERTY(QString selectedKeyInterpolation READ selectedKeyInterpolation NOTIFY keySelectionChanged FINAL)
    /// A one-line message about the last edit ("Shortened to the available media"), or "".
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged FINAL)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1, // unique row key
        KindRole,           // "header" | "param" | "toggle" | "timecode" | "choice"
        SectionRole,        // section key ("motion", "opacity", "audio", "e12", "transition")
        SectionKindRole,    // "intrinsic" | "effect" | "transition"
        TitleRole,          // header title or parameter label
        IconRole,
        UnitRole,
        FromRole,
        ToRole,
        StepRole,
        DecimalsRole,
        OriginRole,         // where the slider fill starts
        ValueRole,          // UI units at the playhead
        DefaultValueRole,
        AnimatableRole,
        AnimatedRole,       // the stopwatch is on
        KeyAtPlayheadRole,
        HasPreviousKeyRole,
        HasNextKeyRole,
        KeysRole,           // [{ x: 0..1, selected, shape: interpolation name }]
        EffectEnabledRole,  // headers and their rows
        CollapsedRole,      // headers
        RemovableRole,      // headers: the trash button
        MovableRole,        // headers: the drag handle
        ResettableRole,
        StackIndexRole,     // headers of stack effects: position in the clip's stack
        StackKindRole,      // headers of stack effects: "video" | "audio"
        TextRole,           // timecode / choice rows
        OptionsRole,        // choice rows: [{ value, label }]
        DescriptionRole,
        KnownRole,          // false: an effect this version does not know (kept, bypassed)
        GraphOpenRole,      // the lane shows the value graph (animated parameters)
        GraphRole,          // { path: [x0, y0, x1, y1, ...], keys: [{ index, x, y, selected, shape, hasIn, inX, inY,
                            //   hasOut, outX, outY, broken }], min, max }: x 0..1 across the lane, y 0..1 down
    };
    Q_ENUM(Role)

    explicit EffectControlsModel(ProjectController* controller);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString mode() const { return m_mode; }
    QString title() const { return m_title; }
    QString subtitle() const { return m_subtitle; }
    QString trackName() const { return m_trackName; }
    int colorIndex() const { return m_colorIndex; }
    QString laneStartText() const { return m_laneStartText; }
    QString laneEndText() const { return m_laneEndText; }
    double playheadPosition() const { return m_playheadPosition; }
    int selectedKeyCount() const { return static_cast<int>(m_selectedKeys.size()); }
    QString selectedKeyInterpolation() const;
    QString notice() const { return m_notice; }

    /// Row index of a key, or -1.
    Q_INVOKABLE int rowOf(const QString& key) const;
    /// Every row as a map (tests, the palette).
    Q_INVOKABLE QVariantMap row(const QString& key) const;
    /// Key of row `index` ("" when out of range).
    Q_INVOKABLE QString keyAt(int index) const;
    /// Where a stack effect dragged to row `index` goes in the stack of `stackKind` ("video" / "audio"): the
    /// position of the effect section there, 0 above the stack, the end below it.
    Q_INVOKABLE int stackIndexForRow(int index, const QString& stackKind) const;

    // ---- Parameter rows ----
    /// Sets a parameter (UI units): the constant, or the key at the playhead when it is animated. A drag merges
    /// into one undo step until endEdit().
    Q_INVOKABLE void setValue(const QString& row, double value);
    Q_INVOKABLE void endEdit();
    /// Back to the default (at the playhead when animated).
    Q_INVOKABLE void resetValue(const QString& row);
    /// The stopwatch: on turns the value into a curve with a key at the playhead; off deletes every key and keeps
    /// the value at the playhead.
    Q_INVOKABLE void setAnimated(const QString& row, bool animated);
    /// Adds a key at the playhead, or removes the one there (the diamond button between the arrows).
    Q_INVOKABLE void toggleKeyAtPlayhead(const QString& row);
    /// Moves the playhead to the previous (-1) or next (+1) key of the row.
    Q_INVOKABLE void goToKey(const QString& row, int direction);
    /// A text row (0.2: Simple Text's text, the effect's data "text"): sets it; empty restores the default.
    Q_INVOKABLE void setEffectText(const QString& row, const QString& text);

    // ---- Sections ----
    Q_INVOKABLE void setCollapsed(const QString& section, bool collapsed);
    Q_INVOKABLE void setEffectEnabled(const QString& section, bool enabled);
    /// Removes a stack effect, or the selected transition.
    Q_INVOKABLE void removeSection(const QString& section);
    /// Resets every parameter of the section to its default.
    Q_INVOKABLE void resetSection(const QString& section);
    /// Moves a stack effect to `stackIndex` in its clip's stack.
    Q_INVOKABLE void moveEffect(const QString& section, int stackIndex);

    // ---- Adding effects ----
    /// The effect library for the Effects panel, grouped: [{ title, icon, kind, items: [{ id, name, category,
    /// description, icon, kind }], bins: [{ title, items }] }], filtered by a fuzzy `query`. `items` are by relevance
    /// to the query (else by bin and name); `bins` are Premiere's bins (the categories), alphabetical, each with its
    /// effects in alphabetical order (0.2).
    Q_INVOKABLE QVariantList library(const QString& query = {}) const;
    /// Adds an effect to the selected clip (to its partner of the effect's kind); `stackIndex` -1 appends.
    /// Returns "" or a message saying why not.
    Q_INVOKABLE QString addEffect(const QString& effectId, int stackIndex = -1);
    /// Adds an effect to a clip (or its linked partner of the effect's kind): a drop on the timeline.
    Q_INVOKABLE QString addEffectToClip(const QString& clipId, const QString& effectId);
    /// Whether `effectId` is a transition.
    Q_INVOKABLE bool isTransition(const QString& effectId) const;

    // ---- Keyframe lane (x = 0..1 across the clip) ----
    /// A press at `x` in a row's lane: selects the key within `tolerance` (Shift adds, Ctrl/Cmd toggles); a press
    /// beside every key clears the selection (unless a modifier is held). True when a key was hit.
    Q_INVOKABLE bool pressKey(const QString& row, double x, double tolerance, int modifiers);
    /// Rubber band over rows: selects the keys in [x0, x1] of those rows (Shift/Ctrl/Cmd add).
    Q_INVOKABLE void selectKeysInRange(const QStringList& rows, double x0, double x1, int modifiers);
    Q_INVOKABLE void clearKeySelection();
    /// Starts dragging the selected keys (`duplicate`: Alt — the originals stay).
    Q_INVOKABLE bool beginKeyDrag(bool duplicate);
    /// Moves the dragged keys by `dx` of the lane (frame-quantised, kept inside the clip); one undo step per drag.
    Q_INVOKABLE void dragKeys(double dx);
    Q_INVOKABLE void endKeyDrag();
    /// Interpolation of the selected keys: "linear", "hold", "bezier", "easeIn", "easeOut", "easeInOut".
    Q_INVOKABLE void setKeyInterpolation(const QString& interpolation);
    Q_INVOKABLE void deleteSelectedKeys();
    /// Lane ruler scrub: the playhead to `x` of the clip.
    Q_INVOKABLE void seekLane(double x);

    // ---- Value graph and Bezier handles (M5g) ----
    /// Opens or closes the value graph of a parameter row (animated parameters only).
    Q_INVOKABLE void setGraphOpen(const QString& row, bool open);
    /// A press at (x, y) in a row's graph (0..1 each, y down): starts dragging the Bezier handle within the
    /// tolerances. True when one was hit.
    Q_INVOKABLE bool pressHandle(const QString& row, double x, double y, double toleranceX, double toleranceY);
    /// Drags the handle to (x, y) of the graph: that side's influence (how far along the segment) and slope. The
    /// key's other handle follows with the same slope unless `broken` (Alt). One undo step per drag.
    Q_INVOKABLE void dragHandle(double x, double y, bool broken);
    Q_INVOKABLE void endHandleDrag();

    // ---- Transition rows ----
    Q_INVOKABLE void setTransitionDuration(const QString& timecode);
    /// "center", "startAtCut", "endAtCut".
    Q_INVOKABLE void setTransitionAlignment(const QString& alignment);
    Q_INVOKABLE void setTransitionEffect(const QString& effectId);

    /// 0.2 (Save Preset, EffectPresetsBridge): the clip a section ("motion", "opacity", "audio", "e12") belongs to, and
    /// the sections of a clip in the panel's order.
    std::optional<ClipId> sectionClip(const QString& section) const;
    QStringList sectionsOf(ClipId clip) const;

    /// Rebuilds the rows from the controller (connected to its change signals).
    void refresh();
    /// An edit rebuilds the rows only when it can change them (M5n, P.1): the shown clip, its linked partners or their
    /// media, their tracks' headers, a selected transition's track, the sequence's settings, or anything structural.
    void onRevision();

signals:
    void countChanged();
    void contextChanged();
    void laneChanged();
    void keySelectionChanged();
    void noticeChanged();

private:
    struct Row {
        QString key, kind, section, sectionKind, title, icon, unit, text, description, stackKind;
        double from = 0, to = 100, step = 1, decimals = 0, origin = 0, value = 0, defaultValue = 0;
        bool animatable = false, animated = false, keyAtPlayhead = false, hasPrev = false, hasNext = false;
        bool effectEnabled = true, collapsed = false, removable = false, movable = false, resettable = false;
        bool known = true;
        bool graphOpen = false;
        int stackIndex = -1;
        QVariantList keys, options;
        QVariantMap graph;
        // C++ only
        ClipId clip;
        edit::ParamRef ref;
        UiRange range;
        PropertyFrame frame;
        std::string undoLabel;

        bool sameData(const Row& o) const;
    };
    struct KeyRef {
        QString row;
        std::int64_t curveTime = 0;
        bool operator<(const KeyRef& o) const { return std::tie(row, curveTime) < std::tie(o.row, o.curveTime); }
        bool operator==(const KeyRef&) const = default;
    };

    std::vector<Row> buildRows();
    void addClipSections(std::vector<Row>& rows, const Clip& clip, bool video);
    void addParamRow(std::vector<Row>& rows, const Clip& clip, const QString& key, const QString& section,
                     const QString& sectionKind, const edit::ParamRef& ref, const QString& title, const QString& icon,
                     const UiRange& range, double defaultModel, bool animatable, bool enabled, std::string undoLabel);
    void addTransitionRows(std::vector<Row>& rows, const Track& track, const Transition& transition);
    /// A menu parameter (0.2: Premiere's popups, EffectParamInfo::options) shown as a choice row: its options' values
    /// are the UI values of its whole numbers, the row's text the current one.
    static void makeChoice(Row& row, const EffectParamInfo& param);
    /// A row whose value setValue() changes: a parameter, a switch, or a menu parameter's choice.
    static bool isValueRow(const Row& row);
    void applyRows(std::vector<Row> next);
    const Row* findRow(const QString& key) const;
    /// The clip a row edits (in the current sequence).
    const Clip* clipOf(const Row& row) const;
    std::optional<Ticks> keyTimelineTime(const Clip& clip, Ticks curveTime) const;
    void setNotice(const QString& notice);
    bool setTransitionSides(Ticks before, Ticks after, const char* label);
    /// Keys of a row's curve that the lane shows (timeline times inside the lane).
    std::vector<std::pair<std::size_t, Ticks>> visibleKeys(const Row& row, const Curve& curve, const Clip& clip) const;
    /// The value graph of a parameter row (GraphRole).
    QVariantMap graphOf(const Row& row, const Curve& curve, const Clip& clip) const;

    QPointer<ProjectController> m_controller;
    std::vector<Row> m_rows;
    QString m_mode, m_title, m_subtitle, m_trackName;
    int m_colorIndex = 0;
    QString m_laneStartText, m_laneEndText;
    double m_playheadPosition = -1;
    TimeRange m_lane;
    std::set<QString> m_collapsed;
    std::set<KeyRef> m_selectedKeys;
    std::set<KeyRef> m_shownKeys; ///< keys the rows show (rebuilt with them)
    QString m_notice;
    bool m_refreshing = false;

    struct DragGroup {
        QString row;
        ClipId clip;
        edit::ParamRef ref;
        Curve original;
        std::vector<Ticks> moving; ///< curve times of the dragged keys at the start
    };
    struct KeyDrag {
        bool active = false;
        bool duplicate = false;
        std::vector<DragGroup> groups;
        std::string mergeKey;
    } m_drag;
    int m_dragCounter = 0;

    std::set<QString> m_graphRows; ///< rows whose value graph is open
    struct HandleDrag {
        bool active = false;
        QString row;
        std::size_t index = 0; ///< the key in the curve
        bool out = true;       ///< its leaving side, else its arriving side
        double min = 0, max = 1; ///< the graph's value range, frozen while dragging (UI units)
        std::string mergeKey;
    } m_handleDrag;
};

} // namespace wabcut::app
