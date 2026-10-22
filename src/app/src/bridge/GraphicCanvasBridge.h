#pragma once

// GraphicCanvasBridge: on-canvas handles for motion graphics in the Program Monitor (singleton `GraphicCanvas`, C.13 /
// M5o; docs/internal/c13-graphics-handoff.md, docs/graphics.md).
//
//   * The selected graphic clip (the primary selection when it is a graphic, or set by the Essential Graphics panel) is
//     resolved at the playhead OFF the GUI thread (resolveGraphicClip, what graphic.resolve returns, on a snapshot):
//     every shown layer's matrix to the design canvas and its content box. The design canvas sits in the clip's frame
//     (contain, centred), the frame on the sequence canvas through the clip's Motion; QML gets everything in fractions
//     of the sequence canvas, like the mask overlay.
//   * Handles for `selectedLayer`: move (inside), scale (corners; uniform unless Shift), rotate (beyond the top),
//     anchor point, and for paragraph text the box (right / left edge by alignment, bottom, corner) instead of scale.
//     A drag is one undo step (graphic.updateLayer's edit, merged per gesture); an animated value gets a key at the
//     playhead's graphic time. Moves snap to the canvas edges and centre, the action / title safe margins and the
//     other layers' edges and centres (`guides` while dragging); Ctrl disables snapping.
//   * layerAt() hit-tests the topmost layer under a point; nudge() moves the selected layer with the keyboard;
//     setLayerText() writes a text layer's text (inline editing; one run in the style of its first run).
//   * A pinned layer (Responsive Design - Position) follows its target: its handles are shown but locked.
//   * Picking (0.1.3, the Program Monitor's selection tool, Premiere-like). stackAt() lists what is under a point of
//     the picture at the playhead, top first: the layers of every graphic clip there (not only the selected one),
//     title clips (the box of their text) and the other video clips (their frame through Motion and Crop; a nested
//     clip as one). It reads the frame as the program monitor composites it (playback::evaluateVideoLayers), on a
//     worker after edits and playhead moves settle; a click on a stale frame computes it on the spot.
//     - pickPress(): a click selects the topmost object under the pointer (the timeline selection; a graphic layer
//       also becomes `selectedLayer`), unless the object picked last in the monitor is under the pointer: it stays
//       picked, so an object behind another can be dragged once picked. A timeline selection does not count as
//       picked (a selected full-frame clip would otherwise keep every click).
//     - pickDoubleClick(): one level deeper each time. With the picked object (before the double-click's first
//       press) not under the pointer, it picks the topmost and edits it if it is text (a graphic's text layer or a
//       title: QML opens the inline editor). With a text object picked and not yet edited in this cycle, it edits
//       it; otherwise it picks the next object below (wrapping to the top), without editing it. Picking anything by
//       other means (the timeline, Tab, Esc) starts a new cycle.
//     - pickOutline: the box of the clip or title picked in the monitor while it stays the selection (a graphic
//       layer has its own handles).
//     - setTitleText() writes a title's text (every clip of the title shows it), the title's inline editor.

#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

class QJSEngine;
class QQmlEngine;
class QTimer;

