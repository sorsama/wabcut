#include "SmartRender.h"

#include <wabcut/model/MediaLocator.h>
#include <wabcut/playback/video/Evaluate.h>

extern "C" {
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace wabcut::exporter::detail {

namespace {

struct FormatCloser {
    void operator()(AVFormatContext* ctx) const noexcept { avformat_close_input(&ctx); }
};
using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;

/// A candidate file: its demuxer (opened for the codec parameters and the index) and the frames it has.
struct ProbedFile {
    FormatPtr ctx;
    int stream = -1;
    bool usable = false; ///< the codec parameters match the encoder's
    std::vector<std::int64_t> timestamps; ///< the stream's packet pts, sorted
};

bool sameTags(int source, int encoder, int unspecified) {
    return source == encoder || source == unspecified;
}

/// The stream's packets are exactly what the encoder writes.
bool matches(const AVStream* st, const AVCodecParameters* enc) {
    const AVCodecParameters* p = st->codecpar;
    if (p->codec_id != enc->codec_id || p->width != enc->width || p->height != enc->height || p->format != enc->format) {
        return false;
    }
    if (p->codec_id == AV_CODEC_ID_PRORES && p->codec_tag != enc->codec_tag) {
        return false; // the ProRes flavour ('apcs', 'apcn', 'apch', 'ap4h', ...)
    }
    if (p->codec_id == AV_CODEC_ID_DNXHD && p->profile != enc->profile && p->profile != AV_PROFILE_UNKNOWN) {
        return false;
    }
    if (p->field_order != AV_FIELD_PROGRESSIVE && p->field_order != AV_FIELD_UNKNOWN) {
        return false;
    }
    if (!sameTags(p->color_primaries, enc->color_primaries, AVCOL_PRI_UNSPECIFIED) ||
        !sameTags(p->color_trc, enc->color_trc, AVCOL_TRC_UNSPECIFIED) ||
        !sameTags(p->color_space, enc->color_space, AVCOL_SPC_UNSPECIFIED) ||
        !sameTags(p->color_range, enc->color_range, AVCOL_RANGE_UNSPECIFIED)) {
        return false;
    }
    if (p->sample_aspect_ratio.num != 0 && av_cmp_q(p->sample_aspect_ratio, AVRational{1, 1}) != 0) {
        return false;
    }
    // A display rotation would make the render turn the picture.
    for (int i = 0; i < p->nb_coded_side_data; ++i) {
        if (p->coded_side_data[i].type == AV_PKT_DATA_DISPLAYMATRIX) {
            const auto* matrix = reinterpret_cast<const std::int32_t*>(p->coded_side_data[i].data);
            const double angle = av_display_rotation_get(matrix);
            if (angle == angle && std::abs(angle) > 0.01) {
                return false;
            }
        }
    }
    return true;
}

std::unique_ptr<ProbedFile> probe(const std::filesystem::path& file, int wantedStream, const AVCodecParameters* encoder) {
    auto probed = std::make_unique<ProbedFile>();
    AVFormatContext* raw = nullptr;
    const std::string utf8 = pathToUtf8(file);
    if (avformat_open_input(&raw, utf8.c_str(), nullptr, nullptr) < 0) {
        return probed;
    }
    probed->ctx.reset(raw);
    if (avformat_find_stream_info(raw, nullptr) < 0) {
        return probed;
    }
    const int stream = wantedStream >= 0 ? wantedStream : av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0 || stream >= static_cast<int>(raw->nb_streams)) {
        return probed;
    }
    AVStream* st = raw->streams[stream];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || !matches(st, encoder)) {
        return probed;
    }
    const int entries = avformat_index_get_entries_count(st);
    if (entries <= 0) {
        return probed; // no index: frames cannot be proven to exist
    }
    probed->timestamps.reserve(static_cast<std::size_t>(entries));
    for (int i = 0; i < entries; ++i) {
        if (const AVIndexEntry* e = avformat_index_get_entry(st, i)) {
            probed->timestamps.push_back(e->timestamp);
        }
    }
    std::sort(probed->timestamps.begin(), probed->timestamps.end());
    probed->stream = stream;
    probed->usable = true;
    return probed;
}

/// The packet timestamp of the frame on screen at `t` (ticks from the stream's start), as the decoders pick it: the
/// last frame at or before t, also one stamped up to half a time-base tick after it. nullopt: none.
std::optional<std::int64_t> frameAtTicks(const ProbedFile& f, Ticks t) {
    const AVStream* st = f.ctx->streams[f.stream];
    const std::int64_t start = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
    const AVRational ticks{1, static_cast<int>(kTicksPerSecond / 1000)}; // kTicksPerSecond / 1000 fits an int
    // t in the stream's time base, rounded to nearest: the half-tick snap.
    const std::int64_t target = start + av_rescale_q_rnd(t.value / 1000, ticks, st->time_base, AV_ROUND_NEAR_INF);
    const auto it = std::upper_bound(f.timestamps.begin(), f.timestamps.end(), target);
    if (it == f.timestamps.begin()) {
        return std::nullopt;
    }
    return *(it - 1);
}

bool identity(const render::LayerTransform& t) {
    return t == render::LayerTransform{};
}

} // namespace

