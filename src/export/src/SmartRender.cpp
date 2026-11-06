#include "SmartRender.h"

#include <wabcut/model/MediaLocator.h>
#include <wabcut/playback/video/Evaluate.h>

extern "C" {
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>

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

/// A display rotation would make the render turn the picture, and a reflection (card 154: a negative determinant)
/// would make it flip it.
bool displayTransformed(const AVCodecParameters* p) {
    for (int i = 0; i < p->nb_coded_side_data; ++i) {
        if (p->coded_side_data[i].type == AV_PKT_DATA_DISPLAYMATRIX) {
            const auto* matrix = reinterpret_cast<const std::int32_t*>(p->coded_side_data[i].data);
            const double angle = av_display_rotation_get(matrix);
            if (angle == angle && std::abs(angle) > 0.01) {
                return true;
            }
            if (static_cast<std::int64_t>(matrix[0]) * matrix[4] - static_cast<std::int64_t>(matrix[1]) * matrix[3] < 0) {
                return true;
            }
        }
    }
    return false;
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
    return !displayTransformed(p);
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

/// Both formats sample the picture alike: bits per sample, chroma subsampling and component count (NV12 = YUV420P).
bool sameSampling(int a, int b) {
    const AVPixFmtDescriptor* da = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(a));
    const AVPixFmtDescriptor* db = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(b));
    if (!da || !db) {
        return false;
    }
    return da->nb_components == db->nb_components && da->log2_chroma_w == db->log2_chroma_w &&
           da->log2_chroma_h == db->log2_chroma_h && da->comp[0].depth == db->comp[0].depth &&
           ((da->flags ^ db->flags) & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_ALPHA)) == 0;
}

} // namespace

bool smartRenderEligible(const ExportSettings& s) {
    // 0.7: dynamic HDR metadata is measured on, and written for, every rendered frame: a copied packet has its own (or none).
    if (s.video.dynamicMetadata != DynamicMetadata::None) {
        return false;
    }
    return smartRenderIntra(s) || smartRenderLongGop(s);
}

bool smartRenderIntra(const ExportSettings& s) {
    if (!s.video.enabled || (!s.render.smartRender && !s.render.usePreviews)) {
        return false;
    }
    if (s.video.codec != VideoCodec::ProRes && s.video.codec != VideoCodec::DnxHR) {
        return false;
    }
    // Embedded 608 captions (P.2) ride on each encoded frame: a copied packet carries none.
    return !s.effects.anyVideo() && s.captions.mode != CaptionExport::BurnIn && s.captions.mode != CaptionExport::Embed &&
           !hasAlpha(s.video.pixelFormat) && s.container != Container::Image &&
           !isMxf(s.container); // 0.6 (#73): MXF's constant-size index wants every DNxHD frame from this encoder
}

bool smartRenderLongGop(const ExportSettings& s) {
    if (!s.video.enabled || !s.render.smartRender) {
        return false;
    }
    if (s.video.codec != VideoCodec::H264 && s.video.codec != VideoCodec::Hevc) {
        return false;
    }
    // A size limit is met by encoding the whole programme at the rate that fits; a copied GOP keeps its own size.
    if (s.video.rateControl == RateControl::TargetSize) {
        return false;
    }
    // MP4 and MOV only: Matroska counts time in milliseconds, which cannot hold the frame times of 24, 30 or 60 fps to
    // the tick, and a copied GOP keeps its source's stamps exactly (a rounded 0, 33, 67, 100 is not contiguous).
    if (s.container != Container::Mp4 && s.container != Container::Mov) {
        return false;
    }
    return !s.effects.anyVideo() && s.captions.mode != CaptionExport::BurnIn && s.captions.mode != CaptionExport::Embed &&
           !hasAlpha(s.video.pixelFormat);
}

