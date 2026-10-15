#pragma once

// BrandBridge: the project's brand kit for QML (singleton `Brand`, C.13 / M5o; docs/graphics.md "Brand kit").
//
//   * kit: { name, colors: [{ name, hex "#RRGGBB" or "#RRGGBBAA", color (for QML), rgba }], fonts: [{ role, family, style, weight }],
//     logos: [{ name, media "m…", mediaName }] } ({} without a kit). Graphics, templates and caption styles refer to its
//     colours, font roles and logos by name, so editing or swapping the kit re-themes them all (the model resolves the
//     names: resolveGraphicClip, resolvedCaptionStyle).
//   * Every edit is one undo step: brand.set with the next kit (createKit, setName, add/set/removeColor, add/set/
//     removeFont, add/set/removeLogo, setKit for a whole kit, removeKit). Edits are per gesture: a typed hex or name is
//     applied when it is committed, a picked swatch at once.
//   * colors: [{ name, hex, color }] for colour pickers (the caption style's, Essential Graphics'); fontRoles: [{ role,
//     family, weight, label }] for font pickers; colorHex(name).
//   * Kit files: exportKit / importKit, JSON {"format": "wabcut-brand-kit", "version": 1, "kit": {...}} (the model's
//     brand kit JSON). Logos are media of a project: an imported kit keeps the logos whose media name matches a media
//     item of this project and says how many it left out.
//   * logoCandidates: [{ key "m…", name }]: the project's media with a picture (stills and videos; not generators).

#include <wabcut/model/Graphics.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <functional>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace wabcut {
struct ProjectState;
}

namespace wabcut::app {

class ProjectController;

/// "#RRGGBB" for an opaque colour, else "#RRGGBBAA" (sRGB 0..1 in).
QString brandHex(const std::array<double, 4>& rgba);
/// "#RRGGBB" / "#RRGGBBAA" (with or without '#') to sRGB 0..1; nullopt when it is not one.
std::optional<std::array<double, 4>> brandRgba(const QString& hex);
/// The kit as QML sees it (Brand.kit).
QVariantMap brandKitMap(const BrandKit& kit, const ProjectState& state);
/// Whether `bytes` (a file's first bytes) look like Bodymovin JSON: an object with the animation keys v, fr, ip, op, w
/// and h. Cheap: a file is sniffed before it is read in full.
bool looksLikeLottie(const QByteArray& bytes);

class BrandBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasKit READ hasKit NOTIFY kitChanged FINAL)
    Q_PROPERTY(QVariantMap kit READ kit NOTIFY kitChanged FINAL)
    Q_PROPERTY(QVariantList colors READ colors NOTIFY kitChanged FINAL)
    Q_PROPERTY(QVariantList fontRoles READ fontRoles NOTIFY kitChanged FINAL)
    /// The font families of this computer (QFontDatabase), for the font pickers.
    Q_PROPERTY(QStringList fontFamilies READ fontFamilies CONSTANT FINAL)
    Q_PROPERTY(QVariantList logoCandidates READ logoCandidates NOTIFY mediaChanged FINAL)
    /// The last edit's error or note ("" after a clean one), for a toast.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit BrandBridge(QObject* parent = nullptr);
    static BrandBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    bool hasKit() const;
    QVariantMap kit() const;
    QVariantList colors() const;
    QVariantList fontRoles() const;
    QStringList fontFamilies() const;
    QVariantList logoCandidates() const;
    QString message() const { return m_message; }

    /// A starter kit: Primary, Secondary and Accent colours and the heading and body roles in the platform's sans.
    Q_INVOKABLE QString createKit(const QString& name);
    Q_INVOKABLE QString removeKit();
    /// The whole kit at once (the shape of `kit`); one undo step.
    Q_INVOKABLE QString setKit(const QVariantMap& kit);
    Q_INVOKABLE QString setName(const QString& name);

    Q_INVOKABLE QString addColor(const QString& name, const QString& hex);
    /// Renames and/or recolours colour `index` (an empty name or hex keeps it).
    Q_INVOKABLE QString setColor(int index, const QString& name, const QString& hex);
    Q_INVOKABLE QString removeColor(int index);

    Q_INVOKABLE QString addFont(const QString& role);
    Q_INVOKABLE QString setFont(int index, const QString& role, const QString& family, const QString& style, int weight);
    Q_INVOKABLE QString removeFont(int index);

    Q_INVOKABLE QString addLogo(const QString& name, const QString& mediaKey);
    Q_INVOKABLE QString setLogo(int index, const QString& name, const QString& mediaKey);
    Q_INVOKABLE QString removeLogo(int index);

    /// "#RRGGBB(AA)" of the kit's colour `name`, or "".
    Q_INVOKABLE QString colorHex(const QString& name) const;
    /// The family of the kit's font role, or "".
    Q_INVOKABLE QString fontFamilyOf(const QString& role) const;

    Q_INVOKABLE QString exportKit(const QUrl& file);
    Q_INVOKABLE QString importKit(const QUrl& file);

    /// Asks the window to open the brand kit editor (the Settings popover, Essential Graphics).
    Q_INVOKABLE void openEditor() { emit editorRequested(); }

signals:
    void kitChanged();
    void mediaChanged();
    void messageChanged();
    void editorRequested();

private:
    std::optional<BrandKit> current() const;
    /// brand.set with `next` (or null): one undo step. "" or the error.
    QString apply(const std::optional<BrandKit>& next);
    /// The current kit to edit (an error when there is none).
    QString edit(const std::function<QString(BrandKit&)>& change);
    void onRevision();
    void setMessage(const QString& message);

    QPointer<ProjectController> m_controller;
    std::optional<BrandKit> m_seen; ///< the kit kitChanged was last emitted for
    std::size_t m_seenMedia = 0;    ///< media count mediaChanged was last emitted for
    QString m_message;
    mutable QStringList m_families;
};

struct BrandSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::BrandBridge)
    QML_NAMED_ELEMENT(Brand)
    QML_SINGLETON
public:
    static BrandBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return BrandBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
