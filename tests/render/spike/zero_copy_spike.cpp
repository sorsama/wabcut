// Zero-copy spike (report-only; findings in docs/zero-copy.md): can a D3D11VA decoder surface be sampled by
// QRhi on the SAME D3D11 device without any CPU copy?
//
//  1. Device: the QRhi's own D3D11 device (as QQuickWindow would create it) and, second, a device we create
//  with
//     D3D11_CREATE_DEVICE_VIDEO_SUPPORT and import into QRhi (QRhi::create(..., importDevice)), the route a
//     Wabcut-owned device would take (QQuickGraphicsDevice::fromDeviceAndContext for the window).
//  2. FFmpeg d3d11va on that device (AVD3D11VADeviceContext wraps it), decoder surfaces created with
//     D3D11_BIND_SHADER_RESOURCE so they can be viewed by shaders.
//  3. QRhiTexture::createFrom() on the NV12 array texture: an R8 view (luma plane) and an RG8 view (chroma
//  plane),
//     restricted to the frame's array slice with setArrayRange (QRhi::TextureArrayRange).
//  4. Render through sampler2DArray, read back, compare with the CPU path (software decode + Compositor) by
//  PSNR.
//  5. Time: decode + hwdownload (today's path) vs decode + import (zero-copy), per frame.
//
// Exit codes: 0 = proven on at least one device route; 77 = not applicable (no D3D11 / no hardware decoder);
// 1 = failed. Writes its findings to <build>/test-output/render/zero_copy_spike.txt.

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>

// clang-format off: windows.h first
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
// clang-format on

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::ostringstream g_report;

template <class... Args> void note(const char* fmt, Args... args) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, fmt, args...);
    std::fputs(buf, stdout);
    std::fputs("\n", stdout);
    g_report << buf << "\n";
}

QShader loadShader(const char* name) {
    QFile f(QStringLiteral(":/wabcut/spike/%1.qsb").arg(QLatin1String(name)));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

struct Decoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVBufferRef* device = nullptr;
    int stream = -1;
    std::string error;

    ~Decoder() {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        av_buffer_unref(&device);
    }

    static AVPixelFormat getFormat(AVCodecContext* c, const AVPixelFormat* formats) {
        for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
            if (*f != AV_PIX_FMT_D3D11) {
                continue;
            }
            AVBufferRef* frames = nullptr;
            if (avcodec_get_hw_frames_parameters(c, c->hw_device_ctx, AV_PIX_FMT_D3D11, &frames) < 0) {
                return AV_PIX_FMT_NONE;
            }
            auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
            auto* d3d = static_cast<AVD3D11VAFramesContext*>(fc->hwctx);
            d3d->BindFlags |=
                D3D11_BIND_SHADER_RESOURCE; // the whole point: shaders may view decoder surfaces
            fc->initial_pool_size += 8;     // frames the renderer may hold in flight
            if (av_hwframe_ctx_init(frames) < 0) {
                av_buffer_unref(&frames);
                return AV_PIX_FMT_NONE;
            }
            c->hw_frames_ctx = frames;
            return AV_PIX_FMT_D3D11;
        }
        return AV_PIX_FMT_NONE;
    }

    bool open(const std::string& path, ID3D11Device* dev, ID3D11DeviceContext* immediate) {
        device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        auto* hw =
            static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);
        dev->AddRef();
        immediate->AddRef();
        hw->device = dev; // FFmpeg releases its references when the device context is freed
        hw->device_context = immediate;
        if (int r = av_hwdevice_ctx_init(device); r < 0) {
            error = "av_hwdevice_ctx_init failed on this device (" + std::to_string(r) + ")";
            return false;
        }
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 ||
            avformat_find_stream_info(fmt, nullptr) < 0) {
            error = "cannot open " + path;
            return false;
        }
        const AVCodec* codec = nullptr;
        stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (stream < 0) {
            error = "no video stream";
            return false;
        }
        ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar);
        ctx->hw_device_ctx = av_buffer_ref(device);
        ctx->get_format = &Decoder::getFormat;
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            error = "avcodec_open2 failed";
            return false;
        }
        return true;
    }

    /// Next frame in presentation order (null at the end).
    AVFrame* next() {
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        for (;;) {
            const int r = avcodec_receive_frame(ctx, frame);
            if (r == 0) {
                av_packet_free(&packet);
                return frame;
            }
            if (r != AVERROR(EAGAIN)) {
                break;
            }
            const int rr = av_read_frame(fmt, packet);
            if (rr < 0) {
                avcodec_send_packet(ctx, nullptr);
                continue;
            }
            if (packet->stream_index == stream) {
                avcodec_send_packet(ctx, packet);
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
        return nullptr;
    }
};

double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int w, int h) {
    double sum = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * static_cast<std::size_t>(h); ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const double d = static_cast<double>(a[i * 4 + c]) - static_cast<double>(b[i * 4 + c]);
            sum += d * d;
        }
    }
    const double mse = sum / (static_cast<double>(w) * h * 3);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// Imports `frame`'s surface slice into `rhi` and renders it to RGBA8 (top row first).
