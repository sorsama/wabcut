#pragma once

// OffscreenRenderer: a headless QRhi that renders PreparedComposites with the same Compositor as the monitor
// and reads the result back to CPU RGBA (contract rev 2, "render"). Export and tests use it; no window is
// ever created.
//
// Backend policy (mirrors the app's RhiPolicy): an explicit request, else QSG_RHI_BACKEND (the variable the
// app sets, so preview and export agree), else the platform default: D3D11 on Windows, Metal on macOS, Vulkan
// then OpenGL on Linux. The backend actually created is reported (backendName(), adapterName()).
//
// QRhi offscreen frames are synchronous: render() returns once the GPU finished and the pixels are read back.
// AsyncOffscreenRenderer runs a renderer on its own thread behind a bounded queue, so a producer (export)
// overlaps its own work with rendering and is held back (backpressure) when it gets more than `maxInFlight`
// frames ahead.

#include <wabcut/model/Error.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/PreparedComposite.h>

#include <QSize>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class QRhi;

namespace wabcut::render {

enum class RhiBackendRequest : std::uint8_t { PlatformDefault, D3D11, D3D12, Vulkan, OpenGL, Metal, Null };

std::string_view rhiBackendRequestName(RhiBackendRequest backend);
/// Resolves PlatformDefault: `environment` (QSG_RHI_BACKEND's value: "d3d11", "d3d12", "vulkan",
/// "opengl"/"gl", "metal", "null"; anything else is ignored) and then the platform default.
RhiBackendRequest resolveRhiBackend(RhiBackendRequest requested, std::string_view environment);

struct OffscreenOptions {
    RhiBackendRequest backend = RhiBackendRequest::PlatformDefault;
    /// Consult QSG_RHI_BACKEND when `backend` is PlatformDefault.
    bool honourEnvironment = true;
    bool debugLayer = false;
    /// GPU timestamps (renderTimed()) where the backend supports them. Off by default: on D3D11, reading them back
    /// waits for the frame by polling with a sleep, which rounds a frame that is still running up to the system
    /// timer period (15.6 ms where a background process gets no finer timer, e.g. Windows 11 without a visible
    /// window). Perf harnesses that want GPU execution times turn it on and read wall-clock times with that in mind.
    bool timestamps = false;
    OutputRole role = OutputRole::Export;
    /// Zero-copy: share this renderer's device with media's hardware decoders (render::shareDeviceWithDecoders).
    bool shareDeviceWithDecoders = false;
    OutputAlpha alpha = OutputAlpha::Opaque;
    CompositorOptions compositor{TexturePool::Export, 1, 48};
    /// I.9: render into and read back 10 bits per channel (RGB10A2: R in bits 0-9, G 10-19, B 20-29, alpha 30-31 of
    /// each little-endian 32-bit word), for HDR and 10-bit colour-managed exports. RenderedImage::tenBit says so.
    bool tenBit = false;
};

/// RGBA8 pixels (or RGB10A2 words with OffscreenOptions::tenBit), top row first, tightly packed (stride = 4 * width).
struct RenderedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    bool tenBit = false; ///< `rgba` holds RGB10A2 words (I.9)
    std::uint64_t revision = 0;
    Ticks targetTime{};

    const std::uint8_t* pixel(int x, int y) const {
        return rgba.data() +
               (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) *
                   4;
    }
};

struct OffscreenTiming {
    double cpuMs = 0.0;  ///< Recording + submission + wait, wall clock.
    double gpuMs = -1.0; ///< GPU execution time of the frame; < 0 when the backend reports none.
    CompositeStats composite;
};

class OffscreenRenderer {
public:
    /// Creates the QRhi. OpenGL needs the GUI thread (fallback surface); the other backends work on any
    /// thread, which then owns the renderer.
    static std::expected<std::unique_ptr<OffscreenRenderer>, Error>
    create(const OffscreenOptions& options = {});
    ~OffscreenRenderer();
    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;

