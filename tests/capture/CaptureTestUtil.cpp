#include "CaptureTestUtil.h"

#include <wabcut/capture/FakeBackend.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <system_error>

namespace capturetest {

namespace {

struct FormatDeleter {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
};
struct CodecDeleter {
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
};
struct FrameDeleter {
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* p) const noexcept { sws_free_context(&p); }
};

std::int64_t barcodeOfGray(const AVFrame* gray) {
    std::int64_t value = 0;
    for (int bit = 0; bit < 16; ++bit) {
        const int x = bit * 16 + 8;
        const int y = 8;
        if (gray->data[0][static_cast<std::ptrdiff_t>(y) * gray->linesize[0] + x] >= 128) {
            value |= std::int64_t{1} << bit;
        }
    }
    return value;
}

} // namespace

std::filesystem::path freshOutputDir(const std::string& name) {
    std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "capture" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

Readback readCapture(const std::filesystem::path& file, bool withAudio) {
    Readback out;
    const std::string path = file.generic_string();
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) {
        out.error = "cannot open " + path;
        return out;
    }
    std::unique_ptr<AVFormatContext, FormatDeleter> in(raw);
    if (avformat_find_stream_info(in.get(), nullptr) < 0) {
        out.error = "no stream info";
        return out;
    }
    out.container = in->iformat->name;
    if (const AVDictionaryEntry* title = av_dict_get(in->metadata, "title", nullptr, 0)) {
        out.title = title->value;
    }
    int videoIndex = -1;
    int audioIndex = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVCodecParameters* par = in->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && videoIndex < 0) {
            videoIndex = static_cast<int>(i);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && audioIndex < 0) {
            audioIndex = static_cast<int>(i);
        }
    }
    if (videoIndex < 0) {
        out.error = "no video stream";
        return out;
    }

    auto open = [&](int index) -> std::unique_ptr<AVCodecContext, CodecDeleter> {
        const AVCodecParameters* par = in->streams[index]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            return nullptr;
        }
        std::unique_ptr<AVCodecContext, CodecDeleter> ctx(avcodec_alloc_context3(codec));
        if (!ctx || avcodec_parameters_to_context(ctx.get(), par) < 0 || avcodec_open2(ctx.get(), codec, nullptr) < 0) {
            return nullptr;
        }
        return ctx;
    };
    auto video = open(videoIndex);
    if (!video) {
        out.error = "cannot decode the video";
        return out;
    }
    std::unique_ptr<AVCodecContext, CodecDeleter> audio;
    if (audioIndex >= 0) {
        audio = open(audioIndex);
        if (!audio) {
            out.error = "cannot decode the audio";
            return out;
        }
        out.audioRate = audio->sample_rate;
        out.audioChannels = audio->ch_layout.nb_channels;
        out.audioCodec = avcodec_get_name(audio->codec_id);
    }
    out.videoCodec = avcodec_get_name(video->codec_id);
    out.width = video->width;
    out.height = video->height;
    const AVRational sar = in->streams[videoIndex]->sample_aspect_ratio;
    out.sarNum = sar.num;
    out.sarDen = sar.den;

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> gray(av_frame_alloc());
    std::unique_ptr<SwsContext, SwsDeleter> sws(sws_alloc_context());
    const AVRational tb = in->streams[videoIndex]->time_base;
    double frameDuration = 0.0;

    auto onVideoFrame = [&] {
        gray->format = AV_PIX_FMT_GRAY8;
        gray->width = frame->width;
        gray->height = frame->height;
        av_frame_get_buffer(gray.get(), 0);
        sws_scale_frame(sws.get(), gray.get(), frame.get());
        out.barcodes.push_back(barcodeOfGray(gray.get()));
        const std::int64_t stamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        out.times.push_back(static_cast<double>(stamp) * av_q2d(tb));
        av_frame_unref(gray.get());
    };
    auto drainVideo = [&] {
        while (avcodec_receive_frame(video.get(), frame.get()) >= 0) {
            onVideoFrame();
            av_frame_unref(frame.get());
        }
    };
    auto drainAudio = [&] {
        if (!audio) {
            return;
        }
        while (avcodec_receive_frame(audio.get(), frame.get()) >= 0) {
            out.audioFrames += frame->nb_samples;
            if (withAudio && frame->format == AV_SAMPLE_FMT_S16) {
                const auto* samples = reinterpret_cast<const std::int16_t*>(frame->data[0]);
                for (int i = 0; i < frame->nb_samples * out.audioChannels; ++i) {
                    out.audio.push_back(static_cast<float>(samples[i]) / 32768.0f);
                }
            }
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(in.get(), packet.get()) >= 0) {
        if (packet->stream_index == videoIndex) {
            if (avcodec_send_packet(video.get(), packet.get()) >= 0) {
                drainVideo();
            }
        } else if (audio && packet->stream_index == audioIndex) {
            if (avcodec_send_packet(audio.get(), packet.get()) >= 0) {
                drainAudio();
            }
        }
        av_packet_unref(packet.get());
    }
    avcodec_send_packet(video.get(), nullptr);
    drainVideo();
    if (audio) {
        avcodec_send_packet(audio.get(), nullptr);
        drainAudio();
    }
    if (out.times.size() > 1) {
        frameDuration = (out.times.back() - out.times.front()) / static_cast<double>(out.times.size() - 1);
    }
    out.videoSeconds = out.times.empty() ? 0.0 : out.times.back() + frameDuration;
    out.ok = true;
    return out;
}

double rms(const std::vector<float>& samples, int channels, std::int64_t from, std::int64_t to) {
    double sum = 0.0;
    std::int64_t count = 0;
    const auto frames = static_cast<std::int64_t>(samples.size()) / std::max(channels, 1);
    for (std::int64_t f = std::max<std::int64_t>(from, 0); f < std::min(to, frames); ++f) {
        const double v = samples[static_cast<std::size_t>(f) * static_cast<std::size_t>(channels)];
        sum += v * v;
        ++count;
    }
    return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

capture::CaptureFrame fakeFrame(int width, int height, std::int64_t index, std::int64_t hostNs) {
    return capture::makeFrame(capture::PixelFormat::Nv12, width, height, hostNs, [&](std::uint8_t* bytes) {
        capture::drawFakePicture(capture::PixelFormat::Nv12, width, height, index, bytes);
    });
}

capture::AudioBlock toneBlock(int rate, int channels, std::int64_t firstFrame, std::int64_t frames, std::int64_t hostNs) {
    capture::AudioBlock block;
    block.channels = channels;
    block.firstFrame = firstFrame;
    block.hostNs = hostNs;
    block.samples.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    for (std::int64_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(firstFrame + f) / rate;
        const float v = 0.25f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * t));
        for (int c = 0; c < channels; ++c) {
            block.samples[static_cast<std::size_t>(f) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = v;
        }
    }
    return block;
}

capture::RecorderConfig smallConfig(const std::filesystem::path& file, Rate fps, bool audio) {
    capture::RecorderConfig config;
    config.file = file;
    config.video.width = 320;
    config.video.height = 180;
    config.video.frameRate = fps;
    config.video.pixel = capture::PixelFormat::Nv12;
    config.encoder = "libx264";
    config.maxQueuedFrames = 4096; // the tests push pictures far faster than a device does
    config.audio = audio;
    config.audioRate = 48000;
    config.audioChannels = 2;
    config.title = "Test capture";
    config.provenance = "test provenance";
    return config;
}

} // namespace capturetest
