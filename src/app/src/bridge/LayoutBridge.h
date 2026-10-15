#pragma once

// LayoutBridge: the gaming layout picker (singleton `Layouts`, M5o) on the core's C.15 `layout.apply` - a facecam
// picture-in-picture, a vertical split (gameplay over the facecam) and a vertical blurred fill - plus the new-sequence
// presets (`sequence.presets`, `sequence.create`) for Home and "Create a Shorts sequence".
//
//   * Targets: the gameplay is the selected video clip (else the lowest video clip under the playhead); the facecam is
//     a video clip on a track above it that overlaps it (auto-picked: the one under the playhead, else the longest
//     overlap), or a **facecam region**: a rectangle drawn on the Program Monitor over the webcam inside the capture,
//     kept in fractions of the gameplay's frame (as layout.apply wants it).
//   * Options use layout.apply's names (corner, size, margin, circle, cornerRadius, borderWidth, borderColor "#RRGGBB",
//     shadow, split, facecamOnTop, blur, zoom); params() turns them into the command's parameters, clamped to its
//     ranges, with only what the preset uses.
//   * preview(options): a dry run of layout.apply, debounced and off the GUI thread: it checks the layout and gives the
//     command's refusal before Apply. The monitor cannot draw the layout's effects (masks, borders, blur) until the
//     compositor has them, so the Program Monitor shows a schematic of the boxes instead (schematic(), drawn by
//     LayoutMonitorOverlay while the picker is open).
//   * apply(options): layout.apply on the project, one undo step.
//   * available: the catalog has layout.apply (C.15 on master). Until then the picker shows its schematics and says
//     layouts arrive with the next engine update; Apply is off.

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandRegistry;
}

namespace wabcut::app {

class BridgeJob;
class ProjectController;

class LayoutBridge : public QObject {
    Q_OBJECT
    /// The catalog has layout.apply (C.15).
    Q_PROPERTY(bool available READ available NOTIFY catalogChanged FINAL)
    /// The catalog has sequence.presets / sequence.create.
    Q_PROPERTY(bool sequencePresetsAvailable READ sequencePresetsAvailable NOTIFY catalogChanged FINAL)
    /// [{ preset, name, needsFacecam, vertical, description }]: layout.list's, else the three built in.
    Q_PROPERTY(QVariantList layouts READ layouts NOTIFY catalogChanged FINAL)
    /// Every option's default (layout.list's, else the C.15 defaults); borderColor as "#RRGGBB".
    Q_PROPERTY(QVariantMap defaults READ defaults NOTIFY catalogChanged FINAL)
    /// The gameplay clip ("c12", "" for none) and its name.
    Q_PROPERTY(QString gameplay READ gameplay NOTIFY targetsChanged FINAL)
    Q_PROPERTY(QString gameplayName READ gameplayName NOTIFY targetsChanged FINAL)
    /// The facecam clip ("" for none, or when a region is used); setting it picks a candidate.
    Q_PROPERTY(QString facecam READ facecam WRITE setFacecam NOTIFY targetsChanged FINAL)
    /// [{ key, name, track ("V2") }]: video clips above the gameplay that overlap it, best first.
    Q_PROPERTY(QVariantList facecamCandidates READ facecamCandidates NOTIFY targetsChanged FINAL)
    /// { x, y, width, height } in fractions of the gameplay's frame, or {} (none).
    Q_PROPERTY(QVariantMap facecamRegion READ facecamRegion WRITE setFacecamRegion NOTIFY targetsChanged FINAL)
    /// The same region in fractions of the sequence frame (the Program Monitor's frame area), or {}.
    Q_PROPERTY(QVariantMap facecamRegionInFrame READ facecamRegionInFrame NOTIFY targetsChanged FINAL)
    /// The Program Monitor's region tool is armed (a drag there draws the facecam region).
    Q_PROPERTY(bool drawingRegion READ drawingRegion WRITE setDrawingRegion NOTIFY drawingChanged FINAL)
    /// The picker is open: the monitor shows previewOptions' schematic.
    Q_PROPERTY(bool previewShown READ previewShown WRITE setPreviewShown NOTIFY previewChanged FINAL)
    Q_PROPERTY(QVariantMap previewOptions READ previewOptions NOTIFY previewChanged FINAL)
    /// The last dry run: { busy, ok, error, data }.
    Q_PROPERTY(QVariantMap previewResult READ previewResult NOTIFY previewChanged FINAL)
    /// The sequence is taller than wide (vertical layouts fit it as they are).
    Q_PROPERTY(bool sequenceIsVertical READ sequenceIsVertical NOTIFY targetsChanged FINAL)
    Q_PROPERTY(double frameAspect READ frameAspect NOTIFY targetsChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit LayoutBridge(QObject* parent = nullptr);
    ~LayoutBridge() override;
    static LayoutBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    /// Tests: the catalog the commands run against (default: the editing commands); null = the default.
    void setRegistry(const command::CommandRegistry* registry);
    /// Tests: previews run on the calling thread, without the debounce.
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }

    bool available() const;
    bool sequencePresetsAvailable() const;
    QVariantList layouts() const;
    QVariantMap defaults() const;
    QString gameplay() const;
    QString gameplayName() const { return m_gameplayName; }
    QString facecam() const;
    void setFacecam(const QString& clip);
    QVariantList facecamCandidates() const { return m_candidates; }
    QVariantMap facecamRegion() const { return m_region; }
    void setFacecamRegion(const QVariantMap& region);
    QVariantMap facecamRegionInFrame() const;
    bool drawingRegion() const { return m_drawing; }
    void setDrawingRegion(bool drawing);
    bool previewShown() const { return m_previewShown; }
    void setPreviewShown(bool shown);
    QVariantMap previewOptions() const { return m_previewOptions; }
    QVariantMap previewResult() const { return m_previewResult; }
    bool sequenceIsVertical() const;
    double frameAspect() const;
    QString message() const { return m_message; }

    /// Picks the gameplay and the facecam again (the picker calls it when it opens).
    Q_INVOKABLE void refreshTargets();
    /// layout.apply's parameters for `options` (with "preset"), clamped to the command's ranges, with the targets.
    Q_INVOKABLE QVariantMap params(const QVariantMap& options) const;
    /// The layout's boxes in fractions of an output frame of `aspect` (width / height), bottom to top:
    /// [{ role: "background" | "gameplay" | "facecam", x, y, width, height, circle, radius, border, shadow, blur }].
    Q_INVOKABLE QVariantList schematic(const QVariantMap& options, double aspect) const;
    /// A dry run of layout.apply (debounced, off the GUI thread); the result lands in previewResult.
    Q_INVOKABLE void preview(const QVariantMap& options);
    /// layout.apply: one undo step. "" or why not.
    Q_INVOKABLE QString apply(const QVariantMap& options);
    /// The facecam region from a drag on the monitor: two corners in fractions of the sequence frame, normalised and
    /// mapped into the gameplay's frame (it is fitted into the sequence frame, centred). Sets facecamRegion.
    Q_INVOKABLE QVariantMap setRegionFromFrame(double x0, double y0, double x1, double y1);
    Q_INVOKABLE void clearRegion();
    /// sequence.presets: [{ id, name, group, width, height, frameRate: { num, den, fps } }], or [] when missing.
    Q_INVOKABLE QVariantList sequencePresets() const;
    /// sequence.create { preset: "shorts" } named after the current sequence, then shows it. "" or why not.
    Q_INVOKABLE QString createShortsSequence();
    /// Asks the window to open the picker (the clip menu's Layout…, the timeline toolbar).
    Q_INVOKABLE void open() { emit openRequested(); }

signals:
    void catalogChanged();
    void targetsChanged();
    void drawingChanged();
    void previewChanged();
    void messageChanged();
    void openRequested();
    /// A layout was applied (the picker closes).
    void applied();

private:
    struct Target {
        ClipId clip;
        TrackId track;
        std::size_t trackIndex = 0;
        double aspect = 16.0 / 9.0; ///< the clip's picture (width / height)
    };
    const command::CommandRegistry& registry() const;
    void setMessage(const QString& message);
    void runPreview();
    double facecamAspect() const;

    QPointer<ProjectController> m_controller;
    const command::CommandRegistry* m_registry = nullptr;
    std::unique_ptr<BridgeJob> m_job;
    QTimer m_debounce;
    bool m_synchronous = false;
    std::optional<QVariantMap> m_pendingPreview;
    unsigned m_previewGeneration = 0;

    std::optional<Target> m_gameplay;
    std::optional<Target> m_facecam;
    QString m_gameplayName;
    QVariantList m_candidates;
    QVariantMap m_region;
    bool m_drawing = false;
    bool m_previewShown = false;
    QVariantMap m_previewOptions;
    QVariantMap m_previewResult;
    QString m_message;
};

/// QML registration through the factory (a default-constructible class registered as its own singleton would be built
/// with its default constructor, and create() would never run).
struct LayoutsSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::LayoutBridge)
    QML_NAMED_ELEMENT(Layouts)
    QML_SINGLETON
public:
    static LayoutBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return LayoutBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