bool smartRenderEligible(const ExportSettings& s) {
    if (!s.video.enabled || (!s.render.smartRender && !s.render.usePreviews)) {
        return false;
    }
    if (s.video.codec != VideoCodec::ProRes && s.video.codec != VideoCodec::DnxHR) {
        return false;
    }
    // Embedded 608 captions (P.2) ride on each encoded frame: a copied packet carries none.
    return !s.effects.anyVideo() && s.captions.mode != CaptionExport::BurnIn && s.captions.mode != CaptionExport::Embed &&
           !hasAlpha(s.video.pixelFormat) && s.container != Container::Image;
}

SmartRenderPlan SmartRenderPlan::build(const Inputs& in, const std::stop_token& stop) {
    SmartRenderPlan plan;
    const ResolvedExport& r = *in.resolved;
    const Sequence& seq = *in.sequence;
    plan.m_frames.assign(static_cast<std::size_t>(r.frameCount), CopyFrame{});
    if (!in.encoder || r.frameCount <= 0) {
        return plan;
    }
    // The picture must be the sequence's own: no scaling, no tone mapping, SDR, 8- or 10-bit as rendered.
    const bool sameGeometry = r.width == seq.width && r.height == seq.height && r.renderWidth == r.width &&
                              r.renderHeight == r.height && r.sourceWidth == r.width && r.sourceHeight == r.height;
    if (!sameGeometry || r.dynamicRange != DynamicRange::Sdr || in.project->colorManagement.managed()) {
        return plan;
    }
    std::map<std::string, std::unique_ptr<ProbedFile>> probed; // by path + stream
    std::map<std::string, int> fileIndex;
    std::map<std::string, bool> previewValid; // preview entry file -> still shows the sequence
    std::vector<playback::LayerSpec> specs;
    const auto fileFor = [&](const std::filesystem::path& path, int stream) -> std::pair<ProbedFile*, int> {
        const std::string key = pathToUtf8(path) + "#" + std::to_string(stream);
        auto it = probed.find(key);
        if (it == probed.end()) {
            it = probed.emplace(key, probe(path, stream, in.encoder)).first;
        }
        if (!it->second->usable) {
            return {nullptr, -1};
        }
        auto f = fileIndex.find(key);
        if (f == fileIndex.end()) {
            plan.m_files.push_back(path);
            f = fileIndex.emplace(key, static_cast<int>(plan.m_files.size()) - 1).first;
        }
        return {it->second.get(), f->second};
    };

    for (std::int64_t k = 0; k < r.frameCount; ++k) {
        if ((k & 255) == 0 && stop.stop_requested()) {
            return plan;
        }
        const auto offset = framesToTicks(k, r.frameRate, Rounding::Nearest);
        if (!offset) {
            break;
        }
        const Ticks t = r.range.start + *offset;
        CopyFrame& copy = plan.m_frames[static_cast<std::size_t>(k)];

        // Use Previews: a rendered segment that still shows what the sequence shows here.
        if (in.settings->render.usePreviews && in.previews) {
            if (const auto entry = in.previews->lookup(seq.id, t)) {
                auto valid = previewValid.find(entry->file);
                if (valid == previewValid.end()) {
                    valid = previewValid.emplace(entry->file, in.previews->validAt(in.snapshot ? *in.snapshot : *in.project, seq.id, t).has_value()).first;
                }
                if (valid->second) {
                    const auto [file, index] = fileFor(in.previews->folder() / pathFromUtf8(entry->file), -1);
                    if (file) {
                        if (const auto ts = frameAtTicks(*file, t - entry->range.start)) {
                            copy = CopyFrame{index, file->stream, *ts, true};
                            ++plan.m_copied;
                            ++plan.m_previewCopied;
                            continue;
                        }
                    }
                }
            }
        }

        // Smart Render: one source clip, untouched.
        if (!in.settings->render.smartRender) {
            continue;
        }
        playback::evaluateVideoLayers(*in.project, seq, t, specs, true, nullptr, nullptr);
        if (specs.size() != 1) {
            continue;
        }
        const playback::LayerSpec& spec = specs.front();
        if (!playback::needsDecode(spec) || !spec.item || spec.nested || spec.matte || !identity(spec.transform) ||
            spec.opacity != 1.0 || spec.blend != BlendMode::Normal || !spec.effects.empty() || !spec.redactions.empty() ||
            !spec.sourceRedactions.empty() || spec.transition || spec.inputSpace || spec.bypassedEffects != 0) {
            continue;
        }
        const std::string* path = playback::sourcePathOf(*spec.item, false);
        if (!path) {
            continue;
        }
        std::filesystem::path source = pathFromUtf8(*path);
        if (source.is_relative() && !in.projectFile.empty()) {
            source = in.projectFile.parent_path() / source;
        }
        const auto [file, index] = fileFor(source, spec.containerStream);
        if (!file) {
            continue;
        }
        const AVStream* st = file->ctx->streams[file->stream];
        // Its own rate: the source frame each output frame shows is one of its own, once.
        if (av_cmp_q(st->avg_frame_rate, AVRational{r.frameRate.num, r.frameRate.den}) != 0) {
            continue;
        }
        if (const auto ts = frameAtTicks(*file, spec.sourceTime)) {
            copy = CopyFrame{index, file->stream, *ts, false};
            ++plan.m_copied;
        }
    }
    return plan;
}

