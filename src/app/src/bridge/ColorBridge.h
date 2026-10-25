#pragma once

// ColorBridge: the Color workspace's Color panel for QML (singleton `Color`, I.5, docs/color.md). Owner: Graphics.
//
// It edits the grade ("wabcut.color.grade") of the clip the panel targets - the first selected video clip, else the
// topmost video clip under the playhead - through typed edits only:
//   * scalars through ProjectController::setParameterAtPlayhead (a key at the playhead when the parameter is
//     animated, so everything stays keyframable), merged into one undo step per drag gesture
//     (beginGesture/endGesture);
//   * curves and LUTs as EffectData (edit::setEffect), stage switches and resets, Auto / Color Match / Match Scene
//     through the colour commands (one undo step each);
//   * the program monitor's colour views (mask, before/after split, a reference still side by side) through
//     PlaybackBridge::setColorPreview - preview only, never exported.

#include <wabcut/color/Grade.h>
#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/Ids.h>
#include <wabcut/render/StillImage.h>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QJSEngine;
class QQmlEngine;

namespace wabcut {
struct Clip;
}
namespace wabcut::color {
class FaceFinder;
}

namespace wabcut::app {

class ProjectController;
class PlaybackBridge;

class ColorBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Color)
    QML_SINGLETON

    /// The targeted clip ("" = none) and its name.
    Q_PROPERTY(QString clip READ clip NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString clipName READ clipName NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool hasGrade READ hasGrade NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool gradeEnabled READ gradeEnabled NOTIFY stateChanged FINAL)
    /// Every grade parameter at the playhead, by registry key (defaults when the clip has no grade).
    Q_PROPERTY(QVariantMap params READ params NOTIFY stateChanged FINAL)
    /// Keys of animated parameters (the panel marks them).
    Q_PROPERTY(QStringList animated READ animated NOTIFY stateChanged FINAL)
    /// Curve name ("master", "red", ..., "satSat") -> [[x, y], ...] (absent = identity / neutral).
    Q_PROPERTY(QVariantMap curves READ curves NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString inputLut READ inputLut NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString look READ look NOTIFY stateChanged FINAL)
    /// [{reference, name, kind: "input" | "look" | "file", offline}] for the LUT menus.
    Q_PROPERTY(QVariantList lutChoices READ lutChoices NOTIFY stateChanged FINAL)
    Q_PROPERTY(int missingLuts READ missingLuts NOTIFY stateChanged FINAL)
    /// Monitor views: 0 off, 1 before/after split, 2 reference side by side.
    Q_PROPERTY(int compareMode READ compareMode WRITE setCompareMode NOTIFY viewChanged FINAL)
    Q_PROPERTY(double splitPosition READ splitPosition WRITE setSplitPosition NOTIFY viewChanged FINAL)
    /// HSL secondary matte on the monitor: 0 off, 1 grey, 2 colour on black, 3 colour on white.
    Q_PROPERTY(int maskView READ maskView WRITE setMaskView NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool hasReference READ hasReference NOTIFY viewChanged FINAL)
    Q_PROPERTY(QString referenceLabel READ referenceLabel NOTIFY viewChanged FINAL)
    /// The HSL eyedropper is armed: the next click on the program monitor picks the qualifier (pickColor()).
    Q_PROPERTY(bool eyedropper READ eyedropper WRITE setEyedropper NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    /// What the last tool did ("Auto: temperature -12, ..."), or why it could not.
    Q_PROPERTY(QString status READ status NOTIFY busyChanged FINAL)
    // Colour management (I.9, 0.2; docs/color.md "Colour management"): the Color Management settings dialog and the
    // Color panel's Media Color Space menu.
    /// The project's: { mode: "rec709" | "hdr-hlg" | "hdr-pq" | "aces" | "wide-gamut" | "off", name (Premiere's preset
    /// name), description, hdr, managed, peakNits, referenceWhiteNits (the graphics white), autoDetectLog }.
    Q_PROPERTY(QVariantMap colorManagement READ colorManagement NOTIFY stateChanged FINAL)
    /// [{ id, name, description }] of the modes, [{ id, name }] of the input colour spaces (menu order).
    Q_PROPERTY(QVariantList colorManagementModes READ colorManagementModes CONSTANT FINAL)
    Q_PROPERTY(QVariantList inputColorSpaces READ inputColorSpaces CONSTANT FINAL)
    /// The targeted clip's: { override: id or "" (Auto), auto: what Auto reads (tags, or the detected log), autoName,
    /// detected: the camera log its metadata names or "", effective, name } (empty: no clip).
    Q_PROPERTY(QVariantMap inputColorSpace READ inputColorSpace NOTIFY stateChanged FINAL)
    /// What the program monitor shows when it is not the plain picture: "HDR · tone-mapped SDR preview",
    /// "ACES 1.0 SDR Video", "Wide Gamut · no tone mapping", "Color management off"; empty for Direct Rec.709.
    Q_PROPERTY(QString monitorLabel READ monitorLabel NOTIFY stateChanged FINAL)

public:
    explicit ColorBridge(QObject* parent = nullptr);
    ~ColorBridge() override;
    static ColorBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    QString clip() const;
    QString clipName() const { return m_clipName; }
    bool hasGrade() const { return m_grade.isValid(); }
    bool gradeEnabled() const { return m_gradeEnabled; }
    QVariantMap params() const { return m_params; }
    QStringList animated() const { return m_animated; }
    QVariantMap curves() const { return m_curves; }
    QString inputLut() const { return m_inputLut; }
    QString look() const { return m_look; }
    QVariantList lutChoices() const;
    int missingLuts() const { return m_missingLuts; }
    int compareMode() const { return m_compareMode; }
    void setCompareMode(int mode);
    double splitPosition() const { return m_split; }
    void setSplitPosition(double position);
    int maskView() const { return m_maskView; }
    void setMaskView(int view);
    bool hasReference() const { return m_reference != nullptr; }
    QString referenceLabel() const { return m_referenceLabel; }
    bool eyedropper() const { return m_eyedropper; }
    void setEyedropper(bool on);
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QVariantMap colorManagement() const { return m_colorManagement; }
    QVariantList colorManagementModes() const;
    QVariantList inputColorSpaces() const;
    QVariantMap inputColorSpace() const { return m_inputColorSpace; }
    QString monitorLabel() const { return m_monitorLabel; }

    /// A parameter's registry facts: { name, from, to, default, unit } (the panel builds its sliders from them).
    Q_INVOKABLE QVariantMap paramInfo(const QString& key) const;
    /// One undo step for everything between these (a slider or wheel drag).
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    /// Sets a scalar (registry key) on the targeted clip's grade, adding the grade when the clip has none.
    Q_INVOKABLE bool setParam(const QString& key, double value);
    /// Several scalars in one undo step (a colour wheel's three channels).
    Q_INVOKABLE bool setParams(const QVariantMap& values);
    Q_INVOKABLE bool setStage(const QString& stage, bool enabled);
    Q_INVOKABLE bool setGradeEnabled(bool enabled);
    /// Back to defaults: one stage ("basic", "creative", "curves", "wheels", "hsl", "vignette") or "all".
    Q_INVOKABLE bool resetStage(const QString& stage);
    /// points: [[x, y], ...]; [] resets the curve.
    Q_INVOKABLE bool setCurve(const QString& name, const QVariantList& points);
    /// `count` samples of a curve as the grade evaluates it (x = i / (count - 1); hue curves wrap).
    Q_INVOKABLE QVariantList curveSamples(const QString& name, int count) const;
    /// slot "input" or "look"; reference "builtin:<name>", "asset:<id>" or "" (none).
    Q_INVOKABLE bool setLut(const QString& slot, const QString& reference);
    /// Adds a .cube file to the project (lut.import) and puts it in `slot`. Returns the error ("" = done).
    Q_INVOKABLE QString importLut(const QUrl& file, const QString& slot);
    /// Auto (white balance and levels), Color Match against the reference frame, Match Scene (C.5 shots of one take).
    Q_INVOKABLE bool autoColor();
    /// `faceDetection` (0.2, Premiere's Face Detection): the faces' skin tones are matched first (needs the face
    /// detector, Settings > AI > Local models).
    Q_INVOKABLE bool matchToReference(double strength, bool skinPriority, bool faceDetection = false);
    Q_INVOKABLE bool matchScene(double strength, bool skinPriority, bool faceDetection = false);
    /// The program at the playhead becomes the reference frame (side by side, Color Match).
    Q_INVOKABLE bool setReference();
    Q_INVOKABLE void clearReference();
    /// ASC CDL of the wheels (.cc/.ccc/.cdl). Return the error ("" = done).
    Q_INVOKABLE QString exportCdl(const QUrl& file);
    Q_INVOKABLE QString importCdl(const QUrl& file);
    /// 0.2, Lumetri's Export .cube: the grade at the playhead as a 33-point 3D LUT (lut.export). The error ("" = done).
    Q_INVOKABLE QString exportCube(const QUrl& file);
    /// The grade's effect instance id as text ("" = none): Save Preset of the grade (EffectPresets.saveEffectOfClip).
    Q_INVOKABLE QString gradeId() const { return m_grade.isValid() ? QString::number(m_grade.value) : QString(); }
    /// The eyedropper: the program's colour at (nx, ny) in [0, 1] of the picture sets the HSL qualifier around it.
    Q_INVOKABLE bool pickColor(double nx, double ny);

    /// I.9: sets the project's colour management (keys of colorManagement: mode, peakNits, referenceWhiteNits,
    /// autoDetectLog; only those given change). One undo step (color.management.set).
    Q_INVOKABLE bool setColorManagement(const QVariantMap& values);
    /// I.9: the targeted clip's input colour space by id, or "auto". One undo step (color.inputSpace.set).
    Q_INVOKABLE bool setInputColorSpace(const QString& space);

    /// 0.4 (the Color panel menu's Apply Grade rows): { selection: the other selected video clips, media: the other
    /// video clips of the sequence that show the targeted clip's source (color::sameSource), mediaName: that source's
    /// name elided to 32 characters }. Zeros without a targeted clip.
    Q_INVOKABLE QVariantMap gradeCopyTargets() const;
    /// 0.4: copies the targeted clip's grade over the grade of the other selected video clips ("selection") or of every
    /// other video clip of its source ("media"), one undo step (color.grade.copy). `status` says "Grade applied to 3
    /// clips" or why not.
    Q_INVOKABLE bool applyGradeTo(const QString& scope);

    // ---- C++ ----
    ClipId targetClip() const { return m_clip; }
    EffectInstanceId gradeInstance() const { return m_grade; }

signals:
    void stateChanged();
    void viewChanged();
    void busyChanged();

private:
    ProjectController* controller() const;
    PlaybackBridge* playback() const;
    void attach();
    void refresh();
    /// An edit grades again only when it can change what the panel shows (M5n, P.1): the target clip or its media,
    /// project-wide changes (LUT assets, media), or, when the target is the clip under the playhead, a video track
    /// changed there.
    void onHistory();
    void applyView();
    /// The grade instance of the targeted clip, created (one undo step "Add Color Grade") when missing.
    EffectInstanceId ensureGrade();
    std::string mergeKey(const QString& what) const;
    bool runCommand(const char* name, const QVariantMap& params, QString* summary);
    void setStatus(const QString& status);

    /// 0.2: Color Match's face detector, made the first time a match asks for it (null while not downloaded).
    std::shared_ptr<color::FaceFinder> m_faces;
    ClipId m_clip;
    /// m_clip is a selected clip (else the clip under the playhead, or none).
    bool m_targetFromSelection = false;
    QString m_clipName;
    EffectInstanceId m_grade;
    bool m_gradeEnabled = true;
    QVariantMap m_params;
    QStringList m_animated;
    QVariantMap m_curves;
    QString m_inputLut;
    QString m_look;
    int m_missingLuts = 0;
    int m_compareMode = 0;
    double m_split = 0.5;
    int m_maskView = 0;
    bool m_eyedropper = false;
    std::shared_ptr<const render::StillImage> m_reference;
    Ticks m_referenceTime{};
    QString m_referenceLabel;
    bool m_busy = false;
    QString m_status;
    QVariantMap m_colorManagement;
    ColorManagement m_management; ///< what m_colorManagement and m_monitorLabel describe
    QVariantMap m_inputColorSpace;
    QString m_monitorLabel;
    int m_gesture = 0;
    bool m_inGesture = false;
    bool m_refreshing = false;
    bool m_attached = false;
};

} // namespace wabcut::app