bool renderZeroCopy(QRhi* rhi, AVFrame* frame, int width, int height, std::vector<std::uint8_t>& out,
                    double& importMs, std::string& why) {
    auto* tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const int slice = static_cast<int>(reinterpret_cast<std::intptr_t>(frame->data[1]));
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) {
        why = "surface format is not NV12";
        return false;
    }
    if (!rhi->isFeatureSupported(QRhi::TextureArrayRange)) {
        why = "QRhi::TextureArrayRange unsupported";
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const QSize surface(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
    std::unique_ptr<QRhiTexture> luma(
        rhi->newTextureArray(QRhiTexture::R8, static_cast<int>(desc.ArraySize), surface));
    std::unique_ptr<QRhiTexture> chroma(
        rhi->newTextureArray(QRhiTexture::RG8, static_cast<int>(desc.ArraySize), surface / 2));
    luma->setArrayRange(slice, 1);
    chroma->setArrayRange(slice, 1);
    if (!luma->createFrom({reinterpret_cast<quint64>(tex), 0}) ||
        !chroma->createFrom({reinterpret_cast<quint64>(tex), 0})) {
        why = "QRhiTexture::createFrom failed (no SRV on the decoder surface?)";
        return false;
    }
    importMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    std::unique_ptr<QRhiTexture> target(
        rhi->newTexture(QRhiTexture::RGBA8, QSize(width, height), 1,
                        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    target->create();
    std::unique_ptr<QRhiTextureRenderTarget> rt(rhi->newTextureRenderTarget({target.get()}));
    std::unique_ptr<QRhiRenderPassDescriptor> rpd(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rpd.get());
    rt->create();
    std::unique_ptr<QRhiSampler> sampler(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                                         QRhiSampler::None, QRhiSampler::ClampToEdge,
                                                         QRhiSampler::ClampToEdge));
    sampler->create();
    std::unique_ptr<QRhiBuffer> ubuf(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 96));
    ubuf->create();
    static const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    std::unique_ptr<QRhiBuffer> vbuf(
        rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof quad));
    vbuf->create();
    std::unique_ptr<QRhiShaderResourceBindings> srb(rhi->newShaderResourceBindings());
    srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0,
                                                               QRhiShaderResourceBinding::VertexStage |
                                                                   QRhiShaderResourceBinding::FragmentStage,
                                                               ubuf.get()),
                      QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                                luma.get(), sampler.get()),
                      QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                                chroma.get(), sampler.get())});
    srb->create();
    std::unique_ptr<QRhiGraphicsPipeline> ps(rhi->newGraphicsPipeline());
    ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    ps->setShaderStages({{QRhiShaderStage::Vertex, loadShader("nv12_array.vert")},
                         {QRhiShaderStage::Fragment, loadShader("nv12_array.frag")}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    ps->setVertexInputLayout(layout);
    ps->setShaderResourceBindings(srb.get());
    ps->setRenderPassDescriptor(rpd.get());
    if (!ps->create()) {
        why = "pipeline creation failed";
        return false;
    }

    // Uniforms: corner -> clip, uv scale for the padded surface, left-sited chroma (as the compositor does).
    QMatrix4x4 m = rhi->clipSpaceCorrMatrix();
    m *= QMatrix4x4(2, 0, 0, -1, 0, -2, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1);
    float u[24];
    std::memcpy(u, m.constData(), 64);
    const float sx = static_cast<float>(width) / surface.width();
    const float sy = static_cast<float>(height) / surface.height();
    u[16] = sx;
    u[17] = sy;
    u[18] = 0;
    u[19] = 0;
    u[20] = sx; // chroma uv = uv * (W/2)/(Wc*... ) == luma scale for 4:2:0
    u[21] = sy;
    u[22] = 0.5f / static_cast<float>(surface.width()); // +0.5 luma px left siting, in chroma-uv units
    u[23] = 0.0f;

    QRhiReadbackResult readback;
    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
        why = "beginOffscreenFrame failed";
        return false;
    }
    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(vbuf.get(), quad);
    updates->updateDynamicBuffer(ubuf.get(), 0, sizeof u, u);
    cb->beginPass(rt.get(), Qt::black, {1.0f, 0}, updates);
    cb->setGraphicsPipeline(ps.get());
    cb->setViewport({0, 0, static_cast<float>(width), static_cast<float>(height)});
    cb->setShaderResources();
    const QRhiCommandBuffer::VertexInput vin(vbuf.get(), 0);
    cb->setVertexInput(0, 1, &vin);
    cb->draw(4);
    QRhiResourceUpdateBatch* rb = rhi->nextResourceUpdateBatch();
    rb->readBackTexture(QRhiReadbackDescription(target.get()), &readback);
    cb->endPass(rb);
    rhi->endOffscreenFrame();
    out.assign(reinterpret_cast<const std::uint8_t*>(readback.data.constData()),
               reinterpret_cast<const std::uint8_t*>(readback.data.constData()) + readback.data.size());
    return out.size() == static_cast<std::size_t>(width) * height * 4;
}

