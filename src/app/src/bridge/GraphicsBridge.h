#pragma once

// GraphicsBridge (M5o, QML singleton `Graphics`): the Essential Graphics panel over C.13's motion graphics
// (docs/graphics.md, docs/internal/c13-graphics-handoff.md "Notes for the UI engineer").
//
//   * Browse: `templates` (the built-in ones, the project's library and the user's folder), with live thumbnails at
//     "image://graphicthumb/<library>/<id>/<version>" - the template's graphic resolved and painted off the GUI thread
//     (GraphicPainter, a QQuickAsyncImageProvider on a QThreadPool of its own), cached. instantiate() /
//     instantiateAtPlayhead() run template.instantiate (a drop on the timeline, double-click / Enter) and select the new
//     graphic's first text layer, ready to type; saveTemplate() runs template.save with the fields exposeCandidates()
//     offers.
//   * Built-in templates (C.16: the gaming ones, shipped in the model): template.list's "builtin" entries, with their
//     category and poster time, listed as library "builtin", read-only; their thumbnails are template.resolve's draw
//     lists (or, without it, the graphic template.list gives for one id) at the poster time. None until the catalog has
//     them. template.instantiate finds a built-in after the project's and the user's libraries.
//   * Edit: the primary selected clip when it is a graphic (`clip`): `graphic` (name, canvas, responsive time, template
//     instance), `fields` (a template instance's, edited with setField: one undo step per gesture), `layers` (the tree
//     flat, top first, with depths), `selectedLayer` (shared with the Program Monitor's on-canvas handles) and `layer`
//     (its properties at the playhead). Edits go through GraphicOps.h with ProjectController::applyEditFn: one undo
//     step each, a drag merged into one (setValue / setRun / setColor ... until endEdit()).
//   * Animatable numbers (transform.*, shape.width/height/cornerRadius/outerRadius/innerRadius/trimStart/trimEnd/
//     trimOffset, stroke.width): `layer` shows them as { value, animated, keyed } at the playhead's graphic time;
//     setValue() changes the constant or the key there (adding one), toggleKey() / setAnimated() like Effect Controls.
//   * Presets (graphic.applyPreset), Responsive Design: pins (setPin: the offsets keep the layer where it is) and time.
//   * runCommand(): the keymap's graphics.* commands (New Layer: Text = Ctrl+T, Bring Forward, Select Next Layer...).
//
// Keys: clips "c12", media "m4", template keys "project/<id>", "user/<id>" or "builtin/<id>". Times in seconds.
// Colours "#RRGGBB" or "#RRGGBBAA", or "brand:<name>" for a brand kit colour.

#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

class QJSEngine;
class QQmlEngine;
class QThread;

namespace wabcut::command {
class CommandRegistry;
class CommandServices;
class GraphicsServices;
}

