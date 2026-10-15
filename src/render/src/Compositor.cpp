#include <wabcut/render/Compositor.h>

#include <wabcut/color/Hdr.h>

#include <QColor>
#include <QFloat16>
#include <QFile>
#include <QMatrix4x4>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <string>

namespace wabcut::render {

namespace {

// std140 mirrors of the shader uniform blocks (see shaders/*.vert).
struct LayerUniforms {
    float cornerToClip[16];
    float sampleToRgb[16];
    float toWorking[16];
    float chromaXform[4];
    float srcRect[4];
    float params[4];
    std::int32_t mode[4];
    float cm[4]; ///< I.9 (gaming): HDR captures (layer.frag cm)
};
static_assert(sizeof(LayerUniforms) == 272);

struct OutputUniforms {
    float cornerToClip[16];
    float uvXform[4];
    std::int32_t mode[4];
};
static_assert(sizeof(OutputUniforms) == 96);

// shaders/fx.vert + fx.frag
struct FxUniforms {
    float cornerToClip[16];
    float texel[4];
    float p0[4];
    float p1[4];
    float p2[4];
    float toFrame0[4];
    float toFrame1[4];
    float frame[4];
    std::int32_t mode[4];
};
static_assert(sizeof(FxUniforms) == 192);

// shaders/mask.vert + mask.frag (std140; I.6).
struct MaskUniforms {
    float cornerToClip[16];
    float texel[4];
    float toFrame0[4];
    float toFrame1[4];
    float frame[4];
    std::int32_t mode[4];
    float domain[4];
    float sdfDomain[4];
    float style[4];
    float aspect[4];
    float affine0[16];
    float affine1[16];
    float edge[16];
    std::int32_t combine[16];
};
static_assert(sizeof(MaskUniforms) == 464);

constexpr std::int32_t kMaskSdf = 0;
constexpr std::int32_t kMaskCompose = 1;
constexpr std::int32_t kMaskMix = 2;
constexpr std::int32_t kMaskAlpha = 3;
constexpr std::int32_t kMaskMosaic = 4;
constexpr std::int32_t kMaskSolid = 5;
constexpr int kInitialMaskSlots = 16;
/// Largest side of a mask texture (coverage and distances); masks are sampled with bilinear filtering.
constexpr int kMaxMaskTexture = 2048;
constexpr int kShapesPerCompose = 4;

// shaders/grade.vert + grade.frag (std140; mirrors GradeParams there).
struct GradeUniforms {
    float cornerToClip[16];
    float texel[4];
    float toFrame0[4];
    float toFrame1[4];
    float frame[4];
    std::int32_t mode[4];
    std::int32_t lut[4];
    std::int32_t lutSize[4];
    float in1dMin[4];
    float in1dMax[4];
    float in3dMin[4];
    float in3dMax[4];
    float look1dMin[4];
    float look1dMax[4];
    float look3dMin[4];
    float look3dMax[4];
    float wb[16];
    float hslWb[16];
    float basic[4];
    float toneY[4];
    float toneM0[4];
    float toneM1[4];
    float creative[4];
    float shadowTint[4];
    float highlightTint[4];
    float cdlSlope[4];
    float cdlOffset[4];
    float cdlPower[4];
    float hslHue[4];
    float hslSat[4];
    float hslLuma[4];
    float hslCorr[4];
    float hslOffset[4];
    float vignette[4];
    float split[4];
};
static_assert(sizeof(GradeUniforms) == 704);

enum GradePass : std::int32_t { kGradeMain = 0, kGradeMatte = 1, kGradeThreshold = 2, kGradeApply = 3, kGradeMix = 4 };

// grade.frag mode.z flags.
enum GradeFlag : std::int32_t {
    kGfWb = 1,
    kGfExposure = 2,
    kGfContrast = 4,
    kGfTone = 8,
    kGfSaturation = 16,
    kGfLook = 32,
    kGfFaded = 64,
    kGfTints = 128,
    kGfCreativeSat = 256,
    kGfCurvesRgb = 512,
    kGfCurvesHue = 1024,
    kGfWheels = 2048,
    kGfCdlSat = 4096,
    kGfSecondary = 8192,
    kGfVignette = 16384,
    kGfFinal = 32768,
    kGfInputLut = 65536,
    kGfBasic = 131072,
    kGfCreative = 262144,
    kGfHslWb = 524288,
    kGfHslContrast = 1048576,
    kGfHslSharpen = 2097152,
    kGfHslSat = 4194304,
    kGfCurves = 8388608,
};

/// Rows of the grade's curve texture, and the longest 1D LUT row it stores (longer tables are resampled).
constexpr int kCurveRows = 5;
constexpr int kMax1dEntries = 4096;
/// Grade passes per grade effect (main, matte, threshold, apply, mix) and the fx blur passes it may add (3 blurs).
constexpr int kGradeSlotsPerEffect = 5;
constexpr int kGradeFxSlotsPerEffect = 6;
constexpr int kInitialGradeSlots = 8;

void storeColorMat3(float* out, const color::Mat3& m) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double v = row == col ? 1.0 : 0.0;
            if (row < 3 && col < 3) {
                v = m[static_cast<std::size_t>(row * 3 + col)];
            }
            out[col * 4 + row] = static_cast<float>(v);
        }
    }
}

void storeRgb(float* out, const color::Rgb& v, double w = 0.0) {
    out[0] = static_cast<float>(v[0]);
    out[1] = static_cast<float>(v[1]);
    out[2] = static_cast<float>(v[2]);
    out[3] = static_cast<float>(w);
}

/// The 1D table of a LUT as stored in its texture row (at most kMax1dEntries; CPU reference: Lut::table1d).
int stored1dLength(const color::Lut* lut) { return lut && lut->has1d() ? std::min(lut->size1d, kMax1dEntries) : 0; }

enum FxMode : std::int32_t {
    kFxCopy = 0,
    kFxCrop = 1,
    kFxBlur = 2,
    kFxCorrection = 3,
    kFxVignette = 4,
    kFxSharpen = 5,
    kFxShadow = 6,
    kFxOver = 7,
    kFxTransition = 8,
    kFxBorder = 9,
    kFxKeyMatte = 10, // Ultra Key: the matte before cleanup
    kFxKeyChoke = 11, //            the matte eroded by a disc
    kFxKeyApply = 12, //            contrast, spill suppression, colour correction and the output
    kFxBlend = 13,    // a blend mode over the canvas below the layer (C.15, 0.1.2)
    kFxTrackMatte = 14, // the layer times its track matte (C.15, 0.1.2)
};

/// Offscreen passes an effect needs (uniform slots are reserved up front).
int fxPassCount(EffectType type) {
    switch (type) {
    case EffectType::GaussianBlur:
        return 2;
    case EffectType::Sharpen:
        return 3;
    case EffectType::DropShadow:
        return 4;
    case EffectType::Grade:
        return kGradeFxSlotsPerEffect;
    case EffectType::UltraKey:
        return 5; // matte, choke, two blur passes (soften), apply
    case EffectType::OpacityMask:
    case EffectType::Mosaic:
        return 0; // mask pipeline passes (their own uniform buffer)
    default:
        return 1;
    }
}

/// Above this sigma (in canvas texels) a blur samples a coarser mip level instead of adding taps.
constexpr double kMaxBlurSigma = 24.0;

void storeVec4(float* out, double a, double b, double c, double d) {
    out[0] = static_cast<float>(a);
    out[1] = static_cast<float>(b);
    out[2] = static_cast<float>(c);
    out[3] = static_cast<float>(d);
}

/// Display-referred (sRGB-encoded) 0..1 -> linear light, for user colours (drop shadow).
double srgbToLinear(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

constexpr quint32 kLayerBlockSize = sizeof(LayerUniforms);
constexpr quint32 kOutputBlockSize = sizeof(OutputUniforms);
constexpr int kInitialUniformSlots = 16;

/// A colour matrix times `scale` as a column-major mat4 (as storeMat3 below stores a Mat3).
void storeColorMatrix(float* out, const color::Matrix3& m, double scale) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            const double v = row < 3 && col < 3 ? m[static_cast<std::size_t>(row * 3 + col)] * scale : row == col ? 1.0 : 0.0;
            out[col * 4 + row] = static_cast<float>(v);
        }
    }
}

// layer.frag mode.y of an HDR capture tone-mapped into SDR (I.9, gaming).
constexpr std::int32_t kTfPqSdr = 8;
constexpr std::int32_t kTfHlgSdr = 9;

// Unit square as a triangle strip: (0,0) (1,0) (0,1) (1,1).
constexpr float kQuad[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};

QShader loadShader(const char* name) {
    QFile file(QStringLiteral(":/wabcut/render/shaders/%1.qsb").arg(QLatin1String(name)));
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader();
}

quint32 alignUp(quint32 v, quint32 a) {
    return a == 0 ? v : (v + a - 1) / a * a;
}

/// Row-major Mat3 + offset -> column-major mat4 (offset in the 4th column).
void storeMat3(float* out, const Mat3& m, const std::array<double, 3>& offset = {0, 0, 0}) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double v = 0.0;
            if (row < 3 && col < 3) {
                v = m.m[static_cast<std::size_t>(row * 3 + col)];
            } else if (row < 3 && col == 3) {
                v = offset[static_cast<std::size_t>(row)];
            } else if (row == 3 && col == 3) {
                v = 1.0;
            }
            out[col * 4 + row] = static_cast<float>(v);
        }
    }
}

/// clipSpaceCorr * affine (as a 4x4 acting on (x, y, 0, 1)), column-major.
void storeClip(float* out, const QMatrix4x4& corr, const Affine2D& m) {
    const QMatrix4x4 affine(static_cast<float>(m.a), static_cast<float>(m.c), 0.0f, static_cast<float>(m.e),
                            static_cast<float>(m.b), static_cast<float>(m.d), 0.0f, static_cast<float>(m.f),
                            0.0f, 0.0f, 1.0f, 0.0f, //
                            0.0f, 0.0f, 0.0f, 1.0f);
    std::memcpy(out, (corr * affine).constData(), 16 * sizeof(float));
}

std::int32_t transferCode(TransferFunction t) {
    switch (t) {
    case TransferFunction::SRGB:
        return 1;
    case TransferFunction::Linear:
        return 2;
    case TransferFunction::Gamma22:
        return 3;
    case TransferFunction::Gamma28:
        return 4;
    default:
        return 0; // BT.709 curve (also the MVP fallback for PQ/HLG)
    }
}

std::int32_t outputCode(OutputTransform t) {
    switch (t) {
    case OutputTransform::SRGB:
        return 1;
    case OutputTransform::Linear:
        return 2;
    case OutputTransform::Rec709:
        break;
    }
    return 0;
}

std::int32_t layoutCode(PlaneLayout layout) {
    switch (layout) {
    case PlaneLayout::SemiPlanar:
        return 0;
    case PlaneLayout::Planar3:
        return 1;
    case PlaneLayout::Planar4:
        return 2;
    case PlaneLayout::PackedRGBA:
        return 3;
    }
    return 1;
}

/// Siting offset of chroma samples in luma pixels (see ColorPipeline.h), per subsampled axis.
std::array<double, 2> chromaSiting(ChromaLocation location) {
    switch (location) {
    case ChromaLocation::Center:
        return {0.0, 0.0};
    case ChromaLocation::TopLeft:
        return {0.5, 0.5};
    case ChromaLocation::Top:
        return {0.0, 0.5};
    case ChromaLocation::BottomLeft:
        return {0.5, -0.5};
    case ChromaLocation::Bottom:
        return {0.0, -0.5};
    case ChromaLocation::Left:
    case ChromaLocation::Unknown:
        break;
    }
    return {0.5, 0.0};
}

/// The fixed-function blend of a mode (C.15) in the canvas pass: 0 Normal, 1 Add (both exact for any alpha), -1 a mode
/// blended in a shader over the canvas below it (0.1.2: the canvas pass is split there; kFxBlend).
int blendVariantOf(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal: return 0;
    case BlendMode::Add: return 1;
    default: return -1;
    }
}

/// Premultiplied source cs (alpha as, opacity included) onto the canvas cb (alpha ab), docs/internal/c15-parity-handoff.md
/// ("Blend modes"): co = cs (1 - ab) + cb (1 - as) + as ab B(Cb, Cs), alpha as + ab (1 - as).
QRhiGraphicsPipeline::TargetBlend targetBlendOf(int variant) {
    QRhiGraphicsPipeline::TargetBlend blend; // premultiplied "over": ONE, ONE_MINUS_SRC_ALPHA
    blend.enable = true;
    switch (variant) {
    case 1: // Linear Dodge (Add): cs + cb, exact
        blend.dstColor = QRhiGraphicsPipeline::One;
        break;
    default:
        break;
    }
    return blend;
}

} // namespace

struct Compositor::LayerSrb {
    std::uint64_t setSerial = 0;
    std::uint64_t uniformGeneration = 0;
    std::unique_ptr<QRhiShaderResourceBindings> srb;
};

Compositor::Compositor(QRhi* rhi, CompositorOptions options)
    : m_rhi(rhi), m_options(options),
      m_importer(rhi, FrameImporterOptions{options.pool, options.maxTextureSets, -1}) {
    m_options.previewDivisor = std::clamp(m_options.previewDivisor, 1, 8);
    m_placements.reserve(32);
}

Compositor::~Compositor() {
    releaseResources();
}

void Compositor::setPreviewDivisor(int divisor) {
    m_options.previewDivisor = std::clamp(divisor, 1, 8);
    for (const auto& child : m_children) {
        if (child) {
            child->setPreviewDivisor(divisor);
        }
    }
}

void Compositor::releaseResources() {
    m_nestedSrbs.clear();
    m_children.clear(); // each child releases its own resources
    m_maskSrbs.clear();
    m_sdfTextures.clear();
    m_coverageTextures.clear();
    m_matteTextures.clear();
    m_maskPipeline.reset();
    m_maskLayoutSrb.reset();
    m_maskUniforms.reset();
    m_maskSlots = 0;
    m_maskSampler.reset();
    m_maskNearest.reset();
    m_gradeSrbs.clear();
    m_gradeTextures.clear();
    m_lutTextures.clear();
    m_gradePipeline.reset();
    m_gradeLayoutSrb.reset();
    m_gradeUniforms.reset();
    m_gradeSlots = 0;
    m_nearestSampler.reset();
    m_dummy3d.reset();
    m_dummyCurves.reset();
    m_gradeStaticUploaded = false;
    m_fxSrbs.clear();
    m_intermediates.clear();
    m_fxPipeline.reset();
    m_fxOverPipeline.reset();
    for (auto* variants : {&m_layerBlendPipelines, &m_layerArrayBlendPipelines, &m_fxBlendPipelines}) {
        for (auto& pipeline : *variants) {
            pipeline.reset();
        }
    }
    m_fxLayoutSrb.reset();
    m_layerSrbs.clear();
    m_outputPipeline.reset();
    m_outputSrb.reset();
    m_outputPassCompat.reset();
    m_outputPassSeen = nullptr;
    m_layerPipeline.reset();
    m_layerLayoutSrb.reset();
    m_nativeSrbs.clear();
    m_layerArrayPipeline.reset();
    m_layerArrayLayoutSrb.reset();
    m_dummyArray.reset();
    m_canvasTarget.reset();
    m_canvasPass.reset();
    m_canvas.reset();
    m_dummy.reset();
    m_canvasSampler.reset();
    m_planeSampler.reset();
    m_uniforms.reset();
    m_uniformSlots = 0;
    m_quad.reset();
    m_staticUploaded = false;
    m_importer.releaseResources();
}

