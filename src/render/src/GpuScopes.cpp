#include <wabcut/render/GpuScopes.h>

#include <wabcut/color/ColorSpaces.h>

#include <QFile>
#include <QMatrix4x4>

#include <algorithm>
#include <cstring>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

namespace wabcut::render {

namespace {

// shaders/scopes.comp (std140).
struct ScopeUniforms {
    std::int32_t size[4];
    std::int32_t dims[4];
    float zoom[4];
    std::int32_t waves[4];
    std::int32_t waves2[4];
    float hdr[4]; ///< I.9: reference white, peak, HLG system gamma
};
static_assert(sizeof(ScopeUniforms) == 96);

// shaders/output.vert + output.frag (the fallback's downsampling pass).
struct OutputUniforms {
    float cornerToClip[16];
    float uvXform[4];
    std::int32_t mode[4];
    float toOutput[16]; ///< I.9 (output.frag): working -> Rec.2020
    float cm[4];        ///< I.9: reference white, HLG peak, HLG system gamma, output LUT size
};
static_assert(sizeof(OutputUniforms) == 176);

constexpr int kGroup = 16;

QShader loadShader(const char* name) {
    QFile file(QStringLiteral(":/wabcut/render/shaders/%1.qsb").arg(QLatin1String(name)));
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader();
}

std::int32_t transformCode(OutputTransform t) {
    return t == OutputTransform::SRGB ? 1 : t == OutputTransform::Linear ? 2 : 0;
}

/// Bin offsets inside the storage buffer, in uints.
struct Layout {
    int luma = 0, r = 0, g = 0, b = 0, cb = 0, cr = 0, vector = 0, histogram = 0, total = 0;
};

Layout layoutOf(const color::ScopeSettings& s) {
    Layout l;
    const int wave = s.columns * s.levels;
    l.luma = 0;
    l.r = wave;
    l.g = 2 * wave;
    l.b = 3 * wave;
    l.cb = 4 * wave;
    l.cr = 5 * wave;
    l.vector = 6 * wave;
    l.histogram = l.vector + s.vectorSize * s.vectorSize;
    l.total = l.histogram + 4 * s.levels;
    return l;
}

constexpr float kQuad[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};

} // namespace

struct GpuScopes::Impl {
    QRhi* rhi = nullptr;
    // Compute path.
    std::unique_ptr<QRhiBuffer> clearUniforms;
    std::unique_ptr<QRhiBuffer> accumulateUniforms;
    std::unique_ptr<QRhiBuffer> bins;
    int binCount = 0;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiComputePipeline> pipeline;
    std::unique_ptr<QRhiShaderResourceBindings> clearSrb;
    std::unique_ptr<QRhiShaderResourceBindings> accumulateSrb;
    QRhiTexture* boundCanvas = nullptr;
    QRhiReadbackResult bufferReadback;
    // Fallback path.
    std::unique_ptr<QRhiTexture> small;
    std::unique_ptr<QRhiTextureRenderTarget> smallTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> smallPass;
    std::unique_ptr<QRhiBuffer> quad;
    std::unique_ptr<QRhiBuffer> outputUniforms;
    std::unique_ptr<QRhiShaderResourceBindings> outputSrb;
    std::unique_ptr<QRhiTexture> dummy3d; // output.frag's LUT binding (I.9), unused here
    std::unique_ptr<QRhiGraphicsPipeline> outputPipeline;
    QRhiTexture* outputCanvas = nullptr;
    bool quadUploaded = false;
    QRhiReadbackResult textureReadback;

    bool pending = false;
    color::ScopeSettings settings;
    int scale = 0; // ScopeData::scale of the request
    QSize canvasPixels;
    int step = 1;
    std::function<void(color::ScopeData)> done;

