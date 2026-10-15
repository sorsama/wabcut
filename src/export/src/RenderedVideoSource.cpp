#include <wabcut/export/RenderedVideoSource.h>

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/playback/video/Evaluate.h>

#include <QSize>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <format>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace wabcut::exporter {

namespace {

/// One frame on its way through the source.
struct Work {
    std::int64_t index = 0;
    std::shared_ptr<const render::PreparedComposite> composite; // set by the decode thread
    std::optional<Error> error;
};

/// Burned-in captions of an export, as the preview scheduler takes them.
std::optional<playback::CaptionSettings> captionsOf(const VideoSourceFormat& format) {
    if (!format.burnInCaptions) {
        return std::nullopt;
    }
    playback::CaptionSettings c;
    c.style = format.burnInCaptions->style;
    if (format.burnInCaptions->track) {
        c.track = CaptionTrackId{*format.burnInCaptions->track};
    }
    c.karaoke = format.burnInCaptions->karaoke;
    c.words = format.burnInCaptions->words; // the preview's word timings (P2-8)
    // Bleeped or muted words are drawn as this (evaluateCaption with the project, as the preview; Codex wave-4 P1-3).
    c.redactedWord = format.burnInCaptions->redactedWord;
    return c;
}

class RenderedVideoSource final : public IVideoFrameSource {
public:
    RenderedVideoSource(ProjectState project, Sequence sequence, VideoSourceFormat format,
                        RenderedSourceOptions options)
        : m_project(std::move(project)), m_sequence(std::move(sequence)), m_format(format),
          m_options(std::move(options)), m_maxInFlight(std::clamp(m_options.maxInFlight, 1, 16)) {}

    ~RenderedVideoSource() override { stop(); }

    /// Starts the threads; every render thread has created its QRhi before this returns.
    std::expected<void, Error> start() {
        const int renderers = std::clamp(m_options.renderThreads, 1, 4);
        std::vector<std::promise<std::expected<void, Error>>> started(static_cast<std::size_t>(renderers));
        for (int i = 0; i < renderers; ++i) {
            auto* promise = &started[static_cast<std::size_t>(i)];
            m_renderThreads.emplace_back([this, promise, i] { renderMain(*promise, i); });
        }
        for (auto& promise : started) {
            if (auto result = promise.get_future().get(); !result) {
                stop();
                return result;
            }
        }
        m_decodeThread = std::jthread([this] { decodeMain(); });
        return {};
    }

    std::string description() const override { return m_description; }
    int maxInFlight() const override { return m_maxInFlight; }

