#include "DynamicHdrInternal.h"

#include "FfmpegUtil.h"

#include <wabcut/media/DynamicHdr.h>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/hdr_dynamic_metadata.h>
}

#include <algorithm>
#include <cmath>

namespace wabcut::media {

namespace {

// SMPTE ST 2084 constants.
constexpr double kM1 = 2610.0 / 16384.0;
constexpr double kM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kC1 = 3424.0 / 4096.0;
constexpr double kC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kC3 = 2392.0 / 4096.0 * 32.0;

} // namespace

double pqToNits(unsigned code, int bits) {
    const double maxCode = static_cast<double>((1u << std::clamp(bits, 1, 16)) - 1u);
    const double e = std::clamp(static_cast<double>(code) / maxCode, 0.0, 1.0);
    const double p = std::pow(e, 1.0 / kM2);
    const double numerator = std::max(p - kC1, 0.0);
    const double denominator = kC2 - kC3 * p;
    return denominator <= 0.0 ? 10000.0 : 10000.0 * std::pow(numerator / denominator, 1.0 / kM1);
}

unsigned nitsToPq(double nits, int bits) {
    const double maxCode = static_cast<double>((1u << std::clamp(bits, 1, 16)) - 1u);
    const double y = std::clamp(nits / 10000.0, 0.0, 1.0);
    const double yp = std::pow(y, kM1);
    const double e = std::pow((kC1 + kC2 * yp) / (1.0 + kC3 * yp), kM2);
    return static_cast<unsigned>(std::lround(std::clamp(e, 0.0, 1.0) * maxCode));
}

namespace detail {

std::optional<DolbyVisionInfo> dolbyVisionConfigOf(const AVCodecParameters* par) {
    if (!par) {
        return std::nullopt;
    }
    const AVPacketSideData* sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    if (!sd || !sd->data || sd->size < sizeof(AVDOVIDecoderConfigurationRecord)) {
        return std::nullopt;
    }
    const auto* record = reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(sd->data);
    DolbyVisionInfo info;
    info.profile = record->dv_profile;
    info.level = record->dv_level;
    info.blCompatibilityId = record->dv_bl_signal_compatibility_id;
    info.rpuPresent = record->rpu_present_flag != 0;
    info.elPresent = record->el_present_flag != 0;
    info.blPresent = record->bl_present_flag != 0;
    // Untrusted: a profile or a compatibility id past what the format defines is a damaged box.
    if (info.profile == 0 || info.blCompatibilityId > 15) {
        return std::nullopt;
    }
    return info;
}

std::expected<DynamicHdrReport, Error> scanDynamicHdrIn(AVFormatContext* fmt, AVStream* stream, const std::string& source,
                                                        int maxFrames, std::stop_token stop) {
    DynamicHdrReport report;
    report.dolbyVision = dolbyVisionConfigOf(stream->codecpar);
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        return std::unexpected(Error{ErrorCode::Unsupported, "No decoder for " + source});
    }
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx || avcodec_parameters_to_context(ctx.get(), stream->codecpar) < 0) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Cannot set up the decoder for " + source});
    }
    // The HEVC decoder parses the Dolby Vision RPU NAL units only when asked to export the "enhancements".
    ctx->export_side_data |= AV_CODEC_EXPORT_DATA_ENHANCEMENTS;
    ctx->thread_count = 2;
    if (const int rc = avcodec_open2(ctx.get(), codec, nullptr); rc < 0) {
        return std::unexpected(ffmpegError(ErrorCode::Unsupported, "Cannot open the decoder", rc));
    }
    DecodeLoop loop;
    FramePtr frame(av_frame_alloc());
    if (!frame || !loop.isValid()) {
        return std::unexpected(Error{ErrorCode::Unknown, "Out of memory"});
    }
    double averageSum = 0.0;
    double rpuMeanSum = 0.0;
    double rpuMin = 0.0;
    bool haveRpuMin = false;
    while (report.framesScanned < std::max(1, maxFrames)) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Cancelled"});
        }
        const auto got = loop.receive(fmt, ctx.get(), stream->index, frame.get(), source);
        if (!got) {
            return std::unexpected(got.error());
        }
        if (!*got) {
            break;
        }
        ++report.framesScanned;
        if (const AVFrameSideData* sd = av_frame_get_side_data(frame.get(), AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
            sd && sd->data && sd->size >= sizeof(AVDynamicHDRPlus)) {
            const auto* hdr = reinterpret_cast<const AVDynamicHDRPlus*>(sd->data);
            const AVHDRPlusColorTransformParams& p = hdr->params[0];
            if (report.hdr10PlusFrames == 0) {
                report.hdr10PlusApplicationVersion = hdr->application_version;
                report.targetedDisplayNits = av_q2d(hdr->targeted_system_display_maximum_luminance);
            }
            ++report.hdr10PlusFrames;
            for (const AVRational& m : p.maxscl) {
                report.hdr10PlusPeakNits = std::max(report.hdr10PlusPeakNits, av_q2d(m) * 10000.0);
            }
            averageSum += av_q2d(p.average_maxrgb) * 10000.0;
            report.hdr10PlusToneCurve = report.hdr10PlusToneCurve || p.tone_mapping_flag != 0;
        }
        if (const AVFrameSideData* sd = av_frame_get_side_data(frame.get(), AV_FRAME_DATA_DOVI_METADATA);
            sd && sd->data && sd->size >= sizeof(AVDOVIMetadata)) {
            const auto* rpu = reinterpret_cast<const AVDOVIMetadata*>(sd->data);
            if (report.rpuFrames == 0) {
                report.rpuProfile = av_dovi_get_header(rpu)->vdr_rpu_profile;
                for (int i = 0; i < rpu->num_ext_blocks && i < AV_DOVI_MAX_EXT_BLOCKS; ++i) {
                    const uint8_t level = av_dovi_get_ext(rpu, i)->level;
                    report.rpuLevel2Blocks += (level == 2 || level == 8) ? 1 : 0;
                }
            }
            ++report.rpuFrames;
            if (const AVDOVIDmData* l1 = av_dovi_find_level(rpu, 1)) {
                report.rpuLevel1 = true;
                const double minNits = pqToNits(l1->l1.min_pq, 12);
                report.rpuMaxNits = std::max(report.rpuMaxNits, pqToNits(l1->l1.max_pq, 12));
                rpuMin = haveRpuMin ? std::min(rpuMin, minNits) : minNits;
                haveRpuMin = true;
                rpuMeanSum += pqToNits(l1->l1.avg_pq, 12);
            }
        }
        av_frame_unref(frame.get());
    }
    if (report.hdr10PlusFrames > 0) {
        report.hdr10PlusMeanAverageNits = averageSum / report.hdr10PlusFrames;
    }
    if (report.rpuLevel1 && report.rpuFrames > 0) {
        report.rpuMinNits = rpuMin;
        report.rpuMeanNits = rpuMeanSum / report.rpuFrames;
    }
    return report;
}

} // namespace detail

std::expected<std::optional<DolbyVisionInfo>, Error> readDolbyVisionConfig(const std::string& utf8Path, int streamIndex) {
    auto opened = detail::openInput(utf8Path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto stream = detail::selectStream(opened->get(), AVMEDIA_TYPE_VIDEO, streamIndex);
    if (!stream) {
        return std::unexpected(stream.error());
    }
    return detail::dolbyVisionConfigOf((*stream)->codecpar);
}

std::expected<DynamicHdrReport, Error> scanDynamicHdr(const std::string& utf8Path, int streamIndex, int maxFrames,
                                                      std::stop_token stop) {
    auto opened = detail::openInput(utf8Path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto stream = detail::selectStream(opened->get(), AVMEDIA_TYPE_VIDEO, streamIndex);
    if (!stream) {
        return std::unexpected(stream.error());
    }
    return detail::scanDynamicHdrIn(opened->get(), *stream, utf8Path, maxFrames, std::move(stop));
}

} // namespace wabcut::media
