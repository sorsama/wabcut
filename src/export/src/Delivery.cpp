#include "Delivery.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace wabcut::exporter {

std::string_view captionStandardName(CaptionStandard standard) {
    switch (standard) {
    case CaptionStandard::Cea608And708:
        return "608+708";
    case CaptionStandard::Cea608:
        return "608";
    case CaptionStandard::Cea708:
        return "708";
    }
    return "608+708";
}

std::optional<CaptionStandard> captionStandardFromName(std::string_view name) {
    for (const CaptionStandard s : {CaptionStandard::Cea608And708, CaptionStandard::Cea608, CaptionStandard::Cea708}) {
        if (captionStandardName(s) == name) {
            return s;
        }
    }
    return std::nullopt;
}

std::string_view captionCarriageName(CaptionCarriage carriage) {
    switch (carriage) {
    case CaptionCarriage::Auto:
        return "auto";
    case CaptionCarriage::VideoStream:
        return "video";
    case CaptionCarriage::CaptionTrack:
        return "track";
    case CaptionCarriage::Both:
        return "both";
    }
    return "auto";
}

std::optional<CaptionCarriage> captionCarriageFromName(std::string_view name) {
    for (const CaptionCarriage c : {CaptionCarriage::Auto, CaptionCarriage::VideoStream, CaptionCarriage::CaptionTrack,
                                    CaptionCarriage::Both}) {
        if (captionCarriageName(c) == name) {
            return c;
        }
    }
    return std::nullopt;
}

std::string_view mxfAudioLayoutName(MxfAudioLayout layout) {
    return layout == MxfAudioLayout::OneTrack ? "multichannel" : "mono";
}

std::optional<MxfAudioLayout> mxfAudioLayoutFromName(std::string_view name) {
    if (name == "mono") {
        return MxfAudioLayout::MonoTracks;
    }
    if (name == "multichannel") {
        return MxfAudioLayout::OneTrack;
    }
    return std::nullopt;
}

bool isMxf(Container container) {
    return container == Container::Mxf || container == Container::MxfOpAtom;
}

namespace {

Error invalid(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message)};
}

/// The picture codecs that carry ATSC A/53 caption data in their own stream (H.264 / HEVC SEI, MPEG-2 user data).
/// AVC-Intra does not: its SMPTE RP 2027 stream has a fixed SEI set.
bool videoCarriesCaptions(VideoCodec codec) {
    return codec == VideoCodec::H264 || codec == VideoCodec::Hevc || codec == VideoCodec::Xdcam;
}

std::string_view codecLabel(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return "H.264";
    case VideoCodec::Hevc:
        return "HEVC";
    case VideoCodec::Xdcam:
        return "XDCAM HD422 (MPEG-2)";
    case VideoCodec::AvcIntra:
        return "AVC-Intra 100";
    case VideoCodec::ProRes:
        return "ProRes";
    case VideoCodec::DnxHR:
        return "DNxHD / DNxHR";
    case VideoCodec::Av1:
        return "AV1";
    case VideoCodec::Vp9:
        return "VP9";
    default:
        return videoCodecName(codec);
    }
}

std::string_view containerLabel(Container container) {
    switch (container) {
    case Container::Mp4:
        return "MP4";
    case Container::Mov:
        return "QuickTime";
    case Container::Mkv:
        return "MKV";
    case Container::Mxf:
        return "MXF OP1a";
    case Container::MxfOpAtom:
        return "MXF OP-Atom";
    default:
        return containerName(container);
    }
}

bool sameRate(Rate a, Rate b) {
    return a.isValid() && b.isValid() && a == b;
}

