#pragma once

// Compositor: renders a PreparedComposite with QRhi (contract rev 2, "render"). The program monitor and
// export use the same class, so preview == export (Principle 5).
//
//   1. layer pass: every layer, bottom to top, is imported (FrameImporter), converted to linear Rec.709 with
//      premultiplied alpha and blended "over" into a linear RGBA16F canvas cleared to transparent black;
//      a layer with stack effects, and a transition group (C.3), is first rendered on its own into canvas-size
//      RGBA16F intermediates: the layer with its Motion/Opacity, then one or more passes per effect in stack
//      order (shaders/fx.frag; crop, separable Gaussian blur, basic correction, vignette, sharpen, drop
//      shadow), then the transition mix of its A and B; the result is blended "over" the canvas in its place;
//   2. output pass: the canvas is drawn into the caller's render target inside a viewport (letterboxed),
//   through the
//      role's output transform (ColorPipeline::displayTransform or ::exportTransform). The rest of the target
//      is cleared to the surround colour.
//
// A nested layer (CompositeLayer::nested, C.8) is first rendered by a child compositor - one per nested layer of the
// frame, each with its own canvas, uniforms and intermediates, so they never share a slot - into that child's canvas
// (linear, premultiplied, the nested sequence's size at the same preview divisor); the layer pass then samples that
// canvas like an RGBA frame, through the layer's Motion, effects and transition. Children nest the same way. A nested
// canvas stays float through the nest (values above 1 are kept, I.9), and the colour views reach the graded layers
// inside it: the before/after split at the same place on screen, the mask view of the clip or of everything in it.
//
// Threading: use on the QRhi's thread, while a frame is being recorded, outside a render pass. It never
// decodes and never waits on locks. Steady state allocates nothing of its own (pooled textures, one dynamic
// uniform buffer, cached shader resource bindings).

#include <wabcut/model/Error.h>
#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/FrameImporter.h>
#include <wabcut/render/PreparedComposite.h>

#include <QRect>
#include <QSize>

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;

namespace wabcut::render {

/// What the output pass writes into the alpha channel.
enum class OutputAlpha : std::uint8_t {
    Opaque,        ///< Canvas composited over black, alpha = 1 (monitor, most exports).
    Straight,      ///< Encoded straight colour + coverage alpha (ProRes 4444 / PNG exports).
    Premultiplied, ///< Encoded colour times alpha + alpha.
};

struct CompositorOptions {
    TexturePool pool = TexturePool::Preview;
    /// Canvas resolution divisor (1 = full, 2 = half, 4 = quarter), like Premiere's playback resolution.
    int previewDivisor = 1;
    int maxTextureSets = 48;
};

struct CompositeOutput {
    QRhiRenderTarget* target = nullptr;
    /// Where the canvas goes, in target pixels with a top-left origin. Empty = letterbox into the whole
    /// target.
    QRect viewport;
    /// Clear colour (already encoded, 0..1 RGBA) of the target outside the viewport, or of the whole target
    /// when `drawCanvas` is false.
    std::array<float, 4> surround{0.0f, 0.0f, 0.0f, 1.0f};
    OutputRole role = OutputRole::Display;
    OutputAlpha alpha = OutputAlpha::Opaque;
    /// False: only clear the target to `surround` (e.g. a transparent monitor before the first frame).
    bool drawCanvas = true;
};

struct CompositeStats {
    int layersDrawn = 0;
    int effectsApplied = 0;  ///< Stack effects rendered.
    int bypassedEffects = 0; ///< Stack effects the layers carried as bypassed (unknown or unsupported).
    int transitions = 0;     ///< Transition mixes rendered.
    int effectPasses = 0;    ///< Offscreen passes for effects and transitions (layer, effect, mix).
    int gradePasses = 0;     ///< Of those, colour grade passes (shaders/grade.frag).
    int missingLuts = 0;     ///< Grade LUTs that could not be used (unresolved, or no 3D textures on this GPU).
    int maskPasses = 0;      ///< Mask passes (distances, coverage, masked mixes, redaction styles; I.6).
    int maskTexturesRendered = 0; ///< Distance / coverage textures rendered (the others came from the cache).
    int redactionsApplied = 0;    ///< Redactions drawn over layers.
    int layersFailed = 0; ///< Layers skipped because their frame (or nested composite) could not be imported.
    /// Nested composites rendered (C.8: nested and multicam clips, nested sequences' master stages), at every depth.
    /// The other counts include the layers, effects and redactions drawn inside them.
    int nestedComposites = 0;
    int uploads = 0;      ///< Layers whose pixels were uploaded this frame.
    int residentHits = 0; ///< Layers whose frame was already on the GPU.
    std::uint64_t bytesUploaded = 0;
    bool hdrSource = false; ///< A layer is PQ/HLG.
    int toneMappedCaptures = 0; ///< I.9 (gaming): PQ / HLG layers of an SDR project, tone-mapped into SDR
    int unsupportedBlends = 0;  ///< Layers whose blend mode the compositor cannot draw yet (drawn as Normal; none since 0.1.2)
    int shaderBlends = 0;       ///< Layers blended in a shader over the canvas below them (0.1.2: a canvas-pass split each)
    /// Layers read through a colour-managed input plan (ColorPipeline v2, I.9), of them through a baked LUT.
    int managedInputs = 0;
    int inputLuts = 0;
    int colorPlanErrors = 0; ///< managed inputs or outputs OpenColorIO could not build (the layer or output fell back)
    QSize canvasPixels;     ///< Size of the intermediate canvas actually rendered.
};

class Compositor {
public:
    explicit Compositor(QRhi* rhi, CompositorOptions options = {});
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    QRhi* rhi() const { return m_rhi; }

