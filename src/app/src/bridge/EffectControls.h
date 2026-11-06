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
// 0.4 (Premiere's Effect Controls): section headers can be selected (a click, Ctrl toggles, Shift extends from the
// anchor; clip mode only). Backspace (effectControls.removeSelectedEffect) and Delete remove the selected stack effects
// and reset the selected fixed sections; Copy / Cut put them on an in-session effects clipboard, and Paste puts them on
// every selected clip (newest copy wins over the clip clipboard: ProjectController::clipboardChanged clears it).
// 0.5.1: the lane zooms (Premiere's Effect Controls timeline): it shows [viewStart, viewEnd] of the clip, down to five
// frames; another clip in the lane shows whole again. Lane positions stay 0..1 of the clip; the panel maps its pixels.
// 0.6 (Premiere's Text in Effect Controls): a graphic clip shows one "Text (<layer>)" section per text layer, top layer
// first, above its Transform: Source Text (the text, keyframable as hold keys; font, style, size, alignment, tracking,
// kerning, leading, baseline shift, capitals, faux bold / italic, underline), Appearance (fill, strokes, background,
// shadow) and the layer's Transform. A title clip's Title Style section is its "Text (<name>)" section with the title's
// text and font. Numbers are keyframable like any row (edit::ParamRef::ofLayer: keys in graphic time, so a graphic with
// responsive time maps its design time to the lane); the rest are edits of the layer. Every change is one undo step.
// 0.6: selected keys can be copied (Ctrl+C / Ctrl+X) and pasted at the playhead (Ctrl+V) onto the clip shown, to the
// same parameters. With several clips selected ("multi" mode, Premiere's multi-clip Properties) the panel shows the
// Transform / Opacity and Audio rows of all of them: a value differing between them shows as mixed, and a change sets
// every selected clip of that kind in one undo step (a key at the playhead where the parameter is animated).

#include "BridgeIds.h"
#include "KeyframeGraph.h"
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

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace wabcut {
struct GraphicLayer;
}

