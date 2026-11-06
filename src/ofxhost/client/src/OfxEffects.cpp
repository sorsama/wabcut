#include <wabcut/ofxhost/OfxEffects.h>

#include "OfxParams.h"
#include "OfxPictures.h"

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/ofxhost/Protocol.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>

namespace wabcut::ofxhost {

std::string_view toString(OfxEffectStatus status) {
    switch (status) {
    case OfxEffectStatus::NotOfx:
        return "notOfx";
    case OfxEffectStatus::Ready:
        return "ready";
    case OfxEffectStatus::Missing:
        return "missing";
    case OfxEffectStatus::Crashed:
        return "crashed";
    case OfxEffectStatus::Failed:
        return "failed";
    case OfxEffectStatus::Hung:
        return "hung";
    }
    return "?";
}

EffectInstance makeOfxEffect(const OfxPluginInfo& plugin, EffectInstanceId id) {
    EffectInstance e;
    e.id = id;
    e.effect = plugin.effectId();
    e.version = 1;
    e.enabled = true;
    std::string label = plugin.name + (plugin.group.empty() ? std::string() : " (" + plugin.group + ")");
    if (label.size() > kMaxEffectText) {
        label.resize(kMaxEffectText);
    }
    e.setData(EffectData{std::string(kOfxNameKey), label});
    return e;
}

namespace {

constexpr std::size_t kMaxSlots = 24;
constexpr std::size_t kMaxIdleDecoders = 4;

// FNV-1a over 64-bit words and bytes: the cache key.
struct Hasher {
    std::uint64_t h = 0xcbf29ce484222325ull;
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 0x100000001b3ull;
        }
    }
    void u64(std::uint64_t v) { bytes(&v, sizeof v); }
    void f64(double v) {
        std::uint64_t b = 0;
        std::memcpy(&b, &v, sizeof b);
        u64(b);
    }
    void text(std::string_view s) {
        u64(s.size());
        bytes(s.data(), s.size());
    }
};

/// Decoders for the CPU copies of frames that are decoder surfaces (zero-copy preview): borrowed by one thread at a time.
class DecoderShelf {
public:
    using DecoderPtr = std::unique_ptr<media::VideoDecoder>;

    std::expected<media::FrameHandle, Error> frameAt(const std::string& path, int stream, Ticks time) {
        if (path.empty()) {
            return std::unexpected(Error(ErrorCode::NotFound, "no file to decode a CPU picture from"));
        }
        DecoderPtr decoder;
        {
            std::lock_guard lock(m_mutex);
            for (auto it = m_idle.begin(); it != m_idle.end(); ++it) {
                if (it->path == path && it->stream == stream) {
                    decoder = std::move(it->decoder);
                    m_idle.erase(it);
                    break;
                }
            }
        }
        if (!decoder) {
            media::DecoderOptions options;
            options.outputMode = media::OutputMode::Cpu;
            options.snapToStreamTicks = true; // the frames playback and export show for the same source time
            auto opened = media::VideoDecoder::open(path, stream, options);
            if (!opened) {
                return std::unexpected(opened.error());
            }
            decoder = std::move(*opened);
        }
        auto frame = decoder->frameAt(time);
        {
            std::lock_guard lock(m_mutex);
            m_idle.push_front(Idle{path, stream, std::move(decoder)});
            while (m_idle.size() > kMaxIdleDecoders) {
                m_idle.pop_back();
            }
        }
        return frame;
    }

private:
    struct Idle {
        std::string path;
        int stream = -1;
        DecoderPtr decoder;
    };
    std::mutex m_mutex;
    std::list<Idle> m_idle;
};

} // namespace

struct OfxEffects::State {
    struct Slot {
        std::mutex create; ///< held while the instance is made (it may take seconds); never by status()
        std::mutex mutex;  ///< guards the fields below, briefly
        std::string effectId;
        std::shared_ptr<OfxRemoteInstance> instance;
        std::string createError; ///< the plug-in refused to be created
        std::uint64_t used = 0;
        std::shared_ptr<OfxRemoteInstance> current() {
            std::lock_guard lock(mutex);
            return instance;
        }
    };
    struct CacheEntry {
        std::uint64_t key = 0;
        std::shared_ptr<const render::TitleRaster> raster;
        render::ResolvedColor color;
        std::size_t bytes = 0;
    };

    std::shared_ptr<OfxPluginHost> host;
    std::shared_ptr<OfxCatalog> catalog;

    std::mutex slotsMutex;
    std::map<std::uint64_t, std::shared_ptr<Slot>> slots;
    std::uint64_t tick = 0;

    mutable std::mutex cacheMutex;
    std::list<CacheEntry> cache; ///< most recent first
    std::unordered_map<std::uint64_t, std::list<CacheEntry>::iterator> index;
    std::size_t cacheBytes = 0;
    std::size_t budget = std::size_t{192} << 20;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> pictures{0};