namespace wabcut::app {

class ProjectController;

class GraphicsBridge : public QObject {
    Q_OBJECT
    /// [{ key ("project/lower-third"), id, library ("builtin" | "project" | "user"), category (built-in ones: "gaming"),
    ///   name, description, version, width, height, seconds, fields: [{ id, label, kind }], thumbnail
    ///   ("image://graphicthumb/...") }], built-in first.
    Q_PROPERTY(QVariantList templates READ templates NOTIFY templatesChanged FINAL)
    /// Template files of the user's folder that do not read: [{ file, message }].
    Q_PROPERTY(QVariantList libraryErrors READ libraryErrors NOTIFY templatesChanged FINAL)
    /// The user's template folder ("" when there is none).
    Q_PROPERTY(QString userLibrary READ userLibrary NOTIFY templatesChanged FINAL)
    /// The graphic clip the Edit tab shows ("" when the primary selected clip is not a graphic).
    Q_PROPERTY(QString clip READ clip NOTIFY graphicChanged FINAL)
    /// { name, width, height, responsive: { duration, intro, outro } (seconds) or null, template: { id, version } or
    ///   null, seconds (the clip's length) }.
    Q_PROPERTY(QVariantMap graphic READ graphic NOTIFY graphicChanged FINAL)
    /// A template instance's fields: [{ id, label, kind, layer, property, value }] (text, "#RRGGBB" / "brand:Name",
    /// font family, media key, seconds, true / false).
    Q_PROPERTY(QVariantList fields READ fields NOTIFY graphicChanged FINAL)
    /// The layer tree, top first: [{ id, parent, depth, kind, name, visible, text (text layers), children, index,
    ///   siblings, pinned }].
    Q_PROPERTY(QVariantList layers READ layers NOTIFY graphicChanged FINAL)
    /// The selected layer's id (0: none); the canvas handles select through it too.
    Q_PROPERTY(int selectedLayer READ selectedLayer WRITE setSelectedLayer NOTIFY selectedLayerChanged FINAL)
    /// 0.2: every selected layer, the selected one first, then the ones Ctrl-clicked into the selection (Align and
    /// Distribute work on them). Setting selectedLayer selects that one alone.
    Q_PROPERTY(QVariantList selectedLayers READ selectedLayers NOTIFY selectedLayerChanged FINAL)
    /// The selected layer at the playhead (see the file comment); {} for none.
    Q_PROPERTY(QVariantMap layer READ layer NOTIFY layerChanged FINAL)
    /// The project's brand kit for the pickers: { name, colors: [{ name, color }], fonts: [{ role, family, weight }],
    ///   logos: [{ name, media }] } ({} without a kit).
    Q_PROPERTY(QVariantMap brand READ brand NOTIFY brandChanged FINAL)
    /// The project's pictures for image layers and fields: [{ id ("m4"), name }].
    Q_PROPERTY(QVariantList media READ media NOTIFY brandChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit GraphicsBridge(QObject* parent = nullptr);
    ~GraphicsBridge() override;
    static GraphicsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    /// The user's template folder (default: <app data>/templates, or WABCUT_TEMPLATE_LIBRARY). Tests use a temporary one.
    void setUserLibrary(const std::filesystem::path& folder);
    /// The commands' graphics services (the user's library and the Qt text measurer): for the assistant and other hosts.
    std::shared_ptr<command::GraphicsServices> graphicsServices() const;
    /// The command catalog the bridge runs (default: the app's editing commands; tests give a stub). Null: the default.
    void setRegistry(const command::CommandRegistry* registry);

    QVariantList templates() const { return m_templates; }
    QVariantList libraryErrors() const { return m_libraryErrors; }
    QString userLibrary() const;
    QString clip() const;
    QVariantMap graphic() const { return m_graphic; }
    QVariantList fields() const { return m_fields; }
    QVariantList layers() const { return m_layers; }
    int selectedLayer() const { return static_cast<int>(m_selectedLayer); }
    QVariantList selectedLayers() const;
    /// Ctrl-click in the layer list: adds the layer to the selection or takes it out (the selected one passes to the
    /// next when it is taken out).
    Q_INVOKABLE void toggleLayerSelected(int id);
    void setSelectedLayer(int id);
    QVariantMap layer() const { return m_layer; }
    QVariantMap brand() const { return m_brand; }
    QVariantList media() const { return m_media; }
    QString message() const { return m_message; }

    /// Reads the libraries again (the user's folder may have changed).
    Q_INVOKABLE void refreshTemplates();
    /// A template's graphic for previews and tests (nullptr when unknown). Thread-safe.
    std::shared_ptr<const GraphicTemplate> findTemplate(const QString& key) const;

    // ---- Browse ----
    /// template.instantiate at `at` (ticks, or a timecode in the sequence's rate as the timeline's timeAt() gives) on
    /// `trackKey` ("t3", overwriting there: a drop; empty: the first free video track from the targeted one upwards).
    /// Selects the new clip and its first text field's layer (else its top text layer). Returns "" or the error.
    Q_INVOKABLE QString instantiate(const QString& templateKey, const QString& trackKey, const QVariant& at);
    Q_INVOKABLE QString instantiateAtPlayhead(const QString& templateKey);
    /// What the selected graphic can expose as fields: [{ id, label, kind, layer, property, layerName }] - each text
    /// layer's text and font, the fill / stroke / background / shadow colours its layers have, image slots, each
    /// layer's out point and visibility, and the clip's length.
    Q_INVOKABLE QVariantList exposeCandidates() const;
    /// template.save of the selected graphic: `fields` from exposeCandidates() (with labels), library "project" | "user".
    /// An empty id is made from the name; the clip's length is the template's. Returns "" or the error.
    Q_INVOKABLE QString saveTemplate(const QString& id, const QString& name, const QString& description,
                                     const QString& library, const QVariantList& fields);
    /// Removes a template of the project's library (user templates: delete their file; built-in ones are refused).
    Q_INVOKABLE QString removeTemplate(const QString& templateKey);

    // ---- Edit: fields ----
    /// template.setField: a typing or a drag merges into one undo step until endEdit().
    Q_INVOKABLE QString setField(const QString& fieldId, const QVariant& value);

    // ---- Edit: layers ----
    /// A new layer on top: "text", "rectangle", "ellipse", "shape" (= rectangle), "image" or "group". Without a graphic
    /// clip selected, a new graphic clip at the playhead (Premiere's New Layer: Text). Selects the layer. `properties`
    /// (0.2): a JSON merge patch over the new layer, in the same undo step (the Type tool's { transform: { x, y } }).
    Q_INVOKABLE QString addLayer(const QString& kind, const QVariantMap& properties = {});
    Q_INVOKABLE QString removeLayer(int layerId);
    /// "forward", "backward", "front", "back" (inside the layer's group).
    Q_INVOKABLE QString moveLayer(int layerId, const QString& where);
    Q_INVOKABLE QString renameLayer(int layerId, const QString& name);
    Q_INVOKABLE QString setLayerVisible(int layerId, bool visible);
    /// A JSON merge patch over the layer (graphic.updateLayer's): e.g. { blend: "screen" }, { text: { align: "center" } }.
    /// A non-empty `merge` merges repeated edits into one undo step until endEdit().
    Q_INVOKABLE QString updateLayer(int layerId, const QVariantMap& patch, const QString& merge = {});

    // ---- Edit: properties ----
    /// An animatable number (see the file comment): the constant, or the key at the playhead. Merges until endEdit().
    Q_INVOKABLE QString setValue(int layerId, const QString& path, double value);
    /// The key at the playhead: removed when there is one, else added at the current value.
    Q_INVOKABLE QString toggleKey(int layerId, const QString& path);
    /// The stopwatch: on keys the value at the playhead; off makes it the constant it has there.
    Q_INVOKABLE QString setAnimated(int layerId, const QString& path, bool animated);
    /// A text run's style: "fontFamily", "fontStyle", "brandFont", "fontWeight", "fontSize", "fill" (a colour),
    /// "italic", "underline", "tracking", "leading", "baselineShift", or "text". Merges until endEdit().
    Q_INVOKABLE QString setRun(int layerId, int run, const QString& key, const QVariant& value);
    /// The whole text of a text layer (one run in the first run's style when it has one run or none).
    Q_INVOKABLE QString setText(int layerId, const QString& text);
    /// 0.2, type on a path: puts a text layer on a "line", "arc", "circle" or "wave" sized to the canvas, or back on
    /// lines ("none"); one undo step. Its points move on the Program Monitor (GraphicCanvas).
    Q_INVOKABLE QString setTextPath(int layerId, const QString& shape);
    /// "reverse", "perpendicular" or "forceAlignment" of a text layer on a path.
    Q_INVOKABLE QString setTextPathOption(int layerId, const QString& key, bool on);
    /// A colour: "fill" (a shape's fill or every run of a text), "stroke", "background", "shadow" or "stop:<n>" (a
    /// gradient stop). Merges until endEdit().
    Q_INVOKABLE QString setColor(int layerId, const QString& what, const QString& color);
    /// The shape's fill: "none", "solid", "linear", "radial" (gradients start from the fill colour to transparent,
    /// across the shape).
    Q_INVOKABLE QString setFillKind(int layerId, const QString& kind);
    Q_INVOKABLE QString setStopOffset(int layerId, int stop, double offset);
    Q_INVOKABLE QString addStop(int layerId);
    Q_INVOKABLE QString removeStop(int layerId, int stop);
    /// The stroke: "enabled" (bool), "dashes" ("8 4": dash, gap... pixels; "" = solid), "join", "cap".
    Q_INVOKABLE QString setStroke(int layerId, const QString& key, const QVariant& value);
    /// A mask of the layer: "mode", "inverted", "feather", "opacity", "expansion" (the project file's mask keys).
    Q_INVOKABLE QString setMask(int layerId, int mask, const QString& key, const QVariant& value);
    Q_INVOKABLE QString removeMask(int layerId, int mask);
    /// Ends a merged gesture (a drag, typing).
    Q_INVOKABLE void endEdit();

    // ---- Animation and Responsive Design ----
    /// graphic.applyPreset: preset "fade" | "slide" | "scalePop" | "bounce" | "typewriter" | "wordByWord" | "wipe", phase
    /// "in" | "out" | "loop", easing "linear" | "easeIn" | "easeOut" | "easeInOut" | "overshoot", direction for slide /
    /// wipe / bounce.
    Q_INVOKABLE QString applyPreset(int layerId, const QString& preset, const QString& phase, double seconds,
                                    const QString& easing, const QString& direction = QStringLiteral("left"));
    /// Pins the layer's `edges` ("left", "top", "right", "bottom") to sibling `target` where it is now; target 0 unpins.
    Q_INVOKABLE QString setPin(int layerId, int pinTo, const QStringList& edges);
    /// Responsive time: protected intro and outro of a graphic designed `duration` long (seconds); enabled false clears.
    Q_INVOKABLE QString setResponsive(bool enabled, double duration, double intro, double outro);
    /// The siblings a layer can be pinned to: [{ id, name }].
    Q_INVOKABLE QVariantList pinTargets(int layerId) const;

    /// The keymap's graphics.* commands; false for other ids.
    Q_INVOKABLE bool runCommand(const QString& commandId);

    // ---- Tests ----
    /// Thumbnails painted so far, and whether the last one was painted on the GUI thread.
    int thumbnailRenders() const;
    bool lastThumbnailOnGuiThread() const;

signals:
    void templatesChanged();
    void graphicChanged();
    void selectedLayerChanged();
    void layerChanged();
    void brandChanged();
    void messageChanged();

public:
    /// What the thumbnail provider reads, on its own thread.
    struct Shared {
        mutable std::mutex mutex;
        std::map<std::string, std::shared_ptr<const GraphicTemplate>> templates; ///< by key
        std::optional<BrandKit> brandKit;
        /// For the built-in templates' thumbnails (template.resolve on the project as it was last listed).
        const command::CommandRegistry* registry = nullptr;
        std::shared_ptr<const ProjectState> snapshot;
        std::shared_ptr<command::CommandServices> services;
        std::atomic<int> renders{0};
        std::atomic<QThread*> lastThread{nullptr};
    };

private:
    struct Target;
    std::optional<Target> target() const;
    QString setError(const QString& error);
    /// Applies `fn` to the selected graphic clip's graphic: one undo step named `label`, merged under `merge`.
    QString editGraphic(const char* label, const std::function<std::optional<QString>(Graphic&)>& fn,
                        const std::string& merge = {});
    QString editLayer(int layerId, const char* label, const std::function<std::optional<QString>(GraphicLayer&, const Graphic&)>& fn,
                      const std::string& merge = {});
    const command::CommandRegistry& registry() const;
    QString runCommandNow(const std::string& name, const std::string& params, QVariantMap* data = nullptr);
    /// Selects the selected graphic's first text field's layer, else its top text layer.
    void selectFirstTextLayer();
    void rebuild();
    void rebuildLayer();
    void rebuildBrand();
    void setMessage(const QString& message);
    Ticks graphicTime() const;

    QPointer<ProjectController> m_controller;
    std::filesystem::path m_userLibrary;
    const command::CommandRegistry* m_registry = nullptr;
    std::shared_ptr<Shared> m_shared;
    QVariantList m_templates;
    QVariantList m_libraryErrors;
    ClipId m_clip;
    QVariantMap m_graphic;
    QVariantList m_fields;
    QVariantList m_layers;
    std::uint32_t m_selectedLayer = 0;
    std::vector<std::uint32_t> m_extraLayers; ///< also selected (Ctrl-click), in the order they were added
    QVariantMap m_layer;
    QVariantMap m_brand;
    QVariantList m_media;
    QString m_message;
    std::size_t m_projectTemplatesSeen = static_cast<std::size_t>(-1);
    int m_kitRevision = 0; ///< in the thumbnails' urls: a new kit loads them again
};

struct GraphicsBridgeForeign {
    Q_GADGET
    QML_FOREIGN(wabcut::app::GraphicsBridge)
    QML_NAMED_ELEMENT(Graphics)
    QML_SINGLETON
public:
    static GraphicsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return GraphicsBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
