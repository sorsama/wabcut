#include "HdrDynamic.h"

#include <wabcut/color/ColorSpaces.h>
#include <wabcut/model/PreviewCache.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

namespace wabcut::exporter::detail {

namespace {

/// A linearised maxRGB value in nits as ST 2094-40 writes it: a fraction of 10 000 nits in multiples of 0.00001.
AVRational rgbValue(double nits) {
    const double fraction = std::clamp(nits / 10000.0, 0.0, 1.0);
    return AVRational{static_cast<int>(std::lround(fraction * 100000.0)), 100000};
}

struct AvFree {
    void operator()(void* p) const noexcept { av_free(p); }
};

/// How different two neighbouring frames' brightness is, 0 (the same) .. 1: the largest relative change of the mean and
/// of the percentiles (with a floor of 10 nits, so a dark frame's noise is not a cut).
double brightnessJump(const FrameLightStats& a, const FrameLightStats& b) {
    const auto change = [](double x, double y) {
        const double scale = std::max({x, y, 10.0});
        return std::abs(x - y) / scale;
    };
    double jump = change(a.meanMaxRgb, b.meanMaxRgb);
    for (int i = 0; i < kHdr10PlusPercentiles; ++i) {
        if (kHdr10PlusPercentages[i] >= 50.0f) {
            jump = std::max(jump, change(a.percentile[i], b.percentile[i]));
        }
    }
    return jump;
}

/// A candidate edit point is a cut when the picture's brightness jumps there. An overlay that starts over a running shot,
/// a dissolve (a ramp) and a join through edit are not.
constexpr double kCutJump = 0.3;

} // namespace

FrameLightStats measureLightRgb10(const std::uint32_t* words, std::size_t count) {
    FrameLightStats out;
    if (!words || count == 0) {
        return out;
    }
    // 10-bit PQ code value -> nits (the table of the light-level measurement, RenderedVideoSource).
    static const std::array<float, 1024> nits = [] {
        std::array<float, 1024> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            t[i] = static_cast<float>(color::pqToNits(static_cast<double>(i) / 1023.0));
        }
        return t;
    }();
    std::array<std::uint32_t, 1024> histogram{};
    std::uint32_t maxR = 0;
    std::uint32_t maxG = 0;
    std::uint32_t maxB = 0;
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t w = words[i];
        const std::uint32_t r = w & 1023u;
        const std::uint32_t g = (w >> 10) & 1023u;
        const std::uint32_t b = (w >> 20) & 1023u;
        const std::uint32_t m = std::max({r, g, b});
        ++histogram[m];
        maxR = std::max(maxR, r);
        maxG = std::max(maxG, g);
        maxB = std::max(maxB, b);
        sum += nits[m];
    }
    out.maxR = nits[maxR];
    out.maxG = nits[maxG];
    out.maxB = nits[maxB];
    out.meanMaxRgb = static_cast<float>(sum / static_cast<double>(count));
    // The code at which the running share of pixels reaches each percentage.
    std::uint64_t seen = 0;
    int next = 0;
    for (std::size_t code = 0; code < histogram.size() && next < kHdr10PlusPercentiles; ++code) {
        seen += histogram[code];
        while (next < kHdr10PlusPercentiles &&
               static_cast<double>(seen) >= static_cast<double>(count) * static_cast<double>(kHdr10PlusPercentages[next]) / 100.0) {
            out.percentile[next] = nits[code];
            ++next;
        }
    }
    return out;
}