namespace wabcut::app {

class ProjectController;

/// UX rework: whether the Effects panel lists an effect (its own bins, Favorites, custom bins): not intrinsic effects
/// (every clip's), not Title Style (it comes with each title) and not the Essential Sound record (it does nothing to the
/// sound; the Essential Sound panel adds it).
bool isListedEffect(const EffectDescriptor& descriptor);
/// The Effects panel's icon of an effect or transition: its category's, the same in its own bin, Favorites and custom
/// bins ("sliders-horizontal" for an audio plug-in).
QString effectLibraryIcon(const EffectDescriptor& descriptor);

/// 0.7 (card 044): what the Dynamics Processing graph reads, for Effect Controls and the mixer's effect settings alike.
/// The live levels of the effect instance `instance` (its id value): { active (audio went through it since the last look),
/// inputDb, outputDb, reduction: [dB x4] }, the peaks since the last look (this takes them). `seenBlocks` is the caller's
/// memory of the processor's block counter for this instance.
QVariantMap dynamicsMeterReading(std::uint64_t instance, std::uint64_t& seenBlocks);
/// The static transfer curve of a band: `points` output levels (dB) for inputs spread from `fromDb` to `toDb`.
QVariantList dynamicsCurvePoints(int mode, double threshold, double ratio, double range, double knee, double makeup, double fromDb,
                                 double toDb, int points);

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
    /// The keyframe lane covers the clip: the first and last frame it shows (the clip's unless zoomed) as timecode.
    Q_PROPERTY(QString laneStartText READ laneStartText NOTIFY laneChanged FINAL)
    Q_PROPERTY(QString laneEndText READ laneEndText NOTIFY laneChanged FINAL)
    /// 0.5.1: the part of the clip the lane shows, 0..1 of the clip (0 and 1: all of it), and whether that is less.
    Q_PROPERTY(double viewStart READ viewStart NOTIFY laneChanged FINAL)
    Q_PROPERTY(double viewEnd READ viewEnd NOTIFY laneChanged FINAL)
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY laneChanged FINAL)
    /// The playhead in the lane, 0..1; -1 when it is outside the clip.
    Q_PROPERTY(double playheadPosition READ playheadPosition NOTIFY laneChanged FINAL)
    Q_PROPERTY(int selectedKeyCount READ selectedKeyCount NOTIFY keySelectionChanged FINAL)
    /// The interpolation every selected key shares ("linear", "hold", "bezier", "easeIn", "easeOut", "easeInOut"),
    /// or "" when they differ or none is selected.
    Q_PROPERTY(QString selectedKeyInterpolation READ selectedKeyInterpolation NOTIFY keySelectionChanged FINAL)
    /// 0.7 (card 120): where every selected key is pinned ("start", "end", "none" for unpinned), or "" when they differ or
    /// none is selected.
    Q_PROPERTY(QString selectedKeyPin READ selectedKeyPin NOTIFY keySelectionChanged FINAL)
    /// A one-line message about the last edit ("Shortened to the available media"), or "".
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged FINAL)
    /// 0.4: the selected section headers ("motion", "opacity", "audio", "e12"), in panel order.
    Q_PROPERTY(QStringList selectedSections READ selectedSections NOTIFY sectionSelectionChanged FINAL)
    /// 0.4: copied effects wait to be pasted (no clip was copied since): Paste puts them on the selected clips.
    Q_PROPERTY(bool canPasteEffects READ canPasteEffects NOTIFY effectsClipboardChanged FINAL)
    /// 0.6: copied keyframes wait to be pasted (the newest copy: no clip or effect was copied since).
    Q_PROPERTY(bool canPasteKeys READ canPasteKeys NOTIFY keyClipboardChanged FINAL)
    /// 0.6, "multi" mode: how many clips the rows edit (0 otherwise).
    Q_PROPERTY(int multiCount READ multiCount NOTIFY contextChanged FINAL)
    /// 0.7 (card 96): the Graph Editor panel's engine, a second view of the same keyframes and key selection.
    Q_PROPERTY(wabcut::app::KeyframeGraph* graph READ graph CONSTANT FINAL)

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
        SectionSelectedRole, // 0.4, headers: the section is selected (selectedSections)
        MixedRole,          // 0.6, multi mode: the selected clips differ (the value shown is the first clip's)
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
    double viewStart() const { return m_viewStart; }
    double viewEnd() const { return m_viewEnd; }
    bool zoomed() const { return m_viewEnd - m_viewStart < 1.0 - 1e-9; }
    int selectedKeyCount() const { return static_cast<int>(m_selectedKeys.size()); }
    QString selectedKeyInterpolation() const;
    QString selectedKeyPin() const;
    QString notice() const { return m_notice; }
    QStringList selectedSections() const { return m_selectedSections; }
    bool canPasteEffects() const { return effectsPastePending(); }
    bool canPasteKeys() const { return m_keyClipboard != nullptr && m_keysPending; }
    int multiCount() const { return m_multiCount; }
    KeyframeGraph* graph() const { return m_graph; }

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
    /// 0.7 (card #136): steps the Blend Mode of the selected video clips (else the clip shown) to the next (+1) or
    /// previous (-1) mode of Premiere's menu order, wrapping at both ends: one undo step "Blend Mode". The keymap's
    /// effectControls.nextBlendMode / previousBlendMode. False when there is no video clip to step.
    Q_INVOKABLE bool stepBlendMode(int step);
    /// The stopwatch: on turns the value into a curve with a key at the playhead; off deletes every key and keeps
    /// the value at the playhead.
    Q_INVOKABLE void setAnimated(const QString& row, bool animated);
    /// Adds a key at the playhead, or removes the one there (the diamond button between the arrows).
    Q_INVOKABLE void toggleKeyAtPlayhead(const QString& row);
    /// Moves the playhead to the previous (-1) or next (+1) key of the row.
    Q_INVOKABLE void goToKey(const QString& row, int direction);
    /// A text row (0.2: Simple Text's text, the effect's data "text"): sets it; empty restores the default. 0.6: also a
    /// Source Text row ("textarea": a text layer's text at the playhead, a title's text) and a Font row ("font": the
    /// family).
    Q_INVOKABLE void setEffectText(const QString& row, const QString& text);

    // ---- Text sections (0.6) ----
    /// A colour row ("color"): "#RRGGBB". A picker drag merges into one undo step until endEdit().
    Q_INVOKABLE void setColor(const QString& row, const QString& color);
    /// The installed font families (the Font row's picker).
    Q_INVOKABLE QStringList fontFamilies() const;
    /// A row's button: "addStroke" (the Strokes group), "remove" (a stroke's colour row).
    Q_INVOKABLE void rowAction(const QString& row, const QString& action);

    // ---- Sections ----
    Q_INVOKABLE void setCollapsed(const QString& section, bool collapsed);
    Q_INVOKABLE void setEffectEnabled(const QString& section, bool enabled);
    /// Removes a stack effect, or the selected transition.
    Q_INVOKABLE void removeSection(const QString& section);
    /// Resets every parameter of the section to its default.
    Q_INVOKABLE void resetSection(const QString& section);
    /// Moves a stack effect to `stackIndex` in its clip's stack.
    Q_INVOKABLE void moveEffect(const QString& section, int stackIndex);

    // ---- Section selection and the effects clipboard (0.4) ----
    /// A click on a section header (clip mode; not a transition): selects it alone, Ctrl/Cmd toggles it, Shift selects
    /// from the anchor to it in panel order. Clears the selected keyframes.
    Q_INVOKABLE void selectSection(const QString& section, int modifiers);
    /// Every section of the shown clip(s) (Ctrl+A with Effect Controls focused).
    Q_INVOKABLE void selectAllSections();
    Q_INVOKABLE void clearSectionSelection();
    /// Removes the selected stack effects and resets the selected fixed sections (Transform, Opacity, Audio) to their
    /// defaults (keys deleted): one undo step ("Remove Effect", "Remove Effects" or "Reset Effect"). Returns the count.
    Q_INVOKABLE int removeSelectedSections();
    /// Puts the selected sections on the effects clipboard (`cut`: also removes them, one undo step "Cut Effects").
    /// Returns how many sections were copied (0: nothing selected).
    Q_INVOKABLE int copySelectedSections(bool cut = false);
    /// Pastes the effects clipboard onto every selected clip (video part on video clips, audio part on audio clips; a
    /// clip of the other kind stands for its linked partner): one undo step "Paste Effects". Returns "" or why not
    /// (the toast text is ProjectController::clipboardNotice either way).
    Q_INVOKABLE QString pasteEffects();
    /// Whether the stack effect `section` can move by `delta` (-1 up, +1 down) in its stack.
    Q_INVOKABLE bool canMoveSection(const QString& section, int delta) const;
    Q_INVOKABLE void moveSectionBy(const QString& section, int delta);
    /// The keymap commands Effect Controls takes while it has the focus (Main.qml's hook): true when it handled
    /// `commandId` (effectControls.removeSelectedEffect always; edit.clear / edit.copy / edit.cut / edit.deselectAll
    /// with sections selected; edit.selectAll in clip mode), false to let the command fall through.
    Q_INVOKABLE bool runCommand(const QString& commandId);
    /// The commands runCommand() always takes (the palette): effectControls.removeSelectedEffect.
    static bool handlesCommand(const QString& id);
    /// C++ (ProjectController's edit.paste): pastes the effects clipboard when it is pending; false when not.
    bool pasteEffectsIfPending();
    /// The effects clipboard is not empty and no clip was copied after it.
    bool effectsPastePending() const;

    // ---- Adding effects ----
    /// The effect library for the Effects panel, grouped: [{ title, icon, kind, key, count, items: [{ id, name,
    /// category, description, icon, kind }], bins: [{ title, key, count, items }] }], filtered by a fuzzy `query`.
    /// `items` are by relevance to the query (else by bin and name); `bins` are Premiere's bins (the categories),
    /// alphabetical, each with its effects in alphabetical order (0.2). UX rework: `key` is stable and untranslated
    /// (a group's is its kind, a bin's "<kind>/<English category>"); `count` is the number of items.
    Q_INVOKABLE QVariantList library(const QString& query = {}) const;
    /// Adds an effect to the selected clip (to its partner of the effect's kind); `stackIndex` -1 appends.
    /// Returns "" or a message saying why not.
    Q_INVOKABLE QString addEffect(const QString& effectId, int stackIndex = -1);
    /// UX rework (the Effects panel's double-click / Enter): applies an Effects panel item to EVERY selected clip as one
    /// undo step. `itemId`: an effect id (or "effect:<id>"), "preset:<name>", or a transition id. Each selected clip of
    /// the item's kind gets it; a selected clip of the other kind stands for its linked partner of that kind (as
    /// addEffect does for the primary clip). A transition goes on both ends of each selected clip of its kind (Apply
    /// Default Transitions to Selection with this transition, at the default length). Plug-ins: PluginBridge.
    /// Returns { ok, error ("" or why not), message (the status line's text: the result or the error), clips (how many
    /// clips got it) }.
    Q_INVOKABLE QVariantMap applyToSelectedClips(const QString& itemId);
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
    /// 0.7 (card 120): pins the selected keys to their clip's "start" or "end" (each moves there, keeping its value, and
    /// follows that end through trims and slips), or "none" unpins them; one undo step "Pin Keyframes". Clip keys only.
    Q_INVOKABLE void setKeyPin(const QString& pin);
    Q_INVOKABLE void deleteSelectedKeys();
    /// 0.6: puts the selected keys on the keyframe clipboard (`cut`: also deletes them, one undo step "Cut Keyframes"):
    /// each with its parameter, its time after the earliest copied key, value, interpolation and handles. Returns how
    /// many keys were copied.
    Q_INVOKABLE int copySelectedKeys(bool cut = false);
    /// 0.6: pastes the keyframe clipboard at the playhead onto the clip shown (the same parameters; a key at the same
    /// time is replaced): one undo step "Paste Keyframes". Returns "" or why not (also the notice).
    Q_INVOKABLE QString pasteKeys();
    /// Lane ruler scrub: the playhead to `x` of the clip.
    Q_INVOKABLE void seekLane(double x);

    // ---- Lane zoom (0.5.1; Ctrl/Alt+wheel, Shift+wheel and the zoom bar) ----
    /// Zooms the lane by `factor` (> 1 in, < 1 out), the time at `at` (0..1 across the shown lane: the pointer) staying
    /// where it is. Five frames fill the lane at the most; the whole clip at the least.
    Q_INVOKABLE void zoomLaneAt(double factor, double at);
    /// Pans a zoomed lane by `dx` of the clip (it stays inside the clip).
    Q_INVOKABLE void panLane(double dx);
    /// Shows [start, end] of the clip: kept inside it (its width kept when it runs past an end) and to the zoom limits.
    Q_INVOKABLE void setLaneView(double start, double end);
    /// The whole clip again.
    Q_INVOKABLE void resetLaneZoom();

    // ---- Value graph and Bezier handles (M5g) ----
    /// Opens or closes the value graph of a parameter row (animated parameters only).
    Q_INVOKABLE void setGraphOpen(const QString& row, bool open);
    /// A press at (x, y) in a row's graph (0..1 each, y down): starts dragging the Bezier handle within the
    /// tolerances. True when one was hit.
    Q_INVOKABLE bool pressHandle(const QString& row, double x, double y, double toleranceX, double toleranceY);
    /// Drags the handle to (x, y) of the graph: that side's influence (how far along the segment) and slope. The
    /// key's other handle follows with the same slope unless `broken` (Alt). 0.7 (card #79): `flat` (Shift) pins the
    /// handle level with its key (slope 0, a flat ease) and only its length follows the pointer; letting go of Shift
    /// mid-drag frees the height again. One undo step per drag.
    Q_INVOKABLE void dragHandle(double x, double y, bool broken, bool flat = false);
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

    // ---- 0.7 (card #57): pinned favourites ----
    /// Every Favorites effect (the Effects panel's Add to Favorites; video effects on a video clip, audio effects on an
    /// audio one) that the clip does not have yet shows at the end of its stack as a header ready to use: section
    /// "pin:<effect id>", sectionKind "pinned", with the effect's name and an Add button. addPinned() puts that effect on
    /// the shown clip (the Effects panel's double-click: one undo step), and the real section takes its place.
    /// Returns "" or why not.
    Q_INVOKABLE QString addPinned(const QString& section);
    /// The effect id a section stands for: "pin:<id>" or the effect of a stack section ("e12"); "" for others.
    Q_INVOKABLE QString effectIdOfSection(const QString& section) const;
    /// Rebuilds the rows after the Favorites changed (the Effects panel, the section menu's Pin).
    Q_INVOKABLE void refreshPinned();

    // ---- 0.7 (card 044): Dynamics Processing ----
    /// Its section shows a graph row (kind "dynamics", key "<section>:graph", the controls in `graph`) above the rows of the
    /// bands in use. The live levels of the section's effect: { active (audio went through it since the last look), inputDb,
    /// outputDb, reduction: [dB x4] } as the peaks since the last look (a call takes them).
    Q_INVOKABLE QVariantMap dynamicsMeters(const QString& section);
    /// The static transfer curve of a band: `points` output levels (dB) for inputs spread from `fromDb` to `toDb`.
    Q_INVOKABLE QVariantList dynamicsCurve(int mode, double threshold, double ratio, double range, double knee, double makeup,
                                           double fromDb, double toDb, int points) const;
    /// The presets of the bands (names as shown) and applying one to a Dynamics Processing section: its crossovers, bands,
    /// limiter and Auto Gate (Input, Output and Mix stay), one undo step "Dynamics Processing: <preset>". False when the
    /// section is not a Dynamics Processing effect or there is no such preset.
    Q_INVOKABLE QStringList dynamicsPresets() const;
    Q_INVOKABLE bool applyDynamicsPreset(const QString& section, const QString& name);

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
    void sectionSelectionChanged();
    void effectsClipboardChanged();
    void keyClipboardChanged();