bool isMxfRate(Rate r) {
    for (const Rate ok : {kRate23_976, kRate24, kRate25, kRate29_97, kRate30, kRate50, kRate59_94, kRate60}) {
        if (sameRate(r, ok)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::expected<EmbeddedCaptionPlan, Error> embeddedCaptionPlan(const ExportSettings& s) {
    const CaptionExportSettings& c = s.captions;
    EmbeddedCaptionPlan plan;
    plan.cea608 = c.standard != CaptionStandard::Cea708;
    plan.cea708 = c.standard != CaptionStandard::Cea608;
    if (c.channel608 < 1 || c.channel608 > 4) {
        return std::unexpected(invalid("The 608 channel is CC1, CC2, CC3 or CC4"));
    }
    if (c.service708 < 1 || c.service708 > 63) {
        return std::unexpected(invalid("The 708 service is 1 to 63"));
    }
    if (!s.video.enabled) {
        return std::unexpected(invalid("Embedded captions need video: choose a sidecar file instead"));
    }
    const Container container = s.container;
    const VideoCodec codec = s.video.codec;
    if (container == Container::MxfOpAtom) {
        return std::unexpected(invalid("MXF OP-Atom files hold one picture or sound each, so they carry no captions: write "
                                       "an SCC or MCC sidecar (Media Composer imports them), or export MXF OP1a, which "
                                       "carries them in an SMPTE 436M track"));
    }
    const bool hasTracks = container == Container::Mxf || container == Container::Mov;
    if (!hasTracks && container != Container::Mp4 && container != Container::Mkv) {
        return std::unexpected(invalid(std::format("{} files cannot carry embedded captions: choose MP4, MOV, MKV or MXF, a "
                                                   "sidecar file or burn-in",
                                                   containerLabel(container))));
    }
    const bool videoOk = videoCarriesCaptions(codec);
    // A QuickTime caption track (c608) holds field 1 of CEA-608: CC1 or CC2.
    const bool c608Ok = plan.cea608 && c.channel608 <= 2;
    switch (c.carriage) {
    case CaptionCarriage::Auto:
        if (container == Container::Mxf) {
            plan.track = true;
        } else if (container == Container::Mov) {
            // The picture stream when it can carry them (and the c608 track for CC1 / CC2), else the c608 track alone.
            plan.inVideo = videoOk;
            plan.track = !videoOk || c608Ok;
        } else {
            plan.inVideo = true;
        }
        break;
    case CaptionCarriage::VideoStream:
        plan.inVideo = true;
        break;
    case CaptionCarriage::CaptionTrack:
        plan.track = true;
        break;
    case CaptionCarriage::Both:
        plan.inVideo = true;
        plan.track = true;
        break;
    }
    if (plan.inVideo && !videoOk) {
        return std::unexpected(invalid(std::format(
            "{} video cannot carry captions in its stream (H.264, HEVC and XDCAM HD422 can){}", codecLabel(codec),
            hasTracks ? ": put them in the caption track" : ": choose a sidecar file or burn them in")));
    }
    if (plan.track && !hasTracks) {
        return std::unexpected(invalid(std::format("{} has no caption track (MXF and QuickTime have): carry the captions in "
                                                   "the video stream",
                                                   containerLabel(container))));
    }
    if (plan.track && container == Container::Mov) {
        if (!plan.cea608) {
            return std::unexpected(invalid("A QuickTime caption track holds CEA-608 (c608): choose 608, or carry 708 in the "
                                           "H.264 / HEVC stream"));
        }
        if (c.channel608 > 2) {
            return std::unexpected(invalid("A QuickTime caption track carries CC1 or CC2 (field 1): choose CC1 or CC2, or "
                                           "carry CC3 / CC4 in the H.264 / HEVC stream"));
        }
    }
    if (!plan.inVideo && !plan.track) {
        return std::unexpected(invalid(std::format("{} in {} cannot carry embedded captions: choose H.264 or HEVC video, a "
                                                   "sidecar file or burn-in",
                                                   codecLabel(codec), containerLabel(container))));
    }
    // CEA-708 rides in the picture stream or in an MXF ANC track; QuickTime c708 tracks are not written.
    if (plan.cea708 && !plan.inVideo && !(plan.track && container == Container::Mxf)) {
        return std::unexpected(invalid(std::format("{} in {} cannot carry CEA-708 captions (they need H.264, HEVC or XDCAM "
                                                   "HD422 video, or an MXF SMPTE 436M track; QuickTime c708 tracks are not "
                                                   "written): choose 608 only",
                                                   codecLabel(codec), containerLabel(container))));
    }
    return plan;
}

std::string describeEmbeddedCaptions(const ExportSettings& s, const EmbeddedCaptionPlan& plan) {
    std::string what;
    if (plan.cea608) {
        what = std::format("CEA-608 CC{}", static_cast<int>(s.captions.channel608));
    }
    if (plan.cea708) {
        what += std::format("{}CEA-708 service {}", what.empty() ? "" : " + ", static_cast<int>(s.captions.service708));
    }
    std::string where;
    if (plan.inVideo) {
        where = std::format("in the {} stream", s.video.codec == VideoCodec::Xdcam ? std::string_view("MPEG-2")
                                                                                      : codecLabel(s.video.codec));
    }
    if (plan.track) {
        where += std::string(where.empty() ? "in " : " and ") +
                 (s.container == Container::Mxf ? "an SMPTE 436M track" : "a QuickTime c608 track");
    }
    return what + " " + where;
}

namespace detail {

int xdcamGopLength(Rate rate) {
    const long fps = std::lround(rate.toDouble());
    return fps == 30 || fps == 60 ? 15 : 12;
}

std::expected<void, Error> validateDelivery(const ExportSettings& s) {
    const VideoSettings& v = s.video;
    const AudioSettings& a = s.audio;
    const bool broadcastCodec = v.enabled && (v.codec == VideoCodec::Xdcam || v.codec == VideoCodec::AvcIntra);
    if (broadcastCodec && !isMxf(s.container) && s.container != Container::Mov) {
        return std::unexpected(invalid("XDCAM HD422 and AVC-Intra 100 go into MXF or QuickTime (.mov)"));
    }
    if (v.enabled && v.codec == VideoCodec::Xdcam && v.pixelFormat != OutputPixelFormat::Yuv422p) {
        return std::unexpected(invalid("XDCAM HD422 is 8-bit 4:2:2 (yuv422p)"));
    }
    if (v.enabled && v.codec == VideoCodec::AvcIntra && v.pixelFormat != OutputPixelFormat::Yuv422p10) {
        return std::unexpected(invalid("AVC-Intra 100 is 10-bit 4:2:2 (yuv422p10)"));
    }
    if (isMxf(s.container)) {
        const bool opAtom = s.container == Container::MxfOpAtom;
        if (!v.enabled) {
            return std::unexpected(invalid("MXF exports need video; for audio only, export WAV (Broadcast WAV)"));
        }
        if (opAtom && v.codec != VideoCodec::DnxHR) {
            return std::unexpected(invalid("MXF OP-Atom (Avid MediaFiles) holds DNxHD or DNxHR video"));
        }
        if (!opAtom && v.codec != VideoCodec::DnxHR && v.codec != VideoCodec::Xdcam && v.codec != VideoCodec::AvcIntra) {
            return std::unexpected(invalid("MXF OP1a holds DNxHD / DNxHR, XDCAM HD422 or AVC-Intra 100 video"));
        }
        if (a.enabled && ((a.codec != AudioCodec::Pcm16 && a.codec != AudioCodec::Pcm24) || a.sampleRate != 48000)) {
            return std::unexpected(invalid("MXF audio is 16 or 24-bit PCM at 48 kHz"));
        }
        if (s.subtitles.enabled) {
            return std::unexpected(invalid("MXF has no soft subtitle track: embed the captions (an SMPTE 436M track) or "
                                           "write a sidecar file"));
        }
        if (s.mxf.reelName.size() > 64 ||
            std::any_of(s.mxf.reelName.begin(), s.mxf.reelName.end(), [](char ch) {
                return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F;
            })) {
            return std::unexpected(invalid("A reel name is at most 64 bytes of text on one line"));
        }
    }
    if (s.captions.mode == CaptionExport::Embed) {
        if (auto plan = embeddedCaptionPlan(s); !plan) {
            return std::unexpected(plan.error());
        }
    }
    return {};
}

std::optional<std::string> deliveryProblem(const ExportSettings& s, const ResolvedExport& r) {
    if (!s.video.enabled) {
        return std::nullopt;
    }
    const Rate rate = r.frameRate;
    const double fps = rate.toDouble();
    if (isMxf(s.container) && !isMxfRate(rate)) {
        return std::format("MXF is written at 23.976, 24, 25, 29.97, 30, 50, 59.94 or 60 fps (this export is {:.3g} fps): "
                           "set the frame rate",
                           fps);
    }
    if (s.video.codec == VideoCodec::Xdcam) {
        const bool hd1080 = r.width == 1920 && r.height == 1080 && fps < 31.0;
        const bool hd720 = r.width == 1280 && r.height == 720;
        if (!(hd1080 || hd720) || !isMxfRate(rate)) {
            return std::format("XDCAM HD422 is 1920×1080 at 23.976 to 30 fps or 1280×720 at 23.976 to 60 fps (this export "
                               "is {}×{} at {:.3g} fps)",
                               r.width, r.height, fps);
        }
    }
    if (s.video.codec == VideoCodec::AvcIntra) {
        const bool hd1080 = r.width == 1920 && r.height == 1080 && (sameRate(rate, kRate25) || sameRate(rate, kRate29_97));
        const bool hd720 = r.width == 1280 && r.height == 720 && (sameRate(rate, kRate50) || sameRate(rate, kRate59_94));
        if (!hd1080 && !hd720) {
            return std::format("AVC-Intra 100 is 1920×1080 at 25 or 29.97 fps or 1280×720 at 50 or 59.94 fps (SMPTE RP "
                               "2027; this export is {}×{} at {:.3g} fps)",
                               r.width, r.height, fps);
        }
    }
    return std::nullopt;
}

} // namespace detail

} // namespace wabcut::exporter