    std::atomic<std::uint64_t> revision{0};
    std::mutex listenerMutex;
    std::function<void()> listener;
    DecoderShelf decoders;

    /// A status may have changed: bump the revision and tell the listener (any thread).
    void bump() {
        revision.fetch_add(1, std::memory_order_relaxed);
        std::function<void()> l;
        {
            std::lock_guard lock(listenerMutex);
            l = listener;
        }
        if (l) {
            l();
        }
    }

    std::shared_ptr<Slot> slotFor(std::uint64_t id, const std::string& effectId) {
        std::lock_guard lock(slotsMutex);
        auto it = slots.find(id);
        if (it != slots.end() && it->second->effectId != effectId) {
            slots.erase(it); // the effect was replaced by another plug-in under the same id
            it = slots.end();
        }
        if (it == slots.end()) {
            auto slot = std::make_shared<Slot>();
            slot->effectId = effectId;
            it = slots.emplace(id, std::move(slot)).first;
            // Bounded: the least recently used slot that is not busy goes.
            while (slots.size() > kMaxSlots) {
                auto oldest = slots.end();
                for (auto s = slots.begin(); s != slots.end(); ++s) {
                    if (s->first == id) {
                        continue;
                    }
                    if (oldest == slots.end() || s->second->used < oldest->second->used) {
                        oldest = s;
                    }
                }
                if (oldest == slots.end()) {
                    break;
                }
                slots.erase(oldest);
            }
        }
        it->second->used = ++tick;
        return it->second;
    }

    std::shared_ptr<Slot> findSlot(std::uint64_t id) {
        std::lock_guard lock(slotsMutex);
        const auto it = slots.find(id);
        return it == slots.end() ? nullptr : it->second;
    }

    std::shared_ptr<const render::TitleRaster> cached(std::uint64_t key, render::ResolvedColor& color) {
        std::lock_guard lock(cacheMutex);
        const auto it = index.find(key);
        if (it == index.end()) {
            return nullptr;
        }
        cache.splice(cache.begin(), cache, it->second);
        color = it->second->color;
        return it->second->raster;
    }

    void store(std::uint64_t key, std::shared_ptr<const render::TitleRaster> raster, const render::ResolvedColor& color) {
        std::lock_guard lock(cacheMutex);
        if (index.contains(key)) {
            return;
        }
        const std::size_t bytes = raster->rgba().size();
        if (bytes > budget) {
            return;
        }
        cache.push_front(CacheEntry{key, std::move(raster), color, bytes});
        index[key] = cache.begin();
        cacheBytes += bytes;
        while (cacheBytes > budget && !cache.empty()) {
            cacheBytes -= cache.back().bytes;
            index.erase(cache.back().key);
            cache.pop_back();
        }
    }
};

OfxEffects::OfxEffects(std::shared_ptr<OfxPluginHost> host, std::shared_ptr<OfxCatalog> catalog) : m_state(std::make_unique<State>()) {
    m_state->host = std::move(host);
    m_state->catalog = std::move(catalog);
}

OfxEffects::~OfxEffects() = default;

std::shared_ptr<OfxEffects> OfxEffects::create(std::shared_ptr<OfxPluginHost> host, std::shared_ptr<OfxCatalog> catalog) {
    auto effects = std::shared_ptr<OfxEffects>(new OfxEffects(std::move(host), std::move(catalog)));
    // A scan or a cache load can change which plug-ins are usable: the badges are asked again.
    std::weak_ptr<OfxEffects> weak = effects;
    effects->m_state->catalog->setChangedCallback([weak] {
        if (const auto e = weak.lock()) {
            e->m_state->bump();
        }
    });
    return effects;
}

std::shared_ptr<OfxPluginHost> OfxEffects::host() const { return m_state->host; }
std::shared_ptr<OfxCatalog> OfxEffects::catalog() const { return m_state->catalog; }

bool OfxEffects::implements(std::string_view effectId) const {
    if (!effectId.starts_with(kOfxEffectPrefix)) {
        return false;
    }
    const auto plugin = m_state->catalog->find(effectId);
    return plugin && plugin->problem.empty();
}

void OfxEffects::setChangeListener(std::function<void()> listener) {
    std::lock_guard lock(m_state->listenerMutex);
    m_state->listener = std::move(listener);
}

std::uint64_t OfxEffects::statusRevision() const noexcept { return m_state->revision.load(std::memory_order_relaxed); }

OfxCacheStats OfxEffects::cacheStats() const {
    OfxCacheStats s;
    s.hits = m_state->hits.load();
    s.misses = m_state->misses.load();
    s.pictures = m_state->pictures.load();
    std::lock_guard lock(m_state->cacheMutex);
    s.entries = m_state->cache.size();
    s.bytes = m_state->cacheBytes;
    return s;
}