std::vector<std::int64_t> SmartRenderPlan::renderedFrames() const {
    std::vector<std::int64_t> out;
    out.reserve(m_frames.size() - static_cast<std::size_t>(m_copied));
    for (std::size_t k = 0; k < m_frames.size(); ++k) {
        if (m_frames[k].file < 0) {
            out.push_back(static_cast<std::int64_t>(k));
        }
    }
    return out;
}

const CopyFrame* SmartRenderPlan::copyFor(std::int64_t k) const {
    if (k < 0 || k >= static_cast<std::int64_t>(m_frames.size())) {
        return nullptr;
    }
    const CopyFrame& c = m_frames[static_cast<std::size_t>(k)];
    return c.file >= 0 ? &c : nullptr;
}

struct PacketCopier::Input {
    FormatPtr ctx;
    std::int64_t lastRead = AV_NOPTS_VALUE; ///< pts of the last packet read from the stream in use
};

PacketCopier::PacketCopier(const SmartRenderPlan& plan) : m_plan(plan) {
    m_inputs.resize(plan.files().size());
}

PacketCopier::~PacketCopier() = default;

std::expected<PacketPtr, Error> PacketCopier::packet(const CopyFrame& copy, std::int64_t k) {
    if (copy.file < 0 || copy.file >= static_cast<int>(m_inputs.size())) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No such copy source"});
    }
    auto& input = m_inputs[static_cast<std::size_t>(copy.file)];
    const std::string path = pathToUtf8(m_plan.files()[static_cast<std::size_t>(copy.file)]);
    if (!input) {
        input = std::make_unique<Input>();
        AVFormatContext* raw = nullptr;
        if (const int r = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot open " + path, r));
        }
        input->ctx.reset(raw);
        if (const int r = avformat_find_stream_info(raw, nullptr); r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot read " + path, r));
        }
    }
    AVFormatContext* ctx = input->ctx.get();
    if (input->lastRead == AV_NOPTS_VALUE || copy.timestamp <= input->lastRead) {
        // A jump back (or the first read): intra-only, so any packet is a key frame.
        if (const int r = av_seek_frame(ctx, copy.stream, copy.timestamp, AVSEEK_FLAG_BACKWARD); r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot seek in " + path, r));
        }
    }
    PacketPtr packet(av_packet_alloc());
    for (int guard = 0; guard < 100000; ++guard) {
        const int r = av_read_frame(ctx, packet.get());
        if (r < 0) {
            return std::unexpected(Error{ErrorCode::DecoderError, "A smart-rendered frame is missing from " + path});
        }
        if (packet->stream_index != copy.stream) {
            av_packet_unref(packet.get());
            continue;
        }
        input->lastRead = packet->pts;
        if (packet->pts == copy.timestamp) {
            packet->pts = k;
            packet->dts = k;
            packet->duration = 1;
            packet->pos = -1;
            packet->flags |= AV_PKT_FLAG_KEY;
            packet->stream_index = 0;
            return packet;
        }
        if (packet->pts > copy.timestamp) {
            av_packet_unref(packet.get());
            return std::unexpected(Error{ErrorCode::DecoderError, "A smart-rendered frame is missing from " + path});
        }
        av_packet_unref(packet.get());
    }
    return std::unexpected(Error{ErrorCode::DecoderError, "A smart-rendered frame is missing from " + path});
}

} // namespace wabcut::exporter::detail