namespace wabcut::app {

class ProjectController;

class GraphicCanvasBridge : public QObject {
    Q_OBJECT
    /// The graphic clip the handles are for ("c42"; "" none). Follows the primary selection when it is a graphic clip;
    /// the Essential Graphics panel may set it.
    Q_PROPERTY(QString selectedClip READ selectedClip WRITE setSelectedClip NOTIFY selectedClipChanged FINAL)
    /// The layer the handles are on (0 none).
    Q_PROPERTY(int selectedLayer READ selectedLayer WRITE setSelectedLayer NOTIFY selectedLayerChanged FINAL)
    /// A graphic clip is selected, shown at the playhead and resolved: the overlay is live.
    Q_PROPERTY(bool active READ active NOTIFY stateChanged FINAL)
    /// The shown layers bottom to top: [{ id, parent, kind, name, quad: [x0, y0, ... x3, y3] (fractions), pinned,
    /// text }].
    Q_PROPERTY(QVariantList layers READ layers NOTIFY stateChanged FINAL)
    /// The selected layer: { id, kind, name, quad, anchor: {x, y}, handles: [{ kind, x, y }], pinned, paragraph,
    /// editor: { x, y, width, height, fontFamily, fontSize (fraction of the canvas height), fontWeight, italic, color,
    /// align, text, language, rotation } } or {} when none is shown.
    Q_PROPERTY(QVariantMap selection READ selection NOTIFY stateChanged FINAL)
    /// While a move snaps: [{ orientation: "vertical" | "horizontal", position (fraction) }].
    Q_PROPERTY(QVariantList guides READ guides NOTIFY guidesChanged FINAL)
    /// The design canvas in fractions of the sequence canvas: { x, y, width, height }.
    Q_PROPERTY(QVariantMap canvasRect READ canvasRect NOTIFY stateChanged FINAL)
    /// The safe margins snapped to, per side as fractions of the frame (the overlay passes Theme's).
    Q_PROPERTY(double actionSafe READ actionSafe WRITE setActionSafe NOTIFY marginsChanged FINAL)
    Q_PROPERTY(double titleSafe READ titleSafe WRITE setTitleSafe NOTIFY marginsChanged FINAL)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// The clip or title picked in the monitor, while it is the selection: its box [x0, y0, ... x3, y3] (fractions);
    /// [] otherwise.
    Q_PROPERTY(QVariantList pickOutline READ pickOutline NOTIFY pickChanged FINAL)
    /// Diagnostics (tests): resolves finished, and whether the last one ran on a worker thread.
    Q_PROPERTY(int resolveCount READ resolveCount NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool lastResolveOffGuiThread READ lastResolveOffGuiThread NOTIFY stateChanged FINAL)

public:
    explicit GraphicCanvasBridge(QObject* parent = nullptr);
    ~GraphicCanvasBridge() override;
    static GraphicCanvasBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    /// The text measurer resolves use (null = the model's approximation). Must outlive the resolves.
    void setMeasurer(std::shared_ptr<const TextMeasurer> measurer);

    QString selectedClip() const;
    void setSelectedClip(const QString& clip);
    int selectedLayer() const { return static_cast<int>(m_selectedLayer); }
    void setSelectedLayer(int layer);
    bool active() const { return m_active; }
    QVariantList layers() const { return m_layersJs; }
    QVariantMap selection() const { return m_selectionJs; }
    QVariantList guides() const { return m_guidesJs; }
    QVariantMap canvasRect() const;
    double actionSafe() const { return m_actionSafe; }
    void setActionSafe(double inset);
    double titleSafe() const { return m_titleSafe; }
    void setTitleSafe(double inset);
    bool dragging() const { return m_drag.has_value(); }
    QString message() const { return m_message; }
    int resolveCount() const { return m_resolveCount; }
    bool lastResolveOffGuiThread() const { return m_lastOffGuiThread; }

    /// Resolves again (also on every edit, selection and playhead change).
    Q_INVOKABLE void refresh();
    /// The topmost shown layer (not a group) under a point of the sequence canvas (fractions); 0 = none.
    Q_INVOKABLE int layerAt(double fx, double fy) const;
    /// Starts a gesture on the selected layer: "move", "scale-tl" | "scale-tr" | "scale-br" | "scale-bl", "rotate",
    /// "anchor", "box-right" | "box-left" | "box-bottom" | "box-corner". False when it cannot (a pinned layer, none).
    Q_INVOKABLE bool beginDrag(const QString& kind, double fx, double fy);
    /// The pointer moved: applies the gesture (one undo step for the whole drag). `snap`: the snapping reach as a
    /// fraction of the canvas width (0 = none).
    Q_INVOKABLE void dragTo(double fx, double fy, bool shift, bool ctrl, double snap);
    Q_INVOKABLE void endDrag();
    /// Esc: undoes what the gesture changed.
    Q_INVOKABLE void cancelDrag();
    /// Moves the selected layer by design-canvas pixels (the keyboard: arrows, Shift = 10 px); one undo step.
    Q_INVOKABLE bool nudge(double dx, double dy);
    /// Selects the next (+1) or previous (-1) shown layer, wrapping (Tab in the overlay).
    Q_INVOKABLE void selectAdjacent(int direction);
    /// Writes a text layer's text (one run in the style of its first run); one undo step. "" or the error.
    Q_INVOKABLE QString setLayerText(int layer, const QString& text);

