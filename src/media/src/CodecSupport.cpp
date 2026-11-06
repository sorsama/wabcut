#include <wabcut/media/CodecSupport.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <initializer_list>
#include <memory>

namespace wabcut::media {

namespace {

struct ContextDeleter {
    void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};

/// The pixel formats an encoder lists (empty: it does not say).
std::vector<AVPixelFormat> pixelFormatsOf(const AVCodec* codec) {
    std::vector<AVPixelFormat> out;
    std::unique_ptr<AVCodecContext, ContextDeleter> ctx(avcodec_alloc_context3(codec));
    const void* config = nullptr;
    int count = 0;
    if (ctx && avcodec_get_supported_config(ctx.get(), codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &config, &count) >= 0 && config) {
        const auto* formats = static_cast<const AVPixelFormat*>(config);
        out.assign(formats, formats + count);
    }
    return out;
}

bool takesAny(const std::vector<AVPixelFormat>& formats, std::initializer_list<AVPixelFormat> wanted) {
    return std::any_of(wanted.begin(), wanted.end(), [&](AVPixelFormat f) {
        return std::find(formats.begin(), formats.end(), f) != formats.end();
    });
}

/// The name of the first encoder of `codec` that takes one of `formats` ("" when none does), among the encoders this
/// FFmpeg has (hardware ones included: they are listed, whether the machine has the device or not).
std::string firstEncoderTaking(AVCodecID codec, std::initializer_list<AVPixelFormat> formats) {
    void* iterator = nullptr;
    while (const AVCodec* c = av_codec_iterate(&iterator)) {
        if (c->id == codec && av_codec_is_encoder(c) && takesAny(pixelFormatsOf(c), formats)) {
            return c->name;
        }
    }
    return {};
}

std::string decoderName(AVCodecID id) {
    // The decoder libavcodec picks first for the codec (dav1d before the hardware-only "av1" for AV1).
    if (id == AV_CODEC_ID_AV1) {
        if (const AVCodec* d = avcodec_find_decoder_by_name("libdav1d")) {
            return d->name;
        }
    }
    const AVCodec* d = avcodec_find_decoder(id);
    return d ? d->name : std::string();
}

} // namespace

std::vector<CodecSupport> newerCodecSupport() {
    std::vector<CodecSupport> out;

    // APV (Advanced Professional Video, RFC 9924): FFmpeg 8 and newer have a native decoder; the encoder is the external
    // OpenAPV library (libopenapv), which this build does not include.
    {
        CodecSupport apv;
        apv.id = "apv";
        if (const AVCodec* d = avcodec_find_decoder_by_name("apv")) {
            apv.decoder = d->name;
        }
        for (const char* name : {"apv", "libopenapv"}) {
            if (const AVCodec* e = avcodec_find_encoder_by_name(name); e && av_codec_is_encoder(e)) {
                apv.encoder = e->name;
                break;
            }
        }
        out.push_back(std::move(apv));
    }

    // HEVC in 4:2:2: the native decoder reads 8, 10 and 12 bits; an encoder that takes 4:2:2 8-bit exports it (libx265).
    {
        CodecSupport hevc;
        hevc.id = "hevc-422";
        hevc.decoder = decoderName(AV_CODEC_ID_HEVC);
        hevc.encoder = firstEncoderTaking(AV_CODEC_ID_HEVC, {AV_PIX_FMT_YUV422P, AV_PIX_FMT_NV16});
        // 10-bit 4:2:2 is exported only by a software encoder with 10-bit 4:2:2 input (libx265 built with 10-bit support).
        std::string software10;
        void* iterator = nullptr;
        while (const AVCodec* c = av_codec_iterate(&iterator)) {
            if (c->id == AV_CODEC_ID_HEVC && av_codec_is_encoder(c) && std::string_view(c->name).starts_with("lib") &&
                takesAny(pixelFormatsOf(c), {AV_PIX_FMT_YUV422P10LE})) {
                software10 = c->name;
            }
        }
        hevc.encoderEightBitOnly = software10.empty();
        out.push_back(std::move(hevc));
    }

    // AV1 in 4:2:2: dav1d decodes the Professional profile; the encoders (SVT-AV1, the hardware ones) are 4:2:0.
    {
        CodecSupport av1;
        av1.id = "av1-422";
        av1.decoder = decoderName(AV_CODEC_ID_AV1);
        av1.encoder = firstEncoderTaking(AV_CODEC_ID_AV1, {AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV422P10LE});
        out.push_back(std::move(av1));
    }

    // MV-HEVC (spatial video): FFmpeg's HEVC decoder has the view_ids option that selects the views it outputs; by default
    // it outputs the base view, which is what Wabcut shows. There is no encoder.
    {
        CodecSupport mv;
        mv.id = "mv-hevc";
        if (const AVCodec* d = avcodec_find_decoder(AV_CODEC_ID_HEVC); d && d->priv_class) {
            const AVClass* cls = d->priv_class;
            if (av_opt_find2(&cls, "view_ids", nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ, nullptr)) {
                mv.decoder = d->name;
                mv.baseViewOnly = true;
            }
        }
        out.push_back(std::move(mv));
    }
    return out;
}

std::optional<CodecSupport> codecSupportFor(std::string_view codec, std::string_view chroma) {
    const char* id = nullptr;
    if (codec == "apv") {
        id = "apv";
    } else if (codec == "hevc" && chroma == "4:2:2") {
        id = "hevc-422";
    } else if (codec == "av1" && chroma == "4:2:2") {
        id = "av1-422";
    }
    if (!id) {
        return std::nullopt;
    }
    for (CodecSupport& entry : newerCodecSupport()) {
        if (entry.id == id) {
            return std::move(entry);
        }
    }
    return std::nullopt;
}

} // namespace wabcut::media