void OfxEffects::clearCache() {
    std::lock_guard lock(m_state->cacheMutex);
    m_state->cache.clear();
    m_state->index.clear();
    m_state->cacheBytes = 0;
}

void OfxEffects::setCacheBudget(std::size_t bytes) {
    std::lock_guard lock(m_state->cacheMutex);
    m_state->budget = bytes;
    while (m_state->cacheBytes > m_state->budget && !m_state->cache.empty()) {
        m_state->cacheBytes -= m_state->cache.back().bytes;
        m_state->index.erase(m_state->cache.back().key);
        m_state->cache.pop_back();
    }
}

OfxEffectReport OfxEffects::status(const EffectInstance& effect) const {
    OfxEffectReport report;
    if (!effect.effect.starts_with(kOfxEffectPrefix)) {
        return report;
    }
    const auto plugin = m_state->catalog->find(effect.effect);
    if (!plugin || !plugin->problem.empty()) {
        const EffectData* saved = effect.findData(kOfxNameKey);
        const std::string name = saved && saved->text() ? *saved->text() : effect.effect.substr(kOfxEffectPrefix.size());
        report.status = OfxEffectStatus::Missing;
        report.message = plugin ? "Plug-in cannot be used: " + name + " (" + plugin->problem + ")" : "Missing plug-in: " + name;
        return report;
    }
    report.status = OfxEffectStatus::Ready;
    if (const auto slot = m_state->findSlot(effect.id.value)) {
        std::string createError;
        std::shared_ptr<OfxRemoteInstance> instance;
        {
            std::lock_guard lock(slot->mutex);
            createError = slot->createError;
            instance = slot->instance;
        }
        if (!createError.empty()) {
            report.status = OfxEffectStatus::Failed;
            report.message = "Failed: " + createError;
        } else if (instance) {
            report.lastMilliseconds = instance->lastMilliseconds();
            switch (instance->health()) {
            case OfxHealth::Running:
                break;
            case OfxHealth::Crashed:
                report.status = OfxEffectStatus::Crashed;
                report.message = "Crashed: " + instance->problem();
                break;
            case OfxHealth::Failed:
                report.status = OfxEffectStatus::Failed;
                report.message = "Failed: " + instance->problem();
                break;
            case OfxHealth::Hung:
                report.status = OfxEffectStatus::Hung;
                report.message = "Hung: " + instance->problem();
                break;
            }
        }
    }
    return report;
}

std::string OfxEffects::problem(const EffectInstance& effect) const {
    const OfxEffectReport report = status(effect);
    return report.status == OfxEffectStatus::Missing || report.status == OfxEffectStatus::Failed ? report.message : std::string();
}

void OfxEffects::retry(EffectInstanceId effect) {
    {
        std::lock_guard lock(m_state->slotsMutex);
        m_state->slots.erase(effect.value);
    }
    m_state->bump();
}

