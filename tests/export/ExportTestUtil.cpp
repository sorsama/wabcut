#include "ExportTestUtil.h"

#include <wabcut/audio/Mixer.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/playback/audio/MediaAudioFeed.h>
#include <wabcut/playback/audio/TrackAudio.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/OffscreenRenderer.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QGuiApplication>
#include <QSize>

#include <algorithm>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace exporttest {

namespace {

// The rendered video source and the preview reference use QRhi, which needs a QGuiApplication (no window).
int g_argc = 1;
char g_argv0[] = "wabcut_export_tests";
char* g_argv[] = {g_argv0, nullptr};
QGuiApplication* g_app = nullptr;

class QtApplicationListener final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override {
        if (!g_app) {
#if defined(__linux__)
            // Headless CI: no display, no xcb/wayland platform plugin to open.
            if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") &&
                qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
                qputenv("QT_QPA_PLATFORM", "offscreen");
            }
#endif
            g_app = new QGuiApplication(g_argc, g_argv);
        }
    }
    void testRunEnded(const Catch::TestRunStats&) override {
        delete g_app;
        g_app = nullptr;
    }
};

} // namespace

CATCH_REGISTER_LISTENER(QtApplicationListener)

namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

struct FormatCloser {
    void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
};
struct CodecFreer {
    void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
};
struct FrameFreer {
    void operator()(AVFrame* p) const { av_frame_free(&p); }
};
struct PacketFreer {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwsFreer {
    void operator()(SwsContext* p) const { sws_free_context(&p); }
};

/// Opens `file` and a software decoder for its best stream of `type`.
bool openDecoder(const std::filesystem::path& file, AVMediaType type,
                 std::unique_ptr<AVFormatContext, FormatCloser>& fmt,
                 std::unique_ptr<AVCodecContext, CodecFreer>& ctx, int& streamIndex, std::string* error) {
    auto fail = [&](const std::string& what) {
        if (error) {
            *error = what;
        }
        return false;
    };
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, utf8(file).c_str(), nullptr, nullptr) < 0) {
        return fail("cannot open " + utf8(file));
    }
    fmt.reset(raw);
    if (avformat_find_stream_info(raw, nullptr) < 0) {
        return fail("no stream info");
    }
    const AVCodec* codec = nullptr;
    streamIndex = av_find_best_stream(raw, type, -1, -1, &codec, 0);
    if (streamIndex < 0 || !codec) {
        return fail("no stream of that type");
    }
    ctx.reset(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), raw->streams[streamIndex]->codecpar);
    ctx->pkt_timebase = raw->streams[streamIndex]->time_base;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) {
        return fail("cannot open the decoder");
    }
    return true;
}

/// Feeds every packet of `streamIndex` and drains; calls onFrame for each decoded frame.
bool decodeAll(AVFormatContext* fmt, AVCodecContext* ctx, int streamIndex,
               const std::function<bool(const AVFrame*)>& onFrame, std::string* error) {
    std::unique_ptr<AVPacket, PacketFreer> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameFreer> frame(av_frame_alloc());
    auto drain = [&]() -> bool {
        for (;;) {
            const int r = avcodec_receive_frame(ctx, frame.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                return true;
            }
            if (r < 0) {
                if (error) {
                    *error = "decode error";
                }
                return false;
            }
            const bool ok = onFrame(frame.get());
            av_frame_unref(frame.get());
            if (!ok) {
                return false;
            }
        }
    };
    while (av_read_frame(fmt, packet.get()) >= 0) {
        if (packet->stream_index == streamIndex) {
            if (avcodec_send_packet(ctx, packet.get()) < 0) {
                av_packet_unref(packet.get());
                if (error) {
                    *error = "send_packet failed";
                }
                return false;
            }
            if (!drain()) {
                av_packet_unref(packet.get());
                return false;
            }
        }
        av_packet_unref(packet.get());
    }
    avcodec_send_packet(ctx, nullptr);
    return drain();
}

