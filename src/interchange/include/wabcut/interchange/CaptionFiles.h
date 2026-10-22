#pragma once

// Caption files (P.2): import and export of the sidecar formats Premiere Pro reads and writes, for subtitles and
// broadcast closed captions.
//
//     auto file = interchange::readCaptionFile("show.scc", {.startTimecode = sequence.startTimecode});
//     // file->cues: timeline cues; file->report: what was carried, approximated (styles, positions, roll-up) or dropped.
//     auto out = interchange::writeCaptionData(track.cues, range, interchange::CaptionFileFormat::Scc, options);
//
// Formats: SubRip (.srt), WebVTT (.vtt), Scenarist Closed Caption (.scc) and MacCaption VANC (.mcc) for CEA-608
// closed captions, EBU N19 subtitles (.stl), and Timed Text: W3C TTML (.ttml/.xml), DFXP (.dfxp), SMPTE-TT (.xml)
// and Apple iTunes Timed Text (.itt). Cue text is plain text (UTF-8, '\n' between lines): styling a format carries
// (italics, colours, fonts) is reported as approximated on import; positions map to the caption track's placement
// (top or bottom of the safe area) where all cues agree, and to the bottom otherwise.
//
// Every reader takes an untrusted file: sizes, counts and nesting are bounded, malformed input is an Error, never a
// crash (tests/fuzz: fuzz_captions).

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange {

enum class CaptionFileFormat : std::uint8_t { Srt, WebVtt, Scc, Mcc, EbuStl, Ttml, Dfxp, SmpteTt, Itt };

/// "srt", "vtt", "scc", "mcc", "stl", "ttml", "dfxp", "smpte-tt", "itt".
std::string_view toString(CaptionFileFormat format);
/// Premiere's names: "SubRip Subtitle Format (.srt)", "Scenarist Closed Caption File (.scc)", ...
std::string_view displayName(CaptionFileFormat format);
/// ".srt", ".vtt", ".scc", ".mcc", ".stl", ".xml" (TTML, SMPTE-TT), ".dfxp", ".itt".
std::string_view extension(CaptionFileFormat format);
/// The format for a name ("srt", "webvtt", "vtt", "scc", "mcc", "stl", "ebu-stl", "ttml", "xml", "dfxp", "smpte-tt",
/// "itt"; case-insensitive), or nullopt.
std::optional<CaptionFileFormat> captionFormatFromName(std::string_view name);
/// Every format, in the order above.
std::vector<CaptionFileFormat> captionFileFormats();

/// The format of a caption file by its content (the first 64 KiB), else its extension; nullopt when it is none.
std::optional<CaptionFileFormat> detectCaptionFile(std::string_view data, std::string_view fileName = {});
/// Whether `fileName` has a caption file extension (.srt .vtt .scc .mcc .stl .ttml .dfxp .itt; an .xml only by content).
bool hasCaptionFileExtension(std::string_view fileName);

/// Largest caption file read (bytes) and most cues taken from one.
inline constexpr std::size_t kMaxCaptionFileBytes = 64u * 1024u * 1024u;

struct CaptionReadOptions {
    /// Formats timed in frames (SCC, MCC, EBU STL, frame-based TTML): the frame rate when the file does not say
    /// (default: SCC 29.97 drop-frame as its timecodes say, EBU STL its own 25 / 30).
    std::optional<Rate> frameRate;
    /// The timecode of the sequence's first frame (Sequence::startTimecode): timecode-based files (SCC, MCC, STL,
    /// iTT, SMPTE-time TTML) are converted to sequence time by subtracting it - unless their first caption comes
    /// before it, in which case they are read as counting from 00:00:00:00 (reported).
    Ticks startTimecode;
    /// Cues longer than this are cut to it (a caption left on screen by a file that never erases it).
    Ticks maxCueDuration = Ticks::fromSeconds(60);
};

struct CaptionFileContents {
    CaptionFileFormat format = CaptionFileFormat::Srt;
    /// Sorted by start, positive durations, not overlapping, text valid for the model (isValidCaptionText) and at
    /// most kMaxCaptionTextBytes; at most kMaxCaptionCues.
    std::vector<CaptionCue> cues;
    /// ISO 639-1 language when the file names one (TTML xml:lang, STL language code), else empty.
    std::string language;
    /// A title the file carries (STL programme title, TTML ttm:title), else empty.
    std::string title;
    /// Where the cues sit, when the file says and they agree: top or bottom.
    std::optional<CaptionPlacement> placement;
    std::optional<CaptionAlign> alignment;
    /// The frame rate the times were read in (frame-based formats), and whether in drop-frame timecode.
    std::optional<Rate> frameRate;
    bool dropFrame = false;
    /// Carried / approximated / dropped (category Caption): styles, positions, 608 display modes, merged or trimmed
    /// cues, characters replaced.
    ImportReport report;
};

/// Reads caption data of a known format. `fileLabel` names the file in report locations. Errors: ParseError.
std::expected<CaptionFileContents, Error> readCaptionData(std::string_view data, CaptionFileFormat format,
                                                          const CaptionReadOptions& options = {},
                                                          std::string_view fileLabel = {});
/// Reads a caption file in `format`, or detected by content and extension when nullopt. Errors: NotFound, IoError,
/// Unsupported (not a caption file), ParseError.
std::expected<CaptionFileContents, Error> readCaptionFile(const std::filesystem::path& file,
                                                          const CaptionReadOptions& options = {},
                                                          std::optional<CaptionFileFormat> format = std::nullopt);

/// How CEA-608 captions (SCC, MCC) are displayed.
enum class Cea608Display : std::uint8_t { PopOn, RollUp2, RollUp3, RollUp4, PaintOn };
std::string_view toString(Cea608Display display); ///< "pop-on", "roll-up-2", "roll-up-3", "roll-up-4", "paint-on"
std::optional<Cea608Display> cea608DisplayFromString(std::string_view text);

struct CaptionWriteOptions {
    /// Frame-based formats (SCC, MCC, STL, iTT): the frame rate and drop-frame timecode. Default: the sequence's
    /// (SCC: 29.97 always, drop-frame unless the sequence's rate is not NTSC).
    Rate frameRate = kRate29_97;
    bool dropFrame = true;
    /// Added to every time of timecode-based formats: the sequence's start timecode (01:00:00:00 for broadcast).
    Ticks startTimecode;
    std::string language; ///< ISO 639-1; empty: "en" where a format needs one
    std::string title;
    /// Placement and alignment written where the format allows (TTML regions, VTT line settings, 608 rows and
    /// columns, STL vertical position and justification).
    CaptionPlacement placement = CaptionPlacement::Bottom;
    CaptionAlign alignment = CaptionAlign::Center;
    Cea608Display display = Cea608Display::PopOn;
    /// Product name some formats record (MCC Creation Program, STL GSI); and the creation date "YYYY-MM-DD" /
    /// time "hh:mm:ss" (empty: none - deterministic output).
    std::string creationProgram = "Wabcut";
    std::string creationDate;
    std::string creationTime;
};

struct CaptionWriteResult {
    std::string data;
    /// What the format could not hold exactly (category Caption): times rounded to frames, lines rewrapped or cut to
    /// 32 (608) / 40 (STL) characters, characters replaced, captions shown late for lack of time to load them.
    ImportReport report;
    std::size_t cues = 0;
};

/// Writes the cues of `track` inside `range` (shifted so that `range.start` is 0) in `format`. Errors:
/// InvalidArgument (a frame rate the format cannot carry).
std::expected<CaptionWriteResult, Error> writeCaptionData(const std::vector<CaptionCue>& cues, TimeRange range,
                                                          CaptionFileFormat format, const CaptionWriteOptions& options = {},
                                                          std::string_view fileLabel = {});

/// CEA-608 caption data to embed in a video (ATSC A/53 caption SEI: Premiere's Embed In Output File): for each video
/// frame index (frame 0 = `range.start`), the CC1 byte pairs (odd parity) it carries, as an SCC of the same cues
/// times them (29.97 fps pair slots; 30 for 30 / 60 fps video) mapped onto the video's frames. Frames without
/// captions are absent. `options.startTimecode` is ignored. What the 608 grid could not hold goes to `report`.
std::map<std::int64_t, std::vector<std::array<std::uint8_t, 2>>> cea608ForVideo(const std::vector<CaptionCue>& cues,
                                                                                 TimeRange range, Rate videoRate,
                                                                                 const CaptionWriteOptions& options = {},
                                                                                 ImportReport* report = nullptr);

} // namespace wabcut::interchange