std::expected<Hdr10PlusPlan, Error> planHdr10Plus(const std::vector<FrameLightStats>& stats,
                                                  const std::vector<std::int64_t>& candidates, double targetedDisplayNits) {
    if (stats.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No frames were measured for the HDR10+ metadata"});
    }
    const auto count = static_cast<std::int64_t>(stats.size());
    // The shots: the first frame, and every candidate where the picture jumps.
    std::vector<std::int64_t> starts{0};
    std::vector<std::int64_t> sorted = candidates;
    std::sort(sorted.begin(), sorted.end());
    for (const std::int64_t at : sorted) {
        if (at <= starts.back() || at >= count) {
            continue;
        }
        if (brightnessJump(stats[static_cast<std::size_t>(at - 1)], stats[static_cast<std::size_t>(at)]) >= kCutJump) {
            starts.push_back(at);
        }
    }

    Hdr10PlusPlan plan;
    plan.messages.resize(stats.size());
    plan.report.kind = DynamicMetadata::Hdr10Plus;
    plan.report.targetedDisplayNits = targetedDisplayNits;
    double programmeSum = 0.0;
    for (std::size_t s = 0; s < starts.size(); ++s) {
        const std::int64_t first = starts[s];
        const std::int64_t end = s + 1 < starts.size() ? starts[s + 1] : count;
        float maxscl[3] = {0.0f, 0.0f, 0.0f};
        double sum = 0.0;
        for (std::int64_t i = first; i < end; ++i) {
            const FrameLightStats& f = stats[static_cast<std::size_t>(i)];
            maxscl[0] = std::max(maxscl[0], f.maxR);
            maxscl[1] = std::max(maxscl[1], f.maxG);
            maxscl[2] = std::max(maxscl[2], f.maxB);
            sum += f.meanMaxRgb;
        }
        const double shotPeak = std::max({maxscl[0], maxscl[1], maxscl[2]});
        plan.report.shots.push_back(ShotReport{first, end - first, shotPeak, sum / static_cast<double>(end - first)});
        plan.report.peakNits = std::max(plan.report.peakNits, shotPeak);
        programmeSum += sum;
        for (std::int64_t i = first; i < end; ++i) {
            const FrameLightStats& f = stats[static_cast<std::size_t>(i)];
            std::unique_ptr<AVDynamicHDRPlus, AvFree> hdr(av_dynamic_hdr_plus_alloc(nullptr));
            if (!hdr) {
                return std::unexpected(Error{ErrorCode::Unknown, "Out of memory"});
            }
            hdr->itu_t_t35_country_code = 0xB5;
            hdr->application_version = 1;
            hdr->num_windows = 1;
            hdr->targeted_system_display_maximum_luminance = AVRational{static_cast<int>(std::lround(targetedDisplayNits)), 1};
            hdr->targeted_system_display_actual_peak_luminance_flag = 0;
            hdr->mastering_display_actual_peak_luminance_flag = 0;
            AVHDRPlusColorTransformParams& p = hdr->params[0];
            for (int c = 0; c < 3; ++c) {
                p.maxscl[c] = rgbValue(maxscl[c]);
            }
            p.average_maxrgb = rgbValue(f.meanMaxRgb);
            p.num_distribution_maxrgb_percentiles = kHdr10PlusPercentiles;
            double previous = 0.0;
            for (int j = 0; j < kHdr10PlusPercentiles; ++j) {
                const double value = std::max(previous, static_cast<double>(f.percentile[j])); // never decreasing
                previous = value;
                p.distribution_maxrgb[j].percentage = static_cast<std::uint8_t>(kHdr10PlusPercentages[j]);
                p.distribution_maxrgb[j].percentile = rgbValue(value);
            }
            p.fraction_bright_pixels = AVRational{0, 1};
            p.tone_mapping_flag = 0; // no Bezier curve: the display maps by the distribution
            p.color_saturation_mapping_flag = 0;
            std::uint8_t* data = nullptr;
            std::size_t size = 0;
            if (const int rc = av_dynamic_hdr_plus_to_t35(hdr.get(), &data, &size); rc < 0 || !data) {
                return std::unexpected(Error{ErrorCode::EncoderError, "The HDR10+ metadata could not be serialised"});
            }
            std::unique_ptr<std::uint8_t, AvFree> owned(data);
            // The T.35 header: country code 0xB5, terminal provider code 0x003C (Samsung / the HDR10+ LLC), oriented code
            // 0x0001, application identifier 4 (ST 2094-40); FFmpeg's buffer starts at the application version.
            std::vector<std::uint8_t>& message = plan.messages[static_cast<std::size_t>(i)];
            message = {0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04};
            message.insert(message.end(), data, data + size);
        }
    }
    plan.report.frames = count;
    plan.report.averageNits = programmeSum / static_cast<double>(count);
    plan.report.written = true;
    return plan;
}