/// Converts an RGBA image to `format` the way the export's FrameConverter does.
bool convertRgba(const std::uint8_t* rgba, int stride, int width, int height, AVPixelFormat format,
                 AVFrame* out) {
    std::unique_ptr<SwsContext, SwsFreer> sws(sws_alloc_context());
    sws->flags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP;
    sws->threads = 1;
    std::unique_ptr<AVFrame, FrameFreer> src(av_frame_alloc());
    src->format = AV_PIX_FMT_RGBA;
    src->width = width;
    src->height = height;
    src->data[0] = const_cast<std::uint8_t*>(rgba);
    src->linesize[0] = stride;
    src->colorspace = AVCOL_SPC_RGB;
    src->color_range = AVCOL_RANGE_JPEG;
    src->color_primaries = AVCOL_PRI_BT709;
    src->color_trc = AVCOL_TRC_BT709;
    src->chroma_location = AVCHROMA_LOC_LEFT;
    out->format = format;
    out->width = width;
    out->height = height;
    out->colorspace = AVCOL_SPC_BT709;
    out->color_range = AVCOL_RANGE_MPEG;
    out->color_primaries = AVCOL_PRI_BT709;
    out->color_trc = AVCOL_TRC_BT709;
    out->chroma_location = AVCHROMA_LOC_LEFT;
    if (av_frame_get_buffer(out, 0) < 0) {
        return false;
    }
    return sws_scale_frame(sws.get(), out, src.get()) >= 0;
}

YuvFrame toYuv(const AVFrame* f, double seconds) {
    YuvFrame y;
    const auto format = static_cast<AVPixelFormat>(f->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    y.width = f->width;
    y.height = f->height;
    y.chromaWidth = -((-f->width) >> desc->log2_chroma_w);
    y.chromaHeight = -((-f->height) >> desc->log2_chroma_h);
    y.bits = desc->comp[0].depth;
    y.pixelFormat = av_get_pix_fmt_name(format);
    y.seconds = seconds;
    const bool wide = y.bits > 8;
    for (int p = 0; p < 3; ++p) {
        const int w = p == 0 ? y.width : y.chromaWidth;
        const int h = p == 0 ? y.height : y.chromaHeight;
        auto& plane = y.planes[static_cast<std::size_t>(p)];
        plane.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (int row = 0; row < h; ++row) {
            const std::uint8_t* src = f->data[p] + static_cast<std::ptrdiff_t>(row) * f->linesize[p];
            for (int x = 0; x < w; ++x) {
                plane[static_cast<std::size_t>(row) * w + x] =
                    wide ? reinterpret_cast<const std::uint16_t*>(src)[x] : src[x];
            }
        }
    }
    return y;
}

} // namespace

std::filesystem::path mediaDir() {
    return std::filesystem::path(WABCUT_TEST_MEDIA_DIR);
}

bool haveMedia(std::string_view name) {
    return std::filesystem::is_regular_file(mediaDir() / name);
}

std::filesystem::path freshOutputDir(std::string_view name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "export" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::filesystem::path> partialFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (utf8(e.path().filename()).find(".wabcut-partial-") != std::string::npos) {
            out.push_back(e.path());
        }
    }
    return out;
}

// ---- TestProject ------------------------------------------------------------------------------------------

TestProject::TestProject(int width, int height, Rate rate, Ticks duration) {
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Export test";
    seq.width = width;
    seq.height = height;
    seq.frameRate = rate;
    seq.duration = duration;
    sequence = seq.id;
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
}

MediaId TestProject::addMedia(const std::string& file) {
    const std::string path = utf8(mediaDir() / file);
    auto info = media::probe(path);
    if (!info) {
        throw std::runtime_error("cannot probe " + path + ": " + info.error().message);
    }
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = file;
    item.locator.path = path;
    item.info = *info;
    const MediaId id = item.id;
    state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    return id;
}