/// Decodes to frame `index` on (dev, ctx), proves the import, and times both transfer strategies.
bool tryRoute(const char* label, QRhi* rhi, ID3D11Device* dev, ID3D11DeviceContext* ctx,
              const std::string& clip, int index, const std::vector<std::uint8_t>& reference, int width,
              int height) {
    note("--- route: %s ---", label);
    // Decoder and renderer share the immediate context from different threads in production: serialize the
    // API.
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    Decoder decoder;
    if (!decoder.open(clip, dev, ctx)) {
        note("FAILED: %s", decoder.error.c_str());
        return false;
    }
    AVFrame* frame = nullptr;
    for (int i = 0; i <= index; ++i) {
        av_frame_free(&frame);
        frame = decoder.next();
        if (!frame) {
            note("FAILED: decode ended early");
            return false;
        }
    }
    if (frame->format != AV_PIX_FMT_D3D11) {
        note("FAILED: decoder did not output D3D11 surfaces (format %d)", frame->format);
        av_frame_free(&frame);
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    reinterpret_cast<ID3D11Texture2D*>(frame->data[0])->GetDesc(&desc);
    note("decoder surface: %ux%u NV12 array of %u, bind flags 0x%x, frame slice %d", desc.Width, desc.Height,
         desc.ArraySize, desc.BindFlags, static_cast<int>(reinterpret_cast<std::intptr_t>(frame->data[1])));

    std::vector<std::uint8_t> zero;
    double importMs = 0;
    std::string why;
    const bool ok = renderZeroCopy(rhi, frame, width, height, zero, importMs, why);
    if (!ok) {
        note("FAILED: %s", why.c_str());
        av_frame_free(&frame);
        return false;
    }
    const double quality = psnr(zero, reference, width, height);
    note("zero-copy render of frame %d vs CPU path (software decode + Compositor): PSNR %.2f dB; import (2 "
         "SRVs) %.3f ms",
         index, quality, importMs);
    av_frame_free(&frame);

    // Throughput: decode + hwdownload (today's path) vs decode + import only.
    double downloadMs = 0;
    double importOnlyMs = 0;
    int n = 0;
    AVFrame* cpu = av_frame_alloc();
    for (; n < 120; ++n) {
        AVFrame* f = decoder.next();
        if (!f) {
            break;
        }
        const auto a = std::chrono::steady_clock::now();
        av_hwframe_transfer_data(cpu, f, 0);
        const auto b = std::chrono::steady_clock::now();
        auto* t = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        std::unique_ptr<QRhiTexture> y(
            rhi->newTextureArray(QRhiTexture::R8, static_cast<int>(d.ArraySize),
                                 QSize(static_cast<int>(d.Width), static_cast<int>(d.Height))));
        y->setArrayRange(static_cast<int>(reinterpret_cast<std::intptr_t>(f->data[1])), 1);
        y->createFrom({reinterpret_cast<quint64>(t), 0});
        const auto c = std::chrono::steady_clock::now();
        downloadMs += std::chrono::duration<double, std::milli>(b - a).count();
        importOnlyMs += std::chrono::duration<double, std::milli>(c - b).count();
        av_frame_unref(cpu);
        av_frame_free(&f);
    }
    av_frame_free(&cpu);
    if (n > 0) {
        note("per frame over %d frames: hwdownload to CPU %.3f ms (plus the later CPU->GPU upload), "
             "zero-copy import "
             "%.3f ms",
             n, downloadMs / n, importOnlyMs / n);
    }
    note("RESULT: %s", quality >= 45.0 ? "PROVEN (same pixels, no CPU copy)" : "imported but pixels differ");
    return quality >= 45.0;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const std::string clip = (std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / "h264_1080p60_aac.mp4").string();
    if (!std::filesystem::exists(clip)) {
        std::puts("test media missing");
        return 77;
    }
    constexpr int kIndex = 37;
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;

    // CPU reference: software decode + the production Compositor, export transform Rec.709 (pass-through).
    std::vector<std::uint8_t> reference;
    {
        wabcut::media::DecoderOptions o;
        o.allowHardware = false;
        auto dec = wabcut::media::VideoDecoder::open(clip, -1, o);
        auto renderer = wabcut::render::OffscreenRenderer::create({});
        if (!dec || !renderer) {
            std::puts("cannot create the CPU reference path");
            return 1;
        }
        auto f = (*dec)->frameAt((*dec)->firstFrameTime() +
                                 wabcut::Ticks{kIndex * (wabcut::kTicksPerSecond / 60)});
        auto c = std::make_shared<wabcut::render::PreparedComposite>();
        c->canvas = {kWidth, kHeight};
        c->layers.push_back({.frame = *f, .opacity = 1.0});
        auto img = (*renderer)->render(c);
        if (!img) {
            std::puts("CPU reference render failed");
            return 1;
        }
        reference = std::move(img->rgba);
    }

    int proven = 0;
    // Route A: the QRhi's own D3D11 device (what QQuickWindow creates by default).
    {
        QRhiD3D11InitParams params;
        std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::D3D11, &params));
        if (!rhi) {
            std::puts("no D3D11 QRhi");
            return 77;
        }
        note("adapter: %s", rhi->driverInfo().deviceName.constData());
        const auto* h = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        proven +=
            tryRoute("QRhi-created device", rhi.get(), static_cast<ID3D11Device*>(h->dev),
                     static_cast<ID3D11DeviceContext*>(h->context), clip, kIndex, reference, kWidth, kHeight);
    }
    // Route B: an application-created device with video support, imported into QRhi.
    {
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                        D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
            QRhiD3D11InitParams params;
            QRhiD3D11NativeHandles import;
            import.dev = dev;
            import.context = ctx;
            std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::D3D11, &params, {}, &import));
            if (rhi) {
                proven += tryRoute("app-created VIDEO_SUPPORT device imported into QRhi", rhi.get(), dev, ctx,
                                   clip, kIndex, reference, kWidth, kHeight);
            } else {
                note("route B: QRhi::create with an imported device failed");
            }
            rhi.reset();
            ctx->Release();
            dev->Release();
        } else {
            note("route B: D3D11CreateDevice(VIDEO_SUPPORT) failed");
        }
    }

    QDir().mkpath(QStringLiteral(WABCUT_TEST_OUTPUT_DIR "/render"));
    QFile out(QStringLiteral(WABCUT_TEST_OUTPUT_DIR "/render/zero_copy_spike.txt"));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        out.write(QByteArray::fromStdString(g_report.str()));
    }
    return proven > 0 ? 0 : 1;
}
