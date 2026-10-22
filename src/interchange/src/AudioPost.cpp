// Audio post export plan, rendering and audio file layout (AudioPost.h).

#include "AudioPost.h"

#include "Exporters.h"
#include "TextUtil.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace wabcut::interchange::detail::audiopost {

// ---------------------------------------------------------------------------------------------------------------------
// Identifiers and names

std::array<std::uint8_t, 16> stableId(std::string_view text) {
    auto fnv = [&](std::uint64_t h) {
        for (const char c : text) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 0x100000001b3ull;
        }
        return h;
    };
    const std::uint64_t a = fnv(0xcbf29ce484222325ull);
    const std::uint64_t b = fnv(0x84222325cbf29ce4ull ^ 0x9e3779b97f4a7c15ull);
    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(a >> (8 * i));
        out[static_cast<std::size_t>(8 + i)] = static_cast<std::uint8_t>(b >> (8 * i));
    }
    out[6] = static_cast<std::uint8_t>((out[6] & 0x0F) | 0x50); // a name-based UUID (version 5 layout)
    out[8] = static_cast<std::uint8_t>((out[8] & 0x3F) | 0x80); // RFC 4122 variant
    return out;
}

std::string safeFileName(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.size() > 120) {
        std::size_t cut = 120;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut; // not inside a UTF-8 sequence
        }
        out.resize(cut);
    }
    return out.empty() ? std::string("Audio") : out;
}

std::pair<std::int32_t, std::int32_t> amplitudeRational(double db) {
    constexpr std::int32_t kDen = 1 << 24;
    const double amp = std::clamp(std::pow(10.0, db / 20.0), 0.0, 127.0);
    return {static_cast<std::int32_t>(std::llround(amp * kDen)), kDen};
}

// ---------------------------------------------------------------------------------------------------------------------
// Audio files

namespace {

void putLe(std::string& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}
void putBe(std::string& out, std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}
void putFixed(std::string& out, std::string_view text, std::size_t size) {
    const std::size_t n = std::min(text.size(), size);
    out.append(text.substr(0, n));
    out.append(size - n, '\0');
}

} // namespace

std::array<std::uint8_t, 10> extended80(double value) {
    std::array<std::uint8_t, 10> out{};
    if (!(value > 0.0) || !std::isfinite(value)) {
        return out;
    }
    int exponent = 0;
    const double fraction = std::frexp(value, &exponent); // value = fraction * 2^exponent, fraction in [0.5, 1)
    const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
    const int biased = exponent - 1 + 16383;
    out[0] = static_cast<std::uint8_t>((biased >> 8) & 0x7F);
    out[1] = static_cast<std::uint8_t>(biased & 0xFF);
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(2 + i)] = static_cast<std::uint8_t>(mantissa >> (8 * (7 - i)));
    }
    return out;
}

std::string fileHeader(FileKind kind, int channels, int sampleRate, int bits, std::uint64_t frames, const Bext* bext) {
    const std::uint64_t blockAlign = static_cast<std::uint64_t>(channels) * static_cast<std::uint64_t>(bits / 8);
    const std::uint64_t dataBytes = frames * blockAlign;
    const std::uint64_t pad = dataBytes % 2;
    std::string out;
    if (kind == FileKind::Wave) {
        constexpr std::uint64_t kBextSize = 602;
        const std::uint64_t riffSize = 4 + (bext ? 8 + kBextSize : 0) + (8 + 16) + (8 + dataBytes + pad);
        out += "RIFF";
        putLe(out, std::min<std::uint64_t>(riffSize, 0xFFFFFFFFull), 4);
        out += "WAVE";
        if (bext) {
            out += "bext";
            putLe(out, kBextSize, 4);
            putFixed(out, bext->description, 256);
            putFixed(out, bext->originator, 32);
            putFixed(out, bext->originatorReference, 32);
            putFixed(out, bext->originationDate, 10);
            putFixed(out, bext->originationTime, 8);
            putLe(out, bext->timeReference & 0xFFFFFFFFull, 4);
            putLe(out, bext->timeReference >> 32, 4);
            putLe(out, 1, 2); // version 1
            out.append(reinterpret_cast<const char*>(bext->umid.data()), bext->umid.size());
            out.append(32, '\0');  // the extended half of a 64-byte UMID
            out.append(190, '\0'); // reserved
        }
        out += "fmt ";
        putLe(out, 16, 4);
        putLe(out, 1, 2); // PCM
        putLe(out, static_cast<std::uint64_t>(channels), 2);
        putLe(out, static_cast<std::uint64_t>(sampleRate), 4);
        putLe(out, static_cast<std::uint64_t>(sampleRate) * blockAlign, 4);
        putLe(out, blockAlign, 2);
        putLe(out, static_cast<std::uint64_t>(bits), 2);
        out += "data";
        putLe(out, std::min<std::uint64_t>(dataBytes, 0xFFFFFFFFull), 4);
        return out;
    }
    const bool aifc = kind == FileKind::Aifc;
    constexpr std::string_view kCompressionName = "not compressed"; // 14 bytes: count + text = 15, padded to 16
    const std::uint64_t commSize = aifc ? 18 + 4 + 16 : 18;
    const std::uint64_t formSize = 4 + (aifc ? 8 + 4 : 0) + (8 + commSize) + (8 + 8 + dataBytes + pad);
    out += "FORM";
    putBe(out, std::min<std::uint64_t>(formSize, 0xFFFFFFFFull), 4);
    out += aifc ? "AIFC" : "AIFF";
    if (aifc) {
        out += "FVER";
        putBe(out, 4, 4);
        putBe(out, 0xA2805140u, 4); // AIFC version 1
    }
    out += "COMM";
    putBe(out, commSize, 4);
    putBe(out, static_cast<std::uint64_t>(channels), 2);
    putBe(out, std::min<std::uint64_t>(frames, 0xFFFFFFFFull), 4);
    putBe(out, static_cast<std::uint64_t>(bits), 2);
    const auto rate = extended80(static_cast<double>(sampleRate));
    out.append(reinterpret_cast<const char*>(rate.data()), rate.size());
    if (aifc) {
        out += "NONE";
        out.push_back(static_cast<char>(kCompressionName.size()));
        out += kCompressionName;
        out.push_back('\0'); // pad the pascal string to an even length
    }
    out += "SSND";
    putBe(out, std::min<std::uint64_t>(8 + dataBytes, 0xFFFFFFFFull), 4);
    putBe(out, 0, 4); // offset
    putBe(out, 0, 4); // block size
    return out;
}