namespace {
TrackPtr singleClipTrack(ProjectState& state, TrackKind kind, MediaId media, Ticks start, Ticks duration,
                         Ticks sourceIn, double volumeDb) {
    Clip clip;
    clip.id = state.ids.allocate<ClipId>();
    clip.media = media;
    clip.stream = {kind == TrackKind::Video ? StreamKind::Video : StreamKind::Audio, 0};
    clip.timelineRange = {start, duration};
    clip.sourceIn = sourceIn;
    if (volumeDb != 0.0) {
        ClipProperties props;
        props.set(ParamId::VolumeDb, volumeDb);
        clip.setProperties(props);
    }
    Track track;
    track.id = state.ids.allocate<TrackId>();
    track.kind = kind;
    track.clips.push_back(std::move(clip));
    return std::make_shared<const Track>(std::move(track));
}
} // namespace

void TestProject::addVideoTrack(MediaId media, Ticks start, Ticks duration, Ticks sourceIn) {
    TrackPtr track = singleClipTrack(state, TrackKind::Video, media, start, duration, sourceIn, 0.0);
    editSequence([&](Sequence& s) { s.videoTracks.push_back(track); });
}

void TestProject::addAudioTrack(MediaId media, Ticks start, Ticks duration, Ticks sourceIn, double volumeDb) {
    TrackPtr track = singleClipTrack(state, TrackKind::Audio, media, start, duration, sourceIn, volumeDb);
    editSequence([&](Sequence& s) { s.audioTracks.push_back(track); });
}

void TestProject::addTrack(TrackKind kind, std::vector<Clip> clips) {
    Track track;
    track.id = state.ids.allocate<TrackId>();
    track.kind = kind;
    for (Clip& c : clips) {
        track.clips.push_back(std::move(c));
    }
    TrackPtr ptr = std::make_shared<const Track>(std::move(track));
    editSequence(
        [&](Sequence& s) { (kind == TrackKind::Video ? s.videoTracks : s.audioTracks).push_back(ptr); });
}

Clip TestProject::clip(MediaId media, StreamKind kind, Ticks start, Ticks duration, Ticks sourceIn) {
    Clip c;
    c.id = state.ids.allocate<ClipId>();
    c.media = media;
    c.stream = {kind, 0};
    c.timelineRange = {start, duration};
    c.sourceIn = sourceIn;
    return c;
}