SmartRenderPlan SmartRenderPlan::build(const Inputs& in, const std::stop_token& stop) {
    SmartRenderPlan plan;
    const ResolvedExport& r = *in.resolved;
    const Sequence& seq = *in.sequence;
    plan.m_frames.assign(static_cast<std::size_t>(r.frameCount), CopyFrame{});
    if (!in.encoder || r.frameCount <= 0) {
        return plan;
    }
    const bool longGop = isLongGopCodec(in.encoder->codec_id);
    plan.m_longGop = longGop;
    // The picture must be the sequence's own: no scaling, no tone mapping, SDR, 8- or 10-bit as rendered.
    const bool sameGeometry = r.width == seq.width && r.height == seq.height && r.renderWidth == r.width &&
                              r.renderHeight == r.height && r.sourceWidth == r.width && r.sourceHeight == r.height;
    // 145: copied packets carry the render cache's square pixels; an anamorphic export tags its own ratio.
    if (!sameGeometry || !(r.sampleAspect == Rate{1, 1}) || r.dynamicRange != DynamicRange::Sdr ||
        in.project->colorManagement.managed()) {
        if (longGop) {
            plan.m_notes.push_back({!sameGeometry                    ? "The export is scaled, cropped or letterboxed"
                                    : !(r.sampleAspect == Rate{1, 1}) ? "The sequence has non-square pixels"
                                                                      : "HDR or colour-managed export",
                                    r.frameCount, 0, {}});
        }
        return plan;
    }
    if (longGop) {
        buildLongGop(in, stop, plan);
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

// ---- 0.7 (card 049): long-GOP planning -----------------------------------------------------------------------------

namespace {

/// A source file the long-GOP planner reads packets of.
struct GopFile {
    FormatPtr ctx;
    int stream = -1;
    AVCodecID codec = AV_CODEC_ID_NONE;
    int lengthSize = 0;
    std::int64_t period = 0;    ///< stream ticks per frame
    std::int64_t startTime = 0; ///< the stream's start_time (the time of its first frame)
    AVRational timeBase{1, 1};
    std::string reason;         ///< why its pictures are not copied ("" = they can be)
    int planIndex = -1;         ///< its index in SmartRenderPlan::files() once a GOP of it is copied
};

/// Why a frame renders when the clip on it cannot be copied as it is ("" = it can).
std::string untouchedProblem(const std::vector<playback::LayerSpec>& specs) {
    if (specs.empty()) {
        return "Nothing on the picture tracks (black)";
    }
    if (specs.size() > 1) {
        return "Several layers play at once (an overlay, a title or a transition)";
    }
    const playback::LayerSpec& spec = specs.front();
    if (spec.nested) {
        return "A nested sequence";
    }
    if (!playback::needsDecode(spec) || !spec.item) {
        return "A title, graphic or generated clip";
    }
    if (spec.transition) {
        return "A transition";
    }
    if (spec.matte || !identity(spec.transform) || spec.opacity != 1.0 || spec.blend != BlendMode::Normal) {
        return "Motion, opacity or a blend mode on the clip";
    }
    if (!spec.effects.empty() || spec.bypassedEffects != 0) {
        return "Effects on the clip";
    }
    if (!spec.redactions.empty() || !spec.sourceRedactions.empty()) {
        return "Redactions (blur or bleep) on the clip";
    }
    if (spec.inputSpace) {
        return "A colour conversion of the clip";
    }
    return {};
}

/// The source stream's packets are what the export writes, apart from its headers (those are compared separately).
std::string longGopMismatch(const AVStream* st, const AVCodecParameters* enc, Rate outRate) {
    const AVCodecParameters* p = st->codecpar;
    if (p->codec_id != enc->codec_id) {
        return "The source is another codec";
    }
    if (p->width != enc->width || p->height != enc->height) {
        return "The source is another frame size";
    }
    if (!sameSampling(p->format, enc->format)) {
        return "The source has another bit depth or chroma format";
    }
    if (p->field_order != AV_FIELD_PROGRESSIVE && p->field_order != AV_FIELD_UNKNOWN) {
        return "The source is interlaced";
    }
    if (p->sample_aspect_ratio.num != 0 && av_cmp_q(p->sample_aspect_ratio, AVRational{1, 1}) != 0) {
        return "The source has non-square pixels";
    }
    if (displayTransformed(p)) {
        return "The source is rotated or mirrored";
    }
    if (av_cmp_q(st->avg_frame_rate, AVRational{outRate.num, outRate.den}) != 0) {
        return "The source has another frame rate";
    }
    return {};
}

std::unique_ptr<GopFile> openGopFile(const std::filesystem::path& path, int wantedStream, const AVCodecParameters* enc,
                                     Rate outRate, const std::vector<NalUnit>& accepted, int exportLengthSize) {
    auto file = std::make_unique<GopFile>();
    AVFormatContext* raw = nullptr;
    const std::string utf8 = pathToUtf8(path);
    if (avformat_open_input(&raw, utf8.c_str(), nullptr, nullptr) < 0) {
        file->reason = "The source file cannot be opened";
        return file;
    }
    file->ctx.reset(raw);
    if (avformat_find_stream_info(raw, nullptr) < 0) {
        file->reason = "The source file cannot be read";
        return file;
    }
    const int stream = wantedStream >= 0 ? wantedStream : av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0 || stream >= static_cast<int>(raw->nb_streams) ||
        raw->streams[stream]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        file->reason = "The source has no such video stream";
        return file;
    }
    const AVStream* st = raw->streams[stream];
    file->stream = stream;
    file->codec = st->codecpar->codec_id;
    file->timeBase = st->time_base;
    file->startTime = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
    if (std::string why = longGopMismatch(st, enc, outRate); !why.empty()) {
        file->reason = std::move(why);
        return file;
    }
    const std::span<const std::uint8_t> extradata(st->codecpar->extradata, static_cast<std::size_t>(std::max(0, st->codecpar->extradata_size)));
    file->lengthSize = nalLengthSize(file->codec, extradata);
    if (file->lengthSize == 0) {
        file->reason = "The source is a raw stream without a header record";
        return file;
    }
    if (exportLengthSize != 0 && file->lengthSize != exportLengthSize) {
        // The packets are copied as they are: their NAL length prefixes must be the ones the file's header record says.
        file->reason = "The source writes NAL units with another length prefix";
        return file;
    }
    const std::vector<NalUnit> sets = parameterSets(file->codec, extradata);
    if (sets != accepted) {
        file->reason = "The source was encoded with other settings: " + describeHeaderDifference(file->codec, sets, accepted);
        return file;
    }
    // Ticks of the stream's time base per frame: a whole number.
    const std::int64_t num = static_cast<std::int64_t>(st->time_base.den) * outRate.den;
    const std::int64_t den = static_cast<std::int64_t>(st->time_base.num) * outRate.num;
    if (den <= 0 || num % den != 0) {
        file->reason = "The source's frame times are not whole ticks";
        return file;
    }
    file->period = num / den;
    return file;
}

constexpr std::size_t kMaxCopiedGopFrames = 1800;
constexpr std::int64_t kMaxCopiedReorder = 16;

} // namespace

void SmartRenderPlan::buildLongGop(const Inputs& in, const std::stop_token& stop, SmartRenderPlan& plan) {
    const ResolvedExport& r = *in.resolved;
    const Sequence& seq = *in.sequence;
    const AVCodecParameters* enc = in.encoder;
    const std::int64_t total = r.frameCount;
    const std::vector<NalUnit> accepted = parameterSets(
        enc->codec_id, std::span<const std::uint8_t>(enc->extradata, static_cast<std::size_t>(std::max(0, enc->extradata_size))));

    // Why a frame renders: reason ids, one per output frame (-1: copied).
    std::vector<std::string> reasons;
    std::map<std::string, int> reasonIds;
    const auto reasonId = [&](const std::string& text) {
        auto it = reasonIds.find(text);
        if (it == reasonIds.end()) {
            reasons.push_back(text);
            it = reasonIds.emplace(text, static_cast<int>(reasons.size()) - 1).first;
        }
        return it->second;
    };
    std::vector<int> reasonOf(static_cast<std::size_t>(total), reasonId("Not copied"));
    const auto finish = [&] {
        std::map<int, SmartRenderNote> byReason;
        for (std::int64_t k = 0; k < total; ++k) {
            const int id = reasonOf[static_cast<std::size_t>(k)];
            if (id < 0) {
                continue;
            }
            SmartRenderNote& note = byReason[id];
            if (note.frames == 0) {
                note.reason = reasons[static_cast<std::size_t>(id)];
                note.firstFrame = k;
            }
            ++note.frames;
        }
        for (auto& [id, note] : byReason) {
            plan.m_notes.push_back(std::move(note));
        }
        std::stable_sort(plan.m_notes.begin(), plan.m_notes.end(),
                         [](const SmartRenderNote& a, const SmartRenderNote& b) { return a.frames > b.frames; });
    };
    const auto markAll = [&](const std::string& why) {
        std::fill(reasonOf.begin(), reasonOf.end(), reasonId(why));
        finish();
    };
    if (accepted.empty()) {
        markAll("The export's encoder wrote no headers to compare the source with");
        return;
    }
    // The encoders hand the muxer Annex B headers (start codes, not an avcC / hvcC record), and the MP4 and MOV muxers
    // then treat every packet of the stream as Annex B and write the NAL lengths themselves: the copied packets reach them
    // as Annex B too (0 here). An encoder that gives a record (Media Foundation) wants its packets as they are, with the
    // record's length prefix: only sources with that prefix are copied.
    const int exportLengthSize = nalLengthSize(
        enc->codec_id, std::span<const std::uint8_t>(enc->extradata, static_cast<std::size_t>(std::max(0, enc->extradata_size))));

    // ---- Pass 1: which frames show an untouched source clip, one source frame after the other -----------------------
    struct Run {
        std::string key;
        std::filesystem::path path;
        int stream = -1;
        std::int64_t firstFrame = 0;
        std::vector<Ticks> times; ///< the source time each frame shows
    };
    std::vector<Run> runs;
    std::vector<playback::LayerSpec> specs;
    const Ticks framePeriod = framesToTicks(1, r.frameRate, Rounding::Nearest).value_or(Ticks{1});
    bool open = false; // the last run continues with the next frame
    for (std::int64_t k = 0; k < total; ++k) {
        if ((k & 255) == 0 && stop.stop_requested()) {
            markAll("Cancelled");
            return;
        }
        const auto offset = framesToTicks(k, r.frameRate, Rounding::Nearest);
        if (!offset) {
            break;
        }
        const Ticks t = r.range.start + *offset;
        playback::evaluateVideoLayers(*in.project, seq, t, specs, true, nullptr, nullptr);
        std::string problem = untouchedProblem(specs);
        std::filesystem::path source;
        if (problem.empty()) {
            const playback::LayerSpec& spec = specs.front();
            const std::string* path = playback::sourcePathOf(*spec.item, false);
            if (!path) {
                problem = "Media without a file";
            } else {
                source = pathFromUtf8(*path);
                if (source.is_relative() && !in.projectFile.empty()) {
                    source = in.projectFile.parent_path() / source;
                }
            }
        }
        if (!problem.empty()) {
            reasonOf[static_cast<std::size_t>(k)] = reasonId(problem);
            open = false;
            continue;
        }
        const playback::LayerSpec& spec = specs.front();
        const std::string key = pathToUtf8(source) + "#" + std::to_string(spec.containerStream);
        if (open && !runs.empty() && runs.back().key == key) {
            const Ticks step = spec.sourceTime - runs.back().times.back();
            // One frame period forward, give or take the rounding of ticks.
            if (step.value >= framePeriod.value - 2 && step.value <= framePeriod.value + 2) {
                runs.back().times.push_back(spec.sourceTime);
                continue;
            }
            // Within a few seconds of the last frame either way: speed, reverse or a freeze; further: a cut in the source.
            if (std::abs(step.value) <= framePeriod.value * 16) {
                reasonOf[static_cast<std::size_t>(k)] = reasonId("Retimed, reversed or frozen clip");
                open = false;
                continue;
            }
        }
        runs.push_back(Run{key, source, spec.containerStream, k, {spec.sourceTime}});
        open = true;
    }

    // ---- Pass 2: the GOPs of each run ---------------------------------------------------------------------------------
    std::map<std::string, std::unique_ptr<GopFile>> files;
    std::map<std::string, int> planIndex;
    const auto rateLimit = in.settings->video.rateControl == RateControl::Bitrate
                               ? std::optional<double>(1.3 * static_cast<double>(std::max(in.settings->video.bitrate, in.settings->video.maxBitrate)))
                               : std::nullopt;
    for (const Run& run : runs) {
        if (stop.stop_requested()) {
            break;
        }
        const std::int64_t n = static_cast<std::int64_t>(run.times.size());
        if (n < 2) {
            reasonOf[static_cast<std::size_t>(run.firstFrame)] = reasonId("Too short to hold a whole GOP");
            continue;
        }
        auto it = files.find(run.key);
        if (it == files.end()) {
            it = files.emplace(run.key, openGopFile(run.path, run.stream, enc, r.frameRate, accepted, exportLengthSize)).first;
        }
        GopFile& file = *it->second;
        const auto markRun = [&](const std::string& why) {
            for (std::int64_t i = 0; i < n; ++i) {
                reasonOf[static_cast<std::size_t>(run.firstFrame + i)] = reasonId(why);
            }
        };
        if (!file.reason.empty()) {
            markRun(file.reason + " (" + pathToUtf8(run.path.filename()) + ")");
            continue;
        }
        const AVRational ms{1, static_cast<int>(kTicksPerSecond / 1000)};
        const auto targetOf = [&](Ticks time) {
            return file.startTime + av_rescale_q_rnd(time.value / 1000, ms, file.timeBase, AV_ROUND_NEAR_INF);
        };
        GopScanInputs scan;
        scan.ctx = file.ctx.get();
        scan.stream = file.stream;
        scan.codec = file.codec;
        scan.lengthSize = file.lengthSize;
        scan.period = file.period;
        scan.from = targetOf(run.times.front());
        scan.to = targetOf(run.times.back());
        scan.accepted = &accepted;
        auto gops = scanGops(scan, stop);
        if (!gops) {
            markRun(gops.error().code == ErrorCode::Cancelled ? "Cancelled" : "The source could not be read: " + gops.error().message);
            continue;
        }
        // The pictures the scan saw, by presentation time, and the frame each output frame shows.
        std::vector<std::int64_t> shown;
        for (const SourceGop& g : *gops) {
            shown.insert(shown.end(), g.pts.begin(), g.pts.end());
        }
        std::sort(shown.begin(), shown.end());
        std::vector<std::int64_t> ptsOf(static_cast<std::size_t>(n), AV_NOPTS_VALUE);
        for (std::int64_t i = 0; i < n; ++i) {
            const auto above = std::upper_bound(shown.begin(), shown.end(), targetOf(run.times[static_cast<std::size_t>(i)]));
            if (above != shown.begin()) {
                ptsOf[static_cast<std::size_t>(i)] = *(above - 1);
            }
        }
        // Why each scanned GOP cannot be copied ("" = it can), in the order of the stream.
        std::vector<std::string> gopProblem(gops->size());
        std::int64_t previousMax = std::numeric_limits<std::int64_t>::min();
        for (std::size_t g = 0; g < gops->size(); ++g) {
            const SourceGop& gop = (*gops)[g];
            std::string& why = gopProblem[g];
            if (!gop.headersOk) {
                why = "Headers or times inside the source GOP are not usable";
            } else if (!gop.contiguous) {
                why = "Missing or irregular frame times in the source";
            } else if (gop.frames() > kMaxCopiedGopFrames) {
                why = "A source GOP too long to copy";
            } else if (gop.reorderFrames(file.period) > kMaxCopiedReorder) {
                why = "A source GOP with too much picture reordering";
            } else if (previousMax >= gop.minPts && previousMax != std::numeric_limits<std::int64_t>::min()) {
                why = "An open GOP in the source (pictures shown before its IDR)";
            } else if (rateLimit) {
                const double seconds = static_cast<double>(gop.frames()) * static_cast<double>(file.period) *
                                       static_cast<double>(file.timeBase.num) / static_cast<double>(file.timeBase.den);
                if (seconds > 0 && static_cast<double>(gop.bytes) * 8.0 / seconds > *rateLimit) {
                    why = "The source's bitrate is above the export's";
                }
            }
            previousMax = gop.maxPts;
        }
        // Sub-runs: consecutive output frames that show consecutive source pictures.
        std::int64_t a = 0;
        while (a < n) {
            if (ptsOf[static_cast<std::size_t>(a)] == AV_NOPTS_VALUE) {
                reasonOf[static_cast<std::size_t>(run.firstFrame + a)] = reasonId("Before the first IDR picture of the source");
                ++a;
                continue;
            }
            std::int64_t b = a;
            while (b + 1 < n && ptsOf[static_cast<std::size_t>(b + 1)] == ptsOf[static_cast<std::size_t>(b)] + file.period) {
                ++b;
            }
            const std::int64_t pa = ptsOf[static_cast<std::size_t>(a)];
            const std::int64_t pb = ptsOf[static_cast<std::size_t>(b)];
            std::int64_t firstCopied = -1;
            for (std::size_t g = 0; g < gops->size(); ++g) {
                const SourceGop& gop = (*gops)[g];
                if (!gopProblem[g].empty() || gop.minPts < pa || gop.maxPts > pb || (gop.minPts - pa) % file.period != 0) {
                    continue;
                }
                const std::int64_t start = run.firstFrame + a + (gop.minPts - pa) / file.period;
                if (file.planIndex < 0) {
                    const auto known = planIndex.find(run.key);
                    if (known == planIndex.end()) {
                        plan.m_files.push_back(run.path);
                        file.planIndex = static_cast<int>(plan.m_files.size()) - 1;
                        planIndex.emplace(run.key, file.planIndex);
                    } else {
                        file.planIndex = known->second;
                    }
                }
                CopyGop copy;
                copy.file = file.planIndex;
                copy.stream = file.stream;
                copy.lengthSize = file.lengthSize;
                copy.annexB = exportLengthSize == 0;
                copy.keyDts = gop.keyDts;
                copy.pts = gop.pts;
                copy.minPts = gop.minPts;
                copy.period = file.period;
                copy.firstFrame = start;
                copy.reorder = gop.reorderFrames(file.period);
                plan.m_maxReorder = std::max(plan.m_maxReorder, copy.reorder);
                const int index = static_cast<int>(plan.m_gops.size());
                for (std::int64_t j = 0; j < static_cast<std::int64_t>(gop.frames()); ++j) {
                    plan.m_frames[static_cast<std::size_t>(start + j)] = CopyFrame{copy.file, copy.stream, 0, false, index};
                    reasonOf[static_cast<std::size_t>(start + j)] = -1;
                }
                plan.m_copied += static_cast<std::int64_t>(gop.frames());
                if (firstCopied < 0) {
                    firstCopied = start;
                }
                plan.m_gops.push_back(std::move(copy));
            }
            // The frames of the sub-run that were not copied, and why.
            for (std::int64_t i = a; i <= b; ++i) {
                const std::int64_t k = run.firstFrame + i;
                if (reasonOf[static_cast<std::size_t>(k)] < 0) {
                    continue;
                }
                std::string why;
                const std::int64_t p = ptsOf[static_cast<std::size_t>(i)];
                for (std::size_t g = 0; g < gops->size(); ++g) {
                    if (p >= (*gops)[g].minPts && p <= (*gops)[g].maxPts) {
                        why = gopProblem[g];
                        break;
                    }
                }
                if (why.empty()) {
                    why = firstCopied < 0 ? "Shorter than a whole source GOP"
                          : k < firstCopied ? "The clip starts inside a GOP"
                                            : "The clip ends inside a GOP";
                }
                reasonOf[static_cast<std::size_t>(k)] = reasonId(why);
            }
            a = b + 1;
        }
    }
    finish();
}

// ---- Copying -------------------------------------------------------------------------------------------------------

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

PacketCopier::Input& PacketCopier::input(int file, std::string& path, std::expected<void, Error>& status) {
    auto& slot = m_inputs[static_cast<std::size_t>(file)];
    path = pathToUtf8(m_plan.files()[static_cast<std::size_t>(file)]);
    if (!slot) {
        slot = std::make_unique<Input>();
        AVFormatContext* raw = nullptr;
        if (const int r = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); r < 0) {
            status = std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot open " + path, r));
            return *slot;
        }
        slot->ctx.reset(raw);
        if (const int r = avformat_find_stream_info(raw, nullptr); r < 0) {
            status = std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot read " + path, r));
        }
    }
    if (!slot->ctx && status) {
        status = std::unexpected(Error{ErrorCode::IoError, "Cannot open " + path});
    }
    return *slot;
}

