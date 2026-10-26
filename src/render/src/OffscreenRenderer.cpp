#include <wabcut/media/GpuInfo.h>
#include <wabcut/render/OffscreenRenderer.h>
#include <wabcut/render/ZeroCopy.h>

#include <QOffscreenSurface>
#include <QtGlobal>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define WABCUT_RENDER_HAVE_VULKAN 1
#include <QVulkanInstance>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <future>
#include <string>

namespace wabcut::render {

std::string_view rhiBackendRequestName(RhiBackendRequest backend) {
    switch (backend) {
    case RhiBackendRequest::PlatformDefault:
        return "default";
    case RhiBackendRequest::D3D11:
        return "D3D11";
    case RhiBackendRequest::D3D12:
        return "D3D12";
    case RhiBackendRequest::Vulkan:
        return "Vulkan";
    case RhiBackendRequest::OpenGL:
        return "OpenGL";
    case RhiBackendRequest::Metal:
        return "Metal";
    case RhiBackendRequest::Null:
        return "Null";
    }
    return "unknown";
}

RhiBackendRequest resolveRhiBackend(RhiBackendRequest requested, std::string_view environment) {
    if (requested != RhiBackendRequest::PlatformDefault) {
        return requested;
    }
    std::string key;
    for (char c : environment) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
#if defined(Q_OS_WIN)
    if (key == "d3d11") {
        return RhiBackendRequest::D3D11;
    }
    if (key == "d3d12") {
        return RhiBackendRequest::D3D12;
    }
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    if (key == "metal") {
        return RhiBackendRequest::Metal;
    }
#endif
    if (key == "vulkan") {
        return RhiBackendRequest::Vulkan;
    }
    if (key == "opengl" || key == "gl") {
        return RhiBackendRequest::OpenGL;
    }
    if (key == "null") {
        return RhiBackendRequest::Null;
    }
#if defined(Q_OS_WIN)
    return RhiBackendRequest::D3D11;
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    return RhiBackendRequest::Metal;
#else
    return RhiBackendRequest::Vulkan; // create() falls back to OpenGL when no Vulkan instance can be made
#endif
}

// ---- OffscreenRenderer ----

struct OffscreenRenderer::Impl {
    OffscreenOptions options;
#if defined(WABCUT_RENDER_HAVE_VULKAN)
    std::unique_ptr<QVulkanInstance> vulkan;
#endif
    std::unique_ptr<QOffscreenSurface> glSurface;
    std::unique_ptr<QRhi> rhi;
    std::unique_ptr<Compositor> compositor;

    std::unique_ptr<QRhiTexture> target;
    std::unique_ptr<QRhiTextureRenderTarget> targetRt;
    std::unique_ptr<QRhiRenderPassDescriptor> targetPass;
    QRhiReadbackResult readback;
    QRhiCommandBuffer* lastCb = nullptr;

    ~Impl() {
        // Resources die before their QRhi.
        targetRt.reset();
        targetPass.reset();
        target.reset();
        compositor.reset();
        rhi.reset();
        glSurface.reset();
    }

