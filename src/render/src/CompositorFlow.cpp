// The compositor's flow mix (P.2, docs/motion.md): pipeline, bindings and motion textures of shaders/flowmix.frag.
// The pass itself is planned with the other transition mixes in Compositor::renderImpl.

#include <wabcut/render/Compositor.h>

#include <QFile>

#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <cstring>

namespace wabcut::render {

namespace {

QShader loadFlowShader(const char* name) {
    QFile file(QStringLiteral(":/wabcut/render/shaders/%1.qsb").arg(QLatin1String(name)));
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader();
}

/// Largest side of a motion texture: the analysis never makes larger ones (motion::kAnalysisSide); a bound against
/// anything else.
constexpr int kMaxMotionSide = 4096;

} // namespace

std::expected<void, Error> Compositor::ensureFlowPipeline(std::uint32_t blockSize) {
    if (m_flowPipeline) {
        return {};
    }
    const QShader vs = loadFlowShader("fx.vert");
    const QShader fs = loadFlowShader("flowmix.frag");
    if (!vs.isValid() || !fs.isValid()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The flow mix shaders are missing from the resources"});
    }
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    const auto fragment = QRhiShaderResourceBinding::FragmentStage;
    m_flowLayoutSrb.reset(m_rhi->newShaderResourceBindings());
    m_flowLayoutSrb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(), blockSize),
        QRhiShaderResourceBinding::sampledTexture(1, fragment, m_dummy.get(), m_canvasSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, fragment, m_dummy.get(), m_canvasSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, fragment, m_dummy.get(), m_canvasSampler.get()),
    });
    if (!m_flowLayoutSrb->create()) {
        m_flowLayoutSrb.reset();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the flow mix bindings"});
    }
    std::unique_ptr<QRhiGraphicsPipeline> pipeline(m_rhi->newGraphicsPipeline());
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    QRhiVertexInputLayout input;
    input.setBindings({{2 * sizeof(float)}});
    input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    pipeline->setVertexInputLayout(input);
    pipeline->setShaderResourceBindings(m_flowLayoutSrb.get());
    pipeline->setRenderPassDescriptor(m_canvasPass.get());
    if (!pipeline->create()) {
        m_flowLayoutSrb.reset();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the flow mix pipeline"});
    }
    m_flowPipeline = std::move(pipeline);
    return {};
}

QRhiTexture* Compositor::motionTexture(const std::shared_ptr<const motion::FlowPair>& motion, QRhiResourceUpdateBatch* updates) {
    if (!motion || motion->empty() || motion->width > kMaxMotionSide || motion->height > kMaxMotionSide ||
        motion->data.size() != static_cast<std::size_t>(motion->width) * static_cast<std::size_t>(motion->height) * 4) {
        return nullptr;
    }
    MotionTexture& entry = m_motionTextures[motion.get()];
    entry.lastUsedFrame = m_frameCounter;
    if (entry.texture && entry.motion == motion) {
        return entry.texture.get();
    }
    entry.motion = motion;
    entry.texture.reset(m_rhi->newTexture(QRhiTexture::RGBA32F, QSize(motion->width, motion->height)));
    if (!entry.texture->create()) {
        m_motionTextures.erase(motion.get());
        return nullptr;
    }
    QRhiTextureSubresourceUploadDescription data(motion->data.data(),
                                                 static_cast<quint32>(motion->data.size() * sizeof(float)));
    data.setDataStride(static_cast<quint32>(motion->width) * 4u * static_cast<quint32>(sizeof(float)));
    updates->uploadTexture(entry.texture.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, data)));
    m_flowSrbs.clear(); // bindings may name a texture this slot held before
    return entry.texture.get();
}

QRhiShaderResourceBindings* Compositor::flowSrb(QRhiTexture* a, QRhiTexture* b, QRhiTexture* motion, std::uint32_t blockSize) {
    FlowSrb& entry = m_flowSrbs[{a, b, motion}];
    if (!entry.srb || entry.uniformGeneration != m_uniformGeneration) {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        const auto fragment = QRhiShaderResourceBinding::FragmentStage;
        entry.srb.reset(m_rhi->newShaderResourceBindings());
        entry.srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(), blockSize),
            QRhiShaderResourceBinding::sampledTexture(1, fragment, a, m_canvasSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, fragment, b, m_canvasSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, fragment, motion, m_canvasSampler.get()),
        });
        if (!entry.srb->create()) {
            entry.srb.reset();
            return nullptr;
        }
        entry.uniformGeneration = m_uniformGeneration;
    }
    return entry.srb.get();
}

void Compositor::evictMotionTextures() {
    const std::size_t before = m_motionTextures.size();
    std::erase_if(m_motionTextures, [&](const auto& e) { return e.second.lastUsedFrame + 64 < m_frameCounter; });
    if (m_motionTextures.size() != before) {
        m_flowSrbs.clear(); // they may name the textures just dropped
    }
}

void Compositor::releaseFlowResources() {
    m_flowSrbs.clear();
    m_flowPipeline.reset();
    m_flowLayoutSrb.reset();
    m_motionTextures.clear();
}

} // namespace wabcut::render