QRect Compositor::letterboxRect(QSize target, CanvasSize canvas, double pixelAspect) {
    if (target.isEmpty() || !canvas.isValid()) {
        return {};
    }
    const double aspect = canvas.width * (pixelAspect > 0.0 ? pixelAspect : 1.0) / canvas.height;
    int w = target.width();
    int h = static_cast<int>(std::lround(w / aspect));
    if (h > target.height()) {
        h = target.height();
        w = std::min(target.width(), static_cast<int>(std::lround(h * aspect)));
    }
    w = std::max(w, 1);
    h = std::max(h, 1);
    return {(target.width() - w) / 2, (target.height() - h) / 2, w, h};
}

std::expected<void, Error> Compositor::ensureStaticResources() {
    if (!m_rhi) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Compositor has no QRhi"});
    }
    if (m_quad) {
        return {};
    }
    m_quad.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    m_planeSampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                           QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_canvasSampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                                            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_dummy.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    m_dummyArray.reset(m_rhi->newTextureArray(QRhiTexture::RGBA8, 1, QSize(1, 1)));
    if (!m_quad->create() || !m_planeSampler->create() || !m_canvasSampler->create() || !m_dummy->create() ||
        !m_dummyArray->create()) {
        releaseResources();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create compositor resources"});
    }
    m_staticUploaded = false;
    return ensureUniformCapacity(kInitialUniformSlots);
}

std::expected<void, Error> Compositor::ensureUniformCapacity(int slotCount) {
    if (m_uniforms && slotCount <= m_uniformSlots) {
        return {};
    }
    const int capacity = std::max({slotCount, kInitialUniformSlots, m_uniformSlots * 2});
    const quint32 slotSize = alignUp(kLayerBlockSize, static_cast<quint32>(m_rhi->ubufAlignment()));
    m_uniforms.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                      slotSize * static_cast<quint32>(capacity)));
    if (!m_uniforms->create()) {
        m_uniforms.reset();
        m_uniformSlots = 0;
        return std::unexpected(
            Error{ErrorCode::DeviceError, "Could not create the compositor uniform buffer"});
    }
    m_uniformSlots = capacity;
    ++m_uniformGeneration;
    m_outputSrb.reset(); // references the old buffer
    return {};
}

std::expected<void, Error> Compositor::ensureCanvas(QSize pixels) {
    if (m_canvas && m_canvas->pixelSize() == pixels) {
        return {};
    }
    m_canvasTarget.reset();
    m_fxSrbs.clear();
    m_gradeSrbs.clear();
    m_intermediates.clear(); // canvas-size scratch of the old size
    m_canvas.reset(m_rhi->newTexture(QRhiTexture::RGBA16F, pixels, 1,
                                     QRhiTexture::RenderTarget | QRhiTexture::MipMapped |
                                         QRhiTexture::UsedWithGenerateMips));
    m_canvas->setName(QByteArrayLiteral("wabcut canvas"));
    if (!m_canvas->create()) {
        m_canvas.reset();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the RGBA16F canvas"});
    }
    m_canvasTarget.reset(m_rhi->newTextureRenderTarget({m_canvas.get()}));
    if (!m_canvasPass) {
        m_canvasPass.reset(m_canvasTarget->newCompatibleRenderPassDescriptor());
    }
    m_canvasTarget->setRenderPassDescriptor(m_canvasPass.get());
    if (!m_canvasTarget->create()) {
        m_canvasTarget.reset();
        m_canvas.reset();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the canvas render target"});
    }
    m_outputSrb.reset(); // samples the old canvas

    if (!m_layerPipeline) {
        const QShader vs = loadShader("layer.vert");
        const QShader fs = loadShader("layer.frag");
        if (!vs.isValid() || !fs.isValid()) {
            return std::unexpected(
                Error{ErrorCode::Unsupported, "Layer shaders are missing from the resources"});
        }
        // Layout-only bindings for pipeline creation; per-set bindings are layout compatible.
        m_layerLayoutSrb.reset(m_rhi->newShaderResourceBindings());
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        m_layerLayoutSrb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(),
                                                                      kLayerBlockSize),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      m_dummy.get(), m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                      m_dummy.get(), m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                      m_dummy.get(), m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage,
                                                      m_dummy.get(), m_planeSampler.get()),
        });
        m_layerLayoutSrb->create();

        m_layerPipeline.reset(m_rhi->newGraphicsPipeline());
        QRhiGraphicsPipeline::TargetBlend over; // premultiplied "over": ONE, ONE_MINUS_SRC_ALPHA (defaults)
        over.enable = true;
        m_layerPipeline->setTargetBlends({over});
        m_layerPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_layerPipeline->setCullMode(QRhiGraphicsPipeline::None);
        m_layerPipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
        QRhiVertexInputLayout input;
        input.setBindings({{2 * sizeof(float)}});
        input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        m_layerPipeline->setVertexInputLayout(input);
        m_layerPipeline->setShaderResourceBindings(m_layerLayoutSrb.get());
        m_layerPipeline->setRenderPassDescriptor(m_canvasPass.get());
        if (!m_layerPipeline->create()) {
            m_layerPipeline.reset();
            return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the layer pipeline"});
        }
    }
    if (!m_layerArrayPipeline && m_rhi->isFeatureSupported(QRhi::TextureArrays)) {
        const QShader vs = loadShader("layer.vert");
        const QShader fs = loadShader("layer_array.frag");
        if (vs.isValid() && fs.isValid()) {
            const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
            m_layerArrayLayoutSrb.reset(m_rhi->newShaderResourceBindings());
            m_layerArrayLayoutSrb->setBindings({
                QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(),
                                                                          kLayerBlockSize),
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                          m_dummyArray.get(), m_planeSampler.get()),
                QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                          m_dummyArray.get(), m_planeSampler.get()),
            });
            m_layerArrayLayoutSrb->create();
            m_layerArrayPipeline.reset(m_rhi->newGraphicsPipeline());
            QRhiGraphicsPipeline::TargetBlend over;
            over.enable = true;
            m_layerArrayPipeline->setTargetBlends({over});
            m_layerArrayPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
            m_layerArrayPipeline->setCullMode(QRhiGraphicsPipeline::None);
            m_layerArrayPipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
            QRhiVertexInputLayout input;
            input.setBindings({{2 * sizeof(float)}});
            input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
            m_layerArrayPipeline->setVertexInputLayout(input);
            m_layerArrayPipeline->setShaderResourceBindings(m_layerArrayLayoutSrb.get());
            m_layerArrayPipeline->setRenderPassDescriptor(m_canvasPass.get());
            if (!m_layerArrayPipeline->create()) {
                m_layerArrayPipeline.reset(); // zero-copy layers will be skipped (counted as failed)
            }
        }
    }
    return {};
}

std::expected<void, Error> Compositor::ensureOutputPipeline(QRhiRenderPassDescriptor* rpd, int sampleCount) {
    if (!m_outputSrb) {
        m_outputSrb.reset(m_rhi->newShaderResourceBindings());
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        m_outputSrb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(),
                                                                      kOutputBlockSize),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      m_canvas.get(), m_canvasSampler.get()),
        });
        if (!m_outputSrb->create()) {
            m_outputSrb.reset();
            return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the output bindings"});
        }
    }
    if (m_outputPipeline && m_outputPipeline->sampleCount() != sampleCount) {
        m_outputPipeline.reset();
    }
    if (m_outputPipeline && rpd == m_outputPassSeen) {
        return {};
    }
    if (m_outputPipeline && m_outputPassCompat && rpd->isCompatible(m_outputPassCompat.get())) {
        m_outputPassSeen = rpd;
        return {};
    }
    const QShader vs = loadShader("output.vert");
    const QShader fs = loadShader("output.frag");
    if (!vs.isValid() || !fs.isValid()) {
        return std::unexpected(
            Error{ErrorCode::Unsupported, "Output shaders are missing from the resources"});
    }
    m_outputPassCompat.reset(rpd->newCompatibleRenderPassDescriptor());
    m_outputPipeline.reset(m_rhi->newGraphicsPipeline());
    m_outputPipeline->setSampleCount(sampleCount);
    m_outputPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_outputPipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_outputPipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    QRhiVertexInputLayout input;
    input.setBindings({{2 * sizeof(float)}});
    input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    m_outputPipeline->setVertexInputLayout(input);
    m_outputPipeline->setShaderResourceBindings(m_outputSrb.get());
    m_outputPipeline->setRenderPassDescriptor(m_outputPassCompat.get());
    if (!m_outputPipeline->create()) {
        m_outputPipeline.reset();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the output pipeline"});
    }
    m_outputPassSeen = rpd;
    return {};
}

std::expected<void, Error> Compositor::ensureFxPipelines() {
    if (m_fxPipeline && m_fxOverPipeline) {
        return {};
    }
    const QShader vs = loadShader("fx.vert");
    const QShader fs = loadShader("fx.frag");
    if (!vs.isValid() || !fs.isValid()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Effect shaders are missing from the resources"});
    }
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    m_fxLayoutSrb.reset(m_rhi->newShaderResourceBindings());
    m_fxLayoutSrb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(), sizeof(FxUniforms)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_dummy.get(),
                                                  m_canvasSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_dummy.get(),
                                                  m_canvasSampler.get()),
    });
    m_fxLayoutSrb->create();
    const auto makePipeline = [&](bool over) -> std::unique_ptr<QRhiGraphicsPipeline> {
        std::unique_ptr<QRhiGraphicsPipeline> pipeline(m_rhi->newGraphicsPipeline());
        if (over) {
            QRhiGraphicsPipeline::TargetBlend blend; // premultiplied "over"
            blend.enable = true;
            pipeline->setTargetBlends({blend});
        }
        pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
        QRhiVertexInputLayout input;
        input.setBindings({{2 * sizeof(float)}});
        input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        pipeline->setVertexInputLayout(input);
        pipeline->setShaderResourceBindings(m_fxLayoutSrb.get());
        pipeline->setRenderPassDescriptor(m_canvasPass.get());
        if (!pipeline->create()) {
            return nullptr;
        }
        return pipeline;
    };
    for (auto& pipeline : m_fxBlendPipelines) {
        pipeline.reset(); // made from the old m_fxOverPipeline
    }
    m_fxPipeline = makePipeline(false);
    m_fxOverPipeline = makePipeline(true);
    if (!m_fxPipeline || !m_fxOverPipeline) {
        m_fxPipeline.reset();
        m_fxOverPipeline.reset();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the effect pipelines"});
    }
    return {};
}

QRhiGraphicsPipeline* Compositor::blendPipeline(QRhiGraphicsPipeline* base, BlendPipelines& variants, int variant) {
    if (!base || variant <= 0 || variant > kBlendVariants || base->shaderStageCount() != 2) {
        return base;
    }
    std::unique_ptr<QRhiGraphicsPipeline>& pipeline = variants[static_cast<std::size_t>(variant - 1)];
    if (!pipeline) {
        pipeline.reset(m_rhi->newGraphicsPipeline());
        pipeline->setTargetBlends({targetBlendOf(variant)});
        pipeline->setTopology(base->topology());
        pipeline->setCullMode(base->cullMode());
        pipeline->setShaderStages({*base->shaderStageAt(0), *base->shaderStageAt(1)}); // vertex, fragment
        pipeline->setVertexInputLayout(base->vertexInputLayout());
        pipeline->setShaderResourceBindings(base->shaderResourceBindings());
        pipeline->setRenderPassDescriptor(base->renderPassDescriptor());
        if (!pipeline->create()) {
            pipeline.reset();
            return base; // drawn as Normal rather than not at all
        }
    }
    return pipeline.get();
}

QRhiShaderResourceBindings* Compositor::fxSrb(QRhiTexture* in0, QRhiTexture* in1) {
    FxSrb& entry = m_fxSrbs[{in0, in1}];
    if (!entry.srb || entry.uniformGeneration != m_uniformGeneration) {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        entry.srb.reset(m_rhi->newShaderResourceBindings());
        entry.srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(),
                                                                      sizeof(FxUniforms)),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, in0,
                                                      m_canvasSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, in1,
                                                      m_canvasSampler.get()),
        });
        if (!entry.srb->create()) {
            entry.srb.reset();
            return nullptr;
        }
        entry.uniformGeneration = m_uniformGeneration;
    }
    return entry.srb.get();
}

// ---- Colour grade (I.5) -------------------------------------------------------------------------------------

/// The textures one grade samples: its curve texture (RGB / hue / sat curves and the LUTs' 1D rows) and its LUTs'
/// 3D textures (owned by the LUT cache).
struct Compositor::GradeTextures {
    std::unique_ptr<QRhiTexture> curves;
    int width = 0;
    std::shared_ptr<const color::BakedCurves> baked; ///< pins what the texture was made from
    std::shared_ptr<const color::Lut> inputLut;
    std::shared_ptr<const color::Lut> look;
    QRhiTexture* input3d = nullptr;
    QRhiTexture* look3d = nullptr;
    std::uint64_t lastUsedFrame = 0;
};

std::expected<void, Error> Compositor::ensureGradeUniformCapacity(int slotCount) {
    if (m_gradeUniforms && slotCount <= m_gradeSlots) {
        return {};
    }
    const int capacity = std::max({slotCount, kInitialGradeSlots, m_gradeSlots * 2});
    const quint32 slotSize = alignUp(sizeof(GradeUniforms), static_cast<quint32>(m_rhi->ubufAlignment()));
    m_gradeUniforms.reset(
        m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, slotSize * static_cast<quint32>(capacity)));
    if (!m_gradeUniforms->create()) {
        m_gradeUniforms.reset();
        m_gradeSlots = 0;
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the grade uniform buffer"});
    }
    m_gradeSlots = capacity;
    ++m_gradeGeneration;
    m_gradeLayoutSrb.reset();
    m_gradePipeline.reset();
    return {};
}

std::expected<void, Error> Compositor::ensureGradePipeline() {
    if (!m_nearestSampler) {
        m_nearestSampler.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                                 QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
                                                 QRhiSampler::ClampToEdge));
        m_dummy3d.reset(m_rhi->newTexture(QRhiTexture::RGBA8, 1, 1, 1, 1, QRhiTexture::ThreeDimensional));
        m_dummyCurves.reset(m_rhi->newTexture(QRhiTexture::RGBA32F, QSize(color::BakedCurves::kSize, kCurveRows)));
        if (!m_nearestSampler->create() || !m_dummyCurves->create() || !m_dummy3d->create()) {
            m_nearestSampler.reset();
            return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the grade resources"});
        }
        m_gradeStaticUploaded = false;
    }
    if (m_gradePipeline) {
        return {};
    }
    const QShader vs = loadShader("grade.vert");
    const QShader fs = loadShader("grade.frag");
    if (!vs.isValid() || !fs.isValid()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Grade shaders are missing from the resources"});
    }
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    const auto frag = QRhiShaderResourceBinding::FragmentStage;
    m_gradeLayoutSrb.reset(m_rhi->newShaderResourceBindings());
    m_gradeLayoutSrb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_gradeUniforms.get(), sizeof(GradeUniforms)),
        QRhiShaderResourceBinding::sampledTexture(1, frag, m_dummy.get(), m_canvasSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, frag, m_dummy.get(), m_canvasSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, frag, m_dummy.get(), m_canvasSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(4, frag, m_dummyCurves.get(), m_nearestSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(5, frag, m_dummy3d.get(), m_nearestSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(6, frag, m_dummy3d.get(), m_nearestSampler.get()),
    });
    m_gradeLayoutSrb->create();
    m_gradePipeline.reset(m_rhi->newGraphicsPipeline());
    m_gradePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_gradePipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_gradePipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    QRhiVertexInputLayout input;
    input.setBindings({{2 * sizeof(float)}});
    input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    m_gradePipeline->setVertexInputLayout(input);
    m_gradePipeline->setShaderResourceBindings(m_gradeLayoutSrb.get());
    m_gradePipeline->setRenderPassDescriptor(m_canvasPass.get());
    if (!m_gradePipeline->create()) {
        m_gradePipeline.reset();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the grade pipeline"});
    }
    return {};
}