    bool ensureCompute(QRhiTexture* canvas);
    bool ensureFallback(QRhiTexture* canvas, QSize smallSize);
    void deliverCompute();
    void deliverFallback();
};

bool GpuScopes::Impl::ensureCompute(QRhiTexture* canvas) {
    const int needed = layoutOf(settings).total;
    if (!pipeline) {
        const QShader cs = loadShader("scopes.comp");
        if (!cs.isValid()) {
            return false;
        }
        clearUniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ScopeUniforms)));
        accumulateUniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ScopeUniforms)));
        sampler.reset(rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
        if (!clearUniforms->create() || !accumulateUniforms->create() || !sampler->create()) {
            return false;
        }
        bins.reset(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, static_cast<quint32>(needed) * 4));
        if (!bins->create()) {
            return false;
        }
        binCount = needed;
        // Layout-only bindings for the pipeline.
        std::unique_ptr<QRhiShaderResourceBindings> layout(rhi->newShaderResourceBindings());
        layout->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::ComputeStage, clearUniforms.get()),
                             QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::ComputeStage, nullptr, sampler.get()),
                             QRhiShaderResourceBinding::bufferLoadStore(2, QRhiShaderResourceBinding::ComputeStage, bins.get())});
        layout->create();
        pipeline.reset(rhi->newComputePipeline());
        pipeline->setShaderStage({QRhiShaderStage::Compute, cs});
        pipeline->setShaderResourceBindings(layout.get());
        if (!pipeline->create()) {
            pipeline.reset();
            return false;
        }
        boundCanvas = nullptr;
    }
    if (binCount < needed) {
        bins.reset(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, static_cast<quint32>(needed) * 4));
        if (!bins->create()) {
            return false;
        }
        binCount = needed;
        boundCanvas = nullptr;
    }
    if (boundCanvas != canvas || !clearSrb || !accumulateSrb) {
        const auto make = [&](QRhiBuffer* uniforms) {
            std::unique_ptr<QRhiShaderResourceBindings> srb(rhi->newShaderResourceBindings());
            srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::ComputeStage, uniforms),
                              QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::ComputeStage, canvas, sampler.get()),
                              QRhiShaderResourceBinding::bufferLoadStore(2, QRhiShaderResourceBinding::ComputeStage, bins.get())});
            return srb->create() ? std::move(srb) : nullptr;
        };
        clearSrb = make(clearUniforms.get());
        accumulateSrb = make(accumulateUniforms.get());
        boundCanvas = canvas;
    }
    return clearSrb && accumulateSrb;
}

bool GpuScopes::Impl::ensureFallback(QRhiTexture* canvas, QSize smallSize) {
    if (!small || small->pixelSize() != smallSize) {
        outputPipeline.reset();
        outputSrb.reset();
        smallTarget.reset();
        small.reset(rhi->newTexture(QRhiTexture::RGBA8, smallSize, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!small->create()) {
            small.reset();
            return false;
        }
        smallTarget.reset(rhi->newTextureRenderTarget({small.get()}));
        smallPass.reset(smallTarget->newCompatibleRenderPassDescriptor());
        smallTarget->setRenderPassDescriptor(smallPass.get());
        if (!smallTarget->create()) {
            return false;
        }
    }
    if (!quad) {
        quad.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
        outputUniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(OutputUniforms)));
        if (!sampler) {
            sampler.reset(rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                          QRhiSampler::ClampToEdge));
            sampler->create();
        }
        dummy3d.reset(rhi->newTexture(QRhiTexture::RGBA8, 1, 1, 1, 1, QRhiTexture::ThreeDimensional));
        if (!quad->create() || !outputUniforms->create() || !dummy3d->create()) {
            return false;
        }
        quadUploaded = false;
    }
    if (!outputSrb || outputCanvas != canvas) {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        outputSrb.reset(rhi->newShaderResourceBindings());
        outputSrb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, outputUniforms.get()),
                                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, canvas,
                                                                          sampler.get()),
                                QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                                          dummy3d.get(), sampler.get())});
        if (!outputSrb->create()) {
            return false;
        }
        outputCanvas = canvas;
    }
    if (!outputPipeline) {
        const QShader vs = loadShader("output.vert");
        const QShader fs = loadShader("output.frag");
        if (!vs.isValid() || !fs.isValid()) {
            return false;
        }
        outputPipeline.reset(rhi->newGraphicsPipeline());
        outputPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        outputPipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
        QRhiVertexInputLayout input;
        input.setBindings({{2 * sizeof(float)}});
        input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        outputPipeline->setVertexInputLayout(input);
        outputPipeline->setShaderResourceBindings(outputSrb.get());
        outputPipeline->setRenderPassDescriptor(smallPass.get());
        if (!outputPipeline->create()) {
            outputPipeline.reset();
            return false;
        }
    }
    return true;
}

