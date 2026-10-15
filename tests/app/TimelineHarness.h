#pragma once

// Offscreen rendering for TimelineView tests: a hidden QQuickWindow renders into a texture through
// QQuickRenderControl on the real RHI backend and keeps its scene graph between frames, so a frame after a
// scroll, zoom or drag measures what the app does. Nothing is ever shown.

#include "ProjectController.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <string>
#include <string_view>

#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <rhi/qrhi.h>

#include <memory>
#include <vector>

namespace wabcut::test {

// Offscreen window driven frame by frame (persistent scene graph, no visible window).
class RenderHarness {
public:
    explicit RenderHarness(QSize size) : m_window(&m_control), m_size(size) {
        m_window.setGeometry(0, 0, size.width(), size.height());
        m_window.contentItem()->setSize(size);
    }
    ~RenderHarness() {
        // Release the scene graph before the RHI resources it renders into.
        m_items.clear();
        m_control.invalidate();
    }

    bool initialize() {
        if (!m_control.initialize())
            return false;
        QRhi* rhi = m_control.rhi();
        m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, m_size, 1,
                                        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_texture->create())
            return false;
        m_depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, m_size, 1));
        if (!m_depth->create())
            return false;
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(m_texture.get())};
        description.setDepthStencilBuffer(m_depth.get());
        m_target.reset(rhi->newTextureRenderTarget(description));
        m_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_pass.get());
        if (!m_target->create())
            return false;
        m_window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(m_target.get()));
        return true;
    }

    QQuickWindow& window() { return m_window; }

    void frame() {
        m_control.polishItems();
        m_control.beginFrame();
        m_control.sync();
        m_control.render();
        m_control.endFrame();
    }

    void own(std::unique_ptr<QQuickItem> item) { m_items.push_back(std::move(item)); }

private:
    QQuickRenderControl m_control;
    QQuickWindow m_window;
    QSize m_size;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderBuffer> m_depth;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
    std::vector<std::unique_ptr<QQuickItem>> m_items;
};

inline wabcut::app::TimelineView* addView(RenderHarness& harness, wabcut::app::ProjectController& controller,
                                         QSize size) {
    auto view = std::make_unique<wabcut::app::TimelineView>();
    view->setParentItem(harness.window().contentItem());
    view->setSize(size);
    view->setController(&controller);
    wabcut::app::TimelineView* raw = view.get();
    harness.own(std::move(view));
    return raw;
}

/// The model timeline of a Timeline picture under edit, with a TimelineView in an offscreen window and helpers to
/// drive it with real mouse and key events (10 px per frame by default; one picture character is one frame).
struct Rig {
    Timeline tl;
    std::string original;
    wabcut::app::ProjectController controller;
    RenderHarness harness{QSize(900, 260)};
    wabcut::app::TimelineView* view = nullptr;
    double pxPerFrame;

    explicit Rig(std::string_view picture, double pixelsPerFrame = 10.0, bool linkMatching = true)
        : tl(picture), original(picture), pxPerFrame(pixelsPerFrame) {
        if (linkMatching)
            tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        REQUIRE(harness.initialize());
        view = addView(harness, controller, QSize(900, 260));
        view->setPixelsPerSecond(pixelsPerFrame * 25.0);
        view->setScrollX(0);
        view->setScrollY(0);
        harness.frame();
    }

    ClipId clip(std::string_view ref) const { return clipAt(*controller.snapshot(), tl.seq, ref); }
    double x(double frame) const { return frame * pxPerFrame - view->scrollX(); }
    double y(std::string_view track) const {
        const TrackId id = tl.track(track);
        for (const auto& row : view->rows()) {
            if (row.track->id == id)
                return row.y + row.height / 2 - view->scrollY();
        }
        FAIL("no row for " << std::string(track));
        return 0;
    }

    void send(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
              Qt::KeyboardModifiers mods) {
        QMouseEvent event(type, pos, pos, button, buttons, mods);
        QCoreApplication::sendEvent(&harness.window(), &event);
    }
    void press(QPointF pos, Qt::KeyboardModifiers mods = {}, Qt::MouseButton button = Qt::LeftButton) {
        send(QEvent::MouseButtonPress, pos, button, button, mods);
    }
    void move(QPointF pos, Qt::KeyboardModifiers mods = {}) {
        send(QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton, mods);
        harness.frame();
    }
    void release(QPointF pos, Qt::KeyboardModifiers mods = {}) {
        send(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, mods);
        harness.frame();
    }
    /// Press at `from`, move to `to` in steps (one frame each), optionally release there.
    void drag(QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}, bool releaseAtEnd = true, int steps = 6) {
        press(from, mods);
        for (int i = 1; i <= steps; ++i)
            move(from + (to - from) * (static_cast<double>(i) / steps), mods);
        if (releaseAtEnd)
            release(to, mods);
    }
    void key(int key, Qt::KeyboardModifiers mods = {}) {
        QKeyEvent press(QEvent::KeyPress, key, mods);
        QCoreApplication::sendEvent(&harness.window(), &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods);
        QCoreApplication::sendEvent(&harness.window(), &release);
    }

    /// The model timeline now, in the picture's width.
    std::string picture() const { return render(*controller.snapshot(), tl.seq, original); }
    /// What EditOps produce from the starting state.
    std::string expected(const edit::EditOutcome& outcome) const {
        REQUIRE(outcome);
        return render(outcome->state, tl.seq, original);
    }
    std::size_t undoSteps() { return controller.session().undoCount(); }
};

} // namespace wabcut::test
