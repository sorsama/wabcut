#include "LongGop.h"

#include <algorithm>
#include <cstdio>
#include <format>

namespace wabcut::exporter::detail {

namespace {

// H.264 NAL unit types (ITU-T H.264 table 7-1) and HEVC's (ITU-T H.265 table 7-1).
constexpr int kH264Idr = 5;
constexpr int kH264Sps = 7;
constexpr int kH264Pps = 8;
constexpr int kHevcIdrWRadl = 19;
constexpr int kHevcIdrNLp = 20;
constexpr int kHevcVps = 32;
constexpr int kHevcSps = 33;
constexpr int kHevcPps = 34;

int nalTypeOf(AVCodecID codec, std::uint8_t first) {
    return codec == AV_CODEC_ID_H264 ? first & 0x1f : (first >> 1) & 0x3f;
}

bool isParameterSetType(AVCodecID codec, int type) {
    return codec == AV_CODEC_ID_H264 ? (type == kH264Sps || type == kH264Pps)
                                     : (type == kHevcVps || type == kHevcSps || type == kHevcPps);
}

bool isIdrType(AVCodecID codec, int type) {
    return codec == AV_CODEC_ID_H264 ? type == kH264Idr : (type == kHevcIdrWRadl || type == kHevcIdrNLp);
}

std::uint32_t readBe(std::span<const std::uint8_t> data, std::size_t at, int bytes) {
    std::uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v = (v << 8) | data[at + static_cast<std::size_t>(i)];
    }
    return v;
}

/// The NAL units of an Annex B stream (start codes 00 00 01 / 00 00 00 01).
std::vector<NalUnit> annexBNals(std::span<const std::uint8_t> data) {
    std::vector<NalUnit> out;
    std::size_t i = 0;
    std::size_t begin = std::string::npos;
    const auto flush = [&](std::size_t end) {
        if (begin == std::string::npos) {
            return;
        }
        while (end > begin && data[end - 1] == 0) {
            --end; // trailing_zero_8bits / the zero_byte of the next four-byte start code
        }
        if (end > begin) {
            out.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(begin), data.begin() + static_cast<std::ptrdiff_t>(end));
        }
    };
    while (i + 2 < data.size()) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            flush(i);
            begin = i + 3;
            i += 3;
        } else {
            ++i;
        }
    }
    flush(data.size());
    return out;
}

/// The raw byte sequence of a NAL unit: the emulation prevention bytes (00 00 03) taken out.
std::vector<std::uint8_t> unescape(const NalUnit& nal) {
    std::vector<std::uint8_t> out;
    out.reserve(nal.size());
    int zeros = 0;
    for (const std::uint8_t b : nal) {
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        zeros = b == 0 ? zeros + 1 : 0;
        out.push_back(b);
    }
    return out;
}

} // namespace

bool isLongGopCodec(AVCodecID id) {
    return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC;
}