void GpuScopes::Impl::deliverCompute() {
    pending = false;
    const Layout l = layoutOf(settings);
    color::ScopeData d;
    d.settings = settings;
    d.sourceWidth = canvasPixels.width();
    d.sourceHeight = canvasPixels.height();
    d.step = step;
    const auto* bytes = reinterpret_cast<const std::uint32_t*>(bufferReadback.data.constData());
    const std::size_t available = static_cast<std::size_t>(bufferReadback.data.size()) / 4;
    const auto take = [&](int offset, int count) {
        std::vector<std::uint32_t> out(static_cast<std::size_t>(count), 0);
        if (static_cast<std::size_t>(offset + count) <= available) {
            std::memcpy(out.data(), bytes + offset, static_cast<std::size_t>(count) * 4);
        }
        return out;
    };
    const int wave = settings.columns * settings.levels;
    d.waveLuma = take(l.luma, wave);
    d.waveRgb = {take(l.r, wave), take(l.g, wave), take(l.b, wave)};
    d.waveCbCr = {take(l.cb, wave), take(l.cr, wave)};
    d.vector = take(l.vector, settings.vectorSize * settings.vectorSize);
    for (int c = 0; c < 4; ++c) {
        d.histogram[static_cast<std::size_t>(c)] = take(l.histogram + c * settings.levels, settings.levels);
    }
    std::uint64_t pixels = 0;
    for (const std::uint32_t n : d.histogram[3]) {
        pixels += n;
    }
    d.pixels = pixels;
    d.scale = scale;
    if (done) {
        auto callback = std::move(done);
        done = nullptr;
        callback(std::move(d));
    }
}

void GpuScopes::Impl::deliverFallback() {
    pending = false;
    const int w = textureReadback.pixelSize.width();
    const int h = textureReadback.pixelSize.height();
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const bool flip = rhi->isYUpInFramebuffer();
    const auto* src = reinterpret_cast<const std::uint8_t*>(textureReadback.data.constData());
    const std::size_t row = static_cast<std::size_t>(w) * 4;
    if (static_cast<std::size_t>(textureReadback.data.size()) >= row * static_cast<std::size_t>(h)) {
        for (int y = 0; y < h; ++y) {
            std::memcpy(rgba.data() + static_cast<std::size_t>(y) * row, src + static_cast<std::size_t>(flip ? h - 1 - y : y) * row, row);
        }
    }
    // The small picture holds the step grid; bin it as if it were the full canvas (columns by source position).
    color::ScopeSettings s = settings;
    s.step = 1;
    color::ScopeData d = color::computeScopes8(rgba.data(), w, h, static_cast<int>(row), s);
    d.settings = settings;
    d.sourceWidth = canvasPixels.width();
    d.sourceHeight = canvasPixels.height();
    d.step = step;
    d.scale = scale;
    if (done) {
        auto callback = std::move(done);
        done = nullptr;
        callback(std::move(d));
    }
}

GpuScopes::GpuScopes(QRhi* rhi) : m_impl(std::make_unique<Impl>()) { m_impl->rhi = rhi; }

GpuScopes::~GpuScopes() { releaseResources(); }

bool GpuScopes::computeSupported() const { return m_impl->rhi && m_impl->rhi->isFeatureSupported(QRhi::Compute); }

bool GpuScopes::pending() const { return m_impl->pending; }

