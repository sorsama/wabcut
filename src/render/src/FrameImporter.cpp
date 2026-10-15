#include <wabcut/render/FrameImporter.h>

#include <QByteArray>

#include <algorithm>
#include <cstring>
#include <limits>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#if defined(_WIN32)
#include <d3d11.h>
#endif
#include <string>

namespace wabcut::render {

namespace {

QRhiTexture::Format planeFormat(PlaneLayout layout, SampleEncoding encoding, int plane) {
    const bool wide = encoding != SampleEncoding::Bits8;
    switch (layout) {
    case PlaneLayout::SemiPlanar:
        if (plane == 0) {
            return wide ? QRhiTexture::R16 : QRhiTexture::R8;
        }
        return wide ? QRhiTexture::RG16 : QRhiTexture::RG8;
    case PlaneLayout::Planar3:
    case PlaneLayout::Planar4:
        return wide ? QRhiTexture::R16 : QRhiTexture::R8;
    case PlaneLayout::PackedRGBA:
        return wide ? QRhiTexture::UnknownFormat : QRhiTexture::RGBA8;
    }
    return QRhiTexture::UnknownFormat;
}

int bytesPerTexel(QRhiTexture::Format format) {
    switch (format) {
    case QRhiTexture::R8:
        return 1;
    case QRhiTexture::RG8:
    case QRhiTexture::R16:
        return 2;
    case QRhiTexture::RG16:
    case QRhiTexture::RGBA8:
        return 4;
    default:
        return 0;
    }
}

} // namespace

struct FrameImporter::TextureSet {
    std::uint64_t serial = 0;
    SetKey key;
    std::array<std::unique_ptr<QRhiTexture>, 4> textures;
    std::array<std::vector<std::uint8_t>, 4> repack; // only without QRhi::ImageDataStride
    std::size_t bytes = 0;
    bool live = false;
    // Residency.
    const void* identity = nullptr;
    std::weak_ptr<const void> owner;
    std::shared_ptr<const void> keepAlive;
    std::uint64_t lastUsedFrame = 0; // 0 = never used
};

struct FrameImporter::NativeSet {
    void* texture = nullptr;
    int slice = 0;
    std::array<std::unique_ptr<QRhiTexture>, 2> planes;
    std::uint64_t serial = 0;
    std::uint64_t lastUsedFrame = 0;
    std::shared_ptr<const void> keepAlive;
};

namespace {
/// Native views unused for this many frames are dropped (their decoder may be gone).
constexpr std::uint64_t kNativeIdleFrames = 240;
} // namespace

FrameImporter::FrameImporter(QRhi* rhi, FrameImporterOptions options) : m_rhi(rhi), m_options(options) {
    if (m_options.maxTextureSets < 1) {
        m_options.maxTextureSets = 1;
    }
    if (m_options.retireLatencyFrames >= 0) {
        m_retireLatency = std::max(1, m_options.retireLatencyFrames);
    } else if (m_rhi) {
        m_retireLatency = std::max(1, m_rhi->resourceLimit(QRhi::FramesInFlight));
    }
    m_sets.reserve(static_cast<std::size_t>(m_options.maxTextureSets));
}

FrameImporter::~FrameImporter() {
    releaseResources();
}

bool FrameImporter::supports(QRhi* rhi, PlaneLayout layout, SampleEncoding encoding) {
    if (!rhi) {
        return false;
    }
    for (int plane = 0; plane < planeCountOf(layout); ++plane) {
        const QRhiTexture::Format format = planeFormat(layout, encoding, plane);
        if (format == QRhiTexture::UnknownFormat || !rhi->isTextureFormatSupported(format)) {
            return false;
        }
    }
    return true;
}

FrameImporter::SetKey FrameImporter::keyOf(const FrameView& frame) {
    SetKey key;
    key.layout = frame.layout;
    key.encoding = frame.encoding;
    for (int i = 0; i < frame.planeCount; ++i) {
        key.planeSizes[static_cast<std::size_t>(2 * i)] = frame.planes[static_cast<std::size_t>(i)].width;
        key.planeSizes[static_cast<std::size_t>(2 * i + 1)] =
            frame.planes[static_cast<std::size_t>(i)].height;
    }
    return key;
}

bool FrameImporter::isRetired(const TextureSet& set) const {
    return set.lastUsedFrame == 0 ||
           set.lastUsedFrame + static_cast<std::uint64_t>(m_retireLatency) <= m_frame;
}

void FrameImporter::beginFrame() {
    ++m_frame;
    ++m_stats.frames;
    // Upload sources of retired sets are no longer read by the GPU: let them go (the identity stays watched).
    for (const auto& set : m_sets) {
        if (set->keepAlive && isRetired(*set)) {
            set->keepAlive.reset();
        }
    }
    // Zero-copy views: release the decoder surface once the GPU is done; forget long-unused views.
    for (const auto& set : m_native) {
        if (set->keepAlive && set->lastUsedFrame + static_cast<std::uint64_t>(m_retireLatency) <= m_frame) {
            set->keepAlive.reset();
        }
    }
    std::erase_if(m_native, [&](const std::unique_ptr<NativeSet>& set) {
        return !set->keepAlive && set->lastUsedFrame + kNativeIdleFrames < m_frame;
    });
}

bool FrameImporter::onThisDevice(void* d3dTexture) {
    for (const auto& [texture, ok] : m_deviceChecks) {
        if (texture == d3dTexture) {
            return ok;
        }
    }
    bool ok = false;
#if defined(_WIN32)
    if (m_rhi->backend() == QRhi::D3D11) {
        const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(m_rhi->nativeHandles());
        ID3D11Device* owner = nullptr;
        static_cast<ID3D11Texture2D*>(d3dTexture)->GetDevice(&owner);
        ok = owner && handles && owner == handles->dev;
        if (owner) {
            owner->Release();
        }
    }
#endif
    if (m_deviceChecks.size() >= 64) {
        m_deviceChecks.erase(m_deviceChecks.begin());
    }
    m_deviceChecks.emplace_back(d3dTexture, ok);
    return ok;
}

std::expected<ImportedFrame, Error> FrameImporter::importNative(const FrameView& frame) {
    if (!onThisDevice(frame.nativeTexture)) {
        ++m_stats.nativeRejected;
        return std::unexpected(Error{ErrorCode::DeviceError, "The decoder surface belongs to another device"});
    }
    NativeSet* set = nullptr;
    for (const auto& candidate : m_native) {
        if (candidate->texture == frame.nativeTexture && candidate->slice == frame.arraySlice) {
            set = candidate.get();
            break;
        }
    }
    if (!set) {
        int arraySize = 1;
#if defined(_WIN32)
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(frame.nativeTexture)->GetDesc(&desc);
        arraySize = static_cast<int>(desc.ArraySize);
#endif
        auto created = std::make_unique<NativeSet>();
        created->texture = frame.nativeTexture;
        created->slice = frame.arraySlice;
        created->serial = m_nextSerial++;
        const bool wide = frame.encoding != SampleEncoding::Bits8;
        const QSize luma(frame.surfaceWidth, frame.surfaceHeight);
        created->planes[0].reset(
            m_rhi->newTextureArray(wide ? QRhiTexture::R16 : QRhiTexture::R8, arraySize, luma));
        created->planes[1].reset(
            m_rhi->newTextureArray(wide ? QRhiTexture::RG16 : QRhiTexture::RG8, arraySize, luma / 2));
        for (auto& plane : created->planes) {
            plane->setArrayRange(frame.arraySlice, 1); // the view starts at the frame's slice
            if (!plane->createFrom({reinterpret_cast<quint64>(frame.nativeTexture), 0})) {
                ++m_stats.nativeRejected;
                return std::unexpected(Error{ErrorCode::DeviceError, "Cannot view the decoder surface"});
            }
        }
        set = created.get();
        m_native.push_back(std::move(created));
    }
    set->lastUsedFrame = m_frame;
    set->keepAlive = frame.keepAlive ? frame.keepAlive : frame.owner;
    ++m_stats.imports;
    ++m_stats.nativeImports;

    ImportedFrame out;
    out.planes[0] = set->planes[0].get();
    out.planes[1] = set->planes[1].get();
    out.layout = PlaneLayout::SemiPlanar;
    out.encoding = frame.encoding;
    out.width = frame.width;
    out.height = frame.height;
    out.chromaShiftX = 1;
    out.chromaShiftY = 1;
    out.chromaWidth = frame.surfaceWidth / 2;
    out.chromaHeight = frame.surfaceHeight / 2;
    out.color = frame.color;
    out.alpha = AlphaMode::None;
    out.pixelAspect = frame.pixelAspect > 0.0 ? frame.pixelAspect : 1.0;
    out.slot = -1;
    out.setSerial = set->serial;
    out.uploaded = false;
    out.arrayTextures = true;
    out.textureWidth = frame.surfaceWidth;
    out.textureHeight = frame.surfaceHeight;
    return out;
}

std::expected<void, Error> FrameImporter::createTextures(TextureSet& set, const SetKey& key) {
    set.key = key;
    set.bytes = 0;
    set.serial = m_nextSerial++;
    for (int i = 0; i < planeCountOf(key.layout); ++i) {
        const QRhiTexture::Format format = planeFormat(key.layout, key.encoding, i);
        const QSize size(key.planeSizes[static_cast<std::size_t>(2 * i)],
                         key.planeSizes[static_cast<std::size_t>(2 * i + 1)]);
        std::unique_ptr<QRhiTexture> tex(m_rhi->newTexture(format, size, 1, {}));
        tex->setName(QByteArrayLiteral("wabcut frame plane"));
        if (!tex->create()) {
            destroyTextures(set);
            return std::unexpected(Error{ErrorCode::DeviceError, "Could not create a frame plane texture"});
        }
        set.bytes += static_cast<std::size_t>(size.width()) * static_cast<std::size_t>(size.height()) *
                     static_cast<std::size_t>(bytesPerTexel(format));
        set.textures[static_cast<std::size_t>(i)] = std::move(tex);
    }
    set.live = true;
    ++m_stats.setsCreated;
    return {};
}

void FrameImporter::destroyTextures(TextureSet& set) {
    if (set.live) {
        ++m_stats.setsDestroyed;
    }
    for (auto& tex : set.textures) {
        tex.reset(); // QRhi defers the native release until the GPU no longer needs it
    }
    set.live = false;
    set.bytes = 0;
    set.identity = nullptr;
    set.owner.reset();
    set.keepAlive.reset();
    set.lastUsedFrame = 0;
}

void FrameImporter::upload(TextureSet& set, const FrameView& frame, QRhiResourceUpdateBatch* updates) {
    const bool strided = m_rhi->isFeatureSupported(QRhi::ImageDataStride);
    for (int i = 0; i < frame.planeCount; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const PlaneView& p = frame.planes[index];
        QRhiTexture* tex = set.textures[index].get();
        const int rowBytes = p.width * bytesPerTexel(tex->format());
        QRhiTextureSubresourceUploadDescription desc;
        if (strided || p.stride == rowBytes) {
            // No copy: QRhi reads the frame's memory when it executes the upload; keepAlive covers that.
            const qsizetype size = static_cast<qsizetype>(p.stride) * (p.height - 1) + rowBytes;
            desc.setData(QByteArray::fromRawData(reinterpret_cast<const char*>(p.data), size));
            if (p.stride != rowBytes) {
                desc.setDataStride(static_cast<quint32>(p.stride));
            }
        } else {
            std::vector<std::uint8_t>& buf = set.repack[index];
            buf.resize(static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(p.height));
            for (int y = 0; y < p.height; ++y) {
                std::memcpy(buf.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rowBytes),
                            p.data + static_cast<std::ptrdiff_t>(y) * p.stride,
                            static_cast<std::size_t>(rowBytes));
            }
            desc.setData(QByteArray::fromRawData(reinterpret_cast<const char*>(buf.data()),
                                                 static_cast<qsizetype>(buf.size())));
        }
        updates->uploadTexture(tex, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, desc)));
        m_stats.bytesUploaded += static_cast<std::uint64_t>(rowBytes) * static_cast<std::uint64_t>(p.height);
    }
    set.identity = frame.identity;
    set.owner = frame.owner;
    set.keepAlive = frame.keepAlive ? frame.keepAlive : frame.owner;
    ++m_stats.uploads;
}