    std::expected<void, Error> ensureTarget(QSize size) {
        if (target && target->pixelSize() == size) {
            return {};
        }
        targetRt.reset();
        const QRhiTexture::Format format = options.tenBit ? QRhiTexture::RGB10A2 : QRhiTexture::RGBA8; // I.9
        if (options.tenBit && !rhi->isTextureFormatSupported(format)) {
            return std::unexpected(Error{ErrorCode::Unsupported, "This GPU cannot render 10-bit (RGB10A2) frames"});
        }
        target.reset(rhi->newTexture(format, size, 1,
                                     QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!target->create()) {
            target.reset();
            return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the offscreen target"});
        }
        targetRt.reset(rhi->newTextureRenderTarget({target.get()}));
        if (!targetPass) {
            targetPass.reset(targetRt->newCompatibleRenderPassDescriptor());
        }
        targetRt->setRenderPassDescriptor(targetPass.get());
        if (!targetRt->create()) {
            targetRt.reset();
            target.reset();
            return std::unexpected(
                Error{ErrorCode::DeviceError, "Could not create the offscreen render target"});
        }
        return {};
    }

    /// Records one composite into the target; optionally queues a readback.
    std::expected<CompositeStats, Error> record(const std::shared_ptr<const PreparedComposite>& composite,
                                                std::optional<QSize> outputSize, bool readBack) {
        if (!composite) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Null composite"});
        }
        const QSize size = outputSize.value_or(QSize(composite->canvas.width, composite->canvas.height));
        if (size.isEmpty()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Empty output size"});
        }
        if (auto ok = ensureTarget(size); !ok) {
            return std::unexpected(ok.error());
        }
        QRhiCommandBuffer* cb = nullptr;
        const QRhi::FrameOpResult begun = rhi->beginOffscreenFrame(&cb);
        if (begun != QRhi::FrameOpSuccess) {
            return std::unexpected(Error{ErrorCode::DeviceError, begun == QRhi::FrameOpDeviceLost
                                                                     ? "GPU device lost"
                                                                     : "beginOffscreenFrame failed"});
        }
        lastCb = cb;
        CompositeOutput out;
        out.target = targetRt.get();
        out.viewport = QRect(QPoint(0, 0), size);
        out.role = options.role;
        out.alpha = options.alpha;
        auto stats = compositor->render(cb, out, composite);
        if (stats && readBack) {
            readback.completed = nullptr;
            QRhiResourceUpdateBatch* rb = rhi->nextResourceUpdateBatch();
            rb->readBackTexture(QRhiReadbackDescription(target.get()), &readback);
            cb->resourceUpdate(rb);
        }
        const QRhi::FrameOpResult ended = rhi->endOffscreenFrame();
        if (!stats) {
            return std::unexpected(stats.error());
        }
        if (ended != QRhi::FrameOpSuccess) {
            if (ended == QRhi::FrameOpDeviceLost) {
                compositor->releaseResources();
            }
            return std::unexpected(Error{ErrorCode::DeviceError, ended == QRhi::FrameOpDeviceLost
                                                                     ? "GPU device lost"
                                                                     : "endOffscreenFrame failed"});
        }
        return *stats;
    }
};

OffscreenRenderer::OffscreenRenderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
OffscreenRenderer::~OffscreenRenderer() = default;

std::expected<std::unique_ptr<OffscreenRenderer>, Error>
OffscreenRenderer::create(const OffscreenOptions& options) {
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    const QByteArray env = options.honourEnvironment ? qgetenv("QSG_RHI_BACKEND") : QByteArray();
    RhiBackendRequest backend = resolveRhiBackend(
        options.backend, std::string_view(env.constData(), static_cast<std::size_t>(env.size())));

    QRhi::Flags flags;
    if (options.timestamps) {
        flags |= QRhi::EnableTimestamps;
    }
    if (backend == RhiBackendRequest::Vulkan) {
#if defined(WABCUT_RENDER_HAVE_VULKAN)
        impl->vulkan = std::make_unique<QVulkanInstance>();
        impl->vulkan->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (impl->vulkan->create()) {
            QRhiVulkanInitParams params;
            params.inst = impl->vulkan.get();
            impl->rhi.reset(QRhi::create(QRhi::Vulkan, &params, flags));
        }
#endif
        if (!impl->rhi && options.backend == RhiBackendRequest::PlatformDefault) {
            backend = RhiBackendRequest::OpenGL; // the Linux policy: Vulkan, then OpenGL
        }
    }
    switch (backend) {
    case RhiBackendRequest::D3D11: {
#if defined(Q_OS_WIN)
        QRhiD3D11InitParams params;
        params.enableDebugLayer = options.debugLayer;
        // Issue #10: the preview's adapter (media::previewGpu()), so export renders, decodes and previews on one GPU
        // (the CLI too, which sets no QT_D3D_ADAPTER_INDEX; Qt lets that variable win over the LUID).
        QRhiD3D11NativeHandles adapter;
        const std::uint64_t luid = media::previewAdapterLuid();
        adapter.adapterLuidLow = static_cast<quint32>(luid & 0xFFFFFFFFu);
        adapter.adapterLuidHigh = static_cast<qint32>(luid >> 32);
        impl->rhi.reset(QRhi::create(QRhi::D3D11, &params, flags, luid != 0 ? &adapter : nullptr));
#endif
        break;
    }
    case RhiBackendRequest::D3D12: {
#if defined(Q_OS_WIN)
        QRhiD3D12InitParams params;
        params.enableDebugLayer = options.debugLayer;
        QRhiD3D12NativeHandles adapter;
        const std::uint64_t luid = media::previewAdapterLuid();
        adapter.adapterLuidLow = static_cast<quint32>(luid & 0xFFFFFFFFu);
        adapter.adapterLuidHigh = static_cast<qint32>(luid >> 32);
        impl->rhi.reset(QRhi::create(QRhi::D3D12, &params, flags, luid != 0 ? &adapter : nullptr));
#endif
        break;
    }
    case RhiBackendRequest::Metal: {
#if QT_CONFIG(metal)
        QRhiMetalInitParams params;
        impl->rhi.reset(QRhi::create(QRhi::Metal, &params, flags));
#endif
        break;
    }
    case RhiBackendRequest::OpenGL: {
#if QT_CONFIG(opengl)
        impl->glSurface.reset(QRhiGles2InitParams::newFallbackSurface());
        QRhiGles2InitParams params;
        params.fallbackSurface = impl->glSurface.get();
        impl->rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, flags));
#endif
        break;
    }
    case RhiBackendRequest::Null: {
        QRhiNullInitParams params;
        impl->rhi.reset(QRhi::create(QRhi::Null, &params, flags));
        break;
    }
    case RhiBackendRequest::Vulkan:
    case RhiBackendRequest::PlatformDefault:
        break;
    }
    if (!impl->rhi) {
        return std::unexpected(
            Error{ErrorCode::DeviceError,
                  "Could not create a " + std::string(rhiBackendRequestName(backend)) + " QRhi"});
    }
    impl->compositor = std::make_unique<Compositor>(impl->rhi.get(), options.compositor);
    if (options.shareDeviceWithDecoders) {
        (void)shareDeviceWithDecoders(impl->rhi.get()); // best effort: decoders fall back to CPU frames
    }
    return std::unique_ptr<OffscreenRenderer>(new OffscreenRenderer(std::move(impl)));
}