    /// Records the layer and output passes for `composite` into `cb`. Frames are kept alive through their own
    /// owners (their final release may then happen on this thread).
    std::expected<CompositeStats, Error> render(QRhiCommandBuffer* cb, const CompositeOutput& output,
                                                const PreparedComposite& composite);
    /// Same, retaining `composite` itself (normally pooled) while the GPU uses its frames, so the final
    /// release of decoder memory happens on the producer's thread.
    std::expected<CompositeStats, Error> render(QRhiCommandBuffer* cb, const CompositeOutput& output,
                                                const std::shared_ptr<const PreparedComposite>& composite);

    /// Largest rectangle of the canvas' display aspect that fits `target`, centred (integer pixels).
    static QRect letterboxRect(QSize target, CanvasSize canvas, double pixelAspect = 1.0);

    void setPreviewDivisor(int divisor);
    int previewDivisor() const { return m_options.previewDivisor; }

    FrameImporter& importer() { return m_importer; }
    const FrameImporter& importer() const { return m_importer; }

    /// Drops every GPU resource (device loss or before the QRhi goes away). The next render() recreates them.
    void releaseResources();

    /// The linear RGBA16F canvas of the last render() (scopes read it), and its size in pixels; null before the first.
    QRhiTexture* canvasTexture() const { return m_canvas.get(); }

private:
    struct LayerSrb;
    struct GradeTextures;

    /// The colour views as one composite applies them: the top level's ColorView, carried into nested composites
    /// (I.9) with the before/after split as a line in their own canvas.
    struct ViewState {
        int maskView = 0;
        std::uint64_t maskClip = 0; ///< 0 = every graded layer
        bool split = false;
        /// Graded layers show themselves before their grade where line[0]*x + line[1]*y + line[2] < 0 (x, y: 0..1 of
        /// the canvas, y down).
        std::array<double, 3> line{1.0, 0.0, 0.0};

        static ViewState of(const ColorView& view);
        /// The views inside a nested layer placed by `toCanvas` (its canvas texture pixels, `pixels`, to `canvas`
        /// units of the composite holding it), `clipId` its clip: a mask view of that clip shows everything in it.
        ViewState inside(const Affine2D& toCanvas, QSize pixels, CanvasSize canvas, std::uint64_t clipId) const;
    };