std::vector<NalUnit> parameterSets(AVCodecID codec, std::span<const std::uint8_t> extradata) {
    std::vector<NalUnit> out;
    if (!isLongGopCodec(codec) || extradata.size() < 7) {
        return out;
    }
    const auto keep = [&](std::span<const std::uint8_t> nal) {
        if (!nal.empty() && isParameterSetType(codec, nalTypeOf(codec, nal[0]))) {
            out.emplace_back(nal.begin(), nal.end());
        }
    };
    if (extradata[0] == 1 && codec == AV_CODEC_ID_H264) {
        // avcC: version, profile, compat, level, 6 bits reserved + length size - 1, 3 bits reserved + SPS count, SPSs,
        // PPS count, PPSs.
        std::size_t at = 5;
        const int spsCount = extradata[at++] & 0x1f;
        for (int i = 0; i < spsCount; ++i) {
            if (at + 2 > extradata.size()) {
                return {};
            }
            const std::size_t n = readBe(extradata, at, 2);
            at += 2;
            if (at + n > extradata.size()) {
                return {};
            }
            keep(extradata.subspan(at, n));
            at += n;
        }
        if (at >= extradata.size()) {
            return {};
        }
        const int ppsCount = extradata[at++];
        for (int i = 0; i < ppsCount; ++i) {
            if (at + 2 > extradata.size()) {
                return {};
            }
            const std::size_t n = readBe(extradata, at, 2);
            at += 2;
            if (at + n > extradata.size()) {
                return {};
            }
            keep(extradata.subspan(at, n));
            at += n;
        }
    } else if (extradata[0] == 1 && codec == AV_CODEC_ID_HEVC) {
        // hvcC: 22 bytes of profile and level, then the number of arrays; each: type, count (2), {length (2), NAL}.
        if (extradata.size() < 23) {
            return {};
        }
        std::size_t at = 22;
        const int arrays = extradata[at++];
        for (int a = 0; a < arrays; ++a) {
            if (at + 3 > extradata.size()) {
                return {};
            }
            at += 1; // completeness flag + NAL unit type
            const int count = static_cast<int>(readBe(extradata, at, 2));
            at += 2;
            for (int i = 0; i < count; ++i) {
                if (at + 2 > extradata.size()) {
                    return {};
                }
                const std::size_t n = readBe(extradata, at, 2);
                at += 2;
                if (at + n > extradata.size()) {
                    return {};
                }
                keep(extradata.subspan(at, n));
                at += n;
            }
        }
    } else {
        for (const NalUnit& nal : annexBNals(extradata)) {
            keep(nal);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int nalLengthSize(AVCodecID codec, std::span<const std::uint8_t> extradata) {
    if (!isLongGopCodec(codec) || extradata.size() < 7 || extradata[0] != 1) {
        return 0;
    }
    // avcC byte 4 and hvcC byte 21 (lengthSizeMinusOne in the low two bits).
    const std::size_t at = codec == AV_CODEC_ID_H264 ? 4 : 21;
    if (at >= extradata.size()) {
        return 0;
    }
    return (extradata[at] & 3) + 1;
}

std::vector<std::uint8_t> toAnnexB(int lengthSize, std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out;
    if (lengthSize < 1 || lengthSize > 4) {
        return out;
    }
    const std::size_t prefix = static_cast<std::size_t>(lengthSize);
    out.reserve(data.size() + 16);
    std::size_t at = 0;
    while (at + prefix <= data.size()) {
        const std::size_t n = readBe(data, at, lengthSize);
        at += prefix;
        if (n == 0) {
            continue;
        }
        if (at + n > data.size()) {
            return {};
        }
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    if (at != data.size()) {
        return {};
    }
    return out;
}

PacketNals inspectPacket(AVCodecID codec, int lengthSize, std::span<const std::uint8_t> data) {
    PacketNals out;
    if (lengthSize < 1 || lengthSize > 4) {
        out.bad = true;
        return out;
    }
    std::size_t at = 0;
    const std::size_t prefix = static_cast<std::size_t>(lengthSize);
    while (at + prefix <= data.size()) {
        const std::size_t n = readBe(data, at, lengthSize);
        at += prefix;
        if (n == 0) {
            continue;
        }
        if (at + n > data.size()) {
            out.bad = true;
            return out;
        }
        const int type = nalTypeOf(codec, data[at]);
        if (isIdrType(codec, type)) {
            out.idr = true;
        } else if (isParameterSetType(codec, type)) {
            out.parameterSets = true;
            out.sets.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(at),
                                  data.begin() + static_cast<std::ptrdiff_t>(at + n));
        }
        at += n;
    }
    if (at != data.size()) {
        out.bad = true;
    }
    return out;
}

std::string describeHeaderDifference(AVCodecID codec, const std::vector<NalUnit>& source, const std::vector<NalUnit>& exported) {
    const auto sps = [&](const std::vector<NalUnit>& sets) -> const NalUnit* {
        for (const NalUnit& n : sets) {
            if (!n.empty() && nalTypeOf(codec, n[0]) == (codec == AV_CODEC_ID_H264 ? kH264Sps : kHevcSps)) {
                return &n;
            }
        }
        return nullptr;
    };
    const NalUnit* a = sps(source);
    const NalUnit* b = sps(exported);
    if (!a || !b) {
        return "the encoder settings (no headers to compare)";
    }
    const std::vector<std::uint8_t> ra = unescape(*a);
    const std::vector<std::uint8_t> rb = unescape(*b);
    if (codec == AV_CODEC_ID_H264) {
        if (ra.size() > 3 && rb.size() > 3) {
            if (ra[1] != rb[1]) {
                return std::format("the profile (profile_idc {} vs {})", ra[1], rb[1]);
            }
            if (ra[3] != rb[3]) {
                return std::format("the level ({}.{} vs {}.{})", ra[3] / 10, ra[3] % 10, rb[3] / 10, rb[3] % 10);
            }
        }
    } else if (ra.size() > 14 && rb.size() > 14) {
        const int pa = ra[3] & 0x1f;
        const int pb = rb[3] & 0x1f;
        if (pa != pb) {
            return std::format("the profile (general_profile_idc {} vs {})", pa, pb);
        }
        if (ra[14] != rb[14]) {
            return std::format("the level ({}.{} vs {}.{})", ra[14] / 30, (ra[14] % 30) / 3, rb[14] / 30, (rb[14] % 30) / 3);
        }
    }
    return "the encoder settings (the headers differ)";
}

std::int64_t SourceGop::reorderFrames(std::int64_t period) const {
    if (period <= 0 || pts.empty()) {
        return 0;
    }
    std::vector<std::int64_t> sorted = pts;
    std::sort(sorted.begin(), sorted.end());
    std::int64_t worst = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        // The i-th packet in decode order would be the i-th picture shown without reordering.
        worst = std::max(worst, (sorted[i] - pts[i] + period - 1) / period);
    }
    return worst;
}

namespace {

void finish(SourceGop& gop, std::int64_t period, bool complete) {
    gop.complete = complete;
    if (gop.pts.empty()) {
        return;
    }
    std::vector<std::int64_t> sorted = gop.pts;
    std::sort(sorted.begin(), sorted.end());
    gop.minPts = sorted.front();
    gop.maxPts = sorted.back();
    gop.contiguous = period > 0;
    for (std::size_t i = 1; i < sorted.size() && gop.contiguous; ++i) {
        gop.contiguous = sorted[i] - sorted[i - 1] == period;
    }
}

} // namespace

std::expected<std::vector<SourceGop>, Error> scanGops(const GopScanInputs& in, const std::stop_token& stop) {
    std::vector<SourceGop> gops;
    if (!in.ctx || in.stream < 0 || in.period <= 0 || !in.accepted) {
        return gops;
    }
    if (const int r = av_seek_frame(in.ctx, in.stream, in.from, AVSEEK_FLAG_BACKWARD); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot seek in the source", r));
    }
    PacketPtr packet(av_packet_alloc());
    SourceGop current;
    bool open = false;      // a GOP started by an IDR is being read
    bool passedEnd = false; // a packet at or after `to` was read
    std::int64_t read = 0;
    for (;;) {
        const int r = av_read_frame(in.ctx, packet.get());
        if (r == AVERROR_EOF) {
            if (open) {
                finish(current, in.period, true);
                gops.push_back(std::move(current));
            }
            break;
        }
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot read the source", r));
        }
        if ((++read & 255) == 0 && stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Cancelled"});
        }
        if (packet->stream_index != in.stream) {
            av_packet_unref(packet.get());
            continue;
        }
        const PacketNals nals = inspectPacket(in.codec, in.lengthSize, std::span<const std::uint8_t>(packet->data, static_cast<std::size_t>(packet->size)));
        const std::int64_t pts = packet->pts;
        const std::int64_t dts = packet->dts == AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (nals.idr) {
            if (open) {
                finish(current, in.period, true);
                gops.push_back(std::move(current));
                current = SourceGop{};
                open = false;
            }
            if (passedEnd) {
                av_packet_unref(packet.get());
                break;
            }
            open = true;
            current.keyDts = dts;
        }
        if (open) {
            if (pts == AV_NOPTS_VALUE || dts == AV_NOPTS_VALUE || nals.bad) {
                current.headersOk = false; // times or NAL units we cannot trust: this GOP is not copied
            }
            if (nals.parameterSets) {
                for (const NalUnit& set : nals.sets) {
                    if (!std::binary_search(in.accepted->begin(), in.accepted->end(), set)) {
                        current.headersOk = false;
                    }
                }
            }
            current.bytes += packet->size;
            current.pts.push_back(pts == AV_NOPTS_VALUE ? 0 : pts);
            current.dts.push_back(dts == AV_NOPTS_VALUE ? 0 : dts);
        }
        if (pts != AV_NOPTS_VALUE && pts >= in.to) {
            passedEnd = true;
        }
        av_packet_unref(packet.get());
    }
    return gops;
}

} // namespace wabcut::exporter::detail