QRhiTexture* Compositor::lutTexture(const std::shared_ptr<const color::Lut>& lut, QRhiResourceUpdateBatch* updates) {
    if (!lut || !lut->has3d() || !m_rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures)) {
        return nullptr;
    }
    LutTexture& entry = m_lutTextures[lut.get()];
    entry.lastUsedFrame = m_frameCounter;
    if (entry.texture && entry.lut == lut) {
        return entry.texture.get();
    }
    const int n = lut->size3d;
    entry.lut = lut;
    entry.texture.reset(m_rhi->newTexture(QRhiTexture::RGBA32F, n, n, n, 1, QRhiTexture::ThreeDimensional));
    entry.texture->setName(QByteArrayLiteral("wabcut grade 3D LUT"));
    if (!entry.texture->create()) {
        entry.texture.reset();
        return nullptr;
    }
    // One upload per slice (layer = z); the description copies the bytes.
    const std::size_t sliceTexels = static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
    std::vector<float> slice(sliceTexels * 4);
    for (int z = 0; z < n; ++z) {
        for (std::size_t i = 0; i < sliceTexels; ++i) {
            const std::size_t k = (static_cast<std::size_t>(z) * sliceTexels + i) * 3;
            slice[i * 4 + 0] = lut->table3d[k];
            slice[i * 4 + 1] = lut->table3d[k + 1];
            slice[i * 4 + 2] = lut->table3d[k + 2];
            slice[i * 4 + 3] = 1.0f;
        }
        updates->uploadTexture(entry.texture.get(),
                               QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                   z, 0, QRhiTextureSubresourceUploadDescription(slice.data(), static_cast<quint32>(slice.size() * sizeof(float))))));
    }
    return entry.texture.get();
}

Compositor::GradeTextures* Compositor::gradeTextures(const color::GradeSpec& grade, QRhiResourceUpdateBatch* updates,
                                                     int& missingLuts) {
    // One curve texture per (curves, input LUT, look): the LUTs' 1D tables share its rows.
    const auto mixKey = [](std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ull; };
    const color::Lut* in = grade.inputLutActive() ? grade.inputLut.get() : nullptr;
    const color::Lut* look = grade.look && !grade.look->empty() ? grade.look.get() : nullptr;
    std::uint64_t key = 1469598103934665603ull;
    key = mixKey(key, grade.curves ? grade.curves->key : 0);
    key = mixKey(key, reinterpret_cast<std::uintptr_t>(in));
    key = mixKey(key, reinterpret_cast<std::uintptr_t>(look));
    auto& slot = m_gradeTextures[key];
    if (!slot) {
        slot = std::make_unique<GradeTextures>();
    }
    GradeTextures& t = *slot;
    t.lastUsedFrame = m_frameCounter;
    if (!t.curves || t.baked != grade.curves || t.inputLut.get() != in || t.look.get() != look) {
        t.baked = grade.curves;
        t.inputLut = in ? grade.inputLut : nullptr;
        t.look = look ? grade.look : nullptr;
        const int inLen = stored1dLength(in);
        const int lookLen = stored1dLength(look);
        t.width = std::max({color::BakedCurves::kSize, inLen, lookLen});
        t.curves.reset(m_rhi->newTexture(QRhiTexture::RGBA32F, QSize(t.width, kCurveRows)));
        t.curves->setName(QByteArrayLiteral("wabcut grade curves"));
        if (!t.curves->create()) {
            t.curves.reset();
            return nullptr;
        }
        std::vector<float> texels(static_cast<std::size_t>(t.width) * kCurveRows * 4, 0.0f);
        const auto rowAt = [&](int row) { return texels.data() + static_cast<std::size_t>(row) * t.width * 4; };
        if (grade.curves) {
            std::copy(grade.curves->rgbm.begin(), grade.curves->rgbm.end(), rowAt(0));
            std::copy(grade.curves->hue.begin(), grade.curves->hue.end(), rowAt(1));
            std::copy(grade.curves->sat.begin(), grade.curves->sat.end(), rowAt(2));
        }
        const auto store1d = [&](int row, const color::Lut* lut, int length) {
            if (!lut || length == 0) {
                return;
            }
            const std::vector<float> table = color::resampled1d(*lut, length);
            float* out = rowAt(row);
            for (int i = 0; i < length; ++i) {
                out[i * 4 + 0] = table[static_cast<std::size_t>(i) * 3];
                out[i * 4 + 1] = table[static_cast<std::size_t>(i) * 3 + 1];
                out[i * 4 + 2] = table[static_cast<std::size_t>(i) * 3 + 2];
            }
        };
        store1d(3, in, inLen);
        store1d(4, look, lookLen);
        updates->uploadTexture(t.curves.get(),
                               QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                   0, 0, QRhiTextureSubresourceUploadDescription(texels.data(), static_cast<quint32>(texels.size() * sizeof(float))))));
    }
    t.input3d = in && in->has3d() ? lutTexture(grade.inputLut, updates) : nullptr;
    t.look3d = look && look->has3d() ? lutTexture(grade.look, updates) : nullptr;
    missingLuts += grade.missingLuts + (in && in->has3d() && !t.input3d ? 1 : 0) + (look && look->has3d() && !t.look3d ? 1 : 0);
    return &t;
}

QRhiShaderResourceBindings* Compositor::gradeSrb(const std::array<QRhiTexture*, 6>& textures) {
    GradeSrb& entry = m_gradeSrbs[textures];
    if (!entry.srb || entry.uniformGeneration != m_gradeGeneration) {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        const auto frag = QRhiShaderResourceBinding::FragmentStage;
        entry.srb.reset(m_rhi->newShaderResourceBindings());
        entry.srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_gradeUniforms.get(), sizeof(GradeUniforms)),
            QRhiShaderResourceBinding::sampledTexture(1, frag, textures[0], m_canvasSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, frag, textures[1], m_canvasSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, frag, textures[2], m_canvasSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(4, frag, textures[3], m_nearestSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(5, frag, textures[4], m_nearestSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(6, frag, textures[5], m_nearestSampler.get()),
        });
        if (!entry.srb->create()) {
            entry.srb.reset();
            return nullptr;
        }
        entry.uniformGeneration = m_gradeGeneration;
    }
    return entry.srb.get();
}

// ---- Masks (I.6) ---------------------------------------------------------------------------------------------

std::expected<void, Error> Compositor::ensureMaskUniformCapacity(int slotCount) {
    if (m_maskUniforms && slotCount <= m_maskSlots) {
        return {};
    }
    const int capacity = std::max({slotCount, kInitialMaskSlots, m_maskSlots * 2});
    const quint32 slotSize = alignUp(sizeof(MaskUniforms), static_cast<quint32>(m_rhi->ubufAlignment()));
    m_maskUniforms.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, slotSize * static_cast<quint32>(capacity)));
    if (!m_maskUniforms->create()) {
        m_maskUniforms.reset();
        m_maskSlots = 0;
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the mask uniform buffer"});
    }
    m_maskSlots = capacity;
    ++m_maskGeneration;
    m_maskLayoutSrb.reset();
    m_maskPipeline.reset();
    return {};
}

std::expected<void, Error> Compositor::ensureMaskPipeline() {
    if (!m_maskSampler) {
        m_maskSampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                                              QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_maskNearest.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                              QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!m_maskSampler->create() || !m_maskNearest->create()) {
            m_maskSampler.reset();
            return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the mask samplers"});
        }
    }
    if (m_maskPipeline) {
        return {};
    }
    const QShader vs = loadShader("mask.vert");
    const QShader fs = loadShader("mask.frag");
    if (!vs.isValid() || !fs.isValid()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Mask shaders are missing from the resources"});
    }
    m_maskLayoutSrb.reset(m_rhi->newShaderResourceBindings());
    setMaskBindings(*m_maskLayoutSrb, {});
    m_maskLayoutSrb->create();
    m_maskPipeline.reset(m_rhi->newGraphicsPipeline());
    m_maskPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_maskPipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_maskPipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    QRhiVertexInputLayout input;
    input.setBindings({{2 * sizeof(float)}});
    input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    m_maskPipeline->setVertexInputLayout(input);
    m_maskPipeline->setShaderResourceBindings(m_maskLayoutSrb.get());
    m_maskPipeline->setRenderPassDescriptor(m_canvasPass.get());
    if (!m_maskPipeline->create()) {
        m_maskPipeline.reset();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not create the mask pipeline"});
    }
    return {};
}

Compositor::MaskTexture* Compositor::maskTexture(std::unordered_map<std::uint64_t, std::unique_ptr<MaskTexture>>& cache,
                                                 std::uint64_t owner, QSize size) {
    auto& slot = cache[owner];
    if (!slot || slot->size != size) {
        auto created = std::make_unique<MaskTexture>();
        created->size = size;
        created->texture.reset(m_rhi->newTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget));
        created->texture->setName(QByteArrayLiteral("wabcut mask"));
        if (!created->texture->create()) {
            cache.erase(owner);
            return nullptr;
        }
        created->target.reset(m_rhi->newTextureRenderTarget({created->texture.get()}));
        created->target->setRenderPassDescriptor(m_canvasPass.get());
        if (!created->target->create()) {
            cache.erase(owner);
            return nullptr;
        }
        slot = std::move(created);
        m_maskSrbs.clear(); // they may name the texture this one replaced
    }
    slot->lastUsedFrame = m_frameCounter;
    return slot.get();
}

void Compositor::setMaskBindings(QRhiShaderResourceBindings& srb, const std::array<QRhiTexture*, 8>& textures) {
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    const auto frag = QRhiShaderResourceBinding::FragmentStage;
    std::array<QRhiTexture*, 8> t{};
    for (std::size_t i = 0; i < t.size(); ++i) {
        t[i] = textures[i] ? textures[i] : m_dummy.get();
    }
    QRhiSampler* linear = m_maskSampler.get();
    srb.setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_maskUniforms.get(), sizeof(MaskUniforms)),
        QRhiShaderResourceBinding::sampledTexture(1, frag, t[0], linear),
        QRhiShaderResourceBinding::sampledTexture(2, frag, t[1], linear),
        QRhiShaderResourceBinding::sampledTexture(3, frag, t[2], linear),
        QRhiShaderResourceBinding::sampledTexture(4, frag, t[3], linear),
        QRhiShaderResourceBinding::sampledTexture(5, frag, t[4], linear),
        QRhiShaderResourceBinding::sampledTexture(6, frag, t[5], linear),
        QRhiShaderResourceBinding::sampledTexture(7, frag, t[6], linear),
        QRhiShaderResourceBinding::sampledTexture(8, frag, t[7], m_maskNearest.get()),
    });
}

QRhiShaderResourceBindings* Compositor::maskSrb(const std::array<QRhiTexture*, 8>& textures) {
    MaskSrb& entry = m_maskSrbs[textures];
    if (!entry.srb || entry.uniformGeneration != m_maskGeneration) {
        entry.srb.reset(m_rhi->newShaderResourceBindings());
        setMaskBindings(*entry.srb, textures);
        if (!entry.srb->create()) {
            entry.srb.reset();
            return nullptr;
        }
        entry.uniformGeneration = m_maskGeneration;
    }
    return entry.srb.get();
}

Compositor::Intermediate* Compositor::acquireIntermediate(QSize pixels) {
    for (const auto& candidate : m_intermediates) {
        if (!candidate->inUse) {
            candidate->inUse = true;
            return candidate.get();
        }
    }
    auto created = std::make_unique<Intermediate>();
    created->texture.reset(m_rhi->newTexture(QRhiTexture::RGBA16F, pixels, 1,
                                             QRhiTexture::RenderTarget | QRhiTexture::MipMapped |
                                                 QRhiTexture::UsedWithGenerateMips));
    created->texture->setName(QByteArrayLiteral("wabcut fx intermediate"));
    if (!created->texture->create()) {
        return nullptr;
    }
    created->target.reset(m_rhi->newTextureRenderTarget({created->texture.get()}));
    created->target->setRenderPassDescriptor(m_canvasPass.get());
    if (!created->target->create()) {
        return nullptr;
    }
    created->inUse = true;
    m_intermediates.push_back(std::move(created));
    return m_intermediates.back().get();
}

QRhiShaderResourceBindings* Compositor::layerSrb(const ImportedFrame& frame) {
    if (frame.arrayTextures) {
        if (!m_layerArrayPipeline) {
            return nullptr;
        }
        NativeSrb& entry = m_nativeSrbs[frame.setSerial]; // inserts only for a new decoder-surface view
        entry.lastUsedFrame = m_frameCounter;
        if (!entry.srb || entry.uniformGeneration != m_uniformGeneration) {
            const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
            entry.srb.reset(m_rhi->newShaderResourceBindings());
            entry.srb->setBindings({
                QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(),
                                                                          kLayerBlockSize),
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                          frame.planes[0], m_planeSampler.get()),
                QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                          frame.planes[1], m_planeSampler.get()),
            });
            if (!entry.srb->create()) {
                entry.srb.reset();
                return nullptr;
            }
            entry.uniformGeneration = m_uniformGeneration;
        }
        return entry.srb.get();
    }
    const auto slot = static_cast<std::size_t>(frame.slot);
    if (m_layerSrbs.size() <= slot) {
        m_layerSrbs.resize(slot + 1);
    }
    std::unique_ptr<LayerSrb>& entry = m_layerSrbs[slot];
    if (!entry) {
        entry = std::make_unique<LayerSrb>();
    }
    if (!entry->srb || entry->setSerial != frame.setSerial ||
        entry->uniformGeneration != m_uniformGeneration) {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        const auto tex = [&](int i) {
            QRhiTexture* t = frame.planes[static_cast<std::size_t>(i)];
            return t ? t : m_dummy.get();
        };
        entry->srb.reset(m_rhi->newShaderResourceBindings());
        entry->srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(),
                                                                      kLayerBlockSize),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex(0),
                                                      m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, tex(1),
                                                      m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, tex(2),
                                                      m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage, tex(3),
                                                      m_planeSampler.get()),
        });
        if (!entry->srb->create()) {
            entry->srb.reset();
            return nullptr;
        }
        entry->setSerial = frame.setSerial;
        entry->uniformGeneration = m_uniformGeneration;
    }
    return entry->srb.get();
}