void TestProject::editSequence(const std::function<void(Sequence&)>& edit) {
    Sequence seq = *state.sequences.front();
    edit(seq);
    state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

void TestProject::editMedia(MediaId media, const std::function<void(MediaItem&)>& edit) {
    for (MediaPtr& item : state.media) {
        if (item->id == media) {
            MediaItem copy = *item;
            edit(copy);
            item = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
}

const Sequence& TestProject::seq() const {
    return *state.sequences.front();
}

bool haveTimelineMedia() {
    return haveMedia("h264_1080p60_aac.mp4") && haveMedia("prores_422.mov") && haveMedia("rotated_90.mp4") &&
           haveMedia("ramp_48k.wav") && haveMedia("sine_48k.wav");
}

TestProject threeLayerTimeline(int width, int height, Rate rate, Ticks length) {
    TestProject p(width, height, rate, length);
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    const MediaId prores = p.addMedia("prores_422.mov");
    const MediaId rotated = p.addMedia("rotated_90.mp4");
    const MediaId ramp = p.addMedia("ramp_48k.wav");
    const MediaId sine = p.addMedia("sine_48k.wav");
    const Ticks l = length;
    auto part = [&](std::int64_t num, std::int64_t den) { return Ticks{l.value / den * num}; };
    p.addVideoTrack(h264, Ticks{}, l, Ticks{});
    p.addVideoTrack(prores, part(1, 4), part(2, 4), Ticks{});
    p.addVideoTrack(rotated, part(2, 4), l - part(2, 4), Ticks{});
    p.addAudioTrack(ramp, Ticks{}, l, Ticks::fromSeconds(1));
    p.addAudioTrack(sine, part(1, 8), part(6, 8), ms(250), -6.0);
    return p;
}

namespace {

class CancellingSource final : public exporter::IVideoFrameSource {
public:
    CancellingSource(std::unique_ptr<exporter::IVideoFrameSource> inner, std::int64_t frames,
                     std::stop_source stop)
        : m_inner(std::move(inner)), m_frames(frames), m_stop(std::move(stop)) {}
    std::string description() const override { return m_inner->description(); }
    int maxInFlight() const override { return m_inner->maxInFlight(); }
    std::expected<void, Error> request(std::int64_t frameIndex) override {
        return m_inner->request(frameIndex);
    }
    std::expected<media::FrameHandle, Error> next() override {
        if (++m_taken >= m_frames) {
            m_stop.request_stop();
        }
        return m_inner->next();
    }
    void stop() override { m_inner->stop(); }

private:
    std::unique_ptr<exporter::IVideoFrameSource> m_inner;
    std::int64_t m_frames;
    std::int64_t m_taken = 0;
    std::stop_source m_stop;
};

} // namespace

exporter::VideoSourceFactory cancellingAfter(exporter::VideoSourceFactory inner, std::int64_t frames,
                                             std::stop_source stop) {
    return [inner = std::move(inner), frames, stop](const ProjectState& project, const Sequence& sequence,
                                                    const exporter::VideoSourceFormat& format)
               -> std::expected<std::unique_ptr<exporter::IVideoFrameSource>, Error> {
        auto source = (inner ? inner : exporter::defaultVideoSourceFactory())(project, sequence, format);
        if (!source) {
            return std::unexpected(source.error());
        }
        return std::make_unique<CancellingSource>(std::move(*source), frames, stop);
    };
}

bool haveMotionTimelineMedia() {
    return haveTimelineMedia() && haveMedia("h264_422_10bit.mp4");
}

TestProject motionTimeline(int width, int height, Rate rate) {
    TestProject p(width, height, rate, Ticks::fromSeconds(2));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    const MediaId prores = p.addMedia("prores_422.mov");
    const MediaId rotated = p.addMedia("rotated_90.mp4");
    const MediaId tenBit = p.addMedia("h264_422_10bit.mp4");
    const MediaId ramp = p.addMedia("ramp_48k.wav");
    const MediaId sine = p.addMedia("sine_48k.wav");

    Clip v1 = p.clip(h264, StreamKind::Video, Ticks{}, ms(1500), Ticks::fromSeconds(1));
    ClipProperties v1p;
    v1p.set(ParamId::Scale, static_cast<double>(width) / 1920.0);
    v1.setProperties(v1p);
    p.addTrack(TrackKind::Video, {v1});

    Clip v2 = p.clip(prores, StreamKind::Video, ms(250), ms(1500), Ticks{});
    ClipProperties v2p;
    v2p.set(ParamId::Scale, 0.5);
    v2p.set(ParamId::PositionX, 200.0);
    v2p.set(ParamId::PositionY, -80.0);
    v2p.set(ParamId::Rotation, 10.0);
    v2p.set(ParamId::Opacity, 0.75);
    v2.setProperties(v2p);
    p.addTrack(TrackKind::Video, {v2});

    Clip disabled = p.clip(tenBit, StreamKind::Video, Ticks{}, ms(500), Ticks{});
    disabled.enabled = false;
    Clip v3 = p.clip(rotated, StreamKind::Video, ms(500), ms(1000), Ticks{});
    ClipProperties v3p;
    v3p.set(ParamId::Scale, 0.8);
    v3p.set(ParamId::PositionX, -300.0);
    v3p.set(ParamId::Opacity, 0.6);
    v3.setProperties(v3p);
    p.addTrack(TrackKind::Video, {disabled, v3});

    p.addAudioTrack(ramp, Ticks{}, Ticks::fromSeconds(2), Ticks::fromSeconds(1));
    p.addAudioTrack(sine, ms(250), ms(1500), ms(250), -6.0);
    return p;
}

// ---- PreviewRenderer --------------------------------------------------------------------------------------

struct PreviewRenderer::Impl {
    ProjectState project;
    Sequence sequence;
    std::optional<playback::CaptionSettings> captions;
    std::unique_ptr<render::OffscreenRenderer> renderer;
    std::map<std::uint64_t, std::unique_ptr<media::VideoDecoder>> decoders;
};

PreviewRenderer::PreviewRenderer(const TestProject& project) : m_impl(std::make_unique<Impl>()) {
    m_impl->project = project.state;
    m_impl->sequence = project.seq();
    render::OffscreenOptions options; // like the program monitor: display role, preview pool
    options.role = render::OutputRole::Display;
    options.alpha = render::OutputAlpha::Opaque;
    options.compositor = render::CompositorOptions{render::TexturePool::Preview, 1, 48};
    auto renderer = render::OffscreenRenderer::create(options);
    if (!renderer) {
        throw std::runtime_error("no offscreen renderer: " + renderer.error().message);
    }
    m_impl->renderer = std::move(*renderer);
}

PreviewRenderer::~PreviewRenderer() = default;

void PreviewRenderer::setCaptions(std::optional<playback::CaptionSettings> captions) {
    m_impl->captions = std::move(captions);
}

int PreviewRenderer::width() const {
    return m_impl->sequence.width;
}
int PreviewRenderer::height() const {
    return m_impl->sequence.height;
}

std::vector<std::uint8_t> PreviewRenderer::render(Ticks time, Ticks frameDuration, int width, int height) {
    std::vector<playback::LayerSpec> specs;
    playback::evaluateVideoLayers(m_impl->project, m_impl->sequence, time, specs, true, nullptr,
                                  m_impl->captions ? &*m_impl->captions : nullptr);
    // As the Scheduler assembles it (Scheduler::Impl::publishIfReady).
    auto composite = std::make_shared<render::PreparedComposite>();
    composite->targetTime = time;
    composite->frameDuration = frameDuration;
    composite->canvas = {m_impl->sequence.width, m_impl->sequence.height};
    composite->color = render::ColorPipeline{};
    // As the Scheduler assembles it (playback::buildCompositeLayers); titles are drawn, not decoded.
    std::vector<media::VideoFrame> frames(specs.size());
    for (std::size_t j = 0; j < specs.size(); ++j) {
        const playback::LayerSpec& spec = specs[j];
        if (!playback::needsDecode(spec)) {
            continue;
        }
        auto& decoder = m_impl->decoders[playback::sessionKey(spec)];
        if (!decoder) {
            auto opened =
                media::VideoDecoder::open(*playback::sourcePathOf(*spec.item), spec.containerStream,
                                          media::DecoderOptions{.snapToStreamTicks = true}); // as playback decodes
            if (!opened) {
                throw std::runtime_error("cannot open " + spec.item->name + ": " + opened.error().message);
            }
            decoder = std::move(*opened);
        }
        auto frame = decoder->frameAt(spec.sourceTime);
        if (!frame) {
            continue; // a failed lookup leaves the layer out, like the Scheduler
        }
        frames[j] = std::move(*frame);
    }
    playback::buildCompositeLayers(specs, frames, composite->layers);
    auto image = width > 0 ? m_impl->renderer->render(composite, QSize(width, height))
                           : m_impl->renderer->render(composite);
    if (!image) {
        throw std::runtime_error("preview render failed: " + image.error().message);
    }
    return std::move(image->rgba);
}

exporter::ExportRequest makeRequest(const TestProject& project, exporter::ExportSettings settings,
                                    const std::filesystem::path& output) {
    exporter::ExportRequest r;
    r.snapshot = project.snapshot();
    r.sequence = project.sequence;
    r.settings = std::move(settings);
    r.output = output;
    r.videoSource = exporter::syntheticVideoSourceFactory();
    return r;
}

// ---- ffprobe ----------------------------------------------------------------------------------------------

const std::map<std::string, std::string>* ProbeResult::stream(std::string_view type) const {
    for (const auto& s : streams) {
        const auto it = s.find("codec_type");
        if (it != s.end() && it->second == type) {
            return &s;
        }
    }
    return nullptr;
}

std::optional<ProbeResult> ffprobe(const std::filesystem::path& file, std::string* error) {
    const std::string probeLine = "\"" + utf8(std::filesystem::path(WABCUT_FFPROBE_EXECUTABLE)) +
                                  "\" -v error -show_streams -show_format \"" + utf8(file) + "\"";
#if defined(_WIN32)
    // cmd.exe strips the outer quotes of the whole line, so the line is wrapped in one more pair; /bin/sh must not get
    // them (it would read the whole line as one word).
    const std::string command = "\"" + probeLine + "\"";
#else
    const std::string& command = probeLine;
#endif
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        if (error) {
            *error = "cannot run ffprobe";
        }
        return std::nullopt;
    }
    std::string text;
    char buf[4096];
    while (const std::size_t n = std::fread(buf, 1, sizeof buf, pipe)) {
        text.append(buf, n);
    }
#if defined(_WIN32)
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    if (status != 0) {
        if (error) {
            *error = "ffprobe failed: " + text;
        }
        return std::nullopt;
    }
    ProbeResult result;
    std::map<std::string, std::string>* section = nullptr;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line == "[STREAM]") {
            section = &result.streams.emplace_back();
        } else if (line == "[FORMAT]") {
            section = &result.format;
        } else if (line.starts_with("[/")) {
            section = nullptr;
        } else if (section) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) {
                (*section)[line.substr(0, eq)] = line.substr(eq + 1);
            }
        }
    }
    return result;
}

