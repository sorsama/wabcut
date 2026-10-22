#include "TextUtil.h"
#include "TimeMath.h"

#include <wabcut/interchange/ImportSummary.h>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

namespace wabcut::interchange {

using detail::framesLabel;
using detail::rateLabel;
using detail::timecodeLabel;

namespace {

using Json = nlohmann::ordered_json;

std::string trackLabel(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
}

std::string locationLabel(const SourceLocation& l) {
    std::string out = l.file;
    if (l.line > 0) {
        out += ":" + std::to_string(l.line);
    }
    if (!l.element.empty()) {
        out += " " + l.element;
    }
    return out;
}

std::string pad(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

Json reportJson(const ImportReport& report) {
    Json out;
    out["counts"] = {{"carried", report.count(ReportOutcome::Carried)},
                     {"approximated", report.count(ReportOutcome::Approximated)},
                     {"dropped", report.count(ReportOutcome::Dropped)}};
    Json entries = Json::array();
    for (const ReportEntry& e : report.entries()) {
        entries.push_back({{"outcome", std::string(toString(e.outcome))},
                           {"category", std::string(toString(e.category))},
                           {"reason", e.reason},
                           {"sequence", e.sequence},
                           {"file", e.location.file},
                           {"element", e.location.element},
                           {"line", e.location.line}});
    }
    out["entries"] = std::move(entries);
    return out;
}

bool isOffline(const ImportResult& result, MediaId id) {
    return std::any_of(result.unresolvedMedia.begin(), result.unresolvedMedia.end(),
                       [&](const UnresolvedMedia& u) { return u.media == id; });
}

void appendTrack(std::string& out, const ProjectState& project, const Sequence& seq, const Track& track,
                 const std::string& label, bool listClips) {
    out += "  " + pad(label, 4);
    if (!track.name.empty()) {
        out += "'" + track.name + "' ";
    }
    out += std::to_string(track.clips.size()) + (track.clips.size() == 1 ? " clip" : " clips");
    if (!track.enabled) {
        out += ", output off";
    }
    if (track.locked) {
        out += ", locked";
    }
    out += "\n";
    if (!listClips) {
        return;
    }
    for (const Clip& clip : track.clips) {
        out += "      " + timecodeLabel(clip.start(), seq.frameRate, seq.dropFrame) + " - " +
               timecodeLabel(clip.end(), seq.frameRate, seq.dropFrame) + "  " +
               pad("'" + clip.name() + "'", 28) + " src " +
               timecodeLabel(clip.sourceIn, seq.frameRate, seq.dropFrame);
        if (const MediaItem* media = project.findMedia(clip.media); media && media->name != clip.name()) {
            out += "  [" + media->name + "]";
        }
        if (clip.stream.kind == StreamKind::Audio && clip.stream.index > 0) {
            out += "  stream " + std::to_string(clip.stream.index + 1);
        }
        if (!clip.enabled) {
            out += "  (disabled)";
        }
        if (clip.linkGroup.isValid()) {
            out += "  link " + std::to_string(clip.linkGroup.value);
        }
        out += "\n";
    }
}

} // namespace

std::string formatSummary(const ImportResult& result, const SummaryOptions& options) {
    const ProjectState& project = result.project;
    std::string out;
    const Bin* bin = project.findBin(result.bin);
    out += "Imported " + std::string(displayName(result.format)) + " into bin '" +
           (bin ? bin->name : std::string()) + "': " + std::to_string(result.sequences.size()) +
           " sequence(s), " + std::to_string(result.media.size()) + " media item(s)\n";

    for (const SequenceId id : result.sequences) {
        const Sequence* seq = project.findSequence(id);
        if (!seq) {
            continue;
        }
        out += "\nSequence '" + seq->name + "': " + rateLabel(seq->frameRate) + " fps" +
               (seq->dropFrame ? " drop-frame" : "") + ", " + std::to_string(seq->width) + "x" +
               std::to_string(seq->height) + ", " + std::to_string(seq->audioRate.num / seq->audioRate.den) +
               " Hz, duration " + timecodeLabel(seq->duration, seq->frameRate, seq->dropFrame) + "\n";
        for (std::size_t i = seq->videoTracks.size(); i-- > 0;) {
            appendTrack(out, project, *seq, *seq->videoTracks[i], trackLabel(TrackKind::Video, i),
                        options.listClips);
        }
        for (std::size_t i = 0; i < seq->audioTracks.size(); ++i) {
            appendTrack(out, project, *seq, *seq->audioTracks[i], trackLabel(TrackKind::Audio, i),
                        options.listClips);
        }
        if (seq->markIn || seq->markOut) {
            out += "  In/Out: " +
                   (seq->markIn ? timecodeLabel(*seq->markIn, seq->frameRate, seq->dropFrame) : "-") + " / " +
                   (seq->markOut ? timecodeLabel(*seq->markOut, seq->frameRate, seq->dropFrame) : "-") + "\n";
        }
        for (const Marker& m : seq->markers) {
            out += "  marker " + timecodeLabel(m.start, seq->frameRate, seq->dropFrame) + " '" + m.name + "'";
            if (m.duration.value > 0) {
                out += " (" + framesLabel(m.duration, seq->frameRate) + ")";
            }
            if (!m.comment.empty()) {
                out += " - " + m.comment;
            }
            out += "\n";
        }
    }

    out += "\nMedia (" + std::to_string(result.media.size()) + ", " +
           std::to_string(result.unresolvedMedia.size()) + " offline)\n";
    for (const MediaId id : result.media) {
        const MediaItem* media = project.findMedia(id);
        if (!media) {
            continue;
        }
        out += std::string(isOffline(result, id) ? "  OFFLINE " : "  online  ") +
               pad("'" + media->name + "'", 24) + " " +
               (media->isGenerator()          ? "(" + media->generator->kind + ", no file)"
                : media->locator.path.empty() ? std::string("(no path)")
                                              : media->locator.path) +
               "\n";
    }

    out += "\n" + formatReport(result.report, options.listCarried);
    return out;
}

std::string formatReport(const ImportReport& report, bool listCarried) {
    std::string out;
    out += "Report: " + std::to_string(report.count(ReportOutcome::Carried)) + " carried, " +
           std::to_string(report.count(ReportOutcome::Approximated)) + " approximated, " +
           std::to_string(report.count(ReportOutcome::Dropped)) + " dropped\n";
    for (const ReportOutcome outcome :
         {ReportOutcome::Carried, ReportOutcome::Approximated, ReportOutcome::Dropped}) {
        std::string counts;
        for (std::size_t c = 0; c < kReportCategoryCount; ++c) {
            const auto category = static_cast<ReportCategory>(c);
            if (const std::size_t n = report.count(outcome, category); n > 0) {
                counts +=
                    (counts.empty() ? "" : ", ") + std::to_string(n) + " " + std::string(toString(category));
            }
        }
        if (!counts.empty()) {
            out += "  " + pad(std::string(toString(outcome)), 13) + counts + "\n";
        }
    }
    for (const ReportEntry& e : report.entries()) {
        if (e.outcome == ReportOutcome::Carried && !listCarried) {
            continue;
        }
        std::string tag = std::string(toString(e.outcome));
        std::transform(tag.begin(), tag.end(), tag.begin(),
                       [](char c) { return static_cast<char>(c - ('a' <= c && c <= 'z' ? 32 : 0)); });
        out += "  " + pad(tag, 13) + pad(std::string(toString(e.category)), 17) + e.reason + "\n";
        out += std::string(32, ' ') + "at " + locationLabel(e.location) + "\n";
    }
    return out;
}

std::string toJson(const ImportResult& result) {
    const ProjectState& project = result.project;
    Json root;
    root["format"] = std::string(toString(result.format));
    if (const Bin* bin = project.findBin(result.bin)) {
        root["bin"] = {{"id", result.bin.value}, {"name", bin->name}};
    }
    Json sequences = Json::array();
    for (const SequenceId id : result.sequences) {
        const Sequence* seq = project.findSequence(id);
        if (!seq) {
            continue;
        }
        const auto tc = [&](Ticks t) { return timecodeLabel(t, seq->frameRate, seq->dropFrame); };
        Json s;
        s["id"] = id.value;
        s["name"] = seq->name;
        s["frameRate"] = {{"num", seq->frameRate.num}, {"den", seq->frameRate.den}};
        s["dropFrame"] = seq->dropFrame;
        s["width"] = seq->width;
        s["height"] = seq->height;
        s["audioRate"] = seq->audioRate.num / seq->audioRate.den;
        s["durationTicks"] = seq->duration.value;
        s["duration"] = tc(seq->duration);
        if (seq->markIn) {
            s["markInTicks"] = seq->markIn->value;
        }
        if (seq->markOut) {
            s["markOutTicks"] = seq->markOut->value;
        }
        Json tracks = Json::array();
        for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
            const auto& list = seq->tracks(kind);
            for (std::size_t i = 0; i < list.size(); ++i) {
                const Track& t = *list[i];
                Json track;
                track["label"] = trackLabel(kind, i);
                track["id"] = t.id.value;
                track["name"] = t.name;
                track["enabled"] = t.enabled;
                track["locked"] = t.locked;
                Json clips = Json::array();
                for (const Clip& c : t.clips) {
                    Json clip;
                    clip["id"] = c.id.value;
                    clip["name"] = c.name();
                    clip["media"] = c.media.value;
                    clip["stream"] = {{"kind", c.stream.kind == StreamKind::Video ? "video" : "audio"},
                                      {"index", c.stream.index}};
                    clip["startTicks"] = c.start().value;
                    clip["durationTicks"] = c.duration().value;
                    clip["sourceInTicks"] = c.sourceIn.value;
                    clip["start"] = tc(c.start());
                    clip["end"] = tc(c.end());
                    clip["enabled"] = c.enabled;
                    if (c.linkGroup.isValid()) {
                        clip["linkGroup"] = c.linkGroup.value;
                    }
                    clips.push_back(std::move(clip));
                }
                track["clips"] = std::move(clips);
                tracks.push_back(std::move(track));
            }
        }
        s["tracks"] = std::move(tracks);
        Json markers = Json::array();
        for (const Marker& m : seq->markers) {
            markers.push_back({{"startTicks", m.start.value},
                               {"start", tc(m.start)},
                               {"durationTicks", m.duration.value},
                               {"name", m.name},
                               {"comment", m.comment},
                               {"colorIndex", m.colorIndex}});
        }
        s["markers"] = std::move(markers);
        sequences.push_back(std::move(s));
    }
    root["sequences"] = std::move(sequences);