    /// `color` is the pipeline the layers convert into (the top-level composite's, for nested ones too), `view` the
    /// colour views; with `canvasOnly`, only the canvas is rendered (no output pass, `output` is unused): a nested
    /// composite.
    std::expected<CompositeStats, Error> renderImpl(QRhiCommandBuffer* cb, const CompositeOutput& output,
                                                    const PreparedComposite& composite,
                                                    const std::shared_ptr<const void>& keepAlive,
                                                    const ColorPipeline& color, const ViewState& view,
                                                    bool canvasOnly);
    /// Renders `nested` with the `index`-th child compositor and describes its canvas as a layer frame (C.8).
    std::expected<ImportedFrame, Error> renderNested(QRhiCommandBuffer* cb, const PreparedComposite& nested,
                                                     const std::shared_ptr<const void>& keepAlive,
                                                     const ColorPipeline& color, const ViewState& view,
                                                     std::size_t index, CompositeStats& stats);
    /// The layer pass's bindings for a nested canvas texture.
    QRhiShaderResourceBindings* nestedSrb(QRhiTexture* texture);
    std::expected<void, Error> ensureStaticResources();
    std::expected<void, Error> ensureUniformCapacity(int slotCount);
    std::expected<void, Error> ensureCanvas(QSize pixels);
    std::expected<void, Error> ensureOutputPipeline(QRhiRenderPassDescriptor* rpd, int sampleCount);
    std::expected<void, Error> ensureFxPipelines();
    QRhiShaderResourceBindings* layerSrb(const ImportedFrame& frame);
    /// The layer pass's bindings of `frame` with a colour-management input LUT (I.9).
    QRhiShaderResourceBindings* lutLayerSrb(const ImportedFrame& frame, QRhiTexture* lut);
    /// The 3D texture of a baked colour-management LUT (I.9), uploaded on first use; null without 3D textures.
    QRhiTexture* cmLutTexture(const std::shared_ptr<const color::BakedLut>& lut, QRhiResourceUpdateBatch* updates);
    /// The output pass's bindings with an output LUT (I.9).
    QRhiShaderResourceBindings* outputLutSrb(QRhiTexture* lut);
    QRhiShaderResourceBindings* fxSrb(QRhiTexture* in0, QRhiTexture* in1);
    // Colour grade passes (shaders/grade.frag, I.5).
    std::expected<void, Error> ensureGradePipeline();
    std::expected<void, Error> ensureGradeUniformCapacity(int slotCount);
    GradeTextures* gradeTextures(const color::GradeSpec& grade, QRhiResourceUpdateBatch* updates, int& missingLuts);
    QRhiTexture* lutTexture(const std::shared_ptr<const color::Lut>& lut, QRhiResourceUpdateBatch* updates);
    QRhiShaderResourceBindings* gradeSrb(const std::array<QRhiTexture*, 6>& textures);
    // Mask passes (shaders/mask.frag, I.6).
    struct MaskTexture;
    std::expected<void, Error> ensureMaskPipeline();
    std::expected<void, Error> ensureMaskUniformCapacity(int slotCount);
    /// The mask texture of `owner` in `cache` at `size` (recreated when the size changes).
    MaskTexture* maskTexture(std::unordered_map<std::uint64_t, std::unique_ptr<MaskTexture>>& cache, std::uint64_t owner,
                             QSize size);
    QRhiShaderResourceBindings* maskSrb(const std::array<QRhiTexture*, 8>& textures);
    void setMaskBindings(QRhiShaderResourceBindings& srb, const std::array<QRhiTexture*, 8>& textures);
    // The flow mix (shaders/flowmix.frag, P.2; CompositorFlow.cpp): Time Interpolation's Optical Flow and Morph Cut.
    /// Its pipeline: fx.vert + flowmix.frag, the fx uniform block (`blockSize` bytes) and a third input, the motion.
    std::expected<void, Error> ensureFlowPipeline(std::uint32_t blockSize);
    /// The RGBA32F texture of `motion` (uploaded with `updates` on first use), kept while in use; null on failure.
    QRhiTexture* motionTexture(const std::shared_ptr<const motion::FlowPair>& motion, QRhiResourceUpdateBatch* updates);
    QRhiShaderResourceBindings* flowSrb(QRhiTexture* a, QRhiTexture* b, QRhiTexture* motion, std::uint32_t blockSize);
    /// Drops motion textures not used for a while (every 64 frames, like the grade's).
    void evictMotionTextures();
    void releaseFlowResources();

    /// A canvas-size RGBA16F texture (with mips, for wide blurs) and its render target.
    struct Intermediate {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        bool inUse = false;
    };
    Intermediate* acquireIntermediate(QSize pixels);

    QRhi* m_rhi = nullptr;
    CompositorOptions m_options;
    FrameImporter m_importer;

    std::unique_ptr<QRhiBuffer> m_quad;
    std::unique_ptr<QRhiBuffer> m_uniforms;
    int m_uniformSlots = 0;
    std::uint64_t m_uniformGeneration = 0;
    std::unique_ptr<QRhiSampler> m_planeSampler;
    std::unique_ptr<QRhiSampler> m_canvasSampler;
    std::unique_ptr<QRhiTexture> m_dummy;
    bool m_staticUploaded = false;