// ---- Decoding ---------------------------------------------------------------------------------------------

int countVideoPackets(const std::filesystem::path& file) {
    std::unique_ptr<AVFormatContext, FormatCloser> fmt;
    std::unique_ptr<AVCodecContext, CodecFreer> ctx;
    int index = -1;
    if (!openDecoder(file, AVMEDIA_TYPE_VIDEO, fmt, ctx, index, nullptr)) {
        return -1;
    }
    int count = 0;
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(fmt.get(), packet) >= 0) {
        count += packet->stream_index == index ? 1 : 0;
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return count;
}

bool decodeVideo(const std::filesystem::path& file, const std::function<void(const YuvFrame&)>& onFrame,
                 std::string* error) {
    std::unique_ptr<AVFormatContext, FormatCloser> fmt;
    std::unique_ptr<AVCodecContext, CodecFreer> ctx;
    int index = -1;
    if (!openDecoder(file, AVMEDIA_TYPE_VIDEO, fmt, ctx, index, error)) {
        return false;
    }
    const AVRational tb = fmt->streams[index]->time_base;
    return decodeAll(
        fmt.get(), ctx.get(), index,
        [&](const AVFrame* f) {
            const std::int64_t ts = f->best_effort_timestamp == AV_NOPTS_VALUE ? 0 : f->best_effort_timestamp;
            onFrame(toYuv(f, static_cast<double>(ts) * av_q2d(tb)));
            return true;
        },
        error);
}