std::expected<PacketPtr, Error> PacketCopier::packet(const CopyFrame& copy, std::int64_t k) {
    if (copy.file < 0 || copy.file >= static_cast<int>(m_inputs.size())) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No such copy source"});
    }
    std::string path;
    std::expected<void, Error> status;
    Input& src = input(copy.file, path, status);
    if (!status) {
        return std::unexpected(status.error());
    }
    AVFormatContext* ctx = src.ctx.get();
    if (src.lastRead == AV_NOPTS_VALUE || copy.timestamp <= src.lastRead) {
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
        src.lastRead = packet->pts;
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

std::expected<void, Error> PacketCopier::copyGop(const CopyGop& gop, Muxer& muxer, AVStream* stream, AVRational timeBase,
                                                  std::int64_t delay, const std::stop_token& stop) {
    if (gop.file < 0 || gop.file >= static_cast<int>(m_inputs.size()) || gop.pts.empty() || gop.period <= 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No such copy source"});
    }
    std::string path;
    std::expected<void, Error> status;
    Input& in = input(gop.file, path, status);
    if (!status) {
        return std::unexpected(status.error());
    }
    AVFormatContext* ctx = in.ctx.get();
    in.lastRead = AV_NOPTS_VALUE;
    if (const int r = av_seek_frame(ctx, gop.stream, gop.keyDts, AVSEEK_FLAG_BACKWARD); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot seek in " + path, r));
    }
    PacketPtr packet(av_packet_alloc());
    const std::string missing = "A smart-rendered GOP is not in " + path + " any more";
    std::size_t i = 0;
    int skipped = 0;
    while (i < gop.pts.size()) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Export cancelled"});
        }
        const int r = av_read_frame(ctx, packet.get());
        if (r < 0) {
            return std::unexpected(Error{ErrorCode::DecoderError, missing});
        }
        if (packet->stream_index != gop.stream) {
            av_packet_unref(packet.get());
            continue;
        }
        if (i == 0 && packet->pts != gop.pts[0]) {
            // The seek landed on an earlier key packet: read on to the GOP's IDR.
            av_packet_unref(packet.get());
            if (++skipped > 100000) {
                return std::unexpected(Error{ErrorCode::DecoderError, missing});
            }
            continue;
        }
        if (packet->pts != gop.pts[i]) {
            av_packet_unref(packet.get());
            return std::unexpected(Error{ErrorCode::DecoderError, missing});
        }
        const std::int64_t pts = gop.firstFrame + (gop.pts[i] - gop.minPts) / gop.period;
        const std::int64_t dts = gop.firstFrame + static_cast<std::int64_t>(i) - delay;
        if (pts < dts) {
            av_packet_unref(packet.get());
            return std::unexpected(Error{ErrorCode::EncoderError, "A copied GOP reorders more pictures than the file allows for"});
        }
        PacketPtr out;
        if (gop.annexB) {
            // The muxer reads the stream as Annex B (see CopyGop::annexB): the same NAL units behind start codes.
            const std::vector<std::uint8_t> converted =
                toAnnexB(gop.lengthSize, std::span<const std::uint8_t>(packet->data, static_cast<std::size_t>(packet->size)));
            out.reset(av_packet_alloc());
            if (converted.empty() || !out || av_new_packet(out.get(), static_cast<int>(converted.size())) < 0) {
                av_packet_unref(packet.get());
                return std::unexpected(Error{ErrorCode::DecoderError, missing});
            }
            std::memcpy(out->data, converted.data(), converted.size());
            out->flags = packet->flags;
            av_packet_unref(packet.get());
        } else {
            out = std::move(packet);
            packet.reset(av_packet_alloc());
            av_packet_free_side_data(out.get()); // a source's side data (new extradata, skip samples) is not the file's
        }
        out->pts = pts;
        out->dts = dts;
        out->duration = 1;
        out->pos = -1;
        if (auto ok = muxer.write(out.get(), timeBase, stream); !ok) {
            return ok;
        }
        ++i;
    }
    return {};
}