Compositor::ViewState Compositor::ViewState::of(const ColorView& view) {
    ViewState v;
    v.maskView = view.maskView;
    v.maskClip = view.maskClip;
    v.split = view.split >= 0.0;
    v.line = {1.0, 0.0, -view.split}; // x < split
    return v;
}

Compositor::ViewState Compositor::ViewState::inside(const Affine2D& toCanvas, QSize pixels, CanvasSize canvas,
                                                    std::uint64_t clipId) const {
    ViewState v = *this;
    if (maskClip != 0 && maskClip == clipId) {
        v.maskClip = 0; // the nested clip's mask view: every graded layer in it
    }
    if (split && pixels.width() > 0 && pixels.height() > 0 && canvas.isValid()) {
        // The line in the holder's 0..1 coordinates, pulled back through the placement: a child canvas point (u, v)
        // is at toCanvas(u * pixels.width, v * pixels.height) in holder canvas units.
        const double kw = line[0] / canvas.width;
        const double kh = line[1] / canvas.height;
        v.line = {(kw * toCanvas.a + kh * toCanvas.b) * pixels.width(), (kw * toCanvas.c + kh * toCanvas.d) * pixels.height(),
                  kw * toCanvas.e + kh * toCanvas.f + line[2]};
    }
    return v;
}

std::expected<CompositeStats, Error> Compositor::render(QRhiCommandBuffer* cb, const CompositeOutput& output,
                                                        const PreparedComposite& composite) {
    return renderImpl(cb, output, composite, nullptr, composite.color, ViewState::of(composite.colorView), false);
}

std::expected<CompositeStats, Error>
Compositor::render(QRhiCommandBuffer* cb, const CompositeOutput& output,
                   const std::shared_ptr<const PreparedComposite>& composite) {
    if (!composite) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Null composite"});
    }
    return renderImpl(cb, output, *composite, composite, composite->color, ViewState::of(composite->colorView), false);
}

QRhiShaderResourceBindings* Compositor::nestedSrb(QRhiTexture* texture) {
    NestedSrb& entry = m_nestedSrbs[texture];
    entry.lastUsedFrame = m_frameCounter;
    if (!entry.srb || entry.uniformGeneration != m_uniformGeneration) {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        entry.srb.reset(m_rhi->newShaderResourceBindings());
        entry.srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_uniforms.get(), kLayerBlockSize),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture,
                                                      m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_dummy.get(),
                                                      m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, m_dummy.get(),
                                                      m_planeSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage, m_dummy.get(),
                                                      m_planeSampler.get()),
        });
        if (!entry.srb->create()) {
            entry.srb.reset();
            return nullptr;
        }
        entry.uniformGeneration = m_uniformGeneration;
    }
    return entry.srb.get();
}

std::expected<ImportedFrame, Error> Compositor::renderNested(QRhiCommandBuffer* cb, const PreparedComposite& nested,
                                                             const std::shared_ptr<const void>& keepAlive,
                                                             const ColorPipeline& color, const ViewState& view,
                                                             std::size_t index, CompositeStats& stats) {
    if (index >= m_children.size()) {
        m_children.resize(index + 1);
    }
    std::unique_ptr<Compositor>& child = m_children[index];
    if (!child) {
        child = std::make_unique<Compositor>(m_rhi, m_options);
    }
    child->setPreviewDivisor(m_options.previewDivisor);
    const auto rendered = child->renderImpl(cb, CompositeOutput{}, nested, keepAlive, color, view, true);
    if (!rendered) {
        return std::unexpected(rendered.error());
    }
    const CompositeStats& s = *rendered;
    stats.nestedComposites += 1 + s.nestedComposites;
    stats.layersDrawn += s.layersDrawn;
    stats.effectsApplied += s.effectsApplied;
    stats.bypassedEffects += s.bypassedEffects;
    stats.transitions += s.transitions;
    stats.effectPasses += s.effectPasses;
    stats.gradePasses += s.gradePasses;
    stats.missingLuts += s.missingLuts;
    stats.maskPasses += s.maskPasses;
    stats.maskTexturesRendered += s.maskTexturesRendered;
    stats.redactionsApplied += s.redactionsApplied;
    stats.layersFailed += s.layersFailed;
    stats.uploads += s.uploads;
    stats.residentHits += s.residentHits;
    stats.bytesUploaded += s.bytesUploaded;
    stats.hdrSource = stats.hdrSource || s.hdrSource;
    stats.toneMappedCaptures += s.toneMappedCaptures;
    stats.unsupportedBlends += s.unsupportedBlends;
    stats.shaderBlends += s.shaderBlends;
    QRhiTexture* canvas = child->canvasTexture();
    if (!canvas || s.canvasPixels.isEmpty()) {
        return std::unexpected(Error{ErrorCode::DeviceError, "The nested composite has no canvas"});
    }
    // The child's canvas as an RGBA frame: already linear Rec.709 and premultiplied, so the layer pass converts nothing.
    ImportedFrame f;
    f.planes[0] = canvas;
    f.layout = PlaneLayout::PackedRGBA;
    f.encoding = SampleEncoding::Bits8; // a float texture: sampled values need no scaling
    f.width = s.canvasPixels.width();
    f.height = s.canvasPixels.height();
    f.chromaWidth = f.width;
    f.chromaHeight = f.height;
    f.textureWidth = f.width;
    f.textureHeight = f.height;
    f.color.primaries = ColorPrimaries::BT709;
    f.color.transfer = TransferFunction::Linear;
    f.color.matrix = ColorMatrix::RGB;
    f.color.range = ColorRange::Full;
    f.alpha = AlphaMode::Premultiplied;
    // Canvas pixels are rounded up at a preview divisor: the pixel aspect keeps the nested canvas' exact shape.
    f.pixelAspect = (static_cast<double>(nested.canvas.width) / nested.canvas.height) /
                    (static_cast<double>(f.width) / f.height);
    return f;
}

