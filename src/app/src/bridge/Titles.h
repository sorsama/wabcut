#pragma once

// Titles (M5d, v1 UI): "New Title" and the Essential Graphics-lite style editor (QML singleton `Titles`).
//
// The model's title (TitleOps.h, C.4): a title media item holding the text and font, a clip of it, and the clip's
// "wabcut.title.style" effect holding the numeric styling (size, fill and stroke colours, stroke width, shadow,
// alignment; keyframable). This bridge shows that style in UI terms:
//   { text, font: "sans"|"serif"|"mono"|"display", size, color: "#RRGGBB", bold, alignment: "left"|"center"|"right",
//     strokeEnabled, strokeColor, strokeWidth, shadowEnabled, shadowColor, shadowDistance, shadowSoftness, preset }
// With a title clip selected (editing), every change edits it: text and font through setTitleText / setTitleFont
// (they change every clip of the title), the rest as the style effect's parameters at the playhead (keys when
// animated); a drag merges into one undo step until endEdit(). With no title selected the style is the draft the
// next New Title starts from. New Title runs the typed command title.add at the playhead on a free video track at
// or above the targeted one, then selects the new clip.
//
// Gap (model): the style effect has no shadow colour, so shadowColor only colours the preview here.

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class TitlesBridge : public QObject {
    Q_OBJECT

    /// The style shown (see the file comment): the selected title's, else the draft.
    Q_PROPERTY(QVariantMap style READ style NOTIFY styleChanged FINAL)
    /// The typed command title.add exists, so titles can be created.
    Q_PROPERTY(bool available READ available CONSTANT FINAL)
    /// Why "New Title" is disabled ("" when available).
    Q_PROPERTY(QString unavailableReason READ unavailableReason CONSTANT FINAL)
    /// Font choices: [{ key, label, family }].
    Q_PROPERTY(QVariantList fonts READ fonts CONSTANT FINAL)
    /// A title clip is selected: changes edit it.
    Q_PROPERTY(bool editing READ editing NOTIFY targetChanged FINAL)
    /// The selected title clip's name, or "".
    Q_PROPERTY(QString targetName READ targetName NOTIFY targetChanged FINAL)

public:
    static constexpr const char* kCommand = "title.add";

    explicit TitlesBridge(QObject* parent = nullptr);
    static TitlesBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);

    QVariantMap style() const { return m_style; }
    bool available() const;
    QString unavailableReason() const;
    QVariantList fonts() const;
    bool editing() const { return m_clip.isValid(); }
    QString targetName() const { return m_targetName; }

    /// Changes one style value (see `style`); unknown keys are ignored. Edits the selected title, if any.
    Q_INVOKABLE void setStyleValue(const QString& key, const QVariant& value);
    /// Closes a drag's undo step.
    Q_INVOKABLE void endEdit();
    /// Applies a brand preset (any subset of the style keys, plus `preset`: its name) as one undo step.
    Q_INVOKABLE void applyPreset(const QVariantMap& preset);
    /// Back to the default style (the text stays).
    Q_INVOKABLE void resetStyle();
    /// Creates a title clip at the playhead (title.add) and selects it. Returns "" or a message saying why not.
    Q_INVOKABLE QString createTitle();
    /// The title.add parameters createTitle() sends.
    Q_INVOKABLE QVariantMap commandParams() const;

signals:
    void styleChanged();
    void targetChanged();

private:
    static QVariantMap defaultStyle();
    /// Follows the selection: a selected title clip becomes the target and its style is shown.
    void syncFromSelection();
    /// Writes `values` (style keys) to the target title as one edit.
    void applyToTarget(const QVariantMap& values, const char* label, const std::string& mergeKey);
    QString trackForNewTitle() const;

    QPointer<ProjectController> m_controller;
    QVariantMap m_style;
    QVariantMap m_draft;  ///< the style New Title uses when no title is selected
    ClipId m_clip;        ///< selected title clip
    MediaId m_media;      ///< its title item
    EffectInstanceId m_styleEffect;
    QString m_targetName;
    bool m_applying = false;
};

/// QML registration through the factory (see ProjectControllerSingleton).
struct TitlesBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::TitlesBridge)
    QML_NAMED_ELEMENT(Titles)
    QML_SINGLETON
public:
    static TitlesBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return TitlesBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