    std::unique_ptr<QRhiTexture> m_canvas;
    std::unique_ptr<QRhiTextureRenderTarget> m_canvasTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_canvasPass;
    std::unique_ptr<QRhiGraphicsPipeline> m_layerPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> m_layerLayoutSrb;
    // Zero-copy layers (decoder surfaces as texture-array views): their own pipeline and bindings.
    std::unique_ptr<QRhiTexture> m_dummyArray;
    std::unique_ptr<QRhiGraphicsPipeline> m_layerArrayPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> m_layerArrayLayoutSrb;
    struct NativeSrb {
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::uint64_t uniformGeneration = 0;
        std::uint64_t lastUsedFrame = 0;
    };
    std::unordered_map<std::uint64_t, NativeSrb> m_nativeSrbs; // by the importer's set serial
    std::uint64_t m_frameCounter = 0;

    std::unique_ptr<QRhiShaderResourceBindings> m_outputSrb;
    // Colour management (I.9): baked LUT textures by LUT, the layer bindings that use them, the output's.
    struct CmLutTexture {
        std::shared_ptr<const color::BakedLut> lut; ///< pins the pointer key
        std::unique_ptr<QRhiTexture> texture;
        std::uint64_t lastUsedFrame = 0;
    };
    std::unordered_map<const color::BakedLut*, CmLutTexture> m_cmLuts;
    struct LutLayerKey {
        bool array = false;
        std::uint64_t setSerial = 0;
        QRhiTexture* lut = nullptr;
        bool operator==(const LutLayerKey&) const = default;
    };
    struct LutLayerKeyHash {
        std::size_t operator()(const LutLayerKey& k) const {
            return std::hash<std::uint64_t>{}(k.setSerial * 2 + (k.array ? 1 : 0)) ^ std::hash<const void*>{}(k.lut);
        }
    };
    std::unordered_map<LutLayerKey, NativeSrb, LutLayerKeyHash> m_lutLayerSrbs;
    std::unique_ptr<QRhiShaderResourceBindings> m_outputLutSrb;
    QRhiTexture* m_outputLutSrbTexture = nullptr;
    std::unique_ptr<QRhiGraphicsPipeline> m_outputPipeline;
    std::unique_ptr<QRhiRenderPassDescriptor> m_outputPassCompat; // a compatible descriptor, for later checks
    QRhiRenderPassDescriptor* m_outputPassSeen = nullptr;

    std::vector<std::unique_ptr<LayerSrb>> m_layerSrbs; // indexed by importer slot

    // Effect and transition passes (shaders/fx.frag).
    std::unique_ptr<QRhiGraphicsPipeline> m_fxPipeline;     // replaces the target
    std::unique_ptr<QRhiGraphicsPipeline> m_fxOverPipeline; // premultiplied "over" (results into the canvas)
    // 0.2: the family shaders of the transition and effect library (fx_transition.frag, fx_*.frag): the fx pipeline's
    // vertex shader, uniform block and bindings with another fragment shader, made on first use. Index = FxFamily.
    static constexpr int kFxFamilies = 6;
    std::array<std::unique_ptr<QRhiGraphicsPipeline>, kFxFamilies> m_fxFamilyPipelines;
    /// The pipeline of a family (0 = fx.frag), made when missing; null when its shader cannot be loaded or created.
    QRhiGraphicsPipeline* fxFamilyPipeline(int family);
    // Blend-mode variants (C.15) of the three canvas-pass pipelines, by blendVariantOf() - 1, made on first use: Add.
    static constexpr int kBlendVariants = 1;
    using BlendPipelines = std::array<std::unique_ptr<QRhiGraphicsPipeline>, kBlendVariants>;
    BlendPipelines m_layerBlendPipelines;
    BlendPipelines m_layerArrayBlendPipelines;
    BlendPipelines m_fxBlendPipelines;
    QRhiGraphicsPipeline* blendPipeline(QRhiGraphicsPipeline* base, BlendPipelines& variants, int variant);
    std::unique_ptr<QRhiShaderResourceBindings> m_fxLayoutSrb;
    std::vector<std::unique_ptr<Intermediate>> m_intermediates;
    struct FxSrb {
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::uint64_t uniformGeneration = 0;
    };
    std::map<std::pair<QRhiTexture*, QRhiTexture*>, FxSrb> m_fxSrbs;

