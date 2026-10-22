// Interchange (X.1, P.2): interchange.export writes a sequence as a timeline file other editors read
// (wabcut/interchange/Export.h) or, for audio post, as an AAF or OMF with its audio rendered by the host
// (AudioPostExport.h); captions.import / captions.export read and write caption files (CaptionFiles.h). They name the
// files they read or write, so they are file commands (CommandSpec::touchesFiles).

#include "Catalog.h"

#include <wabcut/command/AudioPostServices.h>
#include <wabcut/command/CaptionFileServices.h>
#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/interchange/CaptionFiles.h>
#include <wabcut/interchange/Export.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>

namespace wabcut::command::detail {

namespace {

Json reportOf(const interchange::ImportReport& report) {
    using interchange::ReportOutcome;
    Json entries = Json::array();
    for (const interchange::ReportEntry& e : report.entries()) {
        if (e.outcome == ReportOutcome::Carried) {
            continue; // counted; the approximated and dropped ones are what a person needs to read
        }
        entries.push_back(Json{{"outcome", std::string(interchange::toString(e.outcome))},
                               {"category", std::string(interchange::toString(e.category))},
                               {"reason", e.reason},
                               {"where", e.location.element}});
    }
    return Json{{"carried", report.count(ReportOutcome::Carried)},
                {"approximated", report.count(ReportOutcome::Approximated)},
                {"dropped", report.count(ReportOutcome::Dropped)},
                {"entries", std::move(entries)}};
}

Json pathParam(std::string description) {
    Json path = schema::string(std::move(description));
    path["maxLength"] = 4096;
    path["minLength"] = 1;
    return path;
}

/// The AAF / OMF settings object (Premiere's AAF and OMF Export Settings).
Json audioPostSchema() {
    return schema::object(
        {{"mixdownVideo", schema::boolean("AAF: render the video to one reference file next to the AAF (Mixdown Video).", false)},
         {"breakoutToMono",
          schema::boolean("AAF: render new mono audio files, stereo tracks split into Left and Right tracks (Breakout to "
                          "Mono). Off: the clips link to their original files.",
                          true)},
         {"renderAudioClipEffects", schema::boolean("Render the clips' audio effects into the new files.", false)},
         {"includeClipCopiesWithoutEffects",
          schema::boolean("With renderAudioClipEffects: clean copies on \"No FX\" tracks.", false)},
         {"sampleRate", schema::integer("48000 or 96000 (Hz). Default 48000.", 48000, 96000)},
         {"bitsPerSample", schema::integer("16 or 24. Default 24.", 16, 24)},
         {"files", schema::enumeration({"embed", "separate"},
                                       "The audio inside the file (Embed Audio / Encapsulate) or in a folder next to it "
                                       "(Separate Audio). Default embed.")},
         {"audioFormat", schema::enumeration({"wav", "aiff"}, "Broadcast WAV or AIFF. Default wav.")},
         {"render", schema::enumeration({"trim", "complete"},
                                        "Trim Audio Files (the parts used, plus handles) or Copy Complete Audio Files. "
                                        "Default trim.")},
         {"handleFrames", schema::integer("Trim: handle frames before and after each clip. Default 24.", 0, 1000)},
         {"omfTitle", schema::string("OMF: the composition's name. Default the sequence's.")},
         {"includePan", schema::boolean("OMF: write the tracks' pan.", true)}},
        {}, "AAF and OMF only.");
}

interchange::AudioPostAudio audioOf(const Json& o) {
    interchange::AudioPostAudio a;
    a.sampleRate = static_cast<int>(o.value("sampleRate", 48000));
    a.bitsPerSample = static_cast<int>(o.value("bitsPerSample", 24));
    a.files = interchange::audioPostFilesFromString(o.value("files", std::string("embed"))).value_or(interchange::AudioPostFiles::Embed);
    a.format = interchange::audioPostFormatFromString(o.value("audioFormat", std::string("wav")))
                   .value_or(interchange::AudioPostFormat::BroadcastWave);
    a.render = interchange::audioPostRenderFromString(o.value("render", std::string("trim")))
                   .value_or(interchange::AudioPostRender::TrimAudioFiles);
    a.handleFrames = static_cast<int>(o.value("handleFrames", 24));
    return a;
}

/// The frame rate a caption file is timed in: "29.97", "30000/1001", "25", ...; empty: `fallback`.
std::optional<Rate> rateParam(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (const std::size_t slash = text.find('/'); slash != std::string::npos) {
        try {
            return Rate::make(std::stoll(text.substr(0, slash)), std::stoll(text.substr(slash + 1)));
        } catch (...) {
            return std::nullopt;
        }
    }
    static constexpr std::pair<std::string_view, Rate> kNames[] = {
        {"23.976", kRate23_976}, {"23.98", kRate23_976}, {"24", kRate24},    {"25", kRate25},   {"29.97", kRate29_97},
        {"30", kRate30},         {"50", kRate50},        {"59.94", kRate59_94}, {"60", kRate60}};
    for (const auto& [name, rate] : kNames) {
        if (text == name) {
            return rate;
        }
    }
    return std::nullopt;
}

Json rateSchema() {
    return schema::string("Frame rate of timecode-based files: \"23.976\", \"24\", \"25\", \"29.97\", \"30\", \"50\", "
                          "\"59.94\", \"60\" or \"num/den\". Default: the sequence's (SCC and MCC: 29.97).");
}

} // namespace

void registerInterchangeCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        command::touchesFiles(
            {"interchange.export", 1, "Export Timeline",
             "Writes the sequence (with the sequences it nests and its multicam sources) as a timeline file other editors "
             "read, at \"path\": Final Cut Pro 7 XML (\"fcp7-xml\": Premiere Pro, DaVinci Resolve), FCPXML (\"fcpxml\": "
             "Final Cut Pro, Resolve; \"fcpxmlVersion\" 1.10 or 1.11), OpenTimelineIO (\"otio\") or a CMX3600 EDL "
             "(\"edl\"); or for audio post as an AAF (\"aaf\": Pro Tools, Nuendo, Resolve / Fairlight, Media Composer) or "
             "an OMF 2.0 file (\"omf\") with its audio rendered to new files (\"audioPost\": Premiere's AAF and OMF "
             "settings). Returns the export report: how many elements were carried exactly, and every one approximated "
             "(rounded to frames, a transition the format has no name for, ...) or dropped (graphics and effects the "
             "format cannot describe). The project does not change; a dry run writes nothing.",
             Capability::Read,
             withSequence({{"format", schema::enumeration({"fcp7-xml", "fcpxml", "otio", "edl", "aaf", "omf"}, "The file format.")},
                           {"path", pathParam("The file to write (UTF-8 path); an existing one is replaced.")},
                           {"fcpxmlVersion", schema::enumeration({"1.10", "1.11"}, "FCPXML only (default 1.10).")},
                           {"audioPost", audioPostSchema()}},
                          {"format", "path"}),
             Json{{"format", "aaf"}, {"path", "C:/Projects/Film (for the mix).aaf"}, {"audioPost", Json{{"files", "separate"}}}},
             {},
             [](ExecutionContext& c) {
                 const std::string formatName = c.string("format");
                 const std::filesystem::path file = pathFromUtf8(c.string("path"));
                 if (formatName == "aaf" || formatName == "omf") {
                     const Json o = c.params().value("audioPost", Json::object());
                     interchange::AudioPostHost host;
                     if (auto* services = findService<AudioPostServices>(c.services())) {
                         host.audio = services->audio();
                         host.video = services->video();
                     }
                     host.dryRun = c.dryRun();
                     std::optional<CommandError> stopped;
                     host.progress = [&](double p) {
                         if (ExecutionControl* control = c.control()) {
                             stopped = control->checkpoint("exporting " + formatName + " " +
                                                           std::to_string(static_cast<int>(p * 100.0)) + "%");
                         }
                         return !stopped.has_value();
                     };
                     std::expected<interchange::AudioPostResult, Error> result;
                     if (formatName == "aaf") {
                         interchange::AafExportOptions options;
                         options.mixdownVideo = o.value("mixdownVideo", false);
                         options.breakoutToMono = o.value("breakoutToMono", true);
                         options.renderAudioClipEffects = o.value("renderAudioClipEffects", false);
                         options.includeClipCopiesWithoutEffects = o.value("includeClipCopiesWithoutEffects", false);
                         options.audio = audioOf(o);
                         result = interchange::exportAafToFile(c.state(), c.sequenceId(), file, options, host);
                     } else {
                         interchange::OmfExportOptions options;
                         options.title = o.value("omfTitle", std::string());
                         options.includePan = o.value("includePan", true);
                         options.audio = audioOf(o);
                         result = interchange::exportOmfToFile(c.state(), c.sequenceId(), file, options, host);
                     }
                     if (stopped) {
                         throw CommandFailure{*stopped};
                     }
                     if (!result) {
                         const wabcut::ErrorCode code = result.error().code;
                         fail(code == wabcut::ErrorCode::InvalidArgument ? ErrorCode::InvalidParams
                              : code == wabcut::ErrorCode::Unsupported ? ErrorCode::InvalidRequest
                                                                       : ErrorCode::EditRejected,
                              result.error().message);
                     }
                     Json files = Json::array();
                     for (const auto& f : result->audioFiles) {
                         files.push_back(pathToUtf8(f));
                     }
                     CommandOutput out;
                     out.state = c.state();
                     out.data = Json{{"path", c.string("path")},
                                     {"format", formatName},
                                     {"bytes", result->bytes},
                                     {"written", !c.dryRun()},
                                     {"tracks", result->tracks},
                                     {"clips", result->clips},
                                     {"audioFiles", std::move(files)},
                                     {"referenceVideo", result->referenceVideo ? Json(pathToUtf8(*result->referenceVideo)) : Json()},
                                     {"report", reportOf(result->report)}};
                     return out;
                 }
                 const auto format = interchange::exportFormatFromName(formatName);
                 if (!format) {
                     fail(ErrorCode::InvalidParams, "unknown format", Json{{"path", "/format"}});
                 }
                 interchange::ExportOptions options;
                 options.fcpxmlVersion = c.string("fcpxmlVersion", "1.10");
                 options.fileLabel = pathToUtf8(file.filename());
                 // A dry run reports what the export would carry and writes nothing (E5-P2-7).
                 auto result = c.dryRun() ? interchange::exportSequence(c.state(), c.sequenceId(), *format, options)
                                          : interchange::exportSequenceToFile(c.state(), c.sequenceId(), *format, file, options);
                 if (!result) {
                     fail(result.error().code == wabcut::ErrorCode::IoError ? ErrorCode::EditRejected : ErrorCode::InvalidParams,
                          result.error().message);
                 }
                 Json sequences = Json::array();
                 for (const SequenceId id : result->sequences) {
                     sequences.push_back(id.value);
                 }
                 CommandOutput out;
                 out.state = c.state();
                 out.data = Json{{"path", c.string("path")},
                                 {"format", std::string(interchange::toString(*format))},
                                 {"bytes", result->data.size()},
                                 {"written", !c.dryRun()},
                                 {"sequences", std::move(sequences)},
                                 {"report", reportOf(result->report)}};
                 return out;
             }},
            {"path"}));

    std::vector<std::string> formatNames;
    for (const interchange::CaptionFileFormat f : interchange::captionFileFormats()) {
        formatNames.emplace_back(interchange::toString(f));
    }
    addBuiltin(
        registry,
        command::touchesFiles(
            {"captions.import", 1, "Import Captions",
             "Reads a caption file into a new caption track of the sequence (as Premiere imports a caption file): SubRip "
             "(.srt), WebVTT (.vtt), Scenarist SCC and MacCaption MCC (CEA-608), EBU N19 (.stl), W3C TTML / DFXP / SMPTE-TT "
             "(.xml, .dfxp) or iTunes Timed Text (.itt); the format comes from the content and the extension unless "
             "\"format\" says. Timecode-based files (SCC, MCC, STL, iTT) count from the sequence's start timecode. Cue "
             "text is plain: styles, positions and 608 display modes are approximated (the track takes the placement the "
             "cues agree on), as the returned report says. Also Premiere's timed transcript text (.txt: \"start - end\" "
             "timecodes, the speaker, the text; a speaker named there becomes the project's speaker of that name), and "
             "the same as a Word document (.docx, transcript.export's). "
             "\"offset\" is added to every cue's time; overlapping cues are trimmed to the next cue's start. Untrusted "
             "input: at most 16 MiB and 100,000 cues, 4096 bytes a cue.",
             Capability::EditSequence,
             withSequence({{"path", pathParam("The caption file (UTF-8 path).")},
                           {"format", schema::enumeration(formatNames, "The file's format. Default: detected.")},
                           {"name", schema::string("The new track's name. Default: the file's name.")},
                           {"language", schema::string("ISO 639-1 language. Default: the file's, else none.")},
                           {"frameRate", rateSchema()},
                           {"preset", schema::string("A caption preset for its style (captions.presets).")},
                           {"offset", timeParam("Added to every cue's time. Default 0.")}},
                          {"path"}),
             Json{{"path", "C:/Delivery/Episode 3.scc"}}, {},
             [](ExecutionContext& c) {
                 const std::filesystem::path file = pathFromUtf8(c.string("path"));
                 const Sequence& seq = c.sequence();
                 interchange::CaptionReadOptions options;
                 options.startTimecode = seq.startTimecode;
                 options.frameRate = rateParam(c.string("frameRate"));
                 if (c.has("frameRate") && !options.frameRate) {
                     fail(ErrorCode::InvalidParams, "unknown frame rate", Json{{"path", "/frameRate"}});
                 }
                 std::string ext = pathToUtf8(file.extension());
                 std::transform(ext.begin(), ext.end(), ext.begin(),
                                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                 static constexpr std::array<std::string_view, 11> kExtensions{
                     ".srt", ".vtt", ".scc", ".mcc", ".stl", ".xml", ".ttml", ".dfxp", ".itt", ".txt", ".docx"};
                 if (!c.has("format") && !ext.empty() &&
                     std::find(kExtensions.begin(), kExtensions.end(), ext) == kExtensions.end()) {
                     fail(ErrorCode::InvalidParams,
                          "not a caption file (srt, vtt, scc, mcc, stl, ttml, dfxp, itt, or a timed transcript .txt or .docx)",
                          Json{{"path", "/path"}});
                 }
                 // 0.2 graphics: a transcript exported as a Word document (transcript.export "docx").
                 if (ext == ".docx" && !c.has("format")) {
                     if (auto cues = detail::importCaptionFileCues(c)) {
                         return std::move(*cues);
                     }
                     fail(ErrorCode::InvalidParams,
                          "the Word document holds no timed transcript blocks (\"start - end\", the speaker, the text)",
                          Json{{"path", "/path"}});
                 }
                 const auto read = interchange::readCaptionFile(
                     file, options,
                     c.has("format") ? interchange::captionFormatFromName(c.string("format")) : std::nullopt);
                 if (!read) {
                     // 0.2 graphics: timed transcript text, and lenient SubRip / WebVTT (broken blocks skipped).
                     if (read.error().code != wabcut::ErrorCode::NotFound && !c.has("format")) {
                         if (auto lenient = detail::importCaptionFileCues(c)) {
                             return std::move(*lenient);
                         }
                     }
                     fail(read.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::InvalidParams,
                          read.error().message);
                 }
                 std::optional<CaptionStyle> style;
                 if (read->placement || read->alignment) {
                     style = CaptionStyle{};
                     if (read->placement) {
                         style->placement = *read->placement;
                     }
                     if (read->alignment) {
                         style->alignment = *read->alignment;
                     }
                 }
                 // The cues follow the import rules (offset, overlaps, limits, preset) of every caption file.
                 return detail::placeImportedCaptions(c, file, read->cues, read->language, std::move(style),
                                                      Json{{"format", std::string(interchange::toString(read->format))},
                                                           {"language", read->language},
                                                           {"report", reportOf(read->report)}});
             }},
            {"path"}));

    addBuiltin(
        registry,
        command::touchesFiles(
            {"captions.export", 1, "Export Captions",
             "Writes a caption track of the sequence to a caption file (Premiere's Create Sidecar File): \"format\" srt, "
             "vtt, scc, mcc (CEA-608: rewrapped to 32 columns and 4 rows; \"display\" pop-on, roll-up or paint-on), stl "
             "(EBU N19), ttml, dfxp, smpte-tt or itt; default from the extension of \"path\". Timecode-based formats "
             "start at the sequence's start timecode. Words the soundtrack bleeps are replaced as in exports. Returns "
             "what the format could not hold exactly. The project does not change; a dry run writes nothing.",
             Capability::Read,
             withSequence({{"path", pathParam("The file to write (UTF-8 path); an existing one is replaced.")},
                           {"format", schema::enumeration(formatNames, "The format. Default: from the extension.")},
                           {"captionTrack", schema::integer("Caption track id. Default: the first shown one.", 1)},
                           {"frameRate", rateSchema()},
                           {"dropFrame", schema::boolean("Drop-frame timecode for 29.97 / 59.94. Default: the sequence's.")},
                           {"display", schema::enumeration({"pop-on", "roll-up-2", "roll-up-3", "roll-up-4", "paint-on"},
                                                           "SCC and MCC: how the 608 captions appear. Default pop-on.")},
                           {"range", schema::enumeration({"sequence", "inOut"},
                                                         "The whole sequence (default) or its In/Out marks, times "
                                                         "counted from the range's start.")}},
                          {"path"}),
             Json{{"path", "C:/Delivery/Episode 3.scc"}, {"format", "scc"}}, {},
             [](ExecutionContext& c) {
                 const Sequence& seq = c.sequence();
                 const std::filesystem::path file = pathFromUtf8(c.string("path"));
                 std::optional<interchange::CaptionFileFormat> format;
                 if (c.has("format")) {
                     format = interchange::captionFormatFromName(c.string("format"));
                 } else {
                     const std::string ext = pathToUtf8(file.extension());
                     format = ext.size() > 1 ? interchange::captionFormatFromName(ext.substr(1)) : std::nullopt;
                     if (!format) {
                         fail(ErrorCode::InvalidParams, "cannot tell the caption format from the file name: pass \"format\"",
                              Json{{"path", "/format"}});
                     }
                 }
                 const CaptionTrack* track = nullptr;
                 for (const CaptionTrack& t : seq.captionTracks) {
                     if (c.has("captionTrack") ? t.id.value == static_cast<std::uint64_t>(c.integer("captionTrack")) : t.enabled) {
                         track = &t;
                         break;
                     }
                 }
                 if (!track) {
                     fail(ErrorCode::NotFound, "the sequence has no such caption track");
                 }
                 CaptionTrack exported = *track;
                 if (auto* services = findService<CaptionFileServices>(c.services()); services && services->redact) {
                     exported = services->redact(c.state(), seq, *track);
                 }
                 interchange::CaptionWriteOptions options;
                 options.frameRate = rateParam(c.string("frameRate")).value_or(seq.frameRate);
                 if (c.has("frameRate") && !rateParam(c.string("frameRate"))) {
                     fail(ErrorCode::InvalidParams, "unknown frame rate", Json{{"path", "/frameRate"}});
                 }
                 options.dropFrame = c.boolean("dropFrame", seq.dropFrame || !supportsDropFrame(options.frameRate));
                 options.language = track->language;
                 options.title = seq.name;
                 options.placement = track->style.placement == CaptionPlacement::Top ? CaptionPlacement::Top
                                                                                     : CaptionPlacement::Bottom;
                 options.alignment = track->style.alignment;
                 options.display = interchange::cea608DisplayFromString(c.string("display", "pop-on"))
                                       .value_or(interchange::Cea608Display::PopOn);
                 // The whole sequence, and every cue of the track (captions may run past the last clip).
                 Ticks end = std::max(seq.duration, seq.contentEnd());
                 if (!track->cues.empty()) {
                     end = std::max(end, track->cues.back().end());
                 }
                 TimeRange range{Ticks{}, end};
                 if (c.string("range", "sequence") == "inOut") {
                     const auto marked = seq.markedRange();
                     if (!marked) {
                         fail(ErrorCode::InvalidParams, "the sequence has no In and Out marks", Json{{"path", "/range"}});
                     }
                     range = *marked;
                 }
                 options.startTimecode = seq.startTimecode + range.start;
                 const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
                 options.creationDate = std::format("{:%Y-%m-%d}", now);
                 options.creationTime = std::format("{:%H:%M:%S}", now);
                 auto written = interchange::writeCaptionData(exported.cues, range, *format, options, pathToUtf8(file.filename()));
                 if (!written) {
                     fail(ErrorCode::InvalidParams, written.error().message);
                 }
                 if (!c.dryRun()) {
                     if (auto saved = io::writeFileAtomically(file, written->data); !saved) {
                         fail(ErrorCode::EditRejected, saved.error().message);
                     }
                 }
                 CommandOutput out;
                 out.state = c.state();
                 out.data = Json{{"path", c.string("path")},
                                 {"format", std::string(interchange::toString(*format))},
                                 {"cues", written->cues},
                                 {"bytes", written->data.size()},
                                 {"written", !c.dryRun()},
                                 {"report", reportOf(written->report)}};
                 return out;
             }},
            {"path"}));
}

} // namespace wabcut::command::detail