QRhi* OffscreenRenderer::rhi() const {
    return m_impl->rhi.get();
}

std::string OffscreenRenderer::backendName() const {
    switch (m_impl->rhi->backend()) {
    case QRhi::D3D11:
        return "D3D11";
    case QRhi::D3D12:
        return "D3D12";
    case QRhi::Vulkan:
        return "Vulkan";
    case QRhi::OpenGLES2:
        return "OpenGL";
    case QRhi::Metal:
        return "Metal";
    case QRhi::Null:
        return "Null";
    }
    return m_impl->rhi->backendName();
}

std::string OffscreenRenderer::adapterName() const {
    return m_impl->rhi->driverInfo().deviceName.toStdString();
}

Compositor& OffscreenRenderer::compositor() {
    return *m_impl->compositor;
}

const OffscreenOptions& OffscreenRenderer::options() const {
    return m_impl->options;
}

std::expected<void, Error>
OffscreenRenderer::render(const std::shared_ptr<const PreparedComposite>& composite, RenderedImage& into,
                          std::optional<QSize> outputSize) {
    auto stats = m_impl->record(composite, outputSize, true);
    if (!stats) {
        return std::unexpected(stats.error());
    }
    const QRhiReadbackResult& rb = m_impl->readback;
    const int w = rb.pixelSize.width();
    const int h = rb.pixelSize.height();
    const auto rowBytes = static_cast<std::size_t>(w) * 4;
    if (w <= 0 || h <= 0 ||
        static_cast<std::size_t>(rb.data.size()) < rowBytes * static_cast<std::size_t>(h)) {
        return std::unexpected(Error{ErrorCode::DeviceError, "Readback returned no data"});
    }
    into.width = w;
    into.height = h;
    into.tenBit = m_impl->options.tenBit;
    into.revision = composite->revision;
    into.targetTime = composite->targetTime;
    into.rgba.resize(rowBytes * static_cast<std::size_t>(h));
    const bool flip = m_impl->rhi->isYUpInFramebuffer();
    const auto* src = reinterpret_cast<const std::uint8_t*>(rb.data.constData());
    for (int y = 0; y < h; ++y) {
        const int srcRow = flip ? h - 1 - y : y;
        std::memcpy(into.rgba.data() + static_cast<std::size_t>(y) * rowBytes,
                    src + static_cast<std::size_t>(srcRow) * rowBytes, rowBytes);
    }
    return {};
}

std::expected<RenderedImage, Error>
OffscreenRenderer::render(const std::shared_ptr<const PreparedComposite>& composite,
                          std::optional<QSize> outputSize) {
    RenderedImage image;
    if (auto ok = render(composite, image, outputSize); !ok) {
        return std::unexpected(ok.error());
    }
    return image;
}

std::expected<OffscreenTiming, Error>
OffscreenRenderer::renderTimed(const std::shared_ptr<const PreparedComposite>& composite,
                               std::optional<QSize> outputSize) {
    const auto start = std::chrono::steady_clock::now();
    auto stats = m_impl->record(composite, outputSize, false);
    if (!stats) {
        return std::unexpected(stats.error());
    }
    // Without a readback the frame may still be executing; finish() waits for the GPU.
    m_impl->rhi->finish();
    OffscreenTiming timing;
    timing.cpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // Backends that time offscreen frames report it here (QRhi's D3D11 backend only times swapchain frames).
    if (m_impl->options.timestamps && m_impl->lastCb && m_impl->rhi->isFeatureSupported(QRhi::Timestamps)) {
        const double seconds = m_impl->lastCb->lastCompletedGpuTime();
        timing.gpuMs = seconds > 0.0 ? seconds * 1000.0 : -1.0;
    }
    timing.composite = *stats;
    return timing;
}