ImportedFrame FrameImporter::describe(const TextureSet& set, int slot, const FrameView& frame,
                                      bool uploaded) const {
    ImportedFrame out;
    for (int i = 0; i < planeCountOf(set.key.layout); ++i) {
        out.planes[static_cast<std::size_t>(i)] = set.textures[static_cast<std::size_t>(i)].get();
    }
    out.layout = set.key.layout;
    out.encoding = set.key.encoding;
    out.width = frame.width;
    out.height = frame.height;
    out.chromaShiftX = frame.layout == PlaneLayout::PackedRGBA ? 0 : frame.chromaShiftX;
    out.chromaShiftY = frame.layout == PlaneLayout::PackedRGBA ? 0 : frame.chromaShiftY;
    const int chromaPlane = frame.planeCount > 1 ? 1 : 0;
    out.chromaWidth = frame.planes[static_cast<std::size_t>(chromaPlane)].width;
    out.chromaHeight = frame.planes[static_cast<std::size_t>(chromaPlane)].height;
    out.color = frame.color;
    out.alpha = frame.alpha;
    out.pixelAspect = frame.pixelAspect > 0.0 ? frame.pixelAspect : 1.0;
    out.slot = slot;
    out.setSerial = set.serial;
    out.uploaded = uploaded;
    out.textureWidth = frame.planes[0].width;
    out.textureHeight = frame.planes[0].height;
    return out;
}