std::expected<render::FrameView, Error> OfxEffects::process(const playback::ExternalSource& source,
                                                            const std::vector<playback::ExternalEffect>& effects) {
    State& s = *m_state;
    if (source.frame == nullptr || !source.frame->isValid() || effects.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "nothing to process"));
    }
    const bool offline = playback::offlineVideoRender();
    // The picture: the frame's own CPU planes, or a CPU copy decoded for it.
    media::FrameHandle copy;
    const media::FrameHandle* frame = source.frame;
    if (frame->cpu() == nullptr) {
        auto decoded = s.decoders.frameAt(source.path, source.stream, source.frame->pts);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (decoded->width != source.frame->width || decoded->height != source.frame->height) {
            // A decoder surface of a proxy or a reduced decode: the original's CPU copy would not be the same picture.
            return std::unexpected(Error(ErrorCode::Unsupported, "the CPU copy of the frame is not the size of the frame shown"));
        }
        copy = std::move(*decoded);
        frame = &copy;
    }
    // Each effect's helper instance first (a plug-in that says its pictures vary from frame to frame puts the time in the
    // key).
    struct Step {
        const playback::ExternalEffect* effect = nullptr;
        std::optional<OfxPluginInfo> plugin;
        std::shared_ptr<State::Slot> slot;
    };
    std::vector<Step> steps;
    for (const playback::ExternalEffect& e : effects) {
        Step step;
        step.effect = &e;
        step.plugin = s.catalog->find(e.effect);
        if (step.plugin && step.plugin->problem.empty()) {
            step.slot = s.slotFor(e.id.value, e.effect);
            std::lock_guard creating(step.slot->create);
            bool needed = false;
            {
                std::lock_guard lock(step.slot->mutex);
                needed = !step.slot->instance && step.slot->createError.empty();
            }
            if (needed) {
                auto made = s.host->instantiate(*step.plugin, frame->width, frame->height, e.fps);
                std::lock_guard lock(step.slot->mutex);
                if (made) {
                    step.slot->instance = *made;
                } else {
                    step.slot->createError = made.error().message;
                    s.bump();
                }
            }
        }
        steps.push_back(std::move(step));
    }

    // The cache key: the frame (which file, which frame, at what size and colour) and every effect with its values.
    Hasher key;
    const media::FrameHandle& f = *frame;
    const bool identified = f.source.fingerprint != 0;
    key.u64(f.source.fingerprint);
    key.u64(f.source.generation);
    key.u64(static_cast<std::uint64_t>(f.streamPts));
    key.u64(static_cast<std::uint64_t>(f.pts.value));
    key.u64(static_cast<std::uint64_t>(f.width) << 32 | static_cast<std::uint64_t>(f.height));
    key.u64(static_cast<std::uint64_t>(f.format));
    key.u64(static_cast<std::uint64_t>(f.alpha));
    key.u64(static_cast<std::uint64_t>(f.color.matrix) << 24 | static_cast<std::uint64_t>(f.color.range) << 16 |
            static_cast<std::uint64_t>(f.color.primaries) << 8 | static_cast<std::uint64_t>(f.color.transfer));
    key.f64(source.pixelAspect);
    for (const Step& st : steps) {
        key.u64(st.effect->id.value);
        key.text(st.effect->effect);
        for (const auto& [k, v] : st.effect->params) {
            key.text(k);
            key.f64(v);
        }
        for (const auto& [k, v] : st.effect->texts) {
            key.text(k);
            key.text(v);
        }
        const std::shared_ptr<OfxRemoteInstance> live = st.slot ? st.slot->current() : nullptr;
        const bool varying = live && live->frameVarying();
        key.u64(st.plugin && st.plugin->problem.empty() ? 1u : 0u);
        if (varying) {
            key.f64(st.effect->timeFrames);
        }
    }
    if (identified) {
        render::ResolvedColor color;
        if (auto raster = s.cached(key.h, color)) {
            s.hits.fetch_add(1, std::memory_order_relaxed);
            return viewOfRaster(raster, color);
        }
    }
    s.misses.fetch_add(1, std::memory_order_relaxed);

    // docs/plugins.md: a picture over 4096 x 4096 is not sent to a plug-in. Said before the picture is converted to float
    // (an 8K frame is about 1 GB of it, on every frame).
    if (static_cast<std::uint64_t>(std::max(f.width, 0)) * static_cast<std::uint64_t>(std::max(f.height, 0)) > kMaxFramePixels) {
        return std::unexpected(Error(ErrorCode::Unsupported, "the picture is larger than a plug-in is given (4096 x 4096)"));
    }
    auto picture = pictureFromFrame(*frame);
    if (!picture) {
        return std::unexpected(picture.error());
    }
    std::vector<float> a = std::move(picture->rgba);
    std::vector<float> b(a.size());
    bool any = false;
    std::string firstError;
    for (const Step& st : steps) {
        if (!st.slot) {
            continue; // a plug-in that is gone: this effect is bypassed
        }
        const std::shared_ptr<OfxRemoteInstance> instance = st.slot->current();
        if (!instance) {
            continue; // refused to be created: bypassed, badged
        }
        OfxRenderRequest request;
        request.width = picture->width;
        request.height = picture->height;
        request.time = st.effect->timeFrames;
        request.fps = st.effect->fps;
        request.pixelAspect = source.pixelAspect;
        request.hasAlpha = picture->hasAlpha;
        const auto value = [&](const std::string& k) {
            for (const auto& [name, v] : st.effect->params) {
                if (name == k) {
                    return v;
                }
            }
            return 0.0;
        };
        const auto text = [&](const std::string& k) {
            for (const auto& [name, v] : st.effect->texts) {
                if (name == k) {
                    return v;
                }
            }
            return std::string();
        };
        request.params = std::make_shared<const nlohmann::json>(renderParams(*st.plugin, value, text));
        const OfxHealth before = instance->health();
        const auto rendered = instance->render(request, a.data(), b.data(), offline);
        if (instance->health() != before) {
            s.bump();
        }
        if (!rendered) {
            if (firstError.empty()) {
                firstError = rendered.error().message;
            }
            continue; // this effect is bypassed; the next one takes the picture as it is
        }
        a.swap(b);
        any = true;
        s.pictures.fetch_add(1, std::memory_order_relaxed);
    }
    if (!any) {
        return std::unexpected(Error(ErrorCode::IoError, firstError.empty() ? "no OpenFX effect could run" : firstError));
    }
    auto raster = rasterFromFloat(a.data(), picture->width, picture->height);
    if (identified) {
        s.store(key.h, raster, picture->color);
    }
    return viewOfRaster(raster, picture->color);
}

} // namespace wabcut::ofxhost