// ---- AsyncOffscreenRenderer ----

AsyncOffscreenRenderer::AsyncOffscreenRenderer(AsyncOffscreenOptions options, Callback onFrame)
    : m_options(std::move(options)), m_onFrame(std::move(onFrame)) {
    m_options.maxInFlight = std::max(1, m_options.maxInFlight);
}

std::expected<std::unique_ptr<AsyncOffscreenRenderer>, Error>
AsyncOffscreenRenderer::create(const AsyncOffscreenOptions& options, Callback onFrame) {
    std::unique_ptr<AsyncOffscreenRenderer> self(new AsyncOffscreenRenderer(options, std::move(onFrame)));
    std::promise<std::expected<void, Error>> started;
    auto ready = started.get_future();
    AsyncOffscreenRenderer* raw = self.get();
    self->m_thread = std::thread([raw, &started] { raw->run(&started); });
    auto result = ready.get();
    if (!result) {
        self->m_thread.join();
        return std::unexpected(result.error());
    }
    return self;
}

AsyncOffscreenRenderer::~AsyncOffscreenRenderer() {
    stop();
}

void AsyncOffscreenRenderer::run(std::promise<std::expected<void, Error>>* started) {
    auto renderer = OffscreenRenderer::create(m_options.renderer);
    if (!renderer) {
        started->set_value(std::unexpected(renderer.error()));
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_backend = (*renderer)->backendName();
    }
    started->set_value({});

    RenderedImage image;
    for (;;) {
        std::pair<std::uint64_t, std::shared_ptr<const PreparedComposite>> job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                break; // stopping with nothing left
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        auto ok = (*renderer)->render(job.second, image);
        job.second.reset(); // release the composite before the callback runs
        AsyncFrameResult result;
        result.sequence = job.first;
        if (ok) {
            result.image = &image;
        } else {
            result.error = ok.error();
        }
        if (m_onFrame) {
            m_onFrame(result);
        }
        {
            std::lock_guard lock(m_mutex);
            --m_inFlight;
            ++(ok ? m_stats.delivered : m_stats.failed);
        }
        m_room.notify_all();
    }
}

std::expected<std::uint64_t, Error>
AsyncOffscreenRenderer::submit(std::shared_ptr<const PreparedComposite> composite) {
    std::unique_lock lock(m_mutex);
    if (m_inFlight >= m_options.maxInFlight && !m_stopping) {
        ++m_stats.blockedSubmits;
        m_room.wait(lock, [&] { return m_stopping || m_inFlight < m_options.maxInFlight; });
    }
    if (m_stopping) {
        return std::unexpected(Error{ErrorCode::Cancelled, "Offscreen renderer stopped"});
    }
    const std::uint64_t sequence = m_nextSequence++;
    m_queue.emplace_back(sequence, std::move(composite));
    ++m_inFlight;
    ++m_stats.submitted;
    m_stats.maxQueued = std::max(m_stats.maxQueued, m_inFlight);
    lock.unlock();
    m_wake.notify_one();
    return sequence;
}

std::optional<std::uint64_t>
AsyncOffscreenRenderer::trySubmit(std::shared_ptr<const PreparedComposite> composite) {
    std::unique_lock lock(m_mutex);
    if (m_stopping || m_inFlight >= m_options.maxInFlight) {
        return std::nullopt;
    }
    const std::uint64_t sequence = m_nextSequence++;
    m_queue.emplace_back(sequence, std::move(composite));
    ++m_inFlight;
    ++m_stats.submitted;
    m_stats.maxQueued = std::max(m_stats.maxQueued, m_inFlight);
    lock.unlock();
    m_wake.notify_one();
    return sequence;
}

void AsyncOffscreenRenderer::flush() {
    std::unique_lock lock(m_mutex);
    m_room.wait(lock, [&] { return m_inFlight == 0 || !m_thread.joinable(); });
}

void AsyncOffscreenRenderer::stop() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    m_room.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

std::string AsyncOffscreenRenderer::backendName() const {
    std::lock_guard lock(m_mutex);
    return m_backend;
}

AsyncOffscreenStats AsyncOffscreenRenderer::stats() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
}

} // namespace wabcut::render