    std::expected<void, Error> request(std::int64_t frameIndex) override {
        std::lock_guard lock(m_mutex);
        if (m_stopped) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Video source stopped"});
        }
        if (frameIndex < 0 || frameIndex >= m_format.frameCount) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Frame index out of range"});
        }
        if (static_cast<int>(m_order.size()) >= m_maxInFlight) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Too many frames in flight"});
        }
        m_order.push_back(frameIndex);
        m_toDecode.push_back(frameIndex);
        m_changed.notify_all();
        return {};
    }

    std::expected<media::FrameHandle, Error> next() override {
        std::unique_lock lock(m_mutex);
        if (m_order.empty()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "next() without a pending request"});
        }
        const std::int64_t index = m_order.front();
        m_changed.wait(lock, [&] { return m_stopped || m_done.contains(index); });
        if (m_stopped && !m_done.contains(index)) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Video source stopped"});
        }
        auto node = m_done.extract(index);
        m_order.pop_front();
        return std::move(node.mapped());
    }

    void stop() override {
        // Callable from any thread, also concurrently (a cancel and the job's own teardown): one caller
        // joins, the others wait for it.
        std::lock_guard stopping(m_stopMutex);
        {
            std::lock_guard lock(m_mutex);
            m_stopped = true;
            m_toDecode.clear();
            m_toRender.clear();
        }
        m_changed.notify_all();
        if (m_decodeThread.joinable()) {
            m_decodeThread.join();
        }
        for (std::jthread& t : m_renderThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        std::lock_guard lock(m_mutex);
        m_done.clear();
    }

private:
    // ---- Decode thread: evaluate + decode ----------------------------------------------------------

    struct Decoder {
        std::unique_ptr<media::VideoDecoder> decoder;
        std::int64_t lastUsed = 0;
    };

    void decodeMain() {
        std::vector<playback::LayerSpec> specs;
        std::map<std::uint64_t, Decoder> decoders; // by sessionKey (clip, and its nesting path): each decodes forward on its own
        const Ticks frameDuration = framesToTicks(1, m_format.frameRate, Rounding::Nearest).value_or(Ticks{});
        for (;;) {
            std::int64_t index = 0;
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, [&] { return m_stopped || !m_toDecode.empty(); });
                if (m_stopped) {
                    return; // decoders close here, on their own thread
                }
                index = m_toDecode.front();
                m_toDecode.pop_front();
            }
            Work work;
            work.index = index;
            // Exact rational output time: start + index frame periods of the output rate.
            const auto offset = framesToTicks(index, m_format.frameRate, Rounding::Nearest);
            const auto time = offset ? checkedAdd(m_format.start, *offset) : std::nullopt;
            if (!time) {
                work.error = Error{ErrorCode::InvalidArgument, "Output time out of range"};
            } else {
                work.composite = evaluate(*time, frameDuration, index, specs, decoders, work.error);
            }
            closeIdleDecoders(decoders, index);
            {
                std::lock_guard lock(m_mutex);
                if (m_stopped) {
                    return;
                }
                m_toRender.push_back(std::move(work));
            }
            m_changed.notify_all();
        }
    }

    std::shared_ptr<const render::PreparedComposite>
    evaluate(Ticks time, Ticks frameDuration, std::int64_t index, std::vector<playback::LayerSpec>& specs,
             std::map<std::uint64_t, Decoder>& decoders, std::optional<Error>& error) {
        playback::evaluateVideoLayers(m_project, m_sequence, time, specs, true, nullptr,
                                      m_captions ? &*m_captions : nullptr);
        // The same request the preview Scheduler publishes (Scheduler::Impl::publishIfReady).
        auto composite = std::make_shared<render::PreparedComposite>();
        composite->targetTime = time;
        composite->frameDuration = frameDuration;
        composite->canvas = {m_sequence.width, m_sequence.height};
        composite->color = m_options.color;
        // Decoded frames per evaluated layer; generated layers (titles) need none. The composite layers are then
        // built exactly as the preview Scheduler builds them (effects, transitions, titles): preview == export.
        std::vector<media::VideoFrame> frames(specs.size());
        for (std::size_t j = 0; j < specs.size(); ++j) {
            const playback::LayerSpec& spec = specs[j];
            if (!playback::needsDecode(spec)) {
                continue;
            }
            Decoder& d = decoders[playback::sessionKey(spec)];
            d.lastUsed = index;
            if (!d.decoder) {
                const std::string* path = playback::sourcePathOf(*spec.item, false); // originals only
                if (!path) {
                    error = Error{ErrorCode::NotFound, std::format("{} is offline", spec.item->name)};
                    return nullptr;
                }
                media::DecoderOptions options;
                options.allowHardware = m_options.hardwareDecode;
                options.snapToStreamTicks = true; // sequence frames, as playback shows them
                auto opened = media::VideoDecoder::open(*path, spec.containerStream, options);
                if (!opened) {
                    error = Error{opened.error().code, std::format("Cannot decode {}: {}", spec.item->name,
                                                                   opened.error().message)};
                    return nullptr;
                }
                d.decoder = std::move(*opened);
            }
            auto frame = d.decoder->frameAt(spec.sourceTime);
            if (!frame) {
                if (frame.error().code == ErrorCode::NotFound) {
                    continue; // past the end of the media: the preview shows nothing there either
                }
                error = Error{frame.error().code,
                              std::format("Cannot decode {} at {:.3f} s: {}", spec.item->name,
                                          spec.sourceTime.toSeconds(), frame.error().message)};
                return nullptr;
            }
            frames[j] = std::move(*frame);
        }
        playback::buildCompositeLayers(specs, frames, composite->layers);
        return composite;
    }

    void closeIdleDecoders(std::map<std::uint64_t, Decoder>& decoders, std::int64_t index) const {
        // Clips not shown for a second of output are done (export runs forward); beyond the cap, the
        // least recently used one goes.
        const std::int64_t idleFrames =
            std::max<std::int64_t>(1, static_cast<std::int64_t>(m_format.frameRate.toDouble()));
        std::erase_if(decoders,
                      [&](const auto& entry) { return index - entry.second.lastUsed > idleFrames; });
        while (static_cast<int>(decoders.size()) > std::max(1, m_options.maxDecoders)) {
            const auto oldest =
                std::min_element(decoders.begin(), decoders.end(), [](const auto& a, const auto& b) {
                    return a.second.lastUsed < b.second.lastUsed;
                });
            decoders.erase(oldest);
        }
    }

    // ---- Render threads: composite + readback ------------------------------------------------------

    /// One of renderThreads workers: each owns a QRhi and takes the next evaluated frame; next() restores the
    /// order (results are keyed by frame index).
    void renderMain(std::promise<std::expected<void, Error>>& started, int worker) {
        render::OffscreenOptions options;
        options.backend = m_options.backend;
        options.role = render::OutputRole::Export;
        options.alpha = render::OutputAlpha::Opaque;
        options.compositor = render::CompositorOptions{render::TexturePool::Export, 1, 48};
        options.timestamps = false;
        auto renderer = render::OffscreenRenderer::create(options);
        if (!renderer) {
            started.set_value(std::unexpected(renderer.error()));
            return;
        }
        if (worker == 0) {
            m_description = std::format(
                "OffscreenRenderer {} · {} · {} decode, {} render thread{}", (*renderer)->backendName(),
                (*renderer)->adapterName(), m_options.hardwareDecode ? "hardware" : "software",
                std::clamp(m_options.renderThreads, 1, 4), m_options.renderThreads > 1 ? "s" : "");
        }
        started.set_value({});

        const QSize size(m_format.width, m_format.height);
        for (;;) {
            Work work;
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, [&] { return m_stopped || !m_toRender.empty(); });
                if (m_stopped) {
                    return;
                }
                work = std::move(m_toRender.front());
                m_toRender.pop_front();
            }
            std::expected<media::FrameHandle, Error> result =
                work.error ? std::expected<media::FrameHandle, Error>(std::unexpected(*work.error))
                           : renderFrame(**renderer, work.composite, size, work.index);
            work.composite.reset(); // releases the decoded layers here, off the encoder's thread
            {
                std::lock_guard lock(m_mutex);
                if (m_stopped) {
                    return;
                }
                m_done.emplace(work.index, std::move(result));
            }
            m_changed.notify_all();
        }
    }

    std::expected<media::FrameHandle, Error>
    renderFrame(render::OffscreenRenderer& renderer,
                const std::shared_ptr<const render::PreparedComposite>& composite, const QSize& size,
                std::int64_t index) {
        auto image = renderer.render(composite, size);
        if (!image) {
            return std::unexpected(image.error());
        }
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(std::move(image->rgba));
        media::FrameHandle f;
        f.pts = composite->targetTime;
        f.duration = composite->frameDuration;
        f.streamPts = index;
        f.timeBase = timeBaseOf(m_format.frameRate);
        f.width = image->width;
        f.height = image->height;
        f.format = media::PixelFormat::RGBA8;
        // The export output transform's code values: BT.709-encoded R'G'B', full range.
        f.color = ColorInfo{ColorMatrix::RGB, ColorRange::Full, ColorPrimaries::BT709,
                            TransferFunction::BT709, ChromaLocation::Unknown};
        media::CpuPlanes cpu;
        cpu.planes[0] = media::Plane{pixels->data(), image->width * 4, image->width, image->height};
        cpu.owner = std::shared_ptr<const void>(pixels, pixels->data());
        f.payload = std::move(cpu);
        return f;
    }

    const ProjectState m_project;
    const Sequence m_sequence;
    const VideoSourceFormat m_format;
    const std::optional<playback::CaptionSettings> m_captions = captionsOf(m_format);
    const RenderedSourceOptions m_options;
    const int m_maxInFlight;
    std::string m_description; // written by the render thread before start() returns

    std::mutex m_stopMutex; // serializes stop()
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::int64_t> m_order;    // requested, not yet returned by next()
    std::deque<std::int64_t> m_toDecode; // requested, not yet evaluated
    std::deque<Work> m_toRender;         // evaluated, not yet rendered
    std::map<std::int64_t, std::expected<media::FrameHandle, Error>> m_done;
    bool m_stopped = false;
    std::vector<std::jthread> m_renderThreads; // last: joined before the state above is destroyed
    std::jthread m_decodeThread;
};

} // namespace

VideoSourceFactory renderedVideoSourceFactory(RenderedSourceOptions options) {
    return [options](
               const ProjectState& snapshot, const Sequence& sequence,
               const VideoSourceFormat& format) -> std::expected<std::unique_ptr<IVideoFrameSource>, Error> {
        auto source = std::make_unique<RenderedVideoSource>(snapshot, sequence, format, options);
        if (auto ok = source->start(); !ok) {
            return std::unexpected(ok.error());
        }
        return source;
    };
}

} // namespace wabcut::exporter