    // Colour grade (I.5): its own dynamic uniform buffer (a larger block than the layer/fx slots), pipeline, bindings
    // per (inputs, curves, LUTs), and the textures of grades and LUTs, kept while in use.
    std::unique_ptr<QRhiBuffer> m_gradeUniforms;
    int m_gradeSlots = 0;
    std::uint64_t m_gradeGeneration = 0;
    std::unique_ptr<QRhiGraphicsPipeline> m_gradePipeline;
    std::unique_ptr<QRhiShaderResourceBindings> m_gradeLayoutSrb;
    std::unique_ptr<QRhiSampler> m_nearestSampler;
    std::unique_ptr<QRhiTexture> m_dummy3d;
    std::unique_ptr<QRhiTexture> m_dummyCurves;
    bool m_gradeStaticUploaded = false;
    struct GradeSrb {
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::uint64_t uniformGeneration = 0;
    };
    std::map<std::array<QRhiTexture*, 6>, GradeSrb> m_gradeSrbs;
    std::unordered_map<std::uint64_t, std::unique_ptr<GradeTextures>> m_gradeTextures;
    struct LutTexture {
        std::shared_ptr<const color::Lut> lut; ///< pins the pointer key
        std::unique_ptr<QRhiTexture> texture;
        std::uint64_t lastUsedFrame = 0;
    };
    std::unordered_map<const color::Lut*, LutTexture> m_lutTextures;

    // Masks (I.6): their own uniform buffer and pipeline; frame-space textures per owner (a layer's effect or redaction,
    // a shape of it): signed distances and coverage, re-rendered only when their content changes.
    struct MaskTexture {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        std::unique_ptr<QRhiTexture> segments; ///< distance textures: the polygon's segments
        int segmentCapacity = 0;
        QSize size;
        std::uint64_t content = 0;  ///< what it holds (MaskSpec / shape key); 0 = nothing yet
        std::uint64_t lastUsedFrame = 0;
    };
    std::unique_ptr<QRhiBuffer> m_maskUniforms;
    int m_maskSlots = 0;
    std::uint64_t m_maskGeneration = 0;
    std::unique_ptr<QRhiGraphicsPipeline> m_maskPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> m_maskLayoutSrb;
    std::unique_ptr<QRhiSampler> m_maskSampler;
    std::unique_ptr<QRhiSampler> m_maskNearest;
    struct MaskSrb {
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::uint64_t uniformGeneration = 0;
    };
    std::map<std::array<QRhiTexture*, 8>, MaskSrb> m_maskSrbs;
    std::unordered_map<std::uint64_t, std::unique_ptr<MaskTexture>> m_sdfTextures;
    std::unordered_map<std::uint64_t, std::unique_ptr<MaskTexture>> m_coverageTextures;
    /// AI-14 matte shapes: the uploaded distance field of a shape (render/Matte.h), a sampled texture only - its own
    /// slots, so a layer that alternates matte and polygon shapes does not recreate either kind.
    struct MatteTexture {
        std::unique_ptr<QRhiTexture> texture;
        QSize size;
        std::uint64_t content = 0;
        std::uint64_t lastUsedFrame = 0;
    };
    std::unordered_map<std::uint64_t, std::unique_ptr<MatteTexture>> m_matteTextures;

    // P.2 flow mix: its pipeline, bindings per (A, B, motion), and the motion textures by their FlowPair.
    std::unique_ptr<QRhiGraphicsPipeline> m_flowPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> m_flowLayoutSrb;
    struct FlowSrb {
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::uint64_t uniformGeneration = 0;
    };
    std::map<std::array<QRhiTexture*, 3>, FlowSrb> m_flowSrbs;
    struct MotionTexture {
        std::shared_ptr<const motion::FlowPair> motion; ///< pins the pointer key
        std::unique_ptr<QRhiTexture> texture;
        std::uint64_t lastUsedFrame = 0;
    };
    std::unordered_map<const motion::FlowPair*, MotionTexture> m_motionTextures;

    struct Placement {
        ImportedFrame frame;
        int uniformSlot = 0;
        Affine2D toCanvas; ///< the layer's frame pixels -> canvas sequence pixels
        std::uint64_t clipId = 0;
        QRhiShaderResourceBindings* srb = nullptr; ///< set for a nested canvas (else by the importer's slot)
    };
    std::vector<Placement> m_placements; // per-frame scratch, capacity kept

    // Nested composites (C.8): child compositors by the order of the nested layers in a frame, created on first use,
    // and the layer bindings of their canvases.
    std::vector<std::unique_ptr<Compositor>> m_children;
    struct NestedSrb {
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::uint64_t uniformGeneration = 0;
        std::uint64_t lastUsedFrame = 0;
    };
    std::unordered_map<QRhiTexture*, NestedSrb> m_nestedSrbs;
};

} // namespace wabcut::render