std::vector<std::int64_t> shotStartsOf(const ProjectState& state, SequenceId sequence, TimeRange range, Rate rate,
                                       std::int64_t frameCount) {
    std::vector<std::int64_t> out{0};
    for (const RenderSegment& segment : renderSegments(state, sequence, range)) {
        const auto frame = ticksToFrames(segment.range.start - range.start, rate, Rounding::Nearest);
        if (frame && *frame > 0 && *frame < frameCount) {
            out.push_back(*frame);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::uint8_t> addEmulationPrevention(std::span<const std::uint8_t> rbsp) {
    std::vector<std::uint8_t> out;
    out.reserve(rbsp.size() + rbsp.size() / 64 + 4);
    int zeros = 0;
    for (const std::uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

std::vector<std::uint8_t> hevcNalUnit(int type, std::span<const std::uint8_t> rbsp) {
    std::vector<std::uint8_t> out{0, 0, 0, 1, static_cast<std::uint8_t>((type & 0x3F) << 1), 0x01};
    const std::vector<std::uint8_t> escaped = addEmulationPrevention(rbsp);
    out.insert(out.end(), escaped.begin(), escaped.end());
    return out;
}

std::vector<std::uint8_t> hevcPrefixSeiNal(std::span<const std::uint8_t> t35) {
    std::vector<std::uint8_t> rbsp;
    rbsp.push_back(4); // payloadType: user_data_registered_itu_t_t35
    std::size_t size = t35.size();
    while (size >= 255) {
        rbsp.push_back(255);
        size -= 255;
    }
    rbsp.push_back(static_cast<std::uint8_t>(size));
    rbsp.insert(rbsp.end(), t35.begin(), t35.end());
    rbsp.push_back(0x80); // rbsp_trailing_bits
    return hevcNalUnit(39, rbsp);           // PREFIX_SEI_NUT
}

namespace {

struct NalPosition {
    std::size_t start = 0;  ///< where its start code begins (a four-byte one includes the leading zero)
    std::size_t header = 0; ///< the first byte of the NAL header
    int type = 0;
};

std::vector<NalPosition> annexBNalUnits(const std::uint8_t* data, std::size_t size) {
    std::vector<NalPosition> out;
    for (std::size_t i = 0; i + 3 < size;) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            NalPosition nal;
            nal.header = i + 3;
            nal.start = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
            nal.type = (data[nal.header] >> 1) & 0x3F;
            out.push_back(nal);
            i += 3;
        } else {
            ++i;
        }
    }
    return out;
}

} // namespace

bool insertNalUnits(AVPacket* packet, std::span<const std::vector<std::uint8_t>> before,
                    std::span<const std::vector<std::uint8_t>> after) {
    if (!packet || !packet->data || packet->size < 6 || (before.empty() && after.empty())) {
        return false;
    }
    const auto size = static_cast<std::size_t>(packet->size);
    const std::vector<NalPosition> nals = annexBNalUnits(packet->data, size);
    const auto firstVcl = std::find_if(nals.begin(), nals.end(), [](const NalPosition& n) { return n.type < 32; });
    if (firstVcl == nals.end()) {
        return false;
    }
    std::size_t extra = 0;
    for (const auto& unit : before) {
        extra += unit.size();
    }
    for (const auto& unit : after) {
        extra += unit.size();
    }
    AVBufferRef* buffer = av_buffer_alloc(static_cast<std::size_t>(size + extra + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!buffer) {
        return false;
    }
    std::uint8_t* out = buffer->data;
    std::memcpy(out, packet->data, firstVcl->start);
    std::size_t at = firstVcl->start;
    for (const auto& unit : before) {
        std::memcpy(out + at, unit.data(), unit.size());
        at += unit.size();
    }
    std::memcpy(out + at, packet->data + firstVcl->start, size - firstVcl->start);
    at += size - firstVcl->start;
    for (const auto& unit : after) {
        std::memcpy(out + at, unit.data(), unit.size());
        at += unit.size();
    }
    std::memset(out + at, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    av_buffer_unref(&packet->buf);
    packet->buf = buffer;
    packet->data = buffer->data;
    packet->size = static_cast<int>(at);
    return true;
}

namespace {

class Hdr10PlusInjector final : public NalInjector {
public:
    explicit Hdr10PlusInjector(std::shared_ptr<const Hdr10PlusPlan> plan) : m_plan(std::move(plan)) {}

    std::expected<DolbyVisionWriter::Units, Error> unitsFor(std::int64_t frameIndex) override {
        DolbyVisionWriter::Units units;
        if (frameIndex >= 0 && frameIndex < static_cast<std::int64_t>(m_plan->messages.size())) {
            units.before.push_back(hevcPrefixSeiNal(m_plan->messages[static_cast<std::size_t>(frameIndex)]));
        }
        return units;
    }

private:
    std::shared_ptr<const Hdr10PlusPlan> m_plan;
};

} // namespace

std::unique_ptr<NalInjector> makeHdr10PlusInjector(std::shared_ptr<const Hdr10PlusPlan> plan) {
    return std::make_unique<Hdr10PlusInjector>(std::move(plan));
}

} // namespace wabcut::exporter::detail