private:
    friend class KeyframeGraph; // 0.7: the Graph Editor edits through the same rows, selection and key drags
    struct Row {
        QString key, kind, section, sectionKind, title, icon, unit, text, description, stackKind;
        double from = 0, to = 100, step = 1, decimals = 0, origin = 0, value = 0, defaultValue = 0;
        bool animatable = false, animated = false, keyAtPlayhead = false, hasPrev = false, hasNext = false;
        bool effectEnabled = true, collapsed = false, removable = false, movable = false, resettable = false;
        bool known = true;
        bool graphOpen = false;
        bool sectionSelected = false;
        bool mixed = false;
        int stackIndex = -1;
        QVariantList keys, options;
        QVariantMap graph;
        // C++ only
        ClipId clip;
        edit::ParamRef ref;
        UiRange range;
        PropertyFrame frame;
        std::string undoLabel;
        /// 0.6: what a text row edits ("align", "fill", "stroke1.position", "titleText" ...; "" for others) and the
        /// graphic layer it belongs to (0: none).
        QString textProp;
        std::uint32_t layer = 0;

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
    // ---- 0.6: text sections ----
    void addTextSections(std::vector<Row>& rows, const Clip& clip);
    void addTitleRows(std::vector<Row>& rows, const Clip& clip, const QString& section);
    /// The curve time of a row's keys at timeline time `t`: graphic time for a layer of a graphic with responsive time
    /// (GraphicResolve.h graphicTimeAt), else the clip's curve time; and back (nullopt: not inside the clip).
    Ticks rowCurveTime(const Row& row, const Clip& clip, Ticks t) const;
    Ticks rowCurveTimeAtPlayhead(const Row& row, const Clip& clip) const;
    std::optional<Ticks> rowTimelineTime(const Row& row, const Clip& clip, Ticks curveTime) const;
    /// 0.7 (cards #98, #136): a clip's own flag row of Transform / Opacity (Nearest Neighbor Scaling, Blend Mode): one undo step.
    void setClipFlag(const Row& row, double value);
    /// 0.7 (card #57): the pinned headers of a clip, and the Favorites effects (cached per library revision).
    void addPinnedSections(std::vector<Row>& rows, const Clip& clip, bool video);
    /// 0.7 (card 044): the graph row of a Dynamics Processing section.
    void addDynamicsGraph(std::vector<Row>& rows, const Clip& clip, const EffectInstance& instance, const QString& section,
                          bool enabled);
    const std::vector<std::string>& pinnedEffects();
    /// Applies `edit` to a copy of a text row's layer (at the playhead's graphic time) as one undo step.
    bool editTextLayer(const Row& row, const char* label, const std::function<bool(GraphicLayer&, Ticks)>& edit,
                       const std::string& mergeKey = {});
    /// A text row that is not a keyframable number: a switch, a choice or a plain number.
    void setTextProperty(const Row& row, double value, const std::string& mergeKey);
    /// A keyframable number of a graphic layer: the constant, or the key at the playhead's graphic time.
    void setLayerParamAtPlayhead(const Row& row, double model, const std::string& mergeKey);
    // ---- 0.6: multi-clip editing ----
    void addMultiRows(std::vector<Row>& rows);
    /// Sets a multi-mode row (UI units) on every selected clip of its kind: one undo step, merged while dragging.
    void setMultiValue(const Row& row, double uiValue);
    /// Deletes the selected keys as one undo step named `label`.
    void deleteKeys(const char* label);
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
    /// 0.7 (KeyframeGraph): a parameter row's value as its clip plays it, and several edits as ONE undo step (merged with the
    /// edits before it that share `mergeKey`).
    ParamValue rowValue(const Row& row) const;
    bool applyChainedEdit(const char* label, const std::vector<std::function<edit::EditOutcome(const ProjectState&)>>& steps,
                          std::string_view mergeKey = {});

    QPointer<ProjectController> m_controller;
    std::vector<Row> m_rows;
    QString m_mode, m_title, m_subtitle, m_trackName;
    int m_colorIndex = 0;
    QString m_laneStartText, m_laneEndText;
    double m_playheadPosition = -1;
    TimeRange m_lane;
    /// 0.5.1: the lane's shown part (0..1 of the clip), the clip it belongs to (another one resets it), and whether
    /// laneChanged is owed for a new view.
    double m_viewStart = 0, m_viewEnd = 1;
    ClipId m_viewClip;
    bool m_viewChanged = false;
    /// The narrowest view: five frames of the lane's clip (1: the clip is that short, or there is no lane).
    double minLaneSpan() const;
    std::set<QString> m_collapsed;
    std::set<KeyRef> m_selectedKeys;
    std::set<KeyRef> m_shownKeys; ///< keys the rows show (rebuilt with them)
    QString m_notice;
    bool m_refreshing = false;
    // 0.7 (card #57): the Favorites effects as of the library revision they were read at.
    std::vector<std::string> m_pinned;
    std::uint64_t m_pinnedRevision = 0;
    bool m_pinnedLoaded = false;
    /// 0.7 (card 044): the block counter of each Dynamics Processing meter at the last look (is audio going through it?).
    std::map<std::uint64_t, std::uint64_t> m_meterBlocks;

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

    KeyframeGraph* m_graph = nullptr; ///< 0.7: the Graph Editor
    /// 0.7: while the Graph Editor is open and a section is collapsed, the rows as they are with every section open (the
    /// editor lists the curves of collapsed sections too; findRow() also finds them); empty otherwise.
    std::vector<Row> m_uncollapsedRows;
    std::set<QString> m_graphRows; ///< rows whose value graph is open
    struct HandleDrag {
        bool active = false;
        QString row;
        std::size_t index = 0; ///< the key in the curve
        bool out = true;       ///< its leaving side, else its arriving side
        double min = 0, max = 1; ///< the graph's value range, frozen while dragging (UI units)
        std::string mergeKey;
    } m_handleDrag;

    // ---- 0.4: section selection and the effects clipboard ----
    /// The sections of the shown clip(s) in panel order (headers that can be selected: not a transition).
    QStringList selectableSections() const;
    /// Sets the selection (kept in panel order) and its anchor; clears the selected keys when sections are selected.
    void setSectionSelection(QStringList sections, const QString& anchor);
    /// The edit steps that remove / reset `sections` (stack effects removed, fixed sections reset from the registry).
    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> removeSteps(const QStringList& sections,
                                                                                  int* removed, int* reset) const;
    /// Snapshot of the selected sections for the effects clipboard (null when nothing can be copied).
    struct EffectsClipboard;
    std::shared_ptr<const EffectsClipboard> snapshotSections() const;
    void setEffectsPending(bool pending);

    std::shared_ptr<const EffectsClipboard> m_effects;
    bool m_effectsPending = false;

    // ---- 0.6: the keyframe clipboard and multi mode ----
    struct KeyClipboard;
    std::shared_ptr<const KeyClipboard> m_keyClipboard;
    bool m_keysPending = false;
    void setKeysPending(bool pending);
    int m_multiCount = 0;
    /// The clips multi mode edits: each selected clip with its linked partners (selection order).
    std::vector<std::vector<ClipId>> m_multiUnits;
    QStringList m_selectedSections;
    QString m_sectionAnchor;
    ClipId m_sectionsOf; ///< the primary clip the selection belongs to (another clip shown clears it)
};

} // namespace wabcut::app