    // ---- Picking (see the file comment). Objects are { key ("c42", or "c42/7" for layer 7 of a graphic clip),
    // clip, layer (0 = the clip), kind ("text" | "shape" | "image" for graphic layers, "title", "clip"), name,
    // text (editable text), quad, editor (titles: as selection.editor) }. ----
    /// What is under a point of the picture (fractions), top first.
    Q_INVOKABLE QVariantList stackAt(double fx, double fy);
    /// What a click there would pick ({} when nothing is under it); changes nothing.
    Q_INVOKABLE QVariantMap pickTarget(double fx, double fy);
    /// A click (each press of a double-click too): picks the target and returns it ({} = nothing under the pointer;
    /// the selected layer is then dropped).
    Q_INVOKABLE QVariantMap pickPress(double fx, double fy);
    /// A double-click: the object picked, plus `action`: "edit" (QML opens the inline editor) or "select".
    Q_INVOKABLE QVariantMap pickDoubleClick(double fx, double fy);
    /// The box of what a click at the point would pick, from the frame already read (no work; [] when stale).
    Q_INVOKABLE QVariantList hoverAt(double fx, double fy) const;
    /// The selection as picking sees it ("c42", "c42/7", "" none).
    Q_INVOKABLE QString pickKey() const;
    /// Writes the text of the title `clip` shows (setTitleText: every clip of the title); one undo step. "" or the error.
    Q_INVOKABLE QString setTitleText(const QString& clip, const QString& text);
    QVariantList pickOutline() const { return m_pickOutlineJs; }

    /// Tests: waits for nothing; resolves on the calling thread when true.
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }

    /// One shown layer, resolved (design-canvas pixels).
    struct Layer {
        std::uint32_t id = 0;
        std::uint32_t parent = 0;
        LayerKind kind = LayerKind::Shape;
        std::string name;
        bool pinned = false;
        Affine2D toDesign;     ///< the layer's own pixels -> design canvas
        Affine2D parentToDesign; ///< its parent's space -> design canvas
        Box bounds;            ///< in its own pixels
        // Transform values at the graphic time (scale in %).
        double x = 0, y = 0, anchorX = 0, anchorY = 0, scaleX = 100, scaleY = 100, rotation = 0;
        // Text layers.
        bool paragraph = false;
        TextAlign align = TextAlign::Left;
        double boxWidth = 0, boxHeight = 0;
        std::string text;
        std::string language;
        std::string fontFamily;
        int fontWeight = 400;
        double fontSize = 72;
        bool italic = false;
        Rgba fill{1, 1, 1, 1};
        /// 0.2, type on a path: the path at the graphic time (its own pixels; empty: none).
        std::vector<MaskVertex> textPath;
        bool textPathClosed = false;
    };
    struct Resolved {
        ClipId clip;
        SequenceId sequence;
        Ticks graphicTime{};
        bool shown = false; ///< the playhead is inside the clip
        double designWidth = 1920, designHeight = 1080;
        double sequenceWidth = 1920, sequenceHeight = 1080;
        Affine2D designToSequence; ///< design canvas -> sequence canvas pixels
        std::vector<Layer> layers;  ///< bottom to top, groups before their children
    };