std::expected<ImportedFrame, Error> FrameImporter::import(const FrameView& frame,
                                                          QRhiResourceUpdateBatch* updates) {
    if (!m_rhi || !updates) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, "FrameImporter needs a QRhi and an update batch"});
    }
    if (!frame.isValid()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid frame view"});
    }
    if (m_frame == 0) {
        beginFrame(); // tolerate a missing first beginFrame()
    }
    if (frame.storage == FrameStorage::D3D11Texture) {
        return importNative(frame);
    }
    if (!supports(m_rhi, frame.layout, frame.encoding)) {
        return std::unexpected(
            Error{ErrorCode::Unsupported, "Texture formats for this frame are not supported"});
    }
    const SetKey key = keyOf(frame);

    // 1. Already resident (the identity's owner is still alive, so the pixels are the same).
    for (std::size_t i = 0; i < m_sets.size(); ++i) {
        TextureSet& set = *m_sets[i];
        if (set.live && set.identity == frame.identity && set.key == key && !set.owner.expired()) {
            set.lastUsedFrame = m_frame;
            if (frame.keepAlive) {
                set.keepAlive = frame.keepAlive; // retain the newest holder until this use retires
            }
            ++m_stats.imports;
            ++m_stats.residentHits;
            return describe(set, static_cast<int>(i), frame, false);
        }
    }

    // 2. Reuse the least recently used retired set of the same geometry.
    TextureSet* target = nullptr;
    int targetSlot = -1;
    for (std::size_t i = 0; i < m_sets.size(); ++i) {
        TextureSet& set = *m_sets[i];
        if (set.live && set.key == key && isRetired(set) &&
            (!target || set.lastUsedFrame < target->lastUsedFrame)) {
            target = &set;
            targetSlot = static_cast<int>(i);
        }
    }

    if (!target) {
        // 3. A free slot (never used, or emptied by releaseResources), else a new slot while under the cap.
        for (std::size_t i = 0; i < m_sets.size() && !target; ++i) {
            if (!m_sets[i]->live) {
                target = m_sets[i].get();
                targetSlot = static_cast<int>(i);
            }
        }
        if (!target && static_cast<int>(m_sets.size()) < m_options.maxTextureSets) {
            m_sets.push_back(std::make_unique<TextureSet>());
            target = m_sets.back().get();
            targetSlot = static_cast<int>(m_sets.size()) - 1;
        }
        // 4. Evict the least recently used retired set of another geometry.
        if (!target) {
            for (std::size_t i = 0; i < m_sets.size(); ++i) {
                TextureSet& set = *m_sets[i];
                if (isRetired(set) && (!target || set.lastUsedFrame < target->lastUsedFrame)) {
                    target = &set;
                    targetSlot = static_cast<int>(i);
                }
            }
            if (target) {
                destroyTextures(*target);
            }
        }
        if (!target) {
            ++m_stats.poolExhausted;
            return std::unexpected(Error{ErrorCode::DeviceError, "Frame texture pool exhausted"});
        }
        if (auto created = createTextures(*target, key); !created) {
            return std::unexpected(created.error());
        }
    }

    upload(*target, frame, updates);
    target->lastUsedFrame = m_frame;
    ++m_stats.imports;
    return describe(*target, targetSlot, frame, true);
}