int barcodeOf(const YuvFrame& frame) {
    if (frame.width < 512 || frame.height < 32) {
        return -1;
    }
    const int mid = 1 << (frame.bits - 1);
    int value = 0;
    for (int bit = 0; bit < 16; ++bit) {
        const int luma = frame.y(32 * bit + 16, 16);
        // White is ~235, black ~16 (8-bit limited range): a value near the middle is unreadable.
        if (std::abs(luma - mid) < (mid >> 2)) {
            return -1;
        }
        if (luma > mid) {
            value |= 1 << bit;
        }
    }
    return value;
}

YuvFrame referenceFrame(int renderWidth, int renderHeight, std::int64_t number, const YuvFrame& like) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(renderWidth) *
                                   static_cast<std::size_t>(renderHeight) * 4);
    exporter::drawSyntheticFrame(rgba.data(), renderWidth, renderHeight, number);
    // The export's crop (Fill) / pad (Fit), offsets rounded down to even.
    const int viewW = std::min(renderWidth, like.width);
    const int viewH = std::min(renderHeight, like.height);
    const int cropX = ((renderWidth - viewW) / 2) & ~1;
    const int cropY = ((renderHeight - viewH) / 2) & ~1;
    const AVPixelFormat format = av_get_pix_fmt(like.pixelFormat.c_str());
    std::unique_ptr<AVFrame, FrameFreer> converted(av_frame_alloc());
    const std::uint8_t* origin = rgba.data() + (static_cast<std::size_t>(cropY) * renderWidth + cropX) * 4;
    if (!convertRgba(origin, renderWidth * 4, viewW, viewH, format, converted.get())) {
        throw std::runtime_error("reference conversion failed");
    }
    YuvFrame view = toYuv(converted.get(), like.seconds);
    if (viewW == like.width && viewH == like.height) {
        return view;
    }
    // Pad: black (limited range) around the converted view.
    YuvFrame out = like;
    const int black = 16 << (like.bits - 8);
    const int grey = 128 << (like.bits - 8);
    const int padX = ((like.width - viewW) / 2) & ~1;
    const int padY = ((like.height - viewH) / 2) & ~1;
    for (int p = 0; p < 3; ++p) {
        const bool chroma = p > 0;
        auto& plane = out.planes[static_cast<std::size_t>(p)];
        std::fill(plane.begin(), plane.end(), static_cast<std::uint16_t>(chroma ? grey : black));
        const int planeW = chroma ? like.chromaWidth : like.width;
        const int sx = chroma && like.chromaWidth < like.width ? 1 : 0;
        const int sy = chroma && like.chromaHeight < like.height ? 1 : 0;
        const int vw = chroma ? view.chromaWidth : view.width;
        const int vh = chroma ? view.chromaHeight : view.height;
        for (int row = 0; row < vh; ++row) {
            for (int x = 0; x < vw; ++x) {
                plane[static_cast<std::size_t>(row + (padY >> sy)) * planeW + (x + (padX >> sx))] =
                    view.planes[static_cast<std::size_t>(p)][static_cast<std::size_t>(row) * vw + x];
            }
        }
    }
    return out;
}

