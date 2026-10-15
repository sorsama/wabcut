#pragma once

// ProgramMonitor: the program monitor's picture (docs/architecture.md, "app" and "render"; Threads table).
//
//   ProgramMonitor { anchors.fill: parent; surroundColor: Theme.videoBlack }
//
// A QQuickRhiItem whose renderer runs the shared render::Compositor on the scene graph's QRhi. On every
// refresh (render()) it takes the composite that is due from a CompositeMailbox (the playback scheduler's
// bounded hand-off): it never decodes, never waits on a lock, holds the last frame when nothing new is ready,
// and shows black only after a jump seek with nothing decoded yet. While playing it keeps asking for the next
// refresh itself; when idle, a publish wakes it. The colour buffer follows the item's size
// times the window's effective device pixel ratio, and the sequence canvas is letterboxed into it with the
// display output transform of the composite's ColorPipeline.
//
// Until the first composite is presented the item stays transparent and `hasFrame` is false, so the QML
// placeholder underneath remains visible. The source is attached from C++ (setMailbox(), or
// setPlaybackController() once the controller is wired into ProjectController); raw times and IDs never cross
// into QML (Principle 9).

#include <QColor>
#include <QPointer>
#include <QQuickRhiItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace wabcut::render {
class CompositeMailbox;
}
namespace wabcut::playback {
class PlaybackController;
}

namespace wabcut::app {

class ProgramMonitorRenderer;

class ProgramMonitor : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

    /// True once a composite has been presented (stays true; black after a seek still counts as a picture).
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged FINAL)
    /// Colour around the letterboxed canvas.
    Q_PROPERTY(
        QColor surroundColor READ surroundColor WRITE setSurroundColor NOTIFY surroundColorChanged FINAL)
    /// Playback resolution: 1 = full, 2 = half, 4 = quarter (Premiere's "Playback Resolution").
    Q_PROPERTY(
        int previewDivisor READ previewDivisor WRITE setPreviewDivisor NOTIFY previewDivisorChanged FINAL)
    /// "D3D11 · NVIDIA GeForce RTX 3060" once rendering started (architecture principle 4).
    Q_PROPERTY(QString renderBackend READ renderBackend NOTIFY renderBackendChanged FINAL)
    /// This monitor feeds the Scopes panel (I.5): while scopes are wanted (ScopeHub) it computes them on the GPU after
    /// a frame, at most about 30 times a second. The program monitor sets it.
    Q_PROPERTY(bool scopes READ scopes WRITE setScopes NOTIFY scopesChanged FINAL)

public:
    explicit ProgramMonitor(QQuickItem* parent = nullptr);
    ~ProgramMonitor() override;

    bool hasFrame() const { return m_hasFrame; }
    QColor surroundColor() const { return m_surroundColor; }
    void setSurroundColor(const QColor& color);
    int previewDivisor() const { return m_previewDivisor; }
    void setPreviewDivisor(int divisor);
    QString renderBackend() const { return m_renderBackend; }
    bool scopes() const { return m_scopes; }
    void setScopes(bool scopes);

    /// The composites to show (null detaches). GUI thread. The mailbox's publish callback is taken over to
    /// wake the monitor when it is idle.
    void setMailbox(std::shared_ptr<render::CompositeMailbox> mailbox);
    std::shared_ptr<render::CompositeMailbox> mailbox() const { return m_mailbox; }
    /// Shows `controller`'s output (its mailbox; the controller must outlive the attachment). Null detaches.
    void setPlaybackController(playback::PlaybackController* controller);
    playback::PlaybackController* playbackController() const { return m_controller; }

signals:
    void hasFrameChanged();
    void surroundColorChanged();
    void previewDivisorChanged();
    void renderBackendChanged();
    void scopesChanged();

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    friend class ProgramMonitorRenderer;

    /// Shared with the mailbox's publish callback (any thread).
    struct WakeState {
        QPointer<ProgramMonitor> monitor;
        std::atomic<bool> wakePending{false};
        std::atomic<bool> refreshing{false}; ///< The renderer is refreshing every frame anyway.
        std::atomic<std::uint64_t> published{0}; ///< Composites published so far (render(): one came late).
    };

    std::shared_ptr<render::CompositeMailbox> m_mailbox;
    std::shared_ptr<WakeState> m_wake;
    playback::PlaybackController* m_controller = nullptr;
    QColor m_surroundColor{Qt::black};
    int m_previewDivisor = 1;
    bool m_hasFrame = false; // GUI-thread view; written in synchronize() while the GUI thread is blocked
    bool m_scopes = false;
    QString m_renderBackend;
};

} // namespace wabcut::app