    Json media = Json::array();
    for (const MediaId id : result.media) {
        if (const MediaItem* m = project.findMedia(id)) {
            Json item{{"id", id.value},
                      {"name", m->name},
                      {"path", m->locator.path},
                      {"relativePath", m->locator.relativePath},
                      {"offline", m->locator.offline}};
            if (m->generator) {
                item["generator"] = m->generator->kind;
            }
            media.push_back(std::move(item));
        }
    }
    root["media"] = std::move(media);

    Json unresolved = Json::array();
    for (const UnresolvedMedia& u : result.unresolvedMedia) {
        unresolved.push_back({{"media", u.media.value},
                              {"name", u.name},
                              {"reference", u.reference},
                              {"path", u.path},
                              {"tried", u.tried},
                              {"note", u.note}});
    }
    root["unresolvedMedia"] = std::move(unresolved);

    root["report"] = reportJson(result.report);
    // Replace invalid UTF-8 (from a malformed source file name, say) instead of throwing.
    return root.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::string formatSummary(const ExportResult& result, const SummaryOptions& options) {
    std::string out = "Exported " + std::to_string(result.sequences.size()) +
                      (result.sequences.size() == 1 ? " sequence" : " sequences") + " as " +
                      std::string(displayName(result.format)) + " (" + result.extension + ", " +
                      std::to_string(result.data.size()) + " bytes)\n\n";
    out += formatReport(result.report, options.listCarried);
    return out;
}

std::string toJson(const ExportResult& result) {
    Json root;
    root["format"] = std::string(toString(result.format));
    root["extension"] = result.extension;
    root["bytes"] = result.data.size();
    Json sequences = Json::array();
    for (const SequenceId id : result.sequences) {
        sequences.push_back(id.value);
    }
    root["sequences"] = std::move(sequences);
    root["report"] = reportJson(result.report);
    return root.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::string formatSummary(const AudioPostResult& result, std::string_view format, const SummaryOptions& options) {
    std::string out = "Exported " + std::string(format) + " " + detail::utf8FromPath(result.file) + " (" + std::to_string(result.bytes) +
                      " bytes): " + std::to_string(result.tracks) + (result.tracks == 1 ? " audio track, " : " audio tracks, ") +
                      std::to_string(result.clips) + (result.clips == 1 ? " clip" : " clips") + "\n";
    if (!result.audioFiles.empty()) {
        out += std::to_string(result.audioFiles.size()) + " audio files in " + detail::utf8FromPath(result.audioFiles.front().parent_path()) +
               "\n";
    }
    if (result.referenceVideo) {
        out += "Reference video: " + detail::utf8FromPath(*result.referenceVideo) + "\n";
    }
    out += "\n" + formatReport(result.report, options.listCarried);
    return out;
}

std::string toJson(const AudioPostResult& result, std::string_view format) {
    Json root;
    root["format"] = std::string(format);
    root["path"] = detail::utf8FromPath(result.file);
    root["bytes"] = result.bytes;
    root["tracks"] = result.tracks;
    root["clips"] = result.clips;
    Json files = Json::array();
    for (const auto& f : result.audioFiles) {
        files.push_back(detail::utf8FromPath(f));
    }
    root["audioFiles"] = std::move(files);
    root["referenceVideo"] = result.referenceVideo ? Json(detail::utf8FromPath(*result.referenceVideo)) : Json();
    root["report"] = reportJson(result.report);
    return root.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

} // namespace wabcut::interchange