std::expected<ImportedFrame, Error> FrameImporter::import(const media::FrameHandle& frame,
                                                          QRhiResourceUpdateBatch* updates,
                                                          std::shared_ptr<const void> keepAlive) {
    auto view = frameViewOf(frame);
    if (!view) {
        return std::unexpected(view.error());
    }
    if (keepAlive) {
        view->keepAlive = std::move(keepAlive);
    }
    return import(*view, updates);
}

void FrameImporter::releaseResources() {
    for (auto& set : m_sets) {
        destroyTextures(*set);
    }
    m_sets.clear();
    m_native.clear();
    m_deviceChecks.clear();
    ++m_deviceGeneration;
}

FrameImporterStats FrameImporter::stats() const {
    FrameImporterStats s = m_stats;
    s.liveSets = 0;
    s.liveTextureBytes = 0;
    s.retainedFrames = 0;
    for (const auto& set : m_sets) {
        if (set->live) {
            ++s.liveSets;
            s.liveTextureBytes += set->bytes;
        }
        if (set->keepAlive) {
            ++s.retainedFrames;
        }
    }
    s.deviceGeneration = m_deviceGeneration;
    s.nativeViews = m_native.size();
    for (const auto& set : m_native) {
        if (set->keepAlive) {
            ++s.retainedFrames;
        }
    }
    return s;
}

} // namespace wabcut::render