    /// One object of the picture a click can pick.
    struct PickObject {
        ClipId clip;
        std::uint32_t layer = 0; ///< a graphic clip's layer; 0 = the clip itself
        QString kind;            ///< "text" | "shape" | "image" (graphic layers), "title", "clip"
        QString name;
        bool text = false;           ///< editable text in place
        std::array<double, 8> quad{}; ///< fractions of the sequence canvas, clockwise from the top-left corner
        QVariantMap editor;          ///< titles: the inline editor's box and look (as selection.editor)
    };
    /// The objects of one frame, top first.
    struct PickStack {
        SequenceId sequence;
        Ticks playhead{};
        int revision = -1;
        std::vector<PickObject> objects;
    };

signals:
    void selectedClipChanged();
    void selectedLayerChanged();
    void stateChanged();
    void guidesChanged();
    void marginsChanged();
    void draggingChanged();
    void messageChanged();
    void pickChanged();

private:
    struct Gate;
    struct Drag {
        QString kind;
        std::uint32_t layer = 0;
        Layer start;           ///< the layer when the gesture began
        double px = 0, py = 0; ///< the pointer at the start (design canvas)
        bool committed = false;
        std::string mergeKey;
        double dx = 0, dy = 0; ///< the move so far (design canvas), for the guides
    };

    void scheduleResolve();
    void finishResolve(std::shared_ptr<const Resolved> resolved, quint64 generation, bool offGuiThread);
    void onSelectionChanged();
    void publish();
    void setMessage(const QString& message);
    const Layer* layer(std::uint32_t id) const;
    /// Sequence fractions <-> design canvas pixels.
    std::array<double, 2> toDesign(double fx, double fy) const;
    QVariantMap pointJs(double dx, double dy) const;
    QVariantList quadJs(const Affine2D& toDesign, const Box& bounds) const;
    /// Applies new transform / box values to the layer as one merged edit.
    bool applyLayer(const Layer& next, const char* label, const std::string& mergeKey);

    // Picking.
    /// The frame's objects now: the one read last when it is still current, else read on the spot.
    std::shared_ptr<const PickStack> stackNow();
    /// Reads the frame again on a worker once edits and playhead moves settle (the outline follows the clip).
    void schedulePickStack();
    void startPickStack();
    void finishPickStack(std::shared_ptr<const PickStack> stack, quint64 generation);
    /// The objects of `stack` under the point, top first.
    static std::vector<const PickObject*> objectsAt(const PickStack& stack, double fx, double fy);
    /// What a click picks among `under` (top first): the object picked last when it is there, else the topmost.
    const PickObject* clickTarget(const std::vector<const PickObject*>& under) const;
    /// Makes `object` the selection (the timeline's, and the layer).
    void pickObject(const PickObject& object);
    QVariantMap objectJs(const PickObject& object) const;
    /// The outline follows the picked clip; a selection made elsewhere ends the picking cycle.
    void refreshPick();

    std::shared_ptr<const PickStack> m_stack;
    QString m_pickedKey;   ///< what the monitor picked last ("" none, or the selection changed since)
    QString m_gesturePrev; ///< the picked object when the click (or double-click) began
    QString m_editedKey;   ///< the text object this cycle edited
    bool m_picking = false;
    bool m_pressValid = false; ///< the last press can be the first of a double-click
    double m_pressX = 0, m_pressY = 0;
    QElapsedTimer m_pressClock;
    QTimer* m_stackSettle = nullptr;
    quint64 m_stackGeneration = 0;
    bool m_stackRunning = false;
    bool m_stackPending = false;
    QVariantList m_pickOutlineJs;

    QPointer<ProjectController> m_controller;
    std::shared_ptr<Gate> m_gate;
    std::shared_ptr<const TextMeasurer> m_measurer;
    std::shared_ptr<const Resolved> m_resolved;
    std::optional<ClipId> m_explicitClip;
    std::uint32_t m_selectedLayer = 0;
    bool m_active = false;
    QVariantList m_layersJs;
    QVariantMap m_selectionJs;
    QVariantList m_guidesJs;
    double m_actionSafe = 0.05;
    double m_titleSafe = 0.1;
    std::optional<Drag> m_drag;
    QString m_message;
    quint64 m_generation = 0; ///< resolves started (the newest result wins)
    quint64 m_gestures = 0;   ///< drags started (each its own undo step)
    bool m_running = false;
    bool m_pending = false;
    bool m_synchronous = false;
    int m_resolveCount = 0;
    bool m_lastOffGuiThread = false;
};

struct GraphicCanvasSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::GraphicCanvasBridge)
    QML_NAMED_ELEMENT(GraphicCanvas)
    QML_SINGLETON
public:
    static GraphicCanvasBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return GraphicCanvasBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