std::expected<CompositeStats, Error> Compositor::renderImpl(QRhiCommandBuffer* cb,
                                                            const CompositeOutput& output,
                                                            const PreparedComposite& composite,
                                                            const std::shared_ptr<const void>& keepAlive,
                                                            const ColorPipeline& colorPipeline, const ViewState& view,
                                                            bool canvasOnly) {
    if (!cb || (!output.target && !canvasOnly)) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, "Compositor::render needs a command buffer and a target"});
    }
    if (!composite.canvas.isValid()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Composite has an invalid canvas size"});
    }
    if (!colorPipeline.isSupportedVersion()) {
        return std::unexpected(
            Error{ErrorCode::VersionMismatch,
                  "Unsupported ColorPipeline version " + std::to_string(colorPipeline.version)});
    }
    if (auto ok = ensureStaticResources(); !ok) {
        return std::unexpected(ok.error());
    }
    const int layerCount = static_cast<int>(composite.layers.size());
    // Uniform slots: per layer its draw, its effect passes, a mix and the blit into the canvas; one for output.
    int slotsNeeded = 1;
    int gradeSlotsNeeded = 0;
    int maskSlotsNeeded = 0;
    // A mask list: one distance pass per shape, the coverage in chunks of kShapesPerCompose, and the masked mix.
    const auto maskSlots = [](const std::shared_ptr<const MaskSpec>& m) {
        const int shapes = m ? static_cast<int>(m->shapes.size()) : 0;
        return shapes + std::max(1, (shapes + kShapesPerCompose - 1) / kShapesPerCompose) + 1;
    };
    for (const CompositeLayer& layer : composite.layers) {
        slotsNeeded += 2 + (layer.transition ? 1 : 0);
        for (const LayerEffect& effect : layer.effects) {
            slotsNeeded += fxPassCount(effect.type);
            gradeSlotsNeeded += effect.type == EffectType::Grade && effect.grade ? kGradeSlotsPerEffect : 0;
            maskSlotsNeeded += (effect.mask ? maskSlots(effect.mask) : 0) + (effect.type == EffectType::Mosaic ? 1 : 0);
        }
        for (const LayerRedaction& r : layer.redactions) {
            slotsNeeded += 2; // a blur's two passes
            maskSlotsNeeded += maskSlots(r.mask) + 1; // and the style pass
        }
    }
    if (auto ok = ensureUniformCapacity(slotsNeeded); !ok) {
        return std::unexpected(ok.error());
    }
    if (gradeSlotsNeeded > 0) {
        if (auto ok = ensureGradeUniformCapacity(gradeSlotsNeeded); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (maskSlotsNeeded > 0) {
        if (auto ok = ensureMaskUniformCapacity(maskSlotsNeeded); !ok) {
            return std::unexpected(ok.error());
        }
    }
    const int div = m_options.previewDivisor;
    const QSize canvasPixels((composite.canvas.width + div - 1) / div,
                             (composite.canvas.height + div - 1) / div);
    if (auto ok = ensureCanvas(canvasPixels); !ok) {
        return std::unexpected(ok.error());
    }
    if (!canvasOnly) {
        if (auto ok = ensureOutputPipeline(output.target->renderPassDescriptor(), output.target->sampleCount()); !ok) {
            return std::unexpected(ok.error());
        }
    }

    CompositeStats stats;
    stats.canvasPixels = canvasPixels;
    const quint32 slotSize = alignUp(kLayerBlockSize, static_cast<quint32>(m_rhi->ubufAlignment()));
    const QMatrix4x4 corr = m_rhi->clipSpaceCorrMatrix();
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    if (!m_staticUploaded) {
        updates->uploadStaticBuffer(m_quad.get(), kQuad);
        const quint32 black = 0;
        updates->uploadTexture(m_dummy.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                                  0, 0, QRhiTextureSubresourceUploadDescription(&black, 4))));
        m_staticUploaded = true;
    }
    if (gradeSlotsNeeded > 0) {
        if (auto ok = ensureGradePipeline(); !ok) {
            updates->release();
            return std::unexpected(ok.error());
        }
        if (!m_gradeStaticUploaded) {
            const quint32 black = 0;
            updates->uploadTexture(m_dummy3d.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                                         0, 0, QRhiTextureSubresourceUploadDescription(&black, 4))));
            const std::vector<float> zeros(static_cast<std::size_t>(color::BakedCurves::kSize) * kCurveRows * 4, 0.0f);
            updates->uploadTexture(m_dummyCurves.get(),
                                   QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                       0, 0, QRhiTextureSubresourceUploadDescription(zeros.data(), static_cast<quint32>(zeros.size() * sizeof(float))))));
            m_gradeStaticUploaded = true;
        }
        if (m_frameCounter % 64 == 0) { // grade and LUT textures not used for a while
            const auto before = m_gradeTextures.size() + m_lutTextures.size();
            std::erase_if(m_gradeTextures, [&](const auto& e) { return e.second->lastUsedFrame + 256 < m_frameCounter; });
            std::erase_if(m_lutTextures, [&](const auto& e) { return e.second.lastUsedFrame + 256 < m_frameCounter; });
            if (m_gradeTextures.size() + m_lutTextures.size() != before) {
                m_gradeSrbs.clear(); // they may name the textures just dropped
            }
        }
    }

    if (maskSlotsNeeded > 0) {
        if (auto ok = ensureMaskPipeline(); !ok) {
            updates->release();
            return std::unexpected(ok.error());
        }
    }
    if (m_frameCounter % 64 == 0 && (!m_sdfTextures.empty() || !m_coverageTextures.empty() || !m_matteTextures.empty())) {
        const auto before = m_sdfTextures.size() + m_coverageTextures.size() + m_matteTextures.size();
        std::erase_if(m_sdfTextures, [&](const auto& e) { return e.second->lastUsedFrame + 256 < m_frameCounter; });
        std::erase_if(m_coverageTextures, [&](const auto& e) { return e.second->lastUsedFrame + 256 < m_frameCounter; });
        std::erase_if(m_matteTextures, [&](const auto& e) { return e.second->lastUsedFrame + 256 < m_frameCounter; });
        if (m_sdfTextures.size() + m_coverageTextures.size() + m_matteTextures.size() != before) {
            m_maskSrbs.clear();
        }
    }

    // ---- Layers: import + uniforms ----
    m_importer.beginFrame();
    ++m_frameCounter;
    if (m_frameCounter % 64 == 0) { // views of decoder surfaces that went away
        std::erase_if(m_nativeSrbs, [&](const auto& entry) { return entry.second.lastUsedFrame + 256 < m_frameCounter; });
    }
    const std::uint64_t bytesBefore = m_importer.stats().bytesUploaded;
    m_placements.clear();
    if (m_frameCounter % 64 == 0) { // canvases of nested composites no longer rendered
        std::erase_if(m_nestedSrbs, [&](const auto& entry) { return entry.second.lastUsedFrame + 256 < m_frameCounter; });
    }
    std::size_t nestedIndex = 0;
    std::vector<int> placementOf(static_cast<std::size_t>(layerCount), -1);
    // Track Matte Keys (0.1.2): each matte's composite on a canvas like this one, by (layer, effect index).
    std::map<std::pair<int, std::size_t>, QRhiTexture*> matteTextures;
    const Affine2D toNdc{2.0 / composite.canvas.width, 0.0, 0.0, -2.0 / composite.canvas.height, -1.0, 1.0};
    for (int i = 0; i < layerCount; ++i) {
        const CompositeLayer& layer = composite.layers[static_cast<std::size_t>(i)];
        stats.bypassedEffects += layer.bypassedEffects;
        const double opacity = std::clamp(layer.opacity, 0.0, 1.0);
        if (opacity <= 0.0 || layer.transform.scale == 0.0 || !std::isfinite(layer.transform.scale)) {
            continue; // invisible (a transition side mixes with transparent instead)
        }
        for (std::size_t e = 0; e < layer.effects.size(); ++e) {
            const LayerEffect& effect = layer.effects[e];
            if (effect.type == EffectType::TrackMatte && effect.matte && effect.matte->canvas.isValid()) {
                const auto matte = renderNested(cb, *effect.matte, keepAlive, colorPipeline, ViewState{}, nestedIndex++, stats);
                if (matte) {
                    matteTextures[{i, e}] = matte->planes[0];
                }
            }
        }
        std::expected<ImportedFrame, Error> imported;
        QRhiShaderResourceBindings* srb = nullptr;
        LayerTransform transform = layer.transform;
        if (layer.nested) {
            // A nested or multicam clip (C.8): its composite, rendered now by a child compositor. When it cannot be
            // rendered the layer is left out (never a picture without the redactions inside it).
            if (!layer.nested->canvas.isValid()) {
                ++stats.layersFailed;
                continue;
            }
            ViewState inner = view;
            if (view.maskView != 0 || view.split) {
                // Where the child's canvas will land (the placement below, from the child canvas' size at this divisor).
                const QSize pixels((layer.nested->canvas.width + div - 1) / div, (layer.nested->canvas.height + div - 1) / div);
                LayerTransform t = transform;
                const double k = static_cast<double>(layer.nested->canvas.height) / pixels.height();
                t.scale *= k;
                t.anchor = {t.anchor.x / k, t.anchor.y / k};
                const double pixelAspect = (static_cast<double>(layer.nested->canvas.width) / layer.nested->canvas.height) /
                                           (static_cast<double>(pixels.width()) / pixels.height());
                const Affine2D place = layerToCanvas(t, pixels.width(), pixels.height(), pixelAspect, composite.canvas);
                inner = view.inside(place, pixels, composite.canvas, layer.clipId);
            }
            imported = renderNested(cb, *layer.nested, keepAlive, colorPipeline, inner, nestedIndex++, stats);
            if (imported) {
                srb = nestedSrb(imported->planes[0]);
                // Motion is defined in the nested canvas' pixels; the canvas texture is smaller at a preview divisor.
                const double k = static_cast<double>(layer.nested->canvas.height) / imported->height;
                transform.scale *= k;
                transform.anchor = {transform.anchor.x / k, transform.anchor.y / k};
            }
        } else if (layer.view) {
            FrameView frameView = *layer.view;
            if (keepAlive) {
                frameView.keepAlive = keepAlive;
            }
            imported = m_importer.import(frameView, updates);
        } else {
            imported = m_importer.import(layer.frame, updates, keepAlive);
        }
        if (!imported) {
            ++stats.layersFailed;
            continue;
        }
        const ImportedFrame& f = *imported;
        if (layer.nested ? !srb : !layerSrb(f)) {
            ++stats.layersFailed;
            continue;
        }
        stats.uploads += f.uploaded ? 1 : 0;
        stats.residentHits += f.uploaded || layer.nested ? 0 : 1;

        const bool isRgb = f.layout == PlaneLayout::PackedRGBA;
        const ResolvedColor color = resolveSourceColor(f.color, colorPipeline, isRgb);
        stats.hdrSource = stats.hdrSource || color.hdr;

        LayerUniforms u{};
        const double w = f.width;
        const double h = f.height;
        const Affine2D toCanvas = layerToCanvas(transform, w, h, f.pixelAspect, composite.canvas);
        // One canvas-texture pixel of fringe, expressed in source pixels.
        const double sx = std::hypot(toCanvas.a, toCanvas.b);
        const double sy = std::hypot(toCanvas.c, toCanvas.d);
        const double fringeX = sx > 0.0 ? std::min(div / sx, w) : 0.0;
        const double fringeY = sy > 0.0 ? std::min(div / sy, h) : 0.0;
        const Affine2D cornerToSrc{w + 2.0 * fringeX, 0.0, 0.0, h + 2.0 * fringeY, -fringeX, -fringeY};
        storeClip(u.cornerToClip, corr, toNdc * toCanvas * cornerToSrc);

        const SampleToRgb s2r = sampleToRgb(color.matrix, color.range, f.encoding, isRgb);
        storeMat3(u.sampleToRgb, s2r.matrix, s2r.offset);
        storeMat3(u.toWorking, primariesToRec709(color.primaries));

        // Chroma siting: chroma uv = (uv * lumaWidth + siting) / (subsampling * chromaWidth).
        const std::array<double, 2> siting = chromaSiting(color.chromaLocation);
        const double subX = f.chromaShiftX > 0 ? 2.0 : 1.0;
        const double subY = f.chromaShiftY > 0 ? 2.0 : 1.0;
        const double offX = f.chromaShiftX > 0 ? siting[0] : 0.0;
        const double offY = f.chromaShiftY > 0 ? siting[1] : 0.0;
        // In luma-texture space (a decoder surface is larger than the picture it holds).
        const double tw = f.textureWidth > 0 ? f.textureWidth : w;
        const double th = f.textureHeight > 0 ? f.textureHeight : h;
        u.chromaXform[0] = static_cast<float>(tw / (subX * f.chromaWidth));
        u.chromaXform[1] = static_cast<float>(th / (subY * f.chromaHeight));
        u.chromaXform[2] = static_cast<float>(offX / (subX * f.chromaWidth));
        u.chromaXform[3] = static_cast<float>(offY / (subY * f.chromaHeight));
        u.srcRect[0] = static_cast<float>(w);
        u.srcRect[1] = static_cast<float>(h);
        u.srcRect[2] = static_cast<float>(fringeX);
        u.srcRect[3] = static_cast<float>(fringeY);
        u.params[0] = static_cast<float>(opacity);
        u.params[1] = f.encoding == SampleEncoding::Bits10Low    ? 65535.0f / 1023.0f
                      : f.encoding == SampleEncoding::Bits10High ? 65535.0f / (64.0f * 1023.0f)
                                                                 : 1.0f;
        u.params[2] = static_cast<float>(w / tw); // displayed part of the texture
        u.params[3] = static_cast<float>(h / th);
        u.mode[0] = layoutCode(f.layout);
        u.mode[1] = transferCode(color.transfer);
        u.mode[2] = f.alpha == AlphaMode::None ? 0 : f.alpha == AlphaMode::Straight ? 1 : 2;
        u.mode[3] = layer.nested ? 1 : 0; // a nested canvas is float: keep what is above 1 (I.9)
        if (!layer.nested &&
            (color.transfer == TransferFunction::PQ || color.transfer == TransferFunction::HLG)) {
            // I.9 (gaming): an HDR game capture in an SDR project, tone-mapped into SDR (color::hdrCaptureToSdr).
            u.mode[1] = color.transfer == TransferFunction::PQ ? kTfPqSdr : kTfHlgSdr;
            storeColorMatrix(u.toWorking, color::rec2020ToRec709(), 1.0);
            u.cm[0] = static_cast<float>(color::hlgSystemGamma(color::kCapturePeakNits) - 1.0);
            u.cm[2] = static_cast<float>(color::kCaptureReferenceWhiteNits);
            u.cm[3] = static_cast<float>(color::kCapturePeakNits);
            ++stats.toneMappedCaptures;
        }

        const int slot = static_cast<int>(m_placements.size());
        updates->updateDynamicBuffer(m_uniforms.get(), static_cast<quint32>(slot) * slotSize, sizeof(u), &u);
        placementOf[static_cast<std::size_t>(i)] = slot;
        m_placements.push_back({f, slot, toCanvas, layer.clipId, srb});
    }
    stats.layersDrawn += static_cast<int>(m_placements.size()); // plus those inside nested composites
    stats.bytesUploaded += m_importer.stats().bytesUploaded - bytesBefore;

    // ---- Effects and transitions: offscreen passes into intermediates ----
    // Plan first (uniforms go into the same batch), record afterwards.
    struct Draw {
        int placement = -1;              ///< a layer drawn directly
        Intermediate* result = nullptr;  ///< or an offscreen result blitted over the canvas
        int slot = 0;
        int blend = 0;                   ///< blendVariantOf() the layer's mode (C.15): 0 Normal, 1 Add
    };
    struct Pass {
        Intermediate* target = nullptr;
        int placement = -1; ///< >= 0: draw this layer; otherwise an fx quad
        int slot = 0;
        QRhiTexture* in0 = nullptr;
        QRhiTexture* in1 = nullptr;
        QRhiTexture* mipsOf = nullptr; ///< generate this texture's mips before the pass (wide blurs)
        GradeTextures* grade = nullptr; ///< a grade pass (grade.frag): its curve and LUT textures
        QRhiTexture* in2 = nullptr;     ///< grade passes: the third input
        bool mask = false;              ///< a mask pass (mask.frag)
        std::array<QRhiTexture*, 8> maskInputs{}; ///< its bindings 1..8
        MaskTexture* maskTarget = nullptr; ///< a frame-space mask texture instead of `target`
        bool canvasDraws = false;          ///< the canvas so far (`draws`) into `target`: a shader blend's backdrop
        std::vector<Draw> draws;
    };
    std::vector<Pass> passes;
    std::vector<Draw> draws;
    int nextSlot = static_cast<int>(m_placements.size());
    const bool yUp = m_rhi->isYUpInFramebuffer();
    for (const auto& intermediate : m_intermediates) {
        intermediate->inUse = false;
    }
    const auto writeFx = [&](const FxUniforms& u) {
        const int slot = nextSlot++;
        updates->updateDynamicBuffer(m_uniforms.get(), static_cast<quint32>(slot) * slotSize, sizeof(u), &u);
        return slot;
    };
    const auto fxBase = [&](std::int32_t mode) {
        FxUniforms u{};
        storeClip(u.cornerToClip, corr, Affine2D{2.0, 0.0, 0.0, -2.0, -1.0, 1.0});
        storeVec4(u.texel, 1.0 / canvasPixels.width(), 1.0 / canvasPixels.height(), canvasPixels.width(),
                  canvasPixels.height());
        storeVec4(u.frame, composite.canvas.width, composite.canvas.height, composite.canvas.width,
                  composite.canvas.height);
        u.mode[0] = mode;
        u.mode[1] = yUp ? 1 : 0;
        return u;
    };
    const auto withFrame = [&](FxUniforms& u, const Placement& placement) {
        const Affine2D inv = placement.toCanvas.inverted();
        storeVec4(u.toFrame0, inv.a, inv.c, inv.e, 0.0);
        storeVec4(u.toFrame1, inv.b, inv.d, inv.f, 0.0);
        u.frame[0] = static_cast<float>(placement.frame.width);
        u.frame[1] = static_cast<float>(placement.frame.height);
    };
    bool outOfMemory = false;
    const auto acquire = [&]() -> Intermediate* {
        Intermediate* t = acquireIntermediate(canvasPixels);
        outOfMemory = outOfMemory || !t;
        return t;
    };
    const auto release = [](Intermediate* t) {
        if (t) {
            t->inUse = false;
        }
    };
    const auto fxPass = [&](const FxUniforms& u, QRhiTexture* in0, QRhiTexture* in1,
                            QRhiTexture* mipsOf = nullptr) -> Intermediate* {
        Intermediate* out = acquire();
        if (!out) {
            return nullptr;
        }
        passes.push_back({out, -1, writeFx(u), in0, in1 ? in1 : m_dummy.get(), mipsOf});
        return out;
    };
    // Separable Gaussian of `in` (sigma in canvas texels); `in` stays acquired.
    const auto blurOf = [&](Intermediate* in, double sigma, bool repeatEdges) -> Intermediate* {
        int lod = 0;
        while (sigma / std::exp2(lod) > kMaxBlurSigma && lod < 8) {
            ++lod;
        }
        FxUniforms h = fxBase(kFxBlur);
        storeVec4(h.p0, 1.0, 0.0, sigma, lod);
        storeVec4(h.p1, repeatEdges ? 1.0 : 0.0, 0.0, 0.0, 0.0);
        Intermediate* horizontal = fxPass(h, in->texture.get(), nullptr, lod > 0 ? in->texture.get() : nullptr);
        if (!horizontal) {
            return nullptr;
        }
        FxUniforms v = fxBase(kFxBlur);
        storeVec4(v.p0, 0.0, 1.0, sigma, lod);
        storeVec4(v.p1, repeatEdges ? 1.0 : 0.0, 0.0, 0.0, 0.0);
        Intermediate* vertical =
            fxPass(v, horizontal->texture.get(), nullptr, lod > 0 ? horizontal->texture.get() : nullptr);
        release(horizontal);
        return vertical;
    };
    // ---- Colour grade passes ----
    const quint32 gradeSlotSize =
        m_gradeUniforms ? alignUp(sizeof(GradeUniforms), static_cast<quint32>(m_rhi->ubufAlignment())) : 0;
    int nextGradeSlot = 0;
    const auto gradeBase = [&](const color::GradeSpec& g, const Placement& placement, bool maskOn) {
        using color::operator+;
        using color::operator-;
        using color::operator*;
        GradeUniforms u{};
        const color::GradeParams& p = g.params;
        storeClip(u.cornerToClip, corr, Affine2D{2.0, 0.0, 0.0, -2.0, -1.0, 1.0});
        storeVec4(u.texel, 1.0 / canvasPixels.width(), 1.0 / canvasPixels.height(), canvasPixels.width(), canvasPixels.height());
        const Affine2D inv = placement.toCanvas.inverted();
        storeVec4(u.toFrame0, inv.a, inv.c, inv.e, 0.0);
        storeVec4(u.toFrame1, inv.b, inv.d, inv.f, 0.0);
        storeVec4(u.frame, placement.frame.width, placement.frame.height, composite.canvas.width, composite.canvas.height);
        std::int32_t flags = 0;
        const auto flag = [&](bool on, std::int32_t f) { flags |= on ? f : 0; };
        // The CPU reference's conditions (color::gradeMain), one flag each.
        flag(g.inputLutActive(), kGfInputLut);
        flag(g.basicActive(), kGfBasic);
        flag(p.temperature != 0 || p.tint != 0, kGfWb);
        flag(p.contrast != 0, kGfContrast);
        flag(p.highlights != 0 || p.shadows != 0 || p.whites != 0 || p.blacks != 0, kGfTone);
        flag(p.saturation != 100 || p.vibrance != 0, kGfSaturation);
        flag(g.creativeActive(), kGfCreative);
        flag(g.look && !g.look->empty(), kGfLook);
        flag(p.fadedFilm > 0, kGfFaded);
        flag(p.shadowTintAmount > 0 || p.highlightTintAmount > 0, kGfTints);
        flag(p.creativeSaturation != 100 || p.creativeVibrance != 0, kGfCreativeSat);
        flag(g.curvesActive(), kGfCurves);
        flag(g.curves && !g.curves->rgbIdentity, kGfCurvesRgb);
        flag(g.curves && (!g.curves->hueNeutral || !g.curves->satNeutral), kGfCurvesHue);
        flag(g.wheelsActive(), kGfWheels);
        flag(p.cdlSaturation != 1.0, kGfCdlSat);
        flag(g.hslActive() || maskOn, kGfSecondary);
        flag(p.hslTemperature != 0 || p.hslTint != 0, kGfHslWb);
        flag(p.hslContrast != 0, kGfHslContrast);
        flag(p.hslSharpen > 0, kGfHslSharpen);
        flag(p.hslSaturation != 100, kGfHslSat);
        flag(g.vignetteActive(), kGfVignette);
        u.mode[1] = yUp ? 1 : 0;
        u.mode[2] = flags;
        u.mode[3] = maskOn ? view.maskView : 0;
        const auto lutBits = [](const color::Lut* l) { return l ? (l->has1d() ? 1 : 0) | (l->has3d() ? 2 : 0) : 0; };
        const color::Lut* in = g.inputLutActive() ? g.inputLut.get() : nullptr;
        const color::Lut* look = g.look && !g.look->empty() ? g.look.get() : nullptr;
        u.lut[0] = lutBits(in);
        u.lut[1] = lutBits(look);
        u.lut[2] = g.interpolation == color::LutInterpolation::Trilinear ? 1 : 0;
        u.lutSize[0] = in ? in->size3d : 0;
        u.lutSize[1] = stored1dLength(in);
        u.lutSize[2] = look ? look->size3d : 0;
        u.lutSize[3] = stored1dLength(look);
        if (in) {
            storeRgb(u.in1dMin, in->domain1dMin);
            storeRgb(u.in1dMax, in->domain1dMax);
            storeRgb(u.in3dMin, in->domain3dMin);
            storeRgb(u.in3dMax, in->domain3dMax);
        }
        if (look) {
            storeRgb(u.look1dMin, look->domain1dMin);
            storeRgb(u.look1dMax, look->domain1dMax);
            storeRgb(u.look3dMin, look->domain3dMin);
            storeRgb(u.look3dMax, look->domain3dMax);
        }
        storeColorMat3(u.wb, color::whiteBalanceMatrix(p.temperature, p.tint));
        storeColorMat3(u.hslWb, color::whiteBalanceMatrix(p.hslTemperature, p.hslTint));
        storeVec4(u.basic, std::exp2(p.exposure), 1.0 + p.contrast / 100.0, p.saturation, p.vibrance);
        const auto tone = color::toneSplineCoefficients(p);
        storeVec4(u.toneY, tone[0], tone[1], tone[3], tone[4]);
        storeVec4(u.toneM0, tone[5], tone[6], tone[7], tone[8]);
        storeVec4(u.toneM1, tone[9], 0.0, 0.0, 0.0);
        storeVec4(u.creative, p.lookIntensity, p.fadedFilm / 100.0, p.creativeSaturation, p.creativeVibrance);
        storeRgb(u.shadowTint, color::hueChroma(p.shadowTintHue / 360.0) * (p.shadowTintAmount / 100.0 * 0.25), p.tintBalance / 100.0);
        storeRgb(u.highlightTint, color::hueChroma(p.highlightTintHue / 360.0) * (p.highlightTintAmount / 100.0 * 0.25));
        storeRgb(u.cdlSlope, p.gain - p.lift, p.cdlSaturation);
        storeRgb(u.cdlOffset, p.lift + p.offset);
        storeRgb(u.cdlPower, p.gamma);
        storeVec4(u.hslHue, p.hueCenter, p.hueWidth, p.hueSoftness, p.invert ? 1.0 : 0.0);
        storeVec4(u.hslSat, p.satLow, p.satHigh, p.satSoftness, 0.0);
        storeVec4(u.hslLuma, p.lumaLow, p.lumaHigh, p.lumaSoftness, 0.0);
        storeVec4(u.hslCorr, 1.0 + p.hslContrast / 100.0, 2.0 * p.hslSharpen / 100.0, p.hslSaturation / 100.0, 0.0);
        storeRgb(u.hslOffset, p.hslOffset);
        storeVec4(u.vignette, p.vignetteAmount, p.vignetteMidpoint, p.vignetteRoundness, p.vignetteFeather);
        storeVec4(u.split, view.line[0], view.line[1], view.line[2], 0.0);
        return u;
    };
    const auto gradePass = [&](GradeUniforms u, std::int32_t pass, GradeTextures* textures, QRhiTexture* in0,
                               QRhiTexture* in1 = nullptr, QRhiTexture* in2 = nullptr) -> Intermediate* {
        Intermediate* out = acquire();
        if (!out) {
            return nullptr;
        }
        u.mode[0] = pass;
        const int slot = nextGradeSlot++;
        updates->updateDynamicBuffer(m_gradeUniforms.get(), static_cast<quint32>(slot) * gradeSlotSize, sizeof(u), &u);
        Pass p;
        p.target = out;
        p.slot = slot;
        p.in0 = in0;
        p.in1 = in1 ? in1 : m_dummy.get();
        p.in2 = in2 ? in2 : m_dummy.get();
        p.grade = textures;
        passes.push_back(p);
        ++stats.gradePasses;
        return out;
    };
    // A grade on `in` (kept; the caller releases it).
    const auto applyGrade = [&](Intermediate* in, const color::GradeSpec& g, const Placement& placement,
                                std::uint64_t clipId) -> Intermediate* {
        const bool maskOn = view.maskView != 0 && g.params.hsl && (view.maskClip == 0 || view.maskClip == clipId);
        const bool split = view.split;
        if (!g.active() && !maskOn && !split) {
            return nullptr; // nothing to do
        }
        GradeTextures* textures = gradeTextures(g, updates, stats.missingLuts);
        if (!textures) {
            outOfMemory = true;
            return nullptr;
        }
        const GradeUniforms base = gradeBase(g, placement, maskOn);
        const bool secondary = (base.mode[2] & kGfSecondary) != 0;
        const bool refine = secondary && (g.params.denoise > 0 || g.params.blur > 0 || g.params.hslSharpen > 0);
        Intermediate* out = nullptr;
        if (!refine) {
            GradeUniforms u = base;
            u.mode[2] |= kGfFinal;
            out = gradePass(u, kGradeMain, textures, in->texture.get());
        } else {
            Intermediate* main = gradePass(base, kGradeMain, textures, in->texture.get());
            Intermediate* matte = main ? gradePass(base, kGradeMatte, textures, main->texture.get()) : nullptr;
            if (matte && g.params.denoise > 0) {
                Intermediate* soft = blurOf(matte, color::denoiseSigma(g.params) / div, true);
                release(matte);
                matte = soft ? gradePass(base, kGradeThreshold, textures, soft->texture.get()) : nullptr;
                release(soft);
            }
            if (matte && g.params.blur > 0) {
                Intermediate* soft = blurOf(matte, color::refineBlurSigma(g.params) / div, true);
                release(matte);
                matte = soft;
            }
            Intermediate* sharp = main && matte && g.params.hslSharpen > 0
                                      ? blurOf(main, color::kSecondarySharpenSigma / div, true)
                                      : nullptr;
            if (main && matte) {
                out = gradePass(base, kGradeApply, textures, main->texture.get(), matte->texture.get(),
                                sharp ? sharp->texture.get() : main->texture.get());
            }
            release(main);
            release(matte);
            release(sharp);
        }
        if (out && split) {
            Intermediate* mixed = gradePass(base, kGradeMix, textures, out->texture.get(), in->texture.get());
            release(out);
            out = mixed;
        }
        return out;
    };

    // ---- Mask passes (I.6) ----
    const quint32 maskSlotSize =
        m_maskUniforms ? alignUp(sizeof(MaskUniforms), static_cast<quint32>(m_rhi->ubufAlignment())) : 0;
    int nextMaskSlot = 0;
    const auto writeMask = [&](const MaskUniforms& u) {
        const int slot = nextMaskSlot++;
        updates->updateDynamicBuffer(m_maskUniforms.get(), static_cast<quint32>(slot) * maskSlotSize, sizeof(u), &u);
        ++stats.maskPasses;
        return slot;
    };
    const auto maskBase = [&](std::int32_t mode, QSize target) {
        MaskUniforms u{};
        storeClip(u.cornerToClip, corr, Affine2D{2.0, 0.0, 0.0, -2.0, -1.0, 1.0});
        storeVec4(u.texel, 1.0 / target.width(), 1.0 / target.height(), target.width(), target.height());
        u.mode[0] = mode;
        u.mode[1] = yUp ? 1 : 0;
        return u;
    };
    const auto maskFrame = [&](MaskUniforms& u, const Placement& placement) {
        const Affine2D inv = placement.toCanvas.inverted();
        storeVec4(u.toFrame0, inv.a, inv.c, inv.e, 0.0);
        storeVec4(u.toFrame1, inv.b, inv.d, inv.f, 0.0);
        storeVec4(u.frame, placement.frame.width, placement.frame.height, composite.canvas.width, composite.canvas.height);
    };
    // The coverage of `spec` over the layer frame of `placement`, owned by `owner` (a layer's effect or redaction):
    // re-rendered only when its content changes.
    const auto coverageOf = [&](const MaskSpec& spec, const Placement& placement, std::uint64_t owner) -> MaskTexture* {
        const double fw = std::max(1, placement.frame.width);
        const double fh = std::max(1, placement.frame.height);
        // Frame pixels at the canvas resolution, at most kMaxMaskTexture on a side.
        const double scale = std::min({1.0 / div, kMaxMaskTexture / fw, kMaxMaskTexture / fh});
        const QSize size(std::max(16, static_cast<int>(std::lround(fw * scale))), std::max(16, static_cast<int>(std::lround(fh * scale))));
        MaskTexture* coverage = maskTexture(m_coverageTextures, owner, size);
        if (!coverage) {
            outOfMemory = true;
            return nullptr;
        }
        if (coverage->content == spec.key && spec.key != 0) {
            return coverage; // cached while static
        }
        const double margin = kMaskSdfMargin;
        const double sdfScale = 1.0 + 2.0 * margin;
        const QSize sdfSize(std::min(kMaxMaskTexture, static_cast<int>(std::lround(size.width() * sdfScale))),
                            std::min(kMaxMaskTexture, static_cast<int>(std::lround(size.height() * sdfScale))));
        struct Use {
            const MaskShapeSpec* shape;
            MaskTexture* sdf;
            std::array<double, 4> domain;
            bool empty = false; ///< covers nothing (kMaskEmptyDistance): no distance texture
            QRhiTexture* distances = nullptr; ///< AI-14: a matte's uploaded field instead of sdf->texture
        };
        std::vector<Use> uses;
        for (std::size_t i = 0; i < spec.shapes.size(); ++i) {
            const MaskShapeSpec& s = spec.shapes[i];
            const auto& m = s.affine;
            const double det = m[0] * m[4] - m[1] * m[3];
            // AI-14 hook: an object matte (render/Matte.h). Its distance field over the frame is uploaded as the shape's
            // distance texture (domain = the frame) instead of rendering the polygon's distances; COMPOSE and every
            // pass after it are unchanged. Re-uploaded only when the matte's identity (the shape key) changes, from the
            // half floats the source made with the field (no per-texel conversion here). A field that is not usable
            // never gets here (maskSpecAt drops it); the polygon below, the bounding box, stands in like on the CPU.
            if (s.matte && usableField(*s.matte) && std::abs(det) >= 1e-12) {
                auto& slot = m_matteTextures[owner * 31u + i + 1];
                const QSize fieldSize(s.matte->width, s.matte->height);
                const bool single = m_rhi->isTextureFormatSupported(QRhiTexture::R16F);
                if (!slot || slot->size != fieldSize) {
                    auto created = std::make_unique<MatteTexture>();
                    created->texture.reset(m_rhi->newTexture(single ? QRhiTexture::R16F : QRhiTexture::RGBA16F, fieldSize));
                    created->texture->setName(QByteArrayLiteral("wabcut matte"));
                    if (!created->texture->create()) {
                        outOfMemory = true;
                        return nullptr;
                    }
                    created->size = fieldSize;
                    slot = std::move(created);
                    m_maskSrbs.clear(); // they may name the texture this one replaced
                }
                slot->lastUsedFrame = m_frameCounter;
                if (slot->content != s.key || s.key == 0) {
                    const int w = s.matte->width, h = s.matte->height, channels = single ? 1 : 4;
                    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
                    const bool precomputed = s.matte->half.size() == count;
                    std::vector<std::uint16_t> texels(count * channels, 0);
                    for (int y = 0; y < h; ++y) {
                        // Stored bottom-up on y-up backends, as the shader's stored() expects of every mask texture.
                        const int row = yUp ? h - 1 - y : y;
                        const std::size_t from = static_cast<std::size_t>(y) * w;
                        std::uint16_t* out = texels.data() + static_cast<std::size_t>(row) * w * channels;
                        if (precomputed && channels == 1) {
                            std::memcpy(out, s.matte->half.data() + from, static_cast<std::size_t>(w) * sizeof(std::uint16_t));
                            continue;
                        }
                        for (int x = 0; x < w; ++x) {
                            out[static_cast<std::size_t>(x) * channels] =
                                precomputed ? s.matte->half[from + x] : toHalf(s.matte->distance[from + x]);
                        }
                    }
                    updates->uploadTexture(slot->texture.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                                                    0, 0, QRhiTextureSubresourceUploadDescription(
                                                                              texels.data(), static_cast<quint32>(texels.size() * sizeof(std::uint16_t))))));
                    slot->content = s.key;
                    ++stats.maskTexturesRendered;
                }
                uses.push_back({&s, nullptr, {0.0, 0.0, 1.0, 1.0}, false, slot->texture.get()});
                continue;
            }
            if (s.polygon.size() < 3 || std::abs(det) < 1e-12) {
                // Covers nothing, but still combines like the CPU reference: inverted it covers everything, as an
                // Intersect it clears everything (Codex wave-4 P2-31). It used to be skipped.
                uses.push_back({&s, nullptr, {}, true});
                continue;
            }
            // The outline can start outside the frame and be tracked back into view. Bound its own geometry,
            // independently of the tracked affine, so transform-only changes still reuse the distance texture.
            double x0 = s.polygon.front().x, x1 = x0, y0 = s.polygon.front().y, y1 = y0;
            for (const Vec2& p : s.polygon) {
                x0 = std::min(x0, p.x);
                x1 = std::max(x1, p.x);
                y0 = std::min(y0, p.y);
                y1 = std::max(y1, p.y);
            }
            // The distance texture covers the outline padded by the margin or by as far as the soft edge reaches past
            // the outline (feather / 2 + expansion, frame heights, through the shape's scale), whichever is more:
            // beyond the texture the distance is clamped and the coverage would plateau (Codex wave-4 P2-32). The pad
            // grows in whole margins (at most kMaxMaskSdfPads), so an animated feather does not re-render the texture
            // every frame, and is part of the texture's content key.
            constexpr double kMaxMaskSdfPads = 64.0;
            const double reach = std::max(0.0, s.feather / 2.0 + std::max(0.0, s.expansion)) / std::sqrt(std::abs(det));
            const double extra = std::min(kMaxMaskSdfPads, std::ceil(std::max(0.0, reach - margin) / margin));
            const double pad = margin * (1.0 + extra);
            const double dw = std::max(1.0, x1 - x0) + 2.0 * pad;
            const double dh = std::max(1.0, y1 - y0) + 2.0 * pad;
            const std::array<double, 4> domain{(x0 + x1 - dw) / 2.0, (y0 + y1 - dh) / 2.0, dw, dh};
            const std::uint64_t sdfKey =
                extra == 0.0 ? s.key : (s.key ^ (static_cast<std::uint64_t>(extra) * 0x9E3779B97F4A7C15ull)) | 1u;
            MaskTexture* sdf = maskTexture(m_sdfTextures, owner * 31u + i + 1, sdfSize);
            if (!sdf) {
                outOfMemory = true;
                return nullptr;
            }
            if (sdf->content != sdfKey || s.key == 0) {
                const int n = static_cast<int>(s.polygon.size());
                if (!sdf->segments || sdf->segmentCapacity < n) {
                    sdf->segments.reset(m_rhi->newTexture(QRhiTexture::RGBA32F, QSize(std::max(n, 64), 1)));
                    if (!sdf->segments->create()) {
                        outOfMemory = true;
                        return nullptr;
                    }
                    sdf->segmentCapacity = std::max(n, 64);
                    m_maskSrbs.clear();
                }
                std::vector<float> segments(static_cast<std::size_t>(sdf->segmentCapacity) * 4, 0.0f);
                for (int k = 0; k < n; ++k) {
                    const Vec2& a = s.polygon[static_cast<std::size_t>(k)];
                    const Vec2& b = s.polygon[static_cast<std::size_t>((k + 1) % n)];
                    const std::size_t o = static_cast<std::size_t>(k) * 4;
                    segments[o] = static_cast<float>(a.x * spec.aspect);
                    segments[o + 1] = static_cast<float>(a.y);
                    segments[o + 2] = static_cast<float>(b.x * spec.aspect);
                    segments[o + 3] = static_cast<float>(b.y);
                }
                updates->uploadTexture(sdf->segments.get(),
                                       QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                           0, 0, QRhiTextureSubresourceUploadDescription(segments.data(), static_cast<quint32>(segments.size() * sizeof(float))))));
                MaskUniforms u = maskBase(kMaskSdf, sdfSize);
                u.mode[2] = n;
                storeVec4(u.domain, domain[0], domain[1], domain[2], domain[3]);
                storeVec4(u.aspect, spec.aspect, 0.0, 0.0, 0.0);
                Pass pass{};
                pass.mask = true;
                pass.slot = writeMask(u);
                pass.maskInputs[7] = sdf->segments.get();
                pass.maskTarget = sdf;
                passes.push_back(pass);
                sdf->content = sdfKey;
                ++stats.maskTexturesRendered;
            }
            uses.push_back({&s, sdf, domain, false});
        }
        // Compose in chunks; more than one chunk ping-pongs through scratch textures.
        const std::int32_t start = !spec.shapes.empty() && spec.shapes.front().mode != 0 ? 1 : 0;
        const int chunks = std::max<int>(1, static_cast<int>((uses.size() + kShapesPerCompose - 1) / kShapesPerCompose));
        MaskTexture* previous = nullptr;
        for (int c = 0; c < chunks; ++c) {
            MaskTexture* target = c == chunks - 1 ? coverage : maskTexture(m_coverageTextures, ~owner - static_cast<std::uint64_t>(c % 2), size);
            if (!target) {
                outOfMemory = true;
                return nullptr;
            }
            MaskUniforms u = maskBase(kMaskCompose, size);
            u.mode[3] = c == 0 ? start : 2;
            storeVec4(u.domain, 0.0, 0.0, 1.0, 1.0);
            storeVec4(u.sdfDomain, -margin, -margin, sdfScale, sdfScale);
            storeVec4(u.aspect, spec.aspect, 1.0 / size.height(), 0.0, 0.0);
            Pass pass{};
            pass.mask = true;
            int count = 0;
            for (std::size_t k = static_cast<std::size_t>(c) * kShapesPerCompose; k < uses.size() && count < kShapesPerCompose; ++k, ++count) {
                const MaskShapeSpec& s = *uses[k].shape;
                storeVec4(u.edge + count * 4, std::max(0.0, s.feather), s.expansion, std::clamp(s.opacity, 0.0, 1.0), s.inverted ? 1.0 : 0.0);
                u.combine[count * 4] = s.mode;
                if (uses[k].empty) {
                    storeVec4(u.affine0 + count * 4, 0.0, 0.0, 0.0, 0.0);
                    storeVec4(u.affine1 + count * 4, 0.0, 0.0, 0.0, 1.0); // the shader's "covers nothing" flag
                    continue; // its sampler slot stays unbound (the dummy): nothing is read from it
                }
                const auto& m = s.affine;
                const double det = m[0] * m[4] - m[1] * m[3];
                const double ia = m[4] / det;
                const double ib = -m[1] / det;
                const double id = -m[3] / det;
                const double ie = m[0] / det;
                const auto& domain = uses[k].domain;
                const double sx = sdfScale / domain[2], sy = sdfScale / domain[3];
                // Fold this shape's texture domain into its lookup affine; the shader's common sdfDomain stays
                // fixed, and the sampled distances remain in original frame-height units.
                storeVec4(u.affine0 + count * 4, sx * ia, sx * ib,
                          sx * (-(ia * m[2] + ib * m[5]) - domain[0]) - margin, std::sqrt(std::abs(det)));
                storeVec4(u.affine1 + count * 4, sy * id, sy * ie,
                          sy * (-(id * m[2] + ie * m[5]) - domain[1]) - margin, 0.0);
                pass.maskInputs[static_cast<std::size_t>(3 + count)] = uses[k].distances ? uses[k].distances : uses[k].sdf->texture.get();
            }
            u.mode[2] = count;
            pass.maskInputs[2] = previous ? previous->texture.get() : nullptr;
            pass.slot = writeMask(u);
            pass.maskTarget = target;
            passes.push_back(pass);
            previous = target;
        }
        coverage->content = spec.key;
        ++stats.maskTexturesRendered;
        return coverage;
    };
    // A canvas-space mask pass into a new intermediate.
    const auto maskApply = [&](std::int32_t mode, QRhiTexture* in0, QRhiTexture* in1, MaskTexture* coverage,
                               const Placement& placement, QRhiTexture* mipsOf = nullptr,
                               const std::array<double, 4>& style = {}) -> Intermediate* {
        Intermediate* out = acquire();
        if (!out) {
            return nullptr;
        }
        MaskUniforms u = maskBase(mode, canvasPixels);
        maskFrame(u, placement);
        storeVec4(u.style, style[0], style[1], style[2], style[3]);
        Pass pass{};
        pass.target = out;
        pass.mask = true;
        pass.mipsOf = mipsOf;
        pass.maskInputs[0] = in0;
        pass.maskInputs[1] = in1;
        pass.maskInputs[2] = coverage ? coverage->texture.get() : nullptr;
        pass.slot = writeMask(u);
        passes.push_back(pass);
        return out;
    };
    // Canvas texels per frame height of a layer (lengths in frame heights become canvas texels).
    const auto frameHeightTexels = [&](const Placement& placement) {
        const Affine2D& m = placement.toCanvas;
        return std::hypot(m.c, m.d) * placement.frame.height / div;
    };
    const auto mosaicOf = [&](Intermediate* in, double cell, const Placement& placement) -> Intermediate* {
        const double lod = std::clamp(std::floor(std::log2(std::max(cell, 1.0))), 0.0, 8.0);
        return maskApply(kMaskMosaic, in->texture.get(), nullptr, nullptr, placement, lod > 0.0 ? in->texture.get() : nullptr,
                         {cell, lod, 0.0, 0.0});
    };
    const auto maskOwner = [](std::uint64_t clip, int layer, int kind, std::size_t index) {
        std::uint64_t h = clip != 0 ? clip : 0x8000000000000000ull + static_cast<std::uint64_t>(layer);
        h = h * 1099511628211ull ^ static_cast<std::uint64_t>(kind);
        return h * 1099511628211ull ^ static_cast<std::uint64_t>(index);
    };
    int currentLayer = -1; // the composite layer being planned (mask owners)

    // One stack effect on `in` (released unless returned).
    const auto applyEffect = [&](Intermediate* in, const LayerEffect& e, const Placement& placement,
                                 std::size_t effectIndex) -> Intermediate* {
        const auto& q = e.params;
        Intermediate* out = nullptr;
        switch (e.type) {
        case EffectType::Crop: {
            FxUniforms u = fxBase(kFxCrop);
            withFrame(u, placement);
            storeVec4(u.p0, q[0], q[1], q[2], q[3]);
            const Affine2D& m = placement.toCanvas;
            storeVec4(u.p1, std::max(0.0, q[4]), std::hypot(m.a, m.b), std::hypot(m.c, m.d), div);
            out = fxPass(u, in->texture.get(), nullptr);
            break;
        }
        case EffectType::GaussianBlur: {
            const double sigma = std::max(0.0, q[0]) / 3.0 / div;
            if (sigma < 0.05) {
                return in; // nothing to blur
            }
            out = blurOf(in, sigma, q[1] > 0.5);
            break;
        }
        case EffectType::BasicCorrection: {
            FxUniforms u = fxBase(kFxCorrection);
            storeVec4(u.p0, q[0], q[1], q[2], q[3]);
            storeVec4(u.p1, q[4], 0.0, 0.0, 0.0);
            out = fxPass(u, in->texture.get(), nullptr);
            break;
        }
        case EffectType::Vignette: {
            FxUniforms u = fxBase(kFxVignette);
            withFrame(u, placement);
            storeVec4(u.p0, q[0], q[1], q[2], q[3]);
            out = fxPass(u, in->texture.get(), nullptr);
            break;
        }
        case EffectType::Sharpen: {
            const double sigma = std::max(0.0, q[1]) / div;
            if (q[0] <= 0.0 || sigma < 0.05) {
                return in;
            }
            Intermediate* blurred = blurOf(in, sigma, true);
            if (!blurred) {
                break;
            }
            FxUniforms u = fxBase(kFxSharpen);
            storeVec4(u.p0, q[0] / 100.0, 0.0, 0.0, 0.0);
            out = fxPass(u, in->texture.get(), blurred->texture.get());
            release(blurred);
            break;
        }
        case EffectType::DropShadow: {
            const double angle = q[1] * 3.14159265358979323846 / 180.0;
            const double distance = std::max(0.0, q[2]) / div;
            FxUniforms u = fxBase(kFxShadow);
            storeVec4(u.p0, distance * std::sin(angle), -distance * std::cos(angle), std::clamp(q[0], 0.0, 1.0), 0.0);
            storeVec4(u.p1, srgbToLinear(q[4]), srgbToLinear(q[5]), srgbToLinear(q[6]), 0.0);
            Intermediate* shadow = fxPass(u, in->texture.get(), nullptr);
            if (!shadow) {
                break;
            }
            const double sigma = std::max(0.0, q[3]) / 3.0 / div;
            if (sigma >= 0.05) {
                Intermediate* soft = blurOf(shadow, sigma, false);
                release(shadow);
                shadow = soft;
                if (!shadow) {
                    break;
                }
            }
            out = fxPass(fxBase(kFxOver), in->texture.get(), shadow->texture.get());
            release(shadow);
            break;
        }
        case EffectType::Grade: {
            if (!e.grade) {
                return in;
            }
            out = applyGrade(in, *e.grade, placement, placement.clipId);
            if (!out) {
                return outOfMemory ? nullptr : in; // nothing to do keeps the layer as it is
            }
            break;
        }
        case EffectType::OpacityMask: {
            if (!e.mask) {
                return in;
            }
            MaskTexture* coverage = coverageOf(*e.mask, placement, maskOwner(placement.clipId, currentLayer, 1, effectIndex));
            if (!coverage) {
                return nullptr;
            }
            out = maskApply(kMaskAlpha, in->texture.get(), nullptr, coverage, placement);
            break;
        }
        case EffectType::Mosaic: {
            const double cell = std::max(0.0, q[0]) * frameHeightTexels(placement);
            if (cell < 1.5) {
                return in;
            }
            out = mosaicOf(in, cell, placement);
            break;
        }
        case EffectType::Border: { // C.15: facecam layouts
            const double width = std::max(0.0, q[0]) / div;
            const double opacity = std::clamp(q[4], 0.0, 1.0);
            if (width < 0.05 || opacity <= 0.0) {
                return in;
            }
            FxUniforms u = fxBase(kFxBorder);
            storeVec4(u.p0, width, opacity, 0.0, 0.0);
            storeVec4(u.p1, srgbToLinear(q[1]), srgbToLinear(q[2]), srgbToLinear(q[3]), 0.0);
            out = fxPass(u, in->texture.get(), nullptr);
            break;
        }
        case EffectType::TrackMatte: { // C.15 Track Matte Key (0.1.2), docs/internal/c15-parity-handoff.md
            const auto found = matteTextures.find({currentLayer, effectIndex});
            QRhiTexture* matte = found != matteTextures.end() ? found->second : nullptr;
            FxUniforms u = fxBase(kFxTrackMatte);
            storeVec4(u.p0, q[1] > 0.5 ? 1.0 : 0.0, q[2] > 0.5 ? 1.0 : 0.0, q[3] > 0.5 ? 1.0 : 0.0, matte ? 1.0 : 0.0);
            out = fxPass(u, in->texture.get(), matte ? matte : m_dummy.get());
            break;
        }
        case EffectType::UltraKey: { // C.15 keyer; CPU reference render/reference/UltraKey.cpp
            const auto unit = [](double v) { return std::isnan(v) ? 0.0 : std::clamp(v, 0.0, 1.0); };
            const std::array<double, 3> key{unit(q[1]), unit(q[2]), unit(q[3])};
            FxUniforms mu = fxBase(kFxKeyMatte);
            storeVec4(mu.p0, key[0], key[1], key[2], q[4]);
            storeVec4(mu.p1, q[5], q[6], q[7], q[8]);
            Intermediate* matte = fxPass(mu, in->texture.get(), nullptr);
            if (!matte) {
                break;
            }
            // The reference's lengths are fractions of the picture width: the canvas width here (any preview divisor).
            const double width = canvasPixels.width();
            const double choke = std::max(0.0, q[9]) / 100.0 * 0.005 * width;
            if (choke >= 1.0) {
                FxUniforms cu = fxBase(kFxKeyChoke);
                storeVec4(cu.p0, choke, std::floor(choke), 0.0, 0.0);
                Intermediate* choked = fxPass(cu, matte->texture.get(), nullptr);
                release(matte);
                matte = choked;
                if (!matte) {
                    break;
                }
            }
            const double soften = std::max(0.0, q[10]) / 100.0 * 0.005 * width;
            if (soften >= 0.05) {
                Intermediate* soft = blurOf(matte, soften, true);
                release(matte);
                matte = soft;
                if (!matte) {
                    break;
                }
            }
            int keyChannel = 0; // the first of the largest in R, G, B order
            for (int i = 1; i < 3; ++i) {
                if (key[static_cast<std::size_t>(i)] > key[static_cast<std::size_t>(keyChannel)]) {
                    keyChannel = i;
                }
            }
            FxUniforms au = fxBase(kFxKeyApply);
            storeVec4(au.p0, key[0], key[1], key[2], std::clamp(std::round(q[0]), 0.0, 2.0));
            storeVec4(au.p1, q[7], q[8], q[11], q[12]);
            storeVec4(au.p2, q[13], q[14], q[15], q[16]);
            // toFrame0 is free in this mode (no frame mapping): saturation, hue (radians), luminance, key channel.
            storeVec4(au.toFrame0, q[17], q[18] * 3.14159265358979323846 / 180.0, q[19], keyChannel);
            out = fxPass(au, in->texture.get(), matte->texture.get());
            release(matte);
            break;
        }
        }
        // A masked effect: its result mixed into its input by the masks' coverage (local corrections).
        if (e.mask && !e.mask->shapes.empty() && e.type != EffectType::OpacityMask && out && out != in) {
            MaskTexture* coverage = coverageOf(*e.mask, placement, maskOwner(placement.clipId, currentLayer, 2, effectIndex));
            Intermediate* mixed = coverage ? maskApply(kMaskMix, out->texture.get(), in->texture.get(), coverage, placement) : nullptr;
            release(out);
            release(in);
            return mixed;
        }
        release(in);
        return out;
    };
    // The layer's redactions on its own pixels (before any effect): each style mixed in by its coverage.
    const auto applyRedactions = [&](Intermediate* current, const CompositeLayer& layer, const Placement& placement) -> Intermediate* {
        for (std::size_t r = 0; r < layer.redactions.size() && current; ++r) {
            const LayerRedaction& red = layer.redactions[r];
            if (!red.mask || red.mask->shapes.empty()) {
                continue;
            }
            const double heights = frameHeightTexels(placement);
            Intermediate* styled = nullptr;
            switch (red.style) {
            case LayerRedaction::Style::Blur:
                styled = blurOf(current, std::max(0.5, red.strength * 0.5 * heights), true);
                break;
            case LayerRedaction::Style::Mosaic:
                styled = mosaicOf(current, std::max(2.0, red.strength * heights), placement);
                break;
            case LayerRedaction::Style::Solid:
                styled = maskApply(kMaskSolid, nullptr, nullptr, nullptr, placement, nullptr,
                                   {srgbToLinear(red.color[0]), srgbToLinear(red.color[1]), srgbToLinear(red.color[2]), 1.0});
                break;
            }
            if (!styled) {
                return nullptr;
            }
            MaskTexture* coverage = coverageOf(*red.mask, placement, maskOwner(placement.clipId, currentLayer, 3, r));
            Intermediate* mixed = coverage ? maskApply(kMaskMix, styled->texture.get(), current->texture.get(), coverage, placement) : nullptr;
            release(styled);
            release(current);
            current = mixed;
            ++stats.redactionsApplied;
        }
        return current;
    };
    // A layer with its effects into an intermediate (null when it shows nothing).
    const auto renderLayer = [&](int compositeIndex) -> Intermediate* {
        const int placementIndex = placementOf[static_cast<std::size_t>(compositeIndex)];
        if (placementIndex < 0) {
            return nullptr;
        }
        const Placement& placement = m_placements[static_cast<std::size_t>(placementIndex)];
        Intermediate* current = acquire();
        if (!current) {
            return nullptr;
        }
        passes.push_back({current, placementIndex, placement.uniformSlot, nullptr, nullptr, nullptr});
        currentLayer = compositeIndex;
        const CompositeLayer& layer = composite.layers[static_cast<std::size_t>(compositeIndex)];
        current = applyRedactions(current, layer, placement);
        if (!current) {
            return nullptr;
        }
        for (std::size_t e = 0; e < layer.effects.size(); ++e) {
            current = applyEffect(current, layer.effects[e], placement, e);
            if (!current) {
                return nullptr;
            }
            ++stats.effectsApplied;
        }
        return current;
    };
    const auto blitSlot = [&]() { return writeFx(fxBase(kFxCopy)); };

    for (int i = 0; i < layerCount && !outOfMemory;) {
        const CompositeLayer& layer = composite.layers[static_cast<std::size_t>(i)];
        if (layer.transition) {
            const LayerTransition& mix = *layer.transition;
            const int count = std::clamp(mix.layerCount, 1, layerCount - i);
            const bool hasA = !(count == 1 && mix.incomingOnly);
            const bool hasB = count == 2 || mix.incomingOnly;
            Intermediate* a = hasA ? renderLayer(i) : nullptr;
            Intermediate* b = hasB ? renderLayer(i + (hasA ? 1 : 0)) : nullptr;
            FxUniforms u = fxBase(kFxTransition);
            const double kind = mix.kind == TransitionKind::DipToColor ? 1.0 : mix.kind == TransitionKind::Wipe ? 2.0 : 0.0;
            storeVec4(u.p0, std::clamp(mix.progress, 0.0, 1.0), kind, mix.angleDegrees * 3.14159265358979323846 / 180.0,
                      std::clamp(mix.feather, 0.0, 1.0));
            storeVec4(u.p1, mix.color[0], mix.color[1], mix.color[2], div);
            Intermediate* mixed = fxPass(u, a ? a->texture.get() : m_dummy.get(), b ? b->texture.get() : nullptr);
            release(a);
            release(b);
            if (mixed) {
                draws.push_back({-1, mixed, blitSlot()});
                ++stats.transitions;
            }
            i += count;
            continue;
        }
        const int placementIndex = placementOf[static_cast<std::size_t>(i)];
        if (placementIndex >= 0) {
            const int blend = blendVariantOf(layer.blend);
            if (blend < 0) {
                // A shader blend (0.1.2): the canvas pass splits here. What is drawn so far goes into an intermediate,
                // the backdrop; the layer and the backdrop make the blended canvas, on which the layers above draw.
                Intermediate* source = renderLayer(i);
                Intermediate* backdrop = source ? acquire() : nullptr;
                if (backdrop) {
                    Pass canvasSoFar{};
                    canvasSoFar.target = backdrop;
                    canvasSoFar.canvasDraws = true;
                    canvasSoFar.draws = std::move(draws);
                    draws.clear();
                    for (const Draw& d : canvasSoFar.draws) {
                        release(d.result); // read by this pass only
                    }
                    passes.push_back(std::move(canvasSoFar));
                    FxUniforms u = fxBase(kFxBlend);
                    u.mode[2] = static_cast<std::int32_t>(layer.blend);
                    const std::uint64_t seed = layer.clipId;
                    storeVec4(u.p1, static_cast<double>((seed >> 48) & 0xFFFFu), static_cast<double>((seed >> 32) & 0xFFFFu),
                              static_cast<double>((seed >> 16) & 0xFFFFu), static_cast<double>(seed & 0xFFFFu));
                    const std::int64_t frame = composite.frameDuration.value > 0
                                                   ? composite.targetTime.value / composite.frameDuration.value
                                                   : 0;
                    const auto frameBits = static_cast<std::uint32_t>(static_cast<std::uint64_t>(frame) & 0xFFFFFFFFu);
                    storeVec4(u.p2, static_cast<double>(frameBits >> 16), static_cast<double>(frameBits & 0xFFFFu), 0.0, 0.0);
                    if (Intermediate* blended = fxPass(u, source->texture.get(), backdrop->texture.get())) {
                        draws.push_back({-1, blended, blitSlot(), 0});
                        ++stats.shaderBlends;
                    }
                }
                release(source);
                release(backdrop);
            } else if (layer.effects.empty() && layer.redactions.empty()) {
                draws.push_back({placementIndex, nullptr, 0, blend});
            } else if (Intermediate* result = renderLayer(i)) {
                draws.push_back({-1, result, blitSlot(), blend});
            }
        }
        ++i;
    }
    if (outOfMemory) {
        for (const Pass& pass : passes) {
            if (pass.maskTarget) {
                pass.maskTarget->content = 0; // planned, never drawn: not what the cache says it holds
            }
        }
        updates->release();
        return std::unexpected(Error{ErrorCode::DeviceError, "Could not allocate effect intermediates"});
    }
    stats.effectPasses += static_cast<int>(passes.size());

    // ---- Output uniforms ----
    const QSize targetPixels = canvasOnly ? canvasPixels : output.target->pixelSize();
    const QRect viewport = canvasOnly                 ? QRect(QPoint(0, 0), canvasPixels)
                           : output.viewport.isEmpty() ? letterboxRect(targetPixels, composite.canvas)
                                                       : output.viewport;
    const int outputSlot = nextSlot++;
    if (!canvasOnly) {
        OutputUniforms u{};
        storeClip(u.cornerToClip, corr, Affine2D{2.0, 0.0, 0.0, -2.0, -1.0, 1.0});
        u.uvXform[0] = 1.0f;
        u.uvXform[1] = yUp ? -1.0f : 1.0f;
        u.uvXform[2] = 0.0f;
        u.uvXform[3] = yUp ? 1.0f : 0.0f;
        u.mode[0] = outputCode(outputTransformFor(colorPipeline, output.role));
        u.mode[1] = output.alpha == OutputAlpha::Opaque ? 0 : output.alpha == OutputAlpha::Straight ? 1 : 2;
        updates->updateDynamicBuffer(m_uniforms.get(), static_cast<quint32>(outputSlot) * slotSize, sizeof(u),
                                     &u);
    }
    if (!passes.empty() || !draws.empty()) {
        if (auto ok = ensureFxPipelines(); !ok && !passes.empty()) {
            updates->release();
            return std::unexpected(ok.error());
        }
    }

    const QRhiCommandBuffer::VertexInput quad(m_quad.get(), 0);
    const QRhiViewport canvasViewport(0, 0, static_cast<float>(canvasPixels.width()),
                                      static_cast<float>(canvasPixels.height()));
    const auto drawLayer = [&](const Placement& p, QRhiGraphicsPipeline*& bound, int blend) {
        QRhiGraphicsPipeline* pipeline =
            p.frame.arrayTextures ? blendPipeline(m_layerArrayPipeline.get(), m_layerArrayBlendPipelines, blend)
                                  : blendPipeline(m_layerPipeline.get(), m_layerBlendPipelines, blend);
        if (pipeline != bound) {
            cb->setGraphicsPipeline(pipeline);
            cb->setViewport(canvasViewport);
            bound = pipeline;
        }
        const QRhiCommandBuffer::DynamicOffset offset(0, static_cast<quint32>(p.uniformSlot) * slotSize);
        QRhiShaderResourceBindings* srb = p.srb                  ? p.srb
                                          : p.frame.arrayTextures ? m_nativeSrbs[p.frame.setSerial].srb.get()
                                                                  : m_layerSrbs[static_cast<std::size_t>(p.frame.slot)]->srb.get();
        cb->setShaderResources(srb, 1, &offset);
        cb->setVertexInput(0, 1, &quad);
        cb->draw(4);
    };
    const auto drawFx = [&](QRhiGraphicsPipeline* pipeline, QRhiGraphicsPipeline*& bound, int slot, QRhiTexture* in0,
                            QRhiTexture* in1) {
        QRhiShaderResourceBindings* srb = fxSrb(in0, in1);
        if (!srb) {
            return;
        }
        if (pipeline != bound) {
            cb->setGraphicsPipeline(pipeline);
            cb->setViewport(canvasViewport);
            bound = pipeline;
        }
        const QRhiCommandBuffer::DynamicOffset offset(0, static_cast<quint32>(slot) * slotSize);
        cb->setShaderResources(srb, 1, &offset);
        cb->setVertexInput(0, 1, &quad);
        cb->draw(4);
    };

    // Blend-mode pipelines (C.15) are made before the passes, not inside them.
    const auto prepareBlends = [&](const std::vector<Draw>& list) {
        for (const Draw& draw : list) {
            if (draw.blend <= 0) {
                continue;
            }
            if (draw.placement >= 0) {
                const Placement& p = m_placements[static_cast<std::size_t>(draw.placement)];
                if (p.frame.arrayTextures) {
                    blendPipeline(m_layerArrayPipeline.get(), m_layerArrayBlendPipelines, draw.blend);
                } else {
                    blendPipeline(m_layerPipeline.get(), m_layerBlendPipelines, draw.blend);
                }
            } else {
                blendPipeline(m_fxOverPipeline.get(), m_fxBlendPipelines, draw.blend);
            }
        }
    };
    prepareBlends(draws);
    for (const Pass& pass : passes) {
        prepareBlends(pass.draws);
    }
    const auto drawCanvas = [&](const std::vector<Draw>& list) {
        QRhiGraphicsPipeline* bound = nullptr;
        for (const Draw& draw : list) {
            if (draw.placement >= 0) {
                drawLayer(m_placements[static_cast<std::size_t>(draw.placement)], bound, draw.blend);
            } else {
                drawFx(blendPipeline(m_fxOverPipeline.get(), m_fxBlendPipelines, draw.blend), bound, draw.slot,
                       draw.result->texture.get(), m_dummy.get());
            }
        }
    };

    // ---- Offscreen passes ----
    QRhiResourceUpdateBatch* pending = updates; // the first pass submits the uniforms and uploads
    bool maskPassFailed = false;
    for (const Pass& pass : passes) {
        if (pass.mipsOf) {
            QRhiResourceUpdateBatch* mips = m_rhi->nextResourceUpdateBatch();
            mips->generateMips(pass.mipsOf);
            cb->resourceUpdate(mips);
        }
        QRhiRenderTarget* rt = pass.maskTarget ? static_cast<QRhiRenderTarget*>(pass.maskTarget->target.get())
                                               : static_cast<QRhiRenderTarget*>(pass.target->target.get());
        cb->beginPass(rt, QColor(0, 0, 0, 0), {1.0f, 0}, pending);
        pending = nullptr;
        QRhiGraphicsPipeline* bound = nullptr;
        if (pass.mask) {
            if (QRhiShaderResourceBindings* srb = maskSrb(pass.maskInputs)) {
                const QSize size = rt->pixelSize();
                cb->setGraphicsPipeline(m_maskPipeline.get());
                cb->setViewport(QRhiViewport(0, 0, static_cast<float>(size.width()), static_cast<float>(size.height())));
                const QRhiCommandBuffer::DynamicOffset offset(0, static_cast<quint32>(pass.slot) * maskSlotSize);
                cb->setShaderResources(srb, 1, &offset);
                cb->setVertexInput(0, 1, &quad);
                cb->draw(4);
            } else if (pass.maskTarget) {
                pass.maskTarget->content = 0;
                maskPassFailed = true;
            }
        } else if (pass.canvasDraws) {
            drawCanvas(pass.draws);
        } else if (pass.placement >= 0) {
            drawLayer(m_placements[static_cast<std::size_t>(pass.placement)], bound, 0);
        } else if (pass.grade) {
            GradeTextures& t = *pass.grade;
            QRhiShaderResourceBindings* srb =
                gradeSrb({pass.in0, pass.in1, pass.in2, t.curves.get(), t.input3d ? t.input3d : m_dummy3d.get(),
                          t.look3d ? t.look3d : m_dummy3d.get()});
            if (srb) {
                cb->setGraphicsPipeline(m_gradePipeline.get());
                cb->setViewport(canvasViewport);
                const QRhiCommandBuffer::DynamicOffset offset(
                    0, static_cast<quint32>(pass.slot) * alignUp(sizeof(GradeUniforms), static_cast<quint32>(m_rhi->ubufAlignment())));
                cb->setShaderResources(srb, 1, &offset);
                cb->setVertexInput(0, 1, &quad);
                cb->draw(4);
            }
        } else {
            drawFx(m_fxPipeline.get(), bound, pass.slot, pass.in0, pass.in1);
        }
        cb->endPass();
    }
    if (maskPassFailed) {
        // A mask pass could not run (no resource bindings): the distance or coverage textures later passes of this frame
        // read from it hold nothing valid, yet their cache keys were set while planning. Forget every mask texture this
        // frame planned, so the next frame renders them again instead of showing stale coverage (Codex wave-4 P2-33).
        for (const Pass& pass : passes) {
            if (pass.maskTarget) {
                pass.maskTarget->content = 0;
            }
        }
    }

    // ---- Layer pass ----
    cb->beginPass(m_canvasTarget.get(), QColor(0, 0, 0, 0), {1.0f, 0}, pending);
    drawCanvas(draws);
    // Downscaled output: sample a mip chain instead of aliasing.
    QRhiResourceUpdateBatch* mips = nullptr;
    if (output.drawCanvas &&
        (viewport.width() < canvasPixels.width() || viewport.height() < canvasPixels.height())) {
        mips = m_rhi->nextResourceUpdateBatch();
        mips->generateMips(m_canvas.get());
    }
    cb->endPass(mips);
    if (canvasOnly) {
        return stats; // a nested composite: the parent samples the canvas itself
    }

    // ---- Output pass ----
    const QColor surround =
        QColor::fromRgbF(output.surround[0], output.surround[1], output.surround[2], output.surround[3]);
    cb->beginPass(output.target, surround, {1.0f, 0});
    if (output.drawCanvas && !viewport.isEmpty()) {
        cb->setGraphicsPipeline(m_outputPipeline.get());
        // QRhiViewport has a bottom-left origin.
        cb->setViewport(
            QRhiViewport(static_cast<float>(viewport.x()),
                         static_cast<float>(targetPixels.height() - viewport.y() - viewport.height()),
                         static_cast<float>(viewport.width()), static_cast<float>(viewport.height())));
        const QRhiCommandBuffer::DynamicOffset offset(0, static_cast<quint32>(outputSlot) * slotSize);
        cb->setShaderResources(m_outputSrb.get(), 1, &offset);
        cb->setVertexInput(0, 1, &quad);
        cb->draw(4);
    }
    cb->endPass();
    return stats;
}

} // namespace wabcut::render
