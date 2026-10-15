#pragma once

// ClipboardBridge: Paste Attributes… and Remove Attributes… for QML (singleton `Clipboard`, C.11 / M5m), on the
// session's clipboard (ProjectController::clipboardServices; Cut / Copy / Paste themselves are ProjectController's).
//
//   * Premiere's dialog (AttributesDialog.qml): Video Attributes (Motion, Opacity, Time Remapping, each video effect of
//     the copied video clip) and Audio Attributes (Volume, Channel Volume / Panner, each audio effect of the copied audio
//     clip), Scale Attribute Times; Remove lists the selection's own effects by id, and "All effects".
//   * attributesInfo(mode) fills the dialog, previewAttributes(mode, choices) is its live preview (clip.pasteAttributes /
//     clip.removeAttributes dry runs: what each clip takes and what it skips, with the reason), and
//     applyAttributes(mode, choices) runs them as one undo step.
//   * Video targets take the copied video clip's attributes and audio targets the copied audio clip's (effect indexes
//     belong to one source stack), so a paste onto a linked pair is one command per kind, in one batch. A kind whose
//     dry run fails (nothing to change there) is left out instead of failing the other.
//   * `message`: the last paste's notes or attribute result, for a toast.
//
// mode: "paste" | "remove". choices: { motion, opacity, timeRemap, volume, panner, videoEffects: [key],
// audioEffects: [key], allEffects (remove), scaleTimes (paste) }, the keys from attributesInfo (paste: the effect's
// index in the source clip's stack; remove: the effect id).

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <utility>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
struct CommandRequest;
}

namespace wabcut::app {

class ProjectController;

class ClipboardBridge : public QObject {
    Q_OBJECT
    /// The clipboard holds clips (ProjectController.canPaste).
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY availabilityChanged FINAL)
    /// Paste Attributes… can open: the clipboard holds a clip and clips are selected.
    Q_PROPERTY(bool canPasteAttributes READ canPasteAttributes NOTIFY availabilityChanged FINAL)
    /// Remove Attributes… can open: clips are selected.
    Q_PROPERTY(bool canRemoveAttributes READ canRemoveAttributes NOTIFY availabilityChanged FINAL)
    /// The last paste's notes or attribute result, for a toast (messageChanged fires for every message).
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit ClipboardBridge(QObject* parent = nullptr);
    static ClipboardBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    ProjectController* controller() const { return m_controller; }

    bool canPaste() const;
    bool canPasteAttributes() const;
    bool canRemoveAttributes() const;
    QString message() const { return m_message; }

    /// Edit > Paste Attributes… / Remove Attributes… (the keymap, the clip menu): emits attributesRequested(mode) for
    /// Main.qml to open the dialog, or says what is missing (`message`) and returns false.
    Q_INVOKABLE bool openAttributes(const QString& mode);
    /// The dialog's content: { mode, available, reason, targets (selected clips), source (the copied clip's name; paste),
    /// video: { available, reason, source, effects: [{ key, name }] }, audio: { same } }. Paste first takes another
    /// Wabcut's clipboard from the system clipboard (ProjectController::adoptSystemClipboard).
    Q_INVOKABLE QVariantMap attributesInfo(const QString& mode);
    /// The live preview: { ok (something would change), error, rows: [{ clip, name, kind: "video"|"audio",
    /// applied: [...], skipped: ["Volume: not an audio clip", ...] }] } in selection order. Changes nothing.
    Q_INVOKABLE QVariantMap previewAttributes(const QString& mode, const QVariantMap& choices);
    /// Applies the choices as one undo step ("Paste Attributes" / "Remove Attributes"); "" or the error.
    Q_INVOKABLE QString applyAttributes(const QString& mode, const QVariantMap& choices);

signals:
    void availabilityChanged();
    void messageChanged();
    void attributesRequested(const QString& mode);

private:
    /// The commands for the choices: one per kind of selected clip (paste) or one (remove).
    struct Plan {
        std::vector<command::CommandRequest> parts;
        std::vector<std::vector<ClipId>> partClips;
        /// Selected clips no part covers, with the reason.
        std::vector<std::pair<ClipId, QString>> left;
        /// The selection (the rows' order).
        std::vector<ClipId> order;
        /// Why nothing can run ("" when parts is not empty).
        QString error;

        Plan();
        ~Plan();
        Plan(Plan&&) noexcept;
        Plan& operator=(Plan&&) noexcept;
    };
    Plan plan(bool paste, const QVariantMap& choices) const;
    void setMessage(const QString& message);

    QPointer<ProjectController> m_controller;
    QString m_message;
};

struct ClipboardSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::ClipboardBridge)
    QML_NAMED_ELEMENT(Clipboard)
    QML_SINGLETON
public:
    static ClipboardBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return ClipboardBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
