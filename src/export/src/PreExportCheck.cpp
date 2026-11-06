#include <wabcut/export/PreExportCheck.h>

#include <wabcut/export/ExportJob.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/Plural.h>
#include <wabcut/model/Resolve.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <set>

namespace wabcut::exporter {

namespace {

/// Effects that take noticeably longer than real time to render (the cost is per frame and grows with the frame size):
/// the stabilizers (their analysis is separate, but the warp itself is a full-frame resample), median, echo, camera blur,
/// fractal noise and the turbulent displacement. A fixed list: there is no per-effect cost data in the model.
struct SlowEffect {
    std::string_view id;
    std::string_view name;
};
constexpr SlowEffect kSlowEffects[] = {
    {kWarpStabilizerEffect, "Warp Stabilizer"},
    {kRollingShutterRepairEffect, "Rolling Shutter Repair"},
    {"wabcut.noise.median", "Median"},
    {"wabcut.time.echo", "Echo"},
    {"wabcut.blur.camera", "Camera Blur"},
    {"wabcut.generate.fractalNoise", "Fractal Noise"},
    {"wabcut.distort.turbulentDisplace", "Turbulent Displace"},
};

const SlowEffect* slowEffectOf(std::string_view id) {
    for (const SlowEffect& e : kSlowEffects) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

std::string clipLabel(const Clip& clip, const ProjectState& state) {
    if (!clip.content().name.empty()) {
        return clip.content().name;
    }
    if (const MediaItem* item = state.findMedia(clip.media)) {
        return item->name;
    }
    return {};
}

/// "24 fps", "23.976 fps", "29.97 fps".
std::string rateText(Rate rate) {
    const double fps = rate.toDouble();
    char buffer[32];
    if (std::abs(fps - std::round(fps)) < 0.0005) {
        std::snprintf(buffer, sizeof buffer, "%d fps", static_cast<int>(std::lround(fps)));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.3f fps", fps);
        std::string text = buffer;
        // "23.976 fps" stays, "29.970" loses its zero.
        const auto dot = text.find('.');
        auto end = text.find(" fps");
        while (dot != std::string::npos && end != std::string::npos && end > dot + 2 && text[end - 1] == '0') {
            text.erase(end - 1, 1);
            --end;
        }
        return text;
    }
    return buffer;
}

struct Interval {
    Ticks start;
    Ticks end;
};

/// The clips of video tracks that show in `range`: enabled tracks, enabled clips, in start order.
struct VideoClip {
    const Clip* clip = nullptr;
    const Track* track = nullptr;
};

std::vector<VideoClip> videoClipsIn(const Sequence& sequence, const TimeRange& range) {
    std::vector<VideoClip> out;
    for (const TrackPtr& track : sequence.videoTracks) {
        if (!track || !videoTrackShown(sequence, *track)) { // the eye and the solo (card 106)
            continue;
        }
        for (const Clip& clip : track->clips) {
            if (clip.enabled && clip.timelineRange.overlaps(range)) {
                out.push_back({&clip, track.get()});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const VideoClip& a, const VideoClip& b) {
        return a.clip->start() < b.clip->start();
    });
    return out;
}

/// Keeps the first kMaxCheckRowsPerKind rows of `kind`; the rest become one "and N more" row.
void limitRows(std::vector<CheckIssue>& rows, CheckKind kind) {
    std::size_t seen = 0;
    std::int64_t dropped = 0;
    CheckSeverity severity = CheckSeverity::Note;
    std::vector<CheckIssue> kept;
    kept.reserve(rows.size());
    for (CheckIssue& row : rows) {
        if (row.kind != kind || ++seen <= kMaxCheckRowsPerKind) {
            kept.push_back(std::move(row));
            continue;
        }
        severity = std::min(severity, row.severity);
        dropped += row.count;
    }
    if (dropped > 0) {
        CheckIssue more;
        more.kind = kind;
        more.severity = severity;
        more.more = true;
        more.count = dropped;
        kept.push_back(std::move(more));
    }
    rows = std::move(kept);
}

} // namespace

std::string_view checkSeverityName(CheckSeverity severity) {
    switch (severity) {
    case CheckSeverity::Problem:
        return "problem";
    case CheckSeverity::Warning:
        return "warning";
    case CheckSeverity::Note:
        return "note";
    }
    return "note";
}

std::string_view checkKindName(CheckKind kind) {
    switch (kind) {
    case CheckKind::InvalidSettings:
        return "invalidSettings";
    case CheckKind::OfflineMedia:
        return "offlineMedia";
    case CheckKind::Gap:
        return "gap";
    case CheckKind::FlashFrame:
        return "flashFrame";
    case CheckKind::MixedFrameRate:
        return "mixedFrameRate";
    case CheckKind::UnanalysedStabilizer:
        return "unanalysedStabilizer";
    case CheckKind::SlowEffects:
        return "slowEffects";
    case CheckKind::SizeMismatch:
        return "sizeMismatch";
    case CheckKind::CaptionsNotInFile:
        return "captionsNotInFile";
    case CheckKind::CaptionsMissing:
        return "captionsMissing";
    case CheckKind::LoudnessOffTarget:
        return "loudnessOffTarget";
    case CheckKind::AudioClips:
        return "audioClips";
    case CheckKind::DynamicMetadataDropped:
        return "dynamicMetadataDropped";
    }
    return "unknown";
}

std::string_view checkFixName(CheckFix fix) {
    switch (fix) {
    case CheckFix::None:
        return "none";
    case CheckFix::Relink:
        return "relink";
    case CheckFix::CloseGap:
        return "closeGap";
    case CheckFix::TurnOnLoudness:
        return "turnOnLoudness";
    case CheckFix::MatchSequenceSize:
        return "matchSequenceSize";
    case CheckFix::TurnOnCaptions:
        return "turnOnCaptions";
    }
    return "none";
}

std::string checkIssueMessage(const CheckIssue& i, const Sequence& sequence) {
    const auto fixed = [](double v, int decimals) { return std::format("{:.{}f}", v, decimals); };
    if (i.more) {
        return "and " + std::to_string(i.count) + " more like this";
    }
    switch (i.kind) {
    case CheckKind::InvalidSettings:
        return "These export settings cannot be used: " + i.subject;
    case CheckKind::OfflineMedia:
        return "\"" + i.subject + "\" is offline or missing: the export would stop on it.";
    case CheckKind::Gap:
        return "No picture for " + fixed(i.length.toSeconds(), 2) + " s: the export shows black there.";
    case CheckKind::FlashFrame: {
        const std::int64_t frames = std::max<std::int64_t>(1, ticksToFrames(i.length, sequence.frameRate, Rounding::Nearest).value_or(1));
        return "Flash frame: \"" + i.subject + "\" is only " + std::to_string(frames) + (frames == 1 ? " frame long." : " frames long.");
    }
    case CheckKind::MixedFrameRate:
        if (i.subject == "variable") {
            return counted(i.count, "clip") + (i.count == 1 ? " has" : " have") +
                   " a variable frame rate: the picture can drift out of sync with the sound.";
        }
        return counted(i.count, "clip") + (i.count == 1 ? " runs" : " run") + " at " + i.subject + " in a " +
               fixed(sequence.frameRate.toDouble(), 3) + " fps sequence: frames are repeated or dropped to fit.";
    case CheckKind::UnanalysedStabilizer:
        return "The stabilizer on \"" + i.subject + "\" was never analyzed: it exports unstabilized.";
    case CheckKind::SlowEffects:
        return counted(i.count, "clip") + (i.count == 1 ? " uses" : " use") + " effects that render slowly (" + i.subject + ").";
    case CheckKind::SizeMismatch:
        if (i.subject == "upscale") {
            return "The export (" + fixed(i.other, 0) + " pixels wide) is larger than the sequence (" + fixed(i.value, 0) +
                   "): the picture is scaled up, not sharper.";
        }
        return "The export's shape differs from the sequence's (" + fixed(i.other, 2) + " against " + fixed(i.value, 2) +
               "): the picture is " + (i.subject == "crop" ? "cropped" : i.subject == "stretch" ? "stretched" : "fitted with black bars") +
               ".";
    case CheckKind::CaptionsNotInFile:
        return counted(i.count, "caption") + " in the sequence will not be in the file.";
    case CheckKind::CaptionsMissing:
        return "Captions are on for this export, but the sequence has none in the exported range.";
    case CheckKind::LoudnessOffTarget:
        return "The loudness is " + fixed(i.value, 1) + " LUFS; " + (i.subject.empty() ? std::string("the destination") : i.subject) +
               " plays at " + fixed(i.other, 0) + " LUFS.";
    case CheckKind::DynamicMetadataDropped:
        if (i.subject == "dolbyVision") {
            return counted(i.count, "clip") + (i.count == 1 ? " carries" : " carry") +
                   " Dolby Vision metadata that this export does not keep: the file has HDR10 or HLG metadata only.";
        }
        return counted(i.count, "clip") + (i.count == 1 ? " carries" : " carry") +
               " HDR10+ dynamic metadata that a re-render cannot keep: set Dynamic metadata to HDR10+ to measure it again, "
               "or the file has static HDR10 metadata only.";
    case CheckKind::AudioClips:
        return "The sound peaks at " + fixed(i.value, 1) + " dBTP: it clips.";
    }
    return {};
}

std::vector<CheckIssue> preExportCheck(const ProjectState& state, const Sequence& sequence, const ExportSettings& settings,
                                       const CheckContext& context) {
    std::vector<CheckIssue> rows;
    const auto resolved = resolve(settings, sequence, state.colorManagement);
    if (!resolved) {
        CheckIssue issue;
        issue.kind = CheckKind::InvalidSettings;
        issue.severity = CheckSeverity::Problem;
        issue.subject = resolved.error().message;
        rows.push_back(std::move(issue));
        return rows;
    }
    const ResolvedExport& r = *resolved;
    const bool hasVideo = settings.video.enabled;
    const TimeRange range = r.range;

    // ---- Offline media: the export stops on it --------------------------------------------------------------------
    for (const MissingOriginal& m : missingOriginals(state, sequence, settings, context.projectFile)) {
        CheckIssue issue;
        issue.kind = CheckKind::OfflineMedia;
        issue.severity = CheckSeverity::Problem;
        issue.fix = CheckFix::Relink;
        issue.subject = m.name.empty() ? m.path : m.name;
        // Where it first plays.
        for (const auto* tracks : {&sequence.videoTracks, &sequence.audioTracks}) {
            for (const TrackPtr& track : *tracks) {
                if (!track) {
                    continue;
                }
                for (const Clip& clip : track->clips) {
                    if (clip.media == m.media && clip.timelineRange.overlaps(range) &&
                        (!issue.at || clip.start() < *issue.at)) {
                        issue.at = std::max(clip.start(), range.start);
                        issue.clip = clip.id;
                        issue.track = track->id;
                    }
                }
            }
        }
        rows.push_back(std::move(issue));
    }
    limitRows(rows, CheckKind::OfflineMedia);

    if (hasVideo) {
        const std::vector<VideoClip> clips = videoClipsIn(sequence, range);
        const Ticks frame = framesToTicks(1, sequence.frameRate, Rounding::Nearest).value_or(Ticks{1});

        // ---- Gaps in the picture ------------------------------------------------------------------------------------
        if (!sequence.videoTracks.empty()) {
            std::vector<Interval> covered;
            for (const VideoClip& c : clips) {
                covered.push_back({std::max(c.clip->start(), range.start), std::min(c.clip->end(), range.end())});
            }
            std::sort(covered.begin(), covered.end(), [](const Interval& a, const Interval& b) { return a.start < b.start; });
            std::vector<CheckIssue> gaps;
            Ticks cursor = range.start;
            const auto addGap = [&](Ticks from, Ticks to) {
                if (to - from < frame) {
                    return;
                }
                CheckIssue issue;
                issue.kind = CheckKind::Gap;
                issue.severity = CheckSeverity::Warning;
                issue.at = from;
                issue.length = to - from;
                // The clip after the gap closes it (Sequence > Close Gap).
                for (const VideoClip& c : clips) {
                    if (c.clip->start() == to) {
                        issue.clip = c.clip->id;
                        issue.track = c.track->id;
                        issue.fix = CheckFix::CloseGap;
                        break;
                    }
                }
                gaps.push_back(std::move(issue));
            };
            for (const Interval& i : covered) {
                if (i.start > cursor) {
                    addGap(cursor, i.start);
                }
                cursor = std::max(cursor, i.end);
            }
            if (range.end() > cursor) {
                addGap(cursor, range.end());
            }
            for (CheckIssue& g : gaps) {
                rows.push_back(std::move(g));
            }
            limitRows(rows, CheckKind::Gap);
        }

        // ---- Flash frames: video clips of one or two frames --------------------------------------------------------
        for (const VideoClip& c : clips) {
            if (c.clip->duration() > Ticks{0} && c.clip->duration() < Ticks{frame.value * 3}) {
                CheckIssue issue;
                issue.kind = CheckKind::FlashFrame;
                issue.severity = CheckSeverity::Warning;
                issue.at = std::max(c.clip->start(), range.start);
                issue.length = c.clip->duration();
                issue.clip = c.clip->id;
                issue.track = c.track->id;
                issue.subject = clipLabel(*c.clip, state);
                rows.push_back(std::move(issue));
            }
        }
        limitRows(rows, CheckKind::FlashFrame);

        // ---- Frame rates --------------------------------------------------------------------------------------------
        struct RateGroup {
            std::string text;
            std::int64_t count = 0;
            Ticks first;
            const VideoClip* clip = nullptr;
            bool variable = false;
        };
        std::map<std::string, RateGroup> groups;
        for (const VideoClip& c : clips) {
            const MediaItem* item = state.findMedia(c.clip->media);
            if (!item || item->isGenerator() || item->info.video.empty()) {
                continue;
            }
            const VideoStreamInfo& v = item->info.video.front();
            const bool still = item->info.video.front().frameCount == 1;
            if (still || !v.frameRate.isValid()) {
                continue;
            }
            const bool differs = std::abs(v.frameRate.toDouble() - sequence.frameRate.toDouble()) >
                                 0.001 * sequence.frameRate.toDouble();
            if (!differs && !v.variableFrameRate) {
                continue;
            }
            const std::string key = v.variableFrameRate ? "variable" : rateText(v.frameRate);
            RateGroup& g = groups[key];
            g.text = v.variableFrameRate ? std::string("variable") : key;
            g.variable = v.variableFrameRate;
            if (g.count == 0 || c.clip->start() < g.first) {
                g.first = std::max(c.clip->start(), range.start);
                g.clip = &c;
            }
            ++g.count;
        }
        for (const auto& entry : groups) {
            const RateGroup& g = entry.second;
            CheckIssue issue;
            issue.kind = CheckKind::MixedFrameRate;
            issue.severity = g.variable ? CheckSeverity::Warning : CheckSeverity::Note;
            issue.at = g.first;
            issue.clip = g.clip->clip->id;
            issue.track = g.clip->track->id;
            issue.subject = g.text;
            issue.count = g.count;
            rows.push_back(std::move(issue));
        }
        limitRows(rows, CheckKind::MixedFrameRate);

        // ---- 0.7: dynamic HDR metadata of the sources an HDR export does not keep ---------------------------------------
        // HDR10+ and Dolby Vision describe the pictures of one file; a render of edited pictures cannot carry them over.
        // HDR10+ is measured again when the export asks for it; Dolby Vision needs a licensed writer.
        if (r.dynamicRange != DynamicRange::Sdr) {
            std::map<std::string, std::int64_t> carriers; // "hdr10plus" / "dolbyVision" -> clips
            std::map<std::string, const VideoClip*> firstClip;
            for (const VideoClip& c : clips) {
                const MediaItem* item = state.findMedia(c.clip->media);
                if (!item || item->info.video.empty()) {
                    continue;
                }
                const VideoStreamInfo& v = item->info.video.front();
                const auto note = [&](const char* key) {
                    if (++carriers[key] == 1) {
                        firstClip[key] = &c;
                    }
                };
                if (v.hdr10Plus && settings.video.dynamicMetadata != DynamicMetadata::Hdr10Plus) {
                    note("hdr10plus");
                }
                if (v.dolbyVision && v.dolbyVision->rpuPresent && settings.video.dynamicMetadata != DynamicMetadata::DolbyVision81 &&
                    settings.video.dynamicMetadata != DynamicMetadata::DolbyVision84) {
                    note("dolbyVision");
                }
            }
            for (const auto& [key, count] : carriers) {
                CheckIssue issue;
                issue.kind = CheckKind::DynamicMetadataDropped;
                issue.severity = CheckSeverity::Warning;
                const VideoClip* first = firstClip[key];
                issue.at = std::max(first->clip->start(), range.start);
                issue.clip = first->clip->id;
                issue.track = first->track->id;
                issue.subject = key;
                issue.count = count;
                rows.push_back(std::move(issue));
            }
            limitRows(rows, CheckKind::DynamicMetadataDropped);
        }

        // ---- Stabilizers that were never analysed; slow effects ---------------------------------------------------
        std::map<std::string, std::int64_t> slowCounts; // display name -> clips
        std::int64_t slowClips = 0;
        const VideoClip* firstSlow = nullptr;
        for (const VideoClip& c : clips) {
            bool slowHere = false;
            for (const EffectInstance& e : c.clip->content().effects) {
                if (!e.enabled) {
                    continue;
                }
                if (e.effect == kWarpStabilizerEffect || e.effect == kRollingShutterRepairEffect) {
                    const EffectData* status = e.findData("status");
                    const std::string* text = status ? status->text() : nullptr;
                    if (!text || *text != "analyzed") {
                        CheckIssue issue;
                        issue.kind = CheckKind::UnanalysedStabilizer;
                        issue.severity = CheckSeverity::Warning;
                        issue.at = std::max(c.clip->start(), range.start);
                        issue.clip = c.clip->id;
                        issue.track = c.track->id;
                        issue.subject = clipLabel(*c.clip, state);
                        rows.push_back(std::move(issue));
                    }
                }
                if (const SlowEffect* slow = slowEffectOf(e.effect)) {
                    ++slowCounts[std::string(slow->name)];
                    slowHere = true;
                }
            }
            if (slowHere) {
                ++slowClips;
                if (!firstSlow) {
                    firstSlow = &c;
                }
            }
        }
        limitRows(rows, CheckKind::UnanalysedStabilizer);
        if (slowClips > 0) {
            CheckIssue issue;
            issue.kind = CheckKind::SlowEffects;
            issue.severity = CheckSeverity::Note;
            issue.at = std::max(firstSlow->clip->start(), range.start);
            issue.clip = firstSlow->clip->id;
            issue.track = firstSlow->track->id;
            issue.count = slowClips;
            for (const auto& entry : slowCounts) {
                issue.subject += (issue.subject.empty() ? std::string() : std::string(", ")) + entry.first;
            }
            rows.push_back(std::move(issue));
        }

        // ---- Size and shape of the export against the sequence --------------------------------------------------
        if (sequence.width > 0 && sequence.height > 0 && r.width > 0 && r.height > 0) {
            const double par = sequence.pixelAspect.isValid() ? sequence.pixelAspect.toDouble() : 1.0;
            const double seenSequence = sequence.width * par / sequence.height;
            const double seenExport = static_cast<double>(r.width) * r.sampleAspect.toDouble() / r.height;
            const double ratio = seenExport / seenSequence;
            if (std::abs(ratio - 1.0) > 0.015) {
                CheckIssue issue;
                issue.kind = CheckKind::SizeMismatch;
                issue.severity = CheckSeverity::Warning;
                issue.fix = CheckFix::MatchSequenceSize;
                issue.subject = settings.video.scaleMode == ScaleMode::Fill      ? "crop"
                                : settings.video.scaleMode == ScaleMode::Stretch ? "stretch"
                                                                                 : "bars";
                issue.value = seenSequence;
                issue.other = seenExport;
                rows.push_back(std::move(issue));
            } else if (r.width >= sequence.width * 1.1 && r.height >= sequence.height * 1.1) {
                CheckIssue issue;
                issue.kind = CheckKind::SizeMismatch;
                issue.severity = CheckSeverity::Note;
                issue.fix = CheckFix::MatchSequenceSize;
                issue.subject = "upscale";
                issue.value = sequence.width;
                issue.other = r.width;
                rows.push_back(std::move(issue));
            }
        }
    }

    // ---- Captions --------------------------------------------------------------------------------------------------
    {
        std::int64_t cues = 0;
        Ticks first{};
        for (const CaptionTrack& track : sequence.captionTracks) {
            if (!track.enabled) {
                continue;
            }
            for (const CaptionCue& cue : track.cues) {
                if (cue.start < range.end() && cue.end() > range.start) {
                    if (cues == 0 || cue.start < first) {
                        first = std::max(cue.start, range.start);
                    }
                    ++cues;
                }
            }
        }
        const bool showsCaptions = settings.captions.mode != CaptionExport::None || settings.subtitles.enabled;
        if (showsCaptions && cues == 0 && hasVideo) {
            CheckIssue issue;
            issue.kind = CheckKind::CaptionsMissing;
            issue.severity = CheckSeverity::Warning;
            rows.push_back(std::move(issue));
        } else if (!showsCaptions && cues > 0 && hasVideo) {
            CheckIssue issue;
            issue.kind = CheckKind::CaptionsNotInFile;
            issue.severity = CheckSeverity::Note;
            issue.fix = CheckFix::TurnOnCaptions;
            issue.at = first;
            issue.count = cues;
            rows.push_back(std::move(issue));
        }
    }

    // ---- Loudness and clipped audio (measured on request) -----------------------------------------------------------
    if (context.loudness && settings.audio.enabled) {
        const LoudnessReading& m = *context.loudness;
        const bool normalised = settings.effects.loudness.applied;
        const double ceiling = normalised ? settings.effects.loudness.effectiveTruePeak() : 0.0;
        if (!normalised && std::isfinite(m.truePeakDbtp) && m.truePeakDbtp >= -0.05) {
            CheckIssue issue;
            issue.kind = CheckKind::AudioClips;
            issue.severity = CheckSeverity::Warning;
            issue.fix = CheckFix::TurnOnLoudness;
            issue.value = m.truePeakDbtp;
            issue.other = ceiling;
            rows.push_back(std::move(issue));
        }
        if (!normalised && context.platformLufs && std::isfinite(m.integratedLufs) &&
            std::abs(m.integratedLufs - *context.platformLufs) > 2.0) {
            CheckIssue issue;
            issue.kind = CheckKind::LoudnessOffTarget;
            issue.severity = CheckSeverity::Warning;
            issue.fix = CheckFix::TurnOnLoudness;
            issue.subject = context.platformName;
            issue.value = m.integratedLufs;
            issue.other = *context.platformLufs;
            rows.push_back(std::move(issue));
        }
    }

    // Most serious first, then by place on the timeline (rows without a place last in their group).
    std::stable_sort(rows.begin(), rows.end(), [](const CheckIssue& a, const CheckIssue& b) {
        if (a.severity != b.severity) {
            return a.severity < b.severity;
        }
        const bool ha = a.at.has_value();
        const bool hb = b.at.has_value();
        if (ha != hb) {
            return ha;
        }
        return ha ? *a.at < *b.at : false;
    });
    return rows;
}

} // namespace wabcut::exporter