    QRhi* rhi() const;
    /// "D3D11", "D3D12", "Vulkan", "OpenGL", "Metal" or "Null".
    std::string backendName() const;
    /// Adapter as reported by the driver, e.g. "NVIDIA GeForce RTX 3060".
    std::string adapterName() const;
    Compositor& compositor();
    const OffscreenOptions& options() const;

    /// Renders `composite` at `outputSize` (default: the canvas size) and reads it back into `into` (its
    /// buffer is reused). Blocks until the pixels are on the CPU.
    std::expected<void, Error> render(const std::shared_ptr<const PreparedComposite>& composite,
                                      RenderedImage& into, std::optional<QSize> outputSize = {});
    std::expected<RenderedImage, Error> render(const std::shared_ptr<const PreparedComposite>& composite,
                                               std::optional<QSize> outputSize = {});
    /// Renders without readback and waits for the GPU; reports timings (perf harness).
    std::expected<OffscreenTiming, Error>
    renderTimed(const std::shared_ptr<const PreparedComposite>& composite,
                std::optional<QSize> outputSize = {});

private:
    struct Impl;
    explicit OffscreenRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

struct AsyncOffscreenOptions {
    OffscreenOptions renderer{};
    /// Frames queued or rendering at most; submit() blocks beyond that.
    int maxInFlight = 3;
};

struct AsyncFrameResult {
    std::uint64_t sequence = 0;           ///< 1, 2, ... in submit order; results arrive in that order.
    const RenderedImage* image = nullptr; ///< Valid during the callback only (the buffer is reused).
    std::optional<Error> error;
};

struct AsyncOffscreenStats {
    std::uint64_t submitted = 0;
    std::uint64_t delivered = 0;
    std::uint64_t failed = 0;
    std::uint64_t blockedSubmits = 0; ///< submit() calls that had to wait for room (backpressure engaged).
    int maxQueued = 0;
};

class AsyncOffscreenRenderer {
public:
    using Callback = std::function<void(const AsyncFrameResult&)>;

    /// Starts the render thread and creates its QRhi there (not OpenGL). `onFrame` runs on that thread.
    static std::expected<std::unique_ptr<AsyncOffscreenRenderer>, Error>
    create(const AsyncOffscreenOptions& options, Callback onFrame);
    ~AsyncOffscreenRenderer(); ///< stop()s.
    AsyncOffscreenRenderer(const AsyncOffscreenRenderer&) = delete;
    AsyncOffscreenRenderer& operator=(const AsyncOffscreenRenderer&) = delete;

    /// Queues a composite; blocks while maxInFlight frames are queued or rendering. Returns its sequence
    /// number, or Cancelled after stop().
    std::expected<std::uint64_t, Error> submit(std::shared_ptr<const PreparedComposite> composite);
    /// Non-blocking: nullopt when the queue is full (or stopped).
    std::optional<std::uint64_t> trySubmit(std::shared_ptr<const PreparedComposite> composite);
    /// Waits until every submitted frame has been delivered.
    void flush();
    /// Finishes the frames already queued, then stops the thread. Idempotent.
    void stop();

    std::string backendName() const;
    AsyncOffscreenStats stats() const;

private:
    AsyncOffscreenRenderer(AsyncOffscreenOptions options, Callback onFrame);
    void run(std::promise<std::expected<void, Error>>* started);

    AsyncOffscreenOptions m_options;
    Callback m_onFrame;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake; // worker: work or stop
    std::condition_variable m_room; // producers: room in the queue / everything delivered
    std::deque<std::pair<std::uint64_t, std::shared_ptr<const PreparedComposite>>> m_queue;
    int m_inFlight = 0; // queued + rendering
    bool m_stopping = false;
    std::uint64_t m_nextSequence = 1;
    std::string m_backend;
    AsyncOffscreenStats m_stats;
};

} // namespace wabcut::render