void GpuScopes::releaseResources() {
    Impl& i = *m_impl;
    if (i.pending && i.rhi) {
        i.rhi->finish(); // completes the readback, which writes into this object
    }
    i.clearSrb.reset();
    i.accumulateSrb.reset();
    i.pipeline.reset();
    i.bins.reset();
    i.binCount = 0;
    i.clearUniforms.reset();
    i.accumulateUniforms.reset();
    i.outputPipeline.reset();
    i.outputSrb.reset();
    i.smallTarget.reset();
    i.smallPass.reset();
    i.small.reset();
    i.quad.reset();
    i.outputUniforms.reset();
    i.dummy3d.reset();
    i.sampler.reset();
    i.boundCanvas = nullptr;
    i.outputCanvas = nullptr;
    i.pending = false;
    i.done = nullptr;
}

std::expected<void, Error> GpuScopes::record(QRhiCommandBuffer* cb, QRhiTexture* canvas, QSize pixels, OutputTransform transform,
                                             const color::ScopeSettings& settings, std::function<void(color::ScopeData)> done,
                                             std::optional<ScopeHdr> hdr) {
    Impl& i = *m_impl;
    if (!cb || !canvas || pixels.isEmpty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "scopes need a command buffer and a canvas"});
    }
    if (i.pending) {
        return std::unexpected(Error{ErrorCode::Unsupported, "a scope readback is still in flight"});
    }
    i.settings = settings;
    i.canvasPixels = pixels;
    i.step = color::scopeStep(settings, pixels.width(), pixels.height());
    i.done = std::move(done);
    i.scale = hdr ? (hdr->hlg ? 2 : 1) : 0;
    // I.9: HDR scopes read the working values as PQ (3) or HLG (4).
    const std::int32_t code = hdr ? (hdr->hlg ? 4 : 3) : transformCode(transform);
    const float hdrParams[4] = {static_cast<float>(hdr ? hdr->referenceWhiteNits : 203.0),
                                static_cast<float>(hdr ? hdr->peakNits : 1000.0),
                                static_cast<float>(color::hlgSystemGamma(hdr ? hdr->peakNits : 1000.0)), 0.0f};
    const bool yUp = i.rhi->isYUpInFramebuffer();
    if (computeSupported() && !m_forceCpu && i.ensureCompute(canvas)) {
        const Layout l = layoutOf(settings);
        ScopeUniforms u{};
        u.size[0] = pixels.width();
        u.size[1] = pixels.height();
        u.size[2] = i.step;
        u.size[3] = code;
        std::memcpy(u.hdr, hdrParams, sizeof(u.hdr));
        u.dims[0] = settings.columns;
        u.dims[1] = settings.levels;
        u.dims[2] = settings.vectorSize;
        u.zoom[0] = static_cast<float>(settings.vectorZoom);
        u.zoom[1] = static_cast<float>(l.total);
        u.zoom[2] = yUp ? 1.0f : 0.0f;
        u.zoom[3] = 1024.0f; // clear mode: invocations per row
        u.waves[0] = l.luma;
        u.waves[1] = l.r;
        u.waves[2] = l.g;
        u.waves[3] = l.b;
        u.waves2[0] = l.cb;
        u.waves2[1] = l.cr;
        u.waves2[2] = l.vector;
        u.waves2[3] = l.histogram;
        ScopeUniforms clear = u;
        clear.dims[3] = 1;
        QRhiResourceUpdateBatch* updates = i.rhi->nextResourceUpdateBatch();
        updates->updateDynamicBuffer(i.clearUniforms.get(), 0, sizeof(clear), &clear);
        updates->updateDynamicBuffer(i.accumulateUniforms.get(), 0, sizeof(u), &u);
        // Clear: one invocation per bin (rows of 1024).
        cb->beginComputePass(updates);
        cb->setComputePipeline(i.pipeline.get());
        cb->setShaderResources(i.clearSrb.get());
        const int clearWidth = 1024;
        cb->dispatch(clearWidth / kGroup, (l.total + clearWidth - 1) / clearWidth / kGroup + 1, 1);
        cb->endComputePass();
        // Accumulate: one invocation per sampled pixel; read back after.
        QRhiResourceUpdateBatch* readback = i.rhi->nextResourceUpdateBatch();
        i.bufferReadback.completed = [&i]() { i.deliverCompute(); };
        readback->readBackBuffer(i.bins.get(), 0, static_cast<quint32>(l.total) * 4, &i.bufferReadback);
        cb->beginComputePass();
        cb->setComputePipeline(i.pipeline.get());
        cb->setShaderResources(i.accumulateSrb.get());
        const int gx = (pixels.width() + i.step - 1) / i.step;
        const int gy = (pixels.height() + i.step - 1) / i.step;
        cb->dispatch((gx + kGroup - 1) / kGroup, (gy + kGroup - 1) / kGroup, 1);
        cb->endComputePass(readback);
        i.pending = true;
        return {};
    }
    // Fallback: the canvas through the output transform into a small target on the step grid, then the CPU.
    const QSize smallSize((pixels.width() + i.step - 1) / i.step, (pixels.height() + i.step - 1) / i.step);
    if (!i.ensureFallback(canvas, smallSize)) {
        i.done = nullptr;
        return std::unexpected(Error{ErrorCode::DeviceError, "could not create the scope readback resources"});
    }
    QRhiResourceUpdateBatch* updates = i.rhi->nextResourceUpdateBatch();
    if (!i.quadUploaded) {
        updates->uploadStaticBuffer(i.quad.get(), kQuad);
        i.quadUploaded = true;
    }
    OutputUniforms u{};
    // Clip space: the unit quad covers the target.
    const QMatrix4x4 corr = i.rhi->clipSpaceCorrMatrix();
    QMatrix4x4 toClip;
    toClip.translate(-1.0f, 1.0f);
    toClip.scale(2.0f, -2.0f);
    std::memcpy(u.cornerToClip, (corr * toClip).constData(), sizeof(u.cornerToClip));
    // Small pixel k's centre samples canvas pixel k * step's centre.
    const double sx = static_cast<double>(smallSize.width()) * i.step / pixels.width();
    const double sy = static_cast<double>(smallSize.height()) * i.step / pixels.height();
    const double ox = (0.5 - 0.5 * i.step) / pixels.width();
    const double oy = (0.5 - 0.5 * i.step) / pixels.height();
    u.uvXform[0] = static_cast<float>(sx);
    u.uvXform[1] = static_cast<float>(yUp ? -sy : sy);
    u.uvXform[2] = static_cast<float>(ox);
    u.uvXform[3] = static_cast<float>(yUp ? 1.0 - oy : oy);
    u.mode[0] = code; // output.frag: 3 = OUT_PQ, 4 = OUT_HLG
    u.mode[1] = 0; // opaque: over black, like the monitor
    for (int k = 0; k < 4; ++k) {
        u.toOutput[k * 5] = 1.0f; // identity: the HDR working space is Rec.2020
    }
    std::memcpy(u.cm, hdrParams, sizeof(u.cm));
    updates->updateDynamicBuffer(i.outputUniforms.get(), 0, sizeof(u), &u);
    cb->beginPass(i.smallTarget.get(), QColor(0, 0, 0, 255), {1.0f, 0}, updates);
    cb->setGraphicsPipeline(i.outputPipeline.get());
    cb->setViewport(QRhiViewport(0, 0, static_cast<float>(smallSize.width()), static_cast<float>(smallSize.height())));
    cb->setShaderResources(i.outputSrb.get());
    const QRhiCommandBuffer::VertexInput quad(i.quad.get(), 0);
    cb->setVertexInput(0, 1, &quad);
    cb->draw(4);
    QRhiResourceUpdateBatch* readback = i.rhi->nextResourceUpdateBatch();
    i.textureReadback.completed = [&i]() { i.deliverFallback(); };
    readback->readBackTexture(QRhiReadbackDescription(i.small.get()), &i.textureReadback);
    cb->endPass(readback);
    i.pending = true;
    return {};
}

} // namespace wabcut::render