YuvFrame yuvFromRgba(const std::vector<std::uint8_t>& rgba, int width, int height, const YuvFrame& like) {
    const AVPixelFormat format = av_get_pix_fmt(like.pixelFormat.c_str());
    std::unique_ptr<AVFrame, FrameFreer> converted(av_frame_alloc());
    if (!convertRgba(rgba.data(), width * 4, width, height, format, converted.get())) {
        throw std::runtime_error("reference conversion failed");
    }
    return toYuv(converted.get(), like.seconds);
}

double psnr(const YuvFrame& a, const YuvFrame& b) {
    if (a.width != b.width || a.height != b.height || a.bits != b.bits) {
        return 0.0;
    }
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t p = 0; p < 3; ++p) {
        const auto& pa = a.planes[p];
        const auto& pb = b.planes[p];
        if (pa.size() != pb.size()) {
            return 0.0;
        }
        for (std::size_t i = 0; i < pa.size(); ++i) {
            const double d = static_cast<double>(pa[i]) - static_cast<double>(pb[i]);
            sum += d * d;
        }
        count += pa.size();
    }
    if (sum == 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double peak = static_cast<double>((1 << a.bits) - 1);
    return 10.0 * std::log10(peak * peak / (sum / static_cast<double>(count)));
}

std::optional<DecodedAudio> decodeAudio(const std::filesystem::path& file, std::string* error) {
    std::unique_ptr<AVFormatContext, FormatCloser> fmt;
    std::unique_ptr<AVCodecContext, CodecFreer> ctx;
    int index = -1;
    if (!openDecoder(file, AVMEDIA_TYPE_AUDIO, fmt, ctx, index, error)) {
        return std::nullopt;
    }
    DecodedAudio out;
    out.codec = avcodec_get_name(ctx->codec_id);
    out.sampleRate = ctx->sample_rate;
    out.channels = ctx->ch_layout.nb_channels;
    const bool ok = decodeAll(
        fmt.get(), ctx.get(), index,
        [&](const AVFrame* f) {
            const int ch = f->ch_layout.nb_channels;
            for (int i = 0; i < f->nb_samples; ++i) {
                for (int c = 0; c < ch; ++c) {
                    switch (f->format) {
                    case AV_SAMPLE_FMT_FLTP:
                        out.samples.push_back(reinterpret_cast<const float*>(f->extended_data[c])[i]);
                        break;
                    case AV_SAMPLE_FMT_S16: {
                        const std::int16_t v = reinterpret_cast<const std::int16_t*>(f->data[0])[i * ch + c];
                        out.pcm.push_back(v);
                        out.samples.push_back(static_cast<float>(v) / 32768.0f);
                        break;
                    }
                    case AV_SAMPLE_FMT_S32: { // pcm_s24le: the 24-bit value << 8
                        const std::int32_t v =
                            reinterpret_cast<const std::int32_t*>(f->data[0])[i * ch + c] / 256;
                        out.pcm.push_back(v);
                        out.samples.push_back(static_cast<float>(v) / 8388608.0f);
                        break;
                    }
                    default:
                        return false;
                    }
                }
            }
            return true;
        },
        error);
    if (!ok) {
        return std::nullopt;
    }
    return out;
}