std::string fileTrailer(std::uint64_t dataBytes) { return dataBytes % 2 == 1 ? std::string(1, '\0') : std::string(); }

void encodeSamples(std::span<const float> samples, int bits, bool bigEndian, std::string& out) {
    const double scale = bits == 16 ? 32767.0 : 8388607.0;
    const int bytes = bits / 8;
    for (const float s : samples) {
        const double x = std::isfinite(s) ? std::clamp(static_cast<double>(s), -1.0, 1.0) : 0.0;
        const auto v = static_cast<std::int32_t>(std::floor(x * scale + 0.5));
        const auto u = static_cast<std::uint32_t>(v);
        if (bigEndian) {
            for (int i = bytes - 1; i >= 0; --i) {
                out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
            }
        } else {
            for (int i = 0; i < bytes; ++i) {
                out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Planning

namespace {

std::string trackLabel(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
}

std::string trackName(const Track& track, TrackKind kind, std::size_t index) {
    if (!track.name.empty()) {
        return track.name;
    }
    return (kind == TrackKind::Video ? "Video " : "Audio ") + std::to_string(index + 1);
}

/// "Left" / "Right" (stereo), "L R C LFE Ls Rs" (5.1), "1".."n" (adaptive).
std::string channelSuffix(int channels, int channel) {
    if (channels == 2) {
        return channel == 0 ? "Left" : "Right";
    }
    if (channels == 6) {
        static constexpr std::string_view kNames[6] = {"L", "R", "C", "LFE", "Ls", "Rs"};
        return std::string(kNames[channel]);
    }
    return std::to_string(channel + 1);
}

/// AAF pan of a split channel: hard left / right, the centre channels in the middle.
double channelPan(int channels, int channel) {
    if (channels == 2) {
        return channel == 0 ? 0.0 : 1.0;
    }
    if (channels == 6) {
        static constexpr double kPan[6] = {0.0, 1.0, 0.5, 0.5, 0.0, 1.0};
        return kPan[channel];
    }
    return 0.5;
}

std::string mediaPath(const MediaItem& media, const std::filesystem::path& projectFile) {
    if (!media.locator.path.empty() && isAbsoluteOnAnyPlatform(media.locator.path)) {
        return media.locator.path;
    }
    if (!media.locator.relativePath.empty() && !projectFile.empty()) {
        return pathToUtf8((projectFile.parent_path() / pathFromUtf8(media.locator.relativePath)).lexically_normal());
    }
    return !media.locator.path.empty() ? media.locator.path : media.info.path;
}

bool linearSide(Interpolation i) { return i == Interpolation::Linear; }

/// Timeline times where a parameter curve must be sampled to follow it with straight segments over [from, to): the ends,
/// every key inside, and points along each segment that is not a straight line (eases, Bezier, holds).
template <class TimelineOf>
std::vector<Ticks> curveSampleTimes(const Curve& curve, Ticks from, Ticks to, Ticks minStep, TimelineOf&& timelineOf) {
    std::vector<Ticks> out{from, to - Ticks{1}};
    for (std::size_t i = 0; i < curve.keys.size(); ++i) {
        const auto t = timelineOf(curve.keys[i].time);
        if (t && *t > from && *t < to) {
            out.push_back(*t);
        }
        if (i + 1 >= curve.keys.size()) {
            continue;
        }
        const Keyframe& a = curve.keys[i];
        const Keyframe& b = curve.keys[i + 1];
        const bool straight = linearSide(a.interpolation) &&
                              (b.interpolation == Interpolation::Linear || b.interpolation == Interpolation::EaseOut ||
                               b.interpolation == Interpolation::Hold);
        if (straight) {
            continue;
        }
        const auto ta = timelineOf(a.time);
        const auto tb = timelineOf(b.time);
        if (!ta || !tb || *tb <= *ta) {
            continue;
        }
        if (a.interpolation == Interpolation::Hold) {
            const Ticks before = *tb - Ticks{1};
            if (before > from && before < to) {
                out.push_back(before);
            }
            continue;
        }
        const Ticks span = *tb - *ta;
        const std::int64_t steps = std::clamp<std::int64_t>(span.value / std::max<std::int64_t>(minStep.value, 1), 2, 32);
        for (std::int64_t k = 1; k < steps; ++k) {
            const Ticks at{ta->value + span.value * k / steps};
            if (at > from && at < to) {
                out.push_back(at);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.size() > 4096) {
        out.resize(4096); // a pathological curve: the rest stays at the last value
    }
    return out;
}

} // namespace

std::expected<Plan, Error> makePlan(const ProjectState& project, SequenceId sequenceId, const PlanOptions& options,
                                    ImportReport& report, const std::string& fileLabel) {
    const Sequence* seq = project.findSequence(sequenceId);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "sequence " + std::to_string(sequenceId.value) + " does not exist"});
    }
    Plan plan;
    plan.sequenceName = seq->name;
    plan.frameRate = seq->frameRate;
    plan.dropFrame = seq->dropFrame;
    plan.sampleRate = options.audio.sampleRate;
    plan.bits = options.audio.bitsPerSample;
    plan.startTimecodeFrames = ticksToFrames(seq->startTimecode, seq->frameRate);
    const Ticks seqLength = std::max(seq->duration, seq->contentEnd());
    plan.length = plan.samples(seqLength);
    plan.lengthFrames = ticksToFrames(seqLength + framesToTicks(1, seq->frameRate) - Ticks{1}, seq->frameRate);
    const Rate audioRate{plan.sampleRate, 1};

    auto location = [&](TrackKind kind, std::size_t track, const std::string& what, Ticks at) {
        SourceLocation loc;
        loc.file = fileLabel;
        loc.element = "'" + seq->name + "' " + trackLabel(kind, track) + (what.empty() ? "" : " '" + what + "'") +
                      " at " + timecodeLabel(at + seq->startTimecode, seq->frameRate, seq->dropFrame);
        return loc;
    };
    auto add = [&](ReportOutcome outcome, ReportCategory category, std::string reason, SourceLocation where) {
        report.add(outcome, category, std::move(reason), std::move(where), seq->name);
    };
    auto sequenceLocation = [&]() {
        SourceLocation loc;
        loc.file = fileLabel;
        loc.element = "'" + seq->name + "'";
        return loc;
    };

    // --- Sequence-level things an audio-post file has no place for ---------------------------------------------
    if (!seq->audioEffects.empty()) {
        add(ReportOutcome::Dropped, ReportCategory::Effect,
            "The master track's audio effects are not written: the mix is done in the audio application",
            sequenceLocation());
    }
    if (!(seq->masterVolume == ParamValue(0.0))) {
        add(ReportOutcome::Dropped, ReportCategory::SequenceSetting, "The master fader is not written", sequenceLocation());
    }
    if (seq->masterFormat == ChannelFormat::Surround51) {
        add(ReportOutcome::Approximated, ReportCategory::AudioMapping,
            "The 5.1 master is not written: each track keeps its own channels (its 5.1 panner is left out)",
            sequenceLocation());
    }

    // --- Markers ---------------------------------------------------------------------------------------------------
    for (const Marker& m : seq->markers) {
        PlanMarker pm;
        pm.frame = plan.frames(m.start);
        pm.lengthFrames = m.duration.value > 0 ? std::max<std::int64_t>(1, plan.frames(m.end()) - pm.frame) : 0;
        pm.name = m.name;
        pm.comment = markerCommentForExport(m);
        pm.colorIndex = m.colorIndex;
        plan.markers.push_back(std::move(pm));
    }

    // --- Video tracks (linked to their original media) -----------------------------------------------------------
    if (options.video) {
        std::map<std::uint64_t, std::size_t> videoSourceOf;
        for (std::size_t ti = 0; ti < seq->videoTracks.size(); ++ti) {
            const Track& track = *seq->videoTracks[ti];
            VideoTrack vt;
            vt.name = trackName(track, TrackKind::Video, ti);
            for (const Clip& clip : track.clips) {
                const MediaItem* media = clip.isNested() ? nullptr : project.findMedia(clip.media);
                const std::string name = !clip.name().empty() ? clip.name() : media ? media->name : std::string("Clip");
                const SourceLocation where = location(TrackKind::Video, ti, name, clip.start());
                if (!clip.enabled) {
                    add(ReportOutcome::Dropped, ReportCategory::Clip, "Disabled clip left out", where);
                    continue;
                }
                if (!media || media->isGenerator() || media->info.video.empty()) {
                    add(ReportOutcome::Dropped, ReportCategory::Clip,
                        clip.isNested() ? "Nested sequence clips are not linked (use Mixdown Video to see them)"
                                        : "Titles, graphics and generated clips are not linked (use Mixdown Video to see them)",
                        where);
                    continue;
                }
                auto [it, inserted] = videoSourceOf.emplace(media->id.value, plan.videoSources.size());
                if (inserted) {
                    VideoSource vs;
                    vs.name = media->name;
                    vs.path = mediaPath(*media, options.projectFile);
                    vs.material = stableId("video\n" + vs.path + "\n" + std::to_string(media->id.value));
                    vs.width = media->info.video.front().width;
                    vs.height = media->info.video.front().height;
                    vs.frameRate = media->info.video.front().frameRate.isValid() ? media->info.video.front().frameRate
                                                                                 : seq->frameRate;
                    const auto extent = media->sourceExtent(clip.stream);
                    vs.lengthFrames = extent ? plan.frames(*extent) : plan.frames(clip.sourceOut());
                    plan.videoSources.push_back(std::move(vs));
                }
                VideoClip vc;
                vc.name = name;
                vc.start = plan.frames(clip.start());
                vc.length = plan.frames(clip.end()) - vc.start;
                vc.source = it->second;
                vc.sourceStart = plan.frames(clip.sourceIn);
                vc.where = where.element;
                if (vc.length <= 0) {
                    continue;
                }
                if (!clip.speed.isUnity() || clip.reversed || clip.isRemapped()) {
                    add(ReportOutcome::Approximated, ReportCategory::Speed,
                        "The video plays at 100 % in the AAF (its speed is not written)", where);
                }
                vt.clips.push_back(std::move(vc));
            }
            if (!track.transitions.empty()) {
                add(ReportOutcome::Dropped, ReportCategory::Transition,
                    "Video transitions are not written (cuts in the AAF)", location(TrackKind::Video, ti, {}, Ticks{}));
            }
            plan.videoTracks.push_back(std::move(vt));
        }
    }

    // --- Audio tracks ------------------------------------------------------------------------------------------------
    const Ticks handleTicks = options.audio.render == AudioPostRender::TrimAudioFiles
                                  ? framesToTicks(options.audio.handleFrames, seq->frameRate)
                                  : Ticks{};
    std::map<std::string, std::size_t> sharedGroups; // Copy Complete: one render per media file and channel layout
    std::map<std::string, std::size_t> linkedSources;

    for (std::size_t ti = 0; ti < seq->audioTracks.size(); ++ti) {
        const Track& track = *seq->audioTracks[ti];
        const std::string baseName = trackName(track, TrackKind::Audio, ti);
        const SourceLocation trackWhere = location(TrackKind::Audio, ti, {}, Ticks{});
        int channels = 1;
        if (options.breakout) {
            channels = channelCount(track.format);
            if (channels == 0) { // adaptive: as many channels as its widest clip
                channels = 2;
                for (const Clip& c : track.clips) {
                    if (const AudioChannelMap* m = c.channelMap()) {
                        channels = std::max(channels, m->channelCount());
                    }
                }
            }
        }
        if (track.muted || !track.enabled) {
            add(ReportOutcome::Approximated, ReportCategory::Track,
                "The track is muted in Wabcut; files carry no track mute, so it plays in the audio application", trackWhere);
        }
        for (const EffectInstance& fx : track.effects) {
            if (fx.enabled) {
                const EffectDescriptor* d = EffectRegistry::builtin().find(fx.effect);
                add(ReportOutcome::Dropped, ReportCategory::Effect,
                    "Track effect " + std::string(d ? d->displayName : std::string_view(fx.effect)) + " is not written",
                    trackWhere);
            }
        }
        if (!track.surround.isDefault()) {
            add(ReportOutcome::Dropped, ReportCategory::AudioMapping, "The track's 5.1 panner is not written", trackWhere);
        }

        const bool anyEffects = std::any_of(track.clips.begin(), track.clips.end(), [](const Clip& c) {
            return std::any_of(c.effects().begin(), c.effects().end(), [](const EffectInstance& e) { return e.enabled; });
        });
        const bool cleanSet = options.breakout && options.clipEffects && options.cleanCopies && anyEffects;
        const int sets = cleanSet ? 2 : 1;
        const std::size_t firstTrack = plan.tracks.size();
        for (int set = 0; set < sets; ++set) {
            for (int ch = 0; ch < channels; ++ch) {
                PlanTrack pt;
                pt.track = track.id;
                pt.channel = options.breakout ? ch : -1;
                pt.name = baseName + (options.breakout && channels > 1 ? " " + channelSuffix(channels, ch) : "") +
                          (cleanSet ? (set == 0 ? " FX" : " No FX") : "");
                plan.tracks.push_back(std::move(pt));
            }
        }

        // Track volume and pan (timeline time).
        auto paramGain = [&](const ParamValue& v, bool isPan) {
            Gain g;
            Pan p;
            if (const Curve* curve = v.curve(); curve && options.trackAutomation) {
                const auto times = curveSampleTimes(*curve, Ticks{}, std::max(seqLength, Ticks{1}),
                                                    framesToTicks(1, seq->frameRate),
                                                    [](Ticks t) { return std::optional<Ticks>(t); });
                for (const Ticks t : times) {
                    const double value = curve->valueAt(t);
                    if (isPan) {
                        p.points.emplace_back(plan.samples(t), (std::clamp(value, -1.0, 1.0) + 1.0) / 2.0);
                    } else {
                        g.points.emplace_back(plan.samples(t), value);
                    }
                }
            } else {
                const double value = v.constant();
                if (isPan) {
                    p.constant = (std::clamp(value, -1.0, 1.0) + 1.0) / 2.0;
                } else {
                    g.constantDb = value;
                }
                if (v.isCurve()) {
                    add(ReportOutcome::Approximated, ReportCategory::Track,
                        std::string(isPan ? "Track pan" : "Track volume") + " keyframes are written as its value at the start",
                        trackWhere);
                }
            }
            return std::pair{g, p};
        };
        const Gain trackVolume = paramGain(track.volume, false).first;
        const Pan trackPan = paramGain(track.pan, true).second;
        for (std::size_t k = firstTrack; k < plan.tracks.size(); ++k) {
            PlanTrack& pt = plan.tracks[k];
            pt.volume = trackVolume;
            if (options.breakout && channels > 1) {
                pt.pan.constant = channelPan(channels, pt.channel);
            } else {
                pt.pan = trackPan;
            }
        }
        if (options.breakout && channels > 1 && !(track.pan == ParamValue(0.0))) {
            add(ReportOutcome::Approximated, ReportCategory::Track,
                "The track's balance is not written: its Left and Right tracks are panned hard left and right", trackWhere);
        }

        // Clips.
        std::map<std::uint64_t, std::vector<std::size_t>> refsOfClip; // clip id -> index of its ClipRef in each track
        for (const Clip& clip : track.clips) {
            const MediaItem* media = clip.isNested() ? nullptr : project.findMedia(clip.media);
            const Sequence* nested = clip.isNested() ? project.findSequence(clip.sequence) : nullptr;
            const std::string name = !clip.name().empty() ? clip.name()
                                     : media                ? media->name
                                     : nested               ? nested->name
                                                            : std::string("Clip");
            const SourceLocation where = location(TrackKind::Audio, ti, name, clip.start());
            if (!clip.enabled) {
                add(ReportOutcome::Dropped, ReportCategory::Clip, "Disabled clip left out", where);
                continue;
            }
            if (clip.hasUnknownSource() || (!media && !nested) || (media && (media->isGenerator() || media->info.audio.empty()))) {
                add(ReportOutcome::Dropped, ReportCategory::Clip, "The clip has no audio to write", where);
                continue;
            }
            if (!options.breakout && nested) {
                add(ReportOutcome::Dropped, ReportCategory::Clip,
                    "Nested and multicam clips need Breakout to Mono (their mix is rendered to new files)", where);
                continue;
            }
            const std::int64_t start = plan.samples(clip.start());
            const std::int64_t length = plan.samples(clip.end()) - start;
            if (length <= 0) {
                continue;
            }
            // Clip gain (curve time -> timeline time).
            Gain gain;
            const ParamValue& volume = clip.properties()[ParamId::VolumeDb];
            if (const Curve* curve = volume.curve()) {
                const auto times = curveSampleTimes(*curve, clip.start(), clip.end(), framesToTicks(1, seq->frameRate),
                                                    [&](Ticks curveTime) { return clip.timelineTimeOfCurveTime(curveTime); });
                for (const Ticks t : times) {
                    gain.points.emplace_back(plan.samples(t) - start, clip.parameterAt(ParamId::VolumeDb, t));
                }
                const bool flat = std::all_of(gain.points.begin(), gain.points.end(),
                                              [&](const auto& p) { return p.second == gain.points.front().second; });
                if (flat && !gain.points.empty()) {
                    gain.constantDb = gain.points.front().second;
                    gain.points.clear();
                }
            } else {
                gain.constantDb = volume.constant();
            }
            const bool hasEffects =
                std::any_of(clip.effects().begin(), clip.effects().end(), [](const EffectInstance& e) { return e.enabled; });
            if (hasEffects && !(options.breakout && options.clipEffects)) {
                std::string names;
                for (const EffectInstance& e : clip.effects()) {
                    if (e.enabled) {
                        const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
                        names += (names.empty() ? "" : ", ") + std::string(d ? d->displayName : std::string_view(e.effect));
                    }
                }
                add(ReportOutcome::Dropped, ReportCategory::Effect,
                    "Clip effects are not written (" + names + "): turn on Render Audio Clip Effects to hear them", where);
            }

            if (!options.breakout) {
                // Linked: the clip plays its original file, channels as they are.
                const std::string path = mediaPath(*media, options.projectFile);
                const std::string key = path + "\n" + std::to_string(clip.stream.index);
                auto [it, inserted] = linkedSources.emplace(key, plan.sources.size());
                if (inserted) {
                    const AudioStreamInfo* info = clip.stream.index >= 0 &&
                                                          clip.stream.index < static_cast<int>(media->info.audio.size())
                                                      ? &media->info.audio[static_cast<std::size_t>(clip.stream.index)]
                                                      : nullptr;
                    Source s;
                    s.rendered = false;
                    s.name = media->name + (media->info.audio.size() > 1 ? " A" + std::to_string(clip.stream.index + 1) : "");
                    s.originalPath = path;
                    s.material = stableId("linked\n" + key);
                    s.channels = info ? std::max(1, info->channels) : 2;
                    s.sampleRate = info && info->sampleRate.isValid() && info->sampleRate.den == 1 ? info->sampleRate.num
                                                                                                   : plan.sampleRate;
                    s.bits = 16;
                    const auto extent = media->sourceExtent(clip.stream);
                    s.length = extent ? plan.samples(*extent) : plan.samples(clip.sourceOut());
                    plan.sources.push_back(std::move(s));
                }
                if (!clip.speed.isUnity() || clip.reversed || clip.isRemapped()) {
                    add(ReportOutcome::Approximated, ReportCategory::Speed,
                        "The clip plays at 100 % from its In point (turn on Breakout to Mono to render its speed)", where);
                }
                if (clip.channelMap() || !(clip.properties()[ParamId::Pan] == ParamValue(0.0))) {
                    add(ReportOutcome::Approximated, ReportCategory::AudioMapping,
                        "The clip's channel map and pan are not written (turn on Breakout to Mono to render them)", where);
                }
                ClipRef ref;
                ref.name = name;
                ref.start = start;
                ref.length = length;
                ref.source = it->second;
                ref.sourceStart = plan.samples(clip.sourceIn);
                ref.gain = gain;
                ref.clip = clip.id;
                ref.where = where.element;
                refsOfClip[clip.id.value].push_back(plan.tracks[firstTrack].clips.size());
                plan.tracks[firstTrack].clips.push_back(std::move(ref));
                add(ReportOutcome::Carried, ReportCategory::Clip, "Clip linked to its original file", where);
                continue;
            }

            // Breakout: the clip's audio rendered, one mono file per channel. Handles: the Trim setting, at least
            // what the clip's transitions need, within the media there is.
            Ticks needBefore{};
            Ticks needAfter{};
            for (const Transition& tr : track.transitions) {
                if (tr.incoming == clip.id && tr.outgoing.isValid()) {
                    needBefore = std::max(needBefore, tr.before);
                }
                if (tr.outgoing == clip.id && tr.incoming.isValid()) {
                    needAfter = std::max(needAfter, tr.after);
                }
            }
            std::optional<Ticks> headRoom;
            std::optional<Ticks> tailRoom;
            if (nested) {
                const Ticks nestedLength = std::max(nested->duration, nested->contentEnd());
                headRoom = clip.headRoom(Ticks{}, nestedLength);
                tailRoom = clip.tailRoom(Ticks{}, nestedLength);
            } else {
                const SourceBounds bounds = media->sourceBounds(clip.stream);
                headRoom = clip.headRoom(bounds.start, bounds.end);
                tailRoom = clip.tailRoom(bounds.start, bounds.end);
            }
            bool complete = options.audio.render == AudioPostRender::CopyCompleteAudioFiles;
            if (complete && (nested || !clip.speed.isUnity() || clip.reversed || clip.isRemapped())) {
                complete = false;
                add(ReportOutcome::Approximated, ReportCategory::Media,
                    "Copy Complete Audio Files: a retimed or nested clip is rendered with its used part only", where);
            }
            Ticks head = std::max(complete ? Ticks::max() : handleTicks, needBefore);
            Ticks tail = std::max(complete ? Ticks::max() : handleTicks, needAfter);
            head = headRoom ? std::min(head, *headRoom) : (complete ? std::max(handleTicks, needBefore) : head);
            tail = tailRoom ? std::min(tail, *tailRoom) : (complete ? std::max(handleTicks, needAfter) : tail);
            head = std::max(head, Ticks{});
            tail = std::max(tail, Ticks{});
            const std::int64_t s0 = plan.samples(clip.start() - head);
            const std::int64_t s1 = plan.samples(clip.end() + tail);

            const bool panned = !(clip.properties()[ParamId::Pan] == ParamValue(0.0));
            const bool simple = complete && !hasEffects && !panned && !clip.channelMap();
            for (int set = 0; set < sets; ++set) {
                const bool withEffects = options.clipEffects && hasEffects && set == 0;
                if (cleanSet && set == 1 && !hasEffects) {
                    // The clean copy of a clip without effects is the same audio: reuse the first set's sources.
                    for (int ch = 0; ch < channels; ++ch) {
                        PlanTrack& first = plan.tracks[firstTrack + static_cast<std::size_t>(ch)];
                        ClipRef ref = first.clips.back();
                        refsOfClip[clip.id.value].push_back(plan.tracks[firstTrack + static_cast<std::size_t>(channels + ch)].clips.size());
                        plan.tracks[firstTrack + static_cast<std::size_t>(channels + ch)].clips.push_back(std::move(ref));
                    }
                    continue;
                }
                const std::string key = simple ? std::to_string(media->id.value) + "/" + std::to_string(clip.stream.index) +
                                                     "/" + std::to_string(channels)
                                               : std::string();
                std::size_t groupIndex = 0;
                std::int64_t groupStart = s0;
                if (const auto found = simple ? sharedGroups.find(key) : sharedGroups.end(); found != sharedGroups.end()) {
                    groupIndex = found->second;
                    groupStart = plan.samples(plan.groups[groupIndex].request.range.start);
                } else {
                    RenderGroup g;
                    g.request.sequence = sequenceId;
                    g.request.track = track.id;
                    g.request.clip = clip.id;
                    g.request.range = TimeRange{samplesToTicks(s0, audioRate), samplesToTicks(s1 - s0, audioRate)};
                    g.request.channels = channels;
                    g.request.sampleRate = plan.sampleRate;
                    g.request.clipEffects = withEffects;
                    g.frames = s1 - s0;
                    g.where = where.element;
                    g.clipName = name;
                    for (int ch = 0; ch < channels; ++ch) {
                        Source s;
                        s.rendered = true;
                        s.name = name + " " + trackLabel(TrackKind::Audio, ti) +
                                 (channels > 1 ? " " + channelSuffix(channels, ch) : "") +
                                 (cleanSet ? (set == 0 ? " FX" : " No FX") : "");
                        s.material = stableId("rendered\n" + std::to_string(sequenceId.value) + "\n" +
                                              std::to_string(track.id.value) + "\n" + std::to_string(clip.id.value) + "\n" +
                                              std::to_string(s0) + "\n" + std::to_string(s1) + "\n" + std::to_string(ch) +
                                              "\n" + std::to_string(withEffects) + "\n" +
                                              std::to_string(plan.sampleRate) + "\n" + std::to_string(plan.bits) + "\n" +
                                              (media ? mediaPath(*media, options.projectFile) : nested->name));
                        s.length = s1 - s0;
                        s.channels = 1;
                        s.sampleRate = plan.sampleRate;
                        s.bits = plan.bits;
                        s.group = plan.groups.size();
                        s.channel = ch;
                        g.sources.push_back(plan.sources.size());
                        plan.sources.push_back(std::move(s));
                    }
                    groupIndex = plan.groups.size();
                    plan.groups.push_back(std::move(g));
                    if (simple) {
                        sharedGroups.emplace(key, groupIndex);
                    }
                }
                for (int ch = 0; ch < channels; ++ch) {
                    ClipRef ref;
                    ref.name = name;
                    ref.start = start;
                    ref.length = length;
                    ref.source = plan.groups[groupIndex].sources[static_cast<std::size_t>(ch)];
                    ref.sourceStart = simple ? plan.samples(clip.start()) - groupStart : start - s0;
                    ref.gain = gain;
                    ref.clip = clip.id;
                    ref.where = where.element;
                    PlanTrack& pt = plan.tracks[firstTrack + static_cast<std::size_t>(set * channels + ch)];
                    refsOfClip[clip.id.value].push_back(pt.clips.size());
                    pt.clips.push_back(std::move(ref));
                }
            }
            add(ReportOutcome::Carried, ReportCategory::Clip,
                std::string("Clip rendered to new audio") + (hasEffects && options.clipEffects ? " with its effects" : ""),
                where);
            if (!clip.speed.isUnity() || clip.reversed || clip.isRemapped()) {
                add(ReportOutcome::Carried, ReportCategory::Speed, "The clip's speed is rendered into its audio", where);
            }
        }

        // Transitions -> fades on every track made from this one.
        for (const Transition& tr : track.transitions) {
            const EffectDescriptor* d = EffectRegistry::builtin().find(tr.effect);
            const std::string trName = d ? std::string(d->displayName) : tr.effect;
            const Clip* a = tr.outgoing.isValid() ? track.findClip(tr.outgoing) : nullptr;
            const Clip* b = tr.incoming.isValid() ? track.findClip(tr.incoming) : nullptr;
            const bool aIn = a && refsOfClip.contains(a->id.value);
            const bool bIn = b && refsOfClip.contains(b->id.value);
            const Ticks cutTicks = b ? b->start() : a ? a->end() : Ticks{};
            const SourceLocation where = location(TrackKind::Audio, ti, trName, cutTicks);
            if (!aIn && !bIn) {
                add(ReportOutcome::Dropped, ReportCategory::Transition, "Transition of clips that are left out", where);
                continue;
            }
            Fade f;
            f.name = trName;
            f.constantPower = tr.effect != "constantGain";
            f.cut = plan.samples(cutTicks);
            if (aIn && bIn) {
                f.kind = Fade::Kind::Cross;
                f.before = f.cut - plan.samples(cutTicks - tr.before);
                f.after = plan.samples(cutTicks + tr.after) - f.cut;
            } else if (aIn) {
                f.kind = Fade::Kind::Out;
                f.cut = plan.samples(a->end());
                f.before = std::min(f.cut - plan.samples(a->end() - tr.duration()), plan.samples(a->end()) - plan.samples(a->start()));
                f.after = 0;
                if (b) {
                    add(ReportOutcome::Approximated, ReportCategory::Transition,
                        "The incoming clip is left out: the crossfade becomes a fade out", where);
                }
            } else {
                f.kind = Fade::Kind::In;
                f.cut = plan.samples(b->start());
                f.before = 0;
                f.after = std::min(plan.samples(b->start() + tr.duration()) - f.cut, plan.samples(b->end()) - f.cut);
                if (a) {
                    add(ReportOutcome::Approximated, ReportCategory::Transition,
                        "The outgoing clip is left out: the crossfade becomes a fade in", where);
                }
            }
            if (f.before + f.after <= 0) {
                continue;
            }
            for (std::size_t k = firstTrack; k < plan.tracks.size(); ++k) {
                plan.tracks[k].fades.push_back(f);
            }
            add(ReportOutcome::Carried, ReportCategory::Transition, trName + " written", where);
        }
    }

    for (PlanTrack& t : plan.tracks) {
        std::sort(t.clips.begin(), t.clips.end(), [](const ClipRef& x, const ClipRef& y) { return x.start < y.start; });
        std::sort(t.fades.begin(), t.fades.end(), [](const Fade& x, const Fade& y) { return x.cut < y.cut; });
    }
    return plan;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rendering

void dropFailedClips(Plan& plan) {
    for (PlanTrack& t : plan.tracks) {
        std::vector<ClipRef> kept;
        std::set<std::int64_t> starts;
        std::set<std::int64_t> ends;
        for (ClipRef& c : t.clips) {
            if (!plan.sources[c.source].failed) {
                starts.insert(c.start);
                ends.insert(c.start + c.length);
                kept.push_back(std::move(c));
            }
        }
        t.clips = std::move(kept);
        std::erase_if(t.fades, [&](const Fade& f) {
            switch (f.kind) {
            case Fade::Kind::Cross:
                return !starts.contains(f.cut) || !ends.contains(f.cut);
            case Fade::Kind::In:
                return !starts.contains(f.cut);
            case Fade::Kind::Out:
                return !ends.contains(f.cut);
            }
            return true;
        });
    }
}

std::expected<void, Error> renderPlan(const ProjectState& project, Plan& plan, AudioPostRenderer& renderer,
                                      const RenderTarget& target, const std::function<bool(double)>& progress,
                                      ImportReport& report, const std::string& fileLabel) {
    std::int64_t total = 0;
    for (const RenderGroup& g : plan.groups) {
        total += g.frames * static_cast<std::int64_t>(g.sources.size());
    }
    std::int64_t done = 0;
    std::set<std::string> usedNames;
    std::vector<std::filesystem::path> written;
    auto cleanup = [&]() {
        std::error_code ec;
        for (const auto& f : written) {
            std::filesystem::remove(f, ec);
        }
    };
    const bool bigEndian = target.kind != FileKind::Wave;
    const std::string extension = target.kind == FileKind::Wave ? ".wav" : ".aif";
    const int bytesPerSample = plan.bits / 8;

    for (std::size_t gi = 0; gi < plan.groups.size(); ++gi) {
        RenderGroup& g = plan.groups[gi];
        const std::size_t channels = g.sources.size();
        // Files and headers.
        std::vector<std::ofstream> outs(channels);
        for (std::size_t ch = 0; ch < channels; ++ch) {
            Source& s = plan.sources[g.sources[ch]];
            std::string fileName;
            if (target.separate) {
                const std::string base = safeFileName(s.name);
                fileName = base + extension;
                for (int n = 2; usedNames.contains(toLowerAscii(fileName)); ++n) {
                    fileName = base + " " + std::to_string(n) + extension;
                }
            } else {
                fileName = "audio" + std::to_string(g.sources[ch]) + extension;
            }
            usedNames.insert(toLowerAscii(fileName));
            s.file = target.folder / pathFromUtf8(fileName);
            Bext bext = target.bextTemplate;
            bext.description = s.name;
            bext.originator = target.originator;
            bext.timeReference = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, target.timecodeOffsetSamples + plan.samples(g.request.range.start)));
            if (target.umidOf) {
                bext.umid = target.umidOf(s);
            }
            s.header = fileHeader(target.kind, 1, plan.sampleRate, plan.bits, static_cast<std::uint64_t>(g.frames),
                                  target.kind == FileKind::Wave && target.writeBext ? &bext : nullptr);
            s.dataBytes = static_cast<std::uint64_t>(g.frames) * static_cast<std::uint64_t>(bytesPerSample);
            outs[ch].open(s.file, std::ios::binary | std::ios::trunc);
            if (!outs[ch]) {
                cleanup();
                return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + pathToUtf8(s.file)});
            }
            written.push_back(s.file);
            outs[ch].write(s.header.data(), static_cast<std::streamsize>(s.header.size()));
        }
        // Samples.
        std::int64_t frames = 0;
        std::vector<std::string> buffers(channels);
        std::vector<float> mono;
        bool ioFailed = false;
        bool cancelled = false;
        auto sink = [&](std::span<const float> block) {
            const std::size_t n = block.size() / std::max<std::size_t>(channels, 1);
            const std::size_t take = static_cast<std::size_t>(std::clamp<std::int64_t>(g.frames - frames, 0, static_cast<std::int64_t>(n)));
            mono.resize(take);
            for (std::size_t ch = 0; ch < channels; ++ch) {
                for (std::size_t i = 0; i < take; ++i) {
                    mono[i] = block[i * channels + ch];
                }
                buffers[ch].clear();
                encodeSamples(mono, plan.bits, bigEndian, buffers[ch]);
                outs[ch].write(buffers[ch].data(), static_cast<std::streamsize>(buffers[ch].size()));
                ioFailed = ioFailed || !outs[ch];
            }
            frames += static_cast<std::int64_t>(take);
            done += static_cast<std::int64_t>(take * channels);
            if (progress && total > 0 && !progress(static_cast<double>(done) / static_cast<double>(total))) {
                cancelled = true;
            }
            return !ioFailed && !cancelled;
        };
        auto rendered = renderer.render(project, g.request, sink);
        if (cancelled || (!rendered && rendered.error().code == ErrorCode::Cancelled)) {
            for (auto& o : outs) {
                o.close();
            }
            cleanup();
            return std::unexpected(Error{ErrorCode::Cancelled, "Export cancelled"});
        }
        if (ioFailed) {
            for (auto& o : outs) {
                o.close();
            }
            cleanup();
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write the audio files (disk full?)"});
        }
        if (!rendered) {
            SourceLocation loc;
            loc.file = fileLabel;
            loc.element = g.where;
            report.add(ReportOutcome::Dropped, ReportCategory::Media,
                       "The clip's audio could not be rendered (" + rendered.error().message + "): left out", loc,
                       plan.sequenceName);
            for (std::size_t ch = 0; ch < channels; ++ch) {
                plan.sources[g.sources[ch]].failed = true;
            }
        }
        // Pad a short render with silence, so every file has the length its header says.
        if (frames < g.frames) {
            if (rendered) {
                SourceLocation loc;
                loc.file = fileLabel;
                loc.element = g.where;
                report.add(ReportOutcome::Approximated, ReportCategory::Media,
                           "The renderer returned less audio than asked: padded with silence", loc, plan.sequenceName);
            }
            const std::string silence(static_cast<std::size_t>(std::min<std::int64_t>(g.frames - frames, 1 << 20)) *
                                          static_cast<std::size_t>(bytesPerSample),
                                      '\0');
            for (std::size_t ch = 0; ch < channels; ++ch) {
                std::int64_t left = g.frames - frames;
                while (left > 0) {
                    const std::size_t n = static_cast<std::size_t>(std::min<std::int64_t>(left, 1 << 20));
                    outs[ch].write(silence.data(), static_cast<std::streamsize>(n * static_cast<std::size_t>(bytesPerSample)));
                    left -= static_cast<std::int64_t>(n);
                }
            }
            done += (g.frames - frames) * static_cast<std::int64_t>(channels);
        }
        for (std::size_t ch = 0; ch < channels; ++ch) {
            Source& s = plan.sources[g.sources[ch]];
            const std::string trailer = fileTrailer(s.dataBytes);
            outs[ch].write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
            outs[ch].close();
            if (!outs[ch]) {
                cleanup();
                return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + pathToUtf8(s.file)});
            }
            s.fileBytes = s.header.size() + s.dataBytes + trailer.size();
        }
    }
    return {};
}

} // namespace wabcut::interchange::detail::audiopost