std::expected<void, Error> drainSegment(AVCodecContext* encoder, AVPacket* packet, Muxer& muxer, AVStream* stream,
                                        bool flushing, SegmentStamp& stamp) {
    for (;;) {
        const int r = avcodec_receive_packet(encoder, packet);
        if (r == AVERROR(EAGAIN)) {
            if (flushing) {
                return std::unexpected(Error{ErrorCode::EncoderError, "Encoder stalled while flushing"});
            }
            return {};
        }
        if (r == AVERROR_EOF) {
            return {};
        }
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Encoding failed", r));
        }
        packet->duration = 1;
        const std::int64_t dts = stamp.start + stamp.index - stamp.delay;
        if (packet->pts == AV_NOPTS_VALUE || packet->pts < dts) {
            av_packet_unref(packet);
            return std::unexpected(Error{ErrorCode::EncoderError, "The encoder reorders more pictures than the file allows for"});
        }
        packet->dts = dts;
        ++stamp.index;
        if (auto ok = muxer.write(packet, encoder->time_base, stream); !ok) {
            return ok;
        }
    }
}

std::int64_t encoderReorderDelay(const AVCodecContext* encoder) {
    // B-frames and their pyramid, with room to spare: libx264 and libx265 decide their depth from the preset and do not
    // always say so in has_b_frames. A larger delay only moves the file's decode times earlier (an edit list hides it).
    const int known = std::max(encoder->has_b_frames, encoder->max_b_frames);
    return std::clamp<std::int64_t>(std::max(known, 3) + 2, 5, 18);
}

} // namespace wabcut::exporter::detail