std::vector<float> referenceMix(const TestProject& project, Ticks start, std::int64_t frames, int channels,
                                int sampleRate) {
    EditSession session(project.state);
    playback::MediaAudioFeed feed(session);
    const Sequence& seq = project.seq();
    const int buses = static_cast<int>(seq.audioTracks.size());
    audio::Mixer mixer(buses, channels, sampleRate, 1024);
    bool anySolo = false;
    for (const TrackPtr& t : seq.audioTracks) {
        anySolo = anySolo || t->solo;
    }
    for (int i = 0; i < buses; ++i) {
        const Track& t = *seq.audioTracks[static_cast<std::size_t>(i)];
        mixer.setBusMuted(i, t.muted || !t.enabled);
        mixer.setBusSoloed(i, t.solo);
    }
    const Rate rate{sampleRate, 1};
    const auto ch = static_cast<std::size_t>(channels);
    std::vector<std::vector<float>> bus(static_cast<std::size_t>(buses), std::vector<float>(1024 * ch));
    std::vector<float> block(1024 * ch);
    std::vector<float> out;
    playback::TrackRenderStats stats;
    Ticks t = start;
    while (static_cast<std::int64_t>(out.size() / ch) < frames) {
        for (int i = 0; i < buses; ++i) {
            const Track& track = *seq.audioTracks[static_cast<std::size_t>(i)];
            playback::renderTrack(track, t, bus[static_cast<std::size_t>(i)], channels, rate, feed, {},
                                  stats);
        }
        mixer.beginBlock(block);
        for (int i = 0; i < buses; ++i) {
            const Track& track = *seq.audioTracks[static_cast<std::size_t>(i)];
            if (!(track.muted || !track.enabled) && (!anySolo || track.solo)) {
                mixer.addBus(i, bus[static_cast<std::size_t>(i)], channels);
            }
        }
        mixer.endBlock();
        out.insert(out.end(), block.begin(), block.end());
        t += Ticks{1024 * rate.ticksPerUnit()};
    }
    out.resize(static_cast<std::size_t>(frames) * ch);
    return out;
}

void metric(const std::string& line) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "export";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "metrics.txt", std::ios::app) << line << '\n';
    std::printf("%s\n", line.c_str());
}

EncoderOrderGuard::~EncoderOrderGuard() {
    exporter::setHardwareEncoderOrder({});
}

} // namespace exporttest
