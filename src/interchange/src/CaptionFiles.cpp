// Caption files (CaptionFiles.h): names, detection, the shared import rules, SubRip and WebVTT, and the CEA-608 glue
// SCC and MCC share. The broadcast formats are in CaptionBroadcast.cpp, Timed Text in CaptionTtml.cpp.

#include "CaptionFormats.h"

#include "Cea608.h"
#include "TextUtil.h"

#include <wabcut/model/CaptionText.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace wabcut::interchange {

using namespace detail;
using namespace detail::captions;

// ---------------------------------------------------------------------------------------------------------------------
// Names and detection

std::string_view toString(CaptionFileFormat format) {
    switch (format) {
    case CaptionFileFormat::Srt:
        return "srt";
    case CaptionFileFormat::WebVtt:
        return "vtt";
    case CaptionFileFormat::Scc:
        return "scc";
    case CaptionFileFormat::Mcc:
        return "mcc";
    case CaptionFileFormat::EbuStl:
        return "stl";
    case CaptionFileFormat::Ttml:
        return "ttml";
    case CaptionFileFormat::Dfxp:
        return "dfxp";
    case CaptionFileFormat::SmpteTt:
        return "smpte-tt";
    case CaptionFileFormat::Itt:
        return "itt";
    }
    return "srt";
}

std::string_view displayName(CaptionFileFormat format) {
    switch (format) {
    case CaptionFileFormat::Srt:
        return "SubRip Subtitle Format (.srt)";
    case CaptionFileFormat::WebVtt:
        return "WebVTT File (.vtt)";
    case CaptionFileFormat::Scc:
        return "Scenarist Closed Caption File (.scc)";
    case CaptionFileFormat::Mcc:
        return "MacCaption VANC File (.mcc)";
    case CaptionFileFormat::EbuStl:
        return "EBU N19 Subtitle File (.stl)";
    case CaptionFileFormat::Ttml:
        return "W3C/SMPTE/EBU Timed Text File (.xml)";
    case CaptionFileFormat::Dfxp:
        return "DFXP Timed Text (.dfxp)";
    case CaptionFileFormat::SmpteTt:
        return "SMPTE Timed Text (.xml)";
    case CaptionFileFormat::Itt:
        return "iTunes Timed Text (.itt)";
    }
    return "";
}

std::string_view extension(CaptionFileFormat format) {
    switch (format) {
    case CaptionFileFormat::Srt:
        return ".srt";
    case CaptionFileFormat::WebVtt:
        return ".vtt";
    case CaptionFileFormat::Scc:
        return ".scc";
    case CaptionFileFormat::Mcc:
        return ".mcc";
    case CaptionFileFormat::EbuStl:
        return ".stl";
    case CaptionFileFormat::Ttml:
    case CaptionFileFormat::SmpteTt:
        return ".xml";
    case CaptionFileFormat::Dfxp:
        return ".dfxp";
    case CaptionFileFormat::Itt:
        return ".itt";
    }
    return ".srt";
}

std::optional<CaptionFileFormat> captionFormatFromName(std::string_view name) {
    const std::string n = toLowerAscii(trim(name));
    for (const CaptionFileFormat f : captionFileFormats()) {
        if (n == toString(f)) {
            return f;
        }
    }
    if (n == "webvtt") {
        return CaptionFileFormat::WebVtt;
    }
    if (n == "ebu-stl" || n == "ebu" || n == "n19") {
        return CaptionFileFormat::EbuStl;
    }
    if (n == "xml" || n == "w3c") {
        return CaptionFileFormat::Ttml;
    }
    if (n == "smpte" || n == "smptett") {
        return CaptionFileFormat::SmpteTt;
    }
    return std::nullopt;
}

std::vector<CaptionFileFormat> captionFileFormats() {
    return {CaptionFileFormat::Srt,  CaptionFileFormat::WebVtt, CaptionFileFormat::Scc,
            CaptionFileFormat::Mcc,  CaptionFileFormat::EbuStl, CaptionFileFormat::Ttml,
            CaptionFileFormat::Dfxp, CaptionFileFormat::SmpteTt, CaptionFileFormat::Itt};
}

std::string_view toString(Cea608Display display) {
    switch (display) {
    case Cea608Display::PopOn:
        return "pop-on";
    case Cea608Display::RollUp2:
        return "roll-up-2";
    case Cea608Display::RollUp3:
        return "roll-up-3";
    case Cea608Display::RollUp4:
        return "roll-up-4";
    case Cea608Display::PaintOn:
        return "paint-on";
    }
    return "pop-on";
}

std::optional<Cea608Display> cea608DisplayFromString(std::string_view text) {
    for (const Cea608Display d : {Cea608Display::PopOn, Cea608Display::RollUp2, Cea608Display::RollUp3,
                                  Cea608Display::RollUp4, Cea608Display::PaintOn}) {
        if (text == toString(d)) {
            return d;
        }
    }
    return std::nullopt;
}

namespace {

std::string_view extensionOf(std::string_view fileName) {
    const std::size_t dot = fileName.rfind('.');
    const std::size_t slash = fileName.find_last_of("/\\");
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    return fileName.substr(dot);
}

std::string_view withoutBom(std::string_view data) {
    return data.starts_with("\xEF\xBB\xBF") ? data.substr(3) : data;
}

/// An SRT timing line somewhere in the first lines.
bool looksLikeSrt(std::string_view text) {
    std::size_t lines = 0;
    std::size_t pos = 0;
    while (pos < text.size() && lines < 40) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        const std::string_view line = trim(text.substr(pos, end - pos));
        if (line.find("-->") != std::string_view::npos && line.find(':') != std::string_view::npos &&
            (line.find(',') != std::string_view::npos || line.find('.') != std::string_view::npos)) {
            return true;
        }
        pos = end + 1;
        ++lines;
    }
    return false;
}

} // namespace

std::optional<CaptionFileFormat> detectCaptionFile(std::string_view data, std::string_view fileName) {
    const std::string_view head = withoutBom(data.substr(0, std::min<std::size_t>(data.size(), 65536)));
    const std::string ext = toLowerAscii(extensionOf(fileName));
    if (head.starts_with("WEBVTT")) {
        return CaptionFileFormat::WebVtt;
    }
    if (head.starts_with("Scenarist_SCC")) {
        return CaptionFileFormat::Scc;
    }
    if (head.starts_with("File Format=MacCaption_MCC")) {
        return CaptionFileFormat::Mcc;
    }
    if (data.size() >= 1024 && data.substr(3, 3) == "STL" && data.substr(8, 3) == ".01") {
        return CaptionFileFormat::EbuStl;
    }
    // Timed Text: an XML document whose root is <tt>.
    const std::size_t lt = head.find('<');
    if (lt != std::string_view::npos) {
        std::size_t at = lt;
        while (at != std::string_view::npos && at + 1 < head.size() && (head[at + 1] == '?' || head[at + 1] == '!')) {
            at = head.find('<', at + 1);
        }
        if (at != std::string_view::npos) {
            const std::string_view tag = head.substr(at + 1, 64);
            const std::size_t colon = tag.find(':');
            const std::string_view local =
                colon != std::string_view::npos && colon < tag.find_first_of(" \t\r\n>") ? tag.substr(colon + 1) : tag;
            if (local.starts_with("tt") && local.size() > 2 && (local[2] == ' ' || local[2] == '>' || local[2] == '\n' ||
                                                                local[2] == '\r' || local[2] == '\t')) {
                if (ext == ".itt" || head.find("itunes") != std::string_view::npos) {
                    return CaptionFileFormat::Itt;
                }
                if (ext == ".dfxp" || head.find("ttaf1") != std::string_view::npos) {
                    return CaptionFileFormat::Dfxp;
                }
                if (head.find("smpte-tt") != std::string_view::npos ||
                    head.find("www.smpte-ra.org/schemas/2052") != std::string_view::npos) {
                    return CaptionFileFormat::SmpteTt;
                }
                return CaptionFileFormat::Ttml;
            }
        }
    }
    if (looksLikeSrt(head) && (ext == ".srt" || ext.empty() || ext == ".txt" || ext == ".sub")) {
        return CaptionFileFormat::Srt;
    }
    if (ext == ".srt") {
        return CaptionFileFormat::Srt;
    }
    if (ext == ".vtt") {
        return CaptionFileFormat::WebVtt;
    }
    if (ext == ".scc") {
        return CaptionFileFormat::Scc;
    }
    if (ext == ".mcc") {
        return CaptionFileFormat::Mcc;
    }
    if (ext == ".stl" && data.size() >= 1024) {
        return CaptionFileFormat::EbuStl;
    }
    if (ext == ".ttml" || ext == ".dfxp" || ext == ".itt") {
        return ext == ".dfxp" ? CaptionFileFormat::Dfxp : ext == ".itt" ? CaptionFileFormat::Itt : CaptionFileFormat::Ttml;
    }
    return std::nullopt;
}

bool hasCaptionFileExtension(std::string_view fileName) {
    static constexpr std::string_view kExtensions[] = {".srt", ".vtt", ".scc", ".mcc", ".stl", ".ttml", ".dfxp", ".itt"};
    const std::string ext = toLowerAscii(extensionOf(fileName));
    return std::find(std::begin(kExtensions), std::end(kExtensions), ext) != std::end(kExtensions);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shared helpers

namespace detail::captions {

void Notes::flush(ImportReport& report, std::string_view fileLabel) const {
    for (const auto& [key, count] : m_counts) {
        SourceLocation where;
        where.file = std::string(fileLabel);
        report.add(key.first, ReportCategory::Caption, count > 1 ? key.second + " (" + std::to_string(count) + " times)" : key.second,
                   where);
    }
}

std::vector<std::string> textLines(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(start, end - start);
        if (line.find_first_not_of(" \t") != std::string_view::npos) {
            out.emplace_back(line);
        }
        if (end >= text.size()) {
            break;
        }
        start = end + (end + 1 < text.size() && text[end] == '\r' && text[end + 1] == '\n' ? 2 : 1);
    }
    return out;
}

std::string clockTime(Ticks t, char separator) {
    const std::int64_t ms = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(static_cast<long double>(t.value) * 1000.0L / kTicksPerSecond + 0.5L));
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld%c%03lld", static_cast<long long>(ms / 3'600'000),
                  static_cast<long long>(ms / 60'000 % 60), static_cast<long long>(ms / 1000 % 60), separator,
                  static_cast<long long>(ms % 1000));
    return buf;
}

} // namespace detail::captions

namespace {

/// "hh:mm:ss,mmm", "hh:mm:ss.mmm", "mm:ss.mmm" (hours of any width) -> ticks.
std::optional<Ticks> parseClock(std::string_view text) {
    text = trim(text);
    std::int64_t fields[3] = {0, 0, 0};
    int count = 0;
    std::size_t pos = 0;
    std::int64_t millis = 0;
    while (pos < text.size()) {
        std::size_t end = pos;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        if (end == pos || end - pos > 9 || count >= 3) {
            return std::nullopt;
        }
        std::int64_t v = 0;
        std::from_chars(text.data() + pos, text.data() + end, v);
        fields[count++] = v;
        if (end >= text.size()) {
            break;
        }
        if (text[end] == ':') {
            pos = end + 1;
            continue;
        }
        if (text[end] == ',' || text[end] == '.') {
            std::size_t fe = end + 1;
            while (fe < text.size() && std::isdigit(static_cast<unsigned char>(text[fe]))) {
                ++fe;
            }
            std::string_view frac = text.substr(end + 1, fe - end - 1);
            if (frac.empty() || fe != text.size()) {
                return std::nullopt;
            }
            std::string digits(frac.substr(0, 3));
            digits.resize(3, '0');
            std::from_chars(digits.data(), digits.data() + 3, millis);
            break;
        }
        return std::nullopt;
    }
    if (count < 2) {
        return std::nullopt;
    }
    const std::int64_t h = count == 3 ? fields[0] : 0;
    const std::int64_t m = count == 3 ? fields[1] : fields[0];
    const std::int64_t s = count == 3 ? fields[2] : fields[1];
    if (m >= 60 || s >= 60 || h > 999) {
        return std::nullopt;
    }
    return Ticks{((h * 3600 + m * 60 + s) * 1000 + millis) * (kTicksPerSecond / 1000)};
}

/// Text that is UTF-8 (Windows-1252 read as such), noted when it was not.
std::string decodedText(std::string_view data, Notes& notes) {
    data = withoutBom(data);
    if (isValidUtf8(data)) {
        return std::string(data);
    }
    notes.add(ReportOutcome::Approximated, "The file is not UTF-8: read as Windows-1252");
    return toUtf8Lenient(data);
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.push_back(line);
        if (end >= text.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

/// SubRip markup: <i> <b> <u> <font ...> tags and ASS overrides ({\an8} sets the position) are removed.
std::string stripSrtMarkup(std::string_view line, RawCue& cue, Notes& notes) {
    std::string out;
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '<') {
            const std::size_t close = line.find('>', i);
            if (close != std::string_view::npos) {
                std::string_view tag = line.substr(i + 1, close - i - 1);
                if (tag.starts_with('/')) {
                    tag.remove_prefix(1);
                }
                const std::string name = toLowerAscii(tag.substr(0, tag.find_first_of(" \t=")));
                if (name == "i" || name == "b" || name == "u" || name == "s" || name == "font") {
                    notes.add(ReportOutcome::Approximated,
                              "Italic, bold, underline and font colour markup removed (cue text is plain)");
                    i = close + 1;
                    continue;
                }
            }
        }
        if (line[i] == '{' && i + 1 < line.size() && line[i + 1] == '\\') {
            const std::size_t close = line.find('}', i);
            if (close != std::string_view::npos) {
                const std::string_view tag = line.substr(i + 2, close - i - 2);
                if (tag.size() == 3 && tag.starts_with("an") && tag[2] >= '1' && tag[2] <= '9') {
                    const int an = tag[2] - '0';
                    cue.placement = an >= 7 ? CaptionPlacement::Top : CaptionPlacement::Bottom;
                    cue.alignment = an % 3 == 1 ? CaptionAlign::Left : an % 3 == 0 ? CaptionAlign::Right : CaptionAlign::Center;
                } else {
                    notes.add(ReportOutcome::Approximated, "SubStation override tags removed");
                }
                i = close + 1;
                continue;
            }
        }
        // The zero-width space SubRip writers (Wabcut's among them) put into "<​x", "{​\\" and
        // "--​>" so that players show those characters as text: the text had none.
        if (line.substr(i).starts_with("\xE2\x80\x8B") && !out.empty() && i + 3 < line.size() &&
            ((out.back() == '<' && (std::isalpha(static_cast<unsigned char>(line[i + 3])) || line[i + 3] == '/')) ||
             (out.back() == '{' && line[i + 3] == '\\') || (out.back() == '-' && out.ends_with("--") && line[i + 3] == '>'))) {
            i += 3;
            continue;
        }
        out.push_back(line[i]);
        ++i;
    }
    return out;
}

/// WebVTT payload text: tags removed (voice spans keep their text), entities decoded.
std::string vttPlainText(std::string_view line, Notes& notes) {
    std::string out;
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '<') {
            const std::size_t close = line.find('>', i);
            if (close == std::string_view::npos) {
                out += line.substr(i);
                break;
            }
            const std::string_view tag = line.substr(i + 1, close - i - 1);
            if (!tag.empty() && (std::isdigit(static_cast<unsigned char>(tag[0])))) {
                notes.add(ReportOutcome::Approximated, "Karaoke timestamps inside cues removed");
            } else if (tag.starts_with("v") || tag.starts_with("/v")) {
                notes.add(ReportOutcome::Approximated, "Voice spans removed (the speaker's name is not kept)");
            } else if (tag.starts_with("ruby") || tag.starts_with("rt") || tag.starts_with("/rt")) {
                notes.add(ReportOutcome::Approximated, "Ruby annotations kept as plain text");
            } else {
                notes.add(ReportOutcome::Approximated, "Italic, bold, underline and class markup removed (cue text is plain)");
            }
            i = close + 1;
            continue;
        }
        if (line[i] == '&') {
            const std::size_t semi = line.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10) {
                const std::string_view entity = line.substr(i + 1, semi - i - 1);
                std::string replacement;
                if (entity == "amp") {
                    replacement = "&";
                } else if (entity == "lt") {
                    replacement = "<";
                } else if (entity == "gt") {
                    replacement = ">";
                } else if (entity == "nbsp") {
                    replacement = "\xC2\xA0";
                } else if (entity == "lrm" || entity == "rlm") {
                    replacement = entity == "lrm" ? "\xE2\x80\x8E" : "\xE2\x80\x8F";
                } else if (entity.starts_with("#")) {
                    std::uint32_t cp = 0;
                    const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                    const std::string_view digits = entity.substr(hex ? 2 : 1);
                    const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
                    if (ec == std::errc{} && p == digits.data() + digits.size() && cp > 0 && cp <= 0x10FFFF &&
                        !(cp >= 0xD800 && cp <= 0xDFFF)) {
                        if (cp < 0x80) {
                            replacement.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            replacement.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            replacement.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else if (cp < 0x10000) {
                            replacement.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            replacement.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            replacement.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            replacement.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                            replacement.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                            replacement.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            replacement.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                    }
                }
                if (!replacement.empty()) {
                    out += replacement;
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(line[i]);
        ++i;
    }
    return out;
}

void applyVttSettings(std::string_view settings, RawCue& cue, Notes& notes) {
    for (const std::string_view word : splitWords(settings)) {
        const std::size_t colon = word.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view key = word.substr(0, colon);
        std::string_view value = word.substr(colon + 1);
        if (key == "line") {
            value = value.substr(0, value.find(','));
            if (value.ends_with('%')) {
                const auto v = parseDouble(value.substr(0, value.size() - 1));
                if (v) {
                    cue.placement = *v < 50.0 ? CaptionPlacement::Top : CaptionPlacement::Bottom;
                }
            } else if (const auto n = parseInt64(value)) {
                cue.placement = *n >= 0 && *n < 8 ? CaptionPlacement::Top : CaptionPlacement::Bottom;
            }
        } else if (key == "align") {
            cue.alignment = value == "start" || value == "left"  ? CaptionAlign::Left
                            : value == "end" || value == "right" ? CaptionAlign::Right
                                                                 : CaptionAlign::Center;
        } else if (key == "position" || key == "size" || key == "vertical" || key == "region") {
            notes.add(ReportOutcome::Approximated, "Cue position, size, region and vertical settings are not kept");
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// SubRip and WebVTT

namespace detail::captions {

std::expected<RawCaptions, Error> readSrt(std::string_view data, std::string_view fileLabel) {
    RawCaptions raw;
    Notes notes;
    const std::string text = decodedText(data, notes);
    const auto all = lines(text);
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::string_view line = trim(all[i]);
        const std::size_t arrow = line.find("-->");
        if (arrow == std::string_view::npos) {
            continue;
        }
        const auto start = parseClock(line.substr(0, arrow));
        std::string_view rest = trim(line.substr(arrow + 3));
        const std::size_t space = rest.find_first_of(" \t");
        const auto end = parseClock(rest.substr(0, space));
        if (!start || !end) {
            notes.add(ReportOutcome::Dropped, "A cue with an unreadable timing line was left out");
            continue;
        }
        if (space != std::string_view::npos) {
            notes.add(ReportOutcome::Approximated, "SubRip display coordinates are not kept");
        }
        RawCue cue;
        cue.start = *start;
        cue.end = *end;
        cue.where = "line " + std::to_string(i + 1);
        std::string body;
        std::size_t k = i + 1;
        for (; k < all.size(); ++k) {
            const std::string_view t = all[k];
            if (trim(t).empty()) {
                break;
            }
            // A missing blank line: the next cue's number and timing line start a new cue.
            if (k + 1 < all.size() && all[k + 1].find("-->") != std::string_view::npos && parseInt64(t)) {
                break;
            }
            if (t.find("-->") != std::string_view::npos && parseClock(t.substr(0, t.find("-->")))) {
                break;
            }
            if (!body.empty()) {
                body.push_back('\n');
            }
            body += stripSrtMarkup(t, cue, notes);
        }
        i = k - 1;
        cue.text = std::move(body);
        raw.cues.push_back(std::move(cue));
        if (raw.cues.size() > kMaxCaptionCues) {
            break;
        }
    }
    if (raw.cues.empty() && trim(text).size() > 0 && !looksLikeSrt(text)) {
        return std::unexpected(parseError("no SubRip cues"));
    }
    notes.flush(raw.report, fileLabel);
    return raw;
}

std::expected<RawCaptions, Error> readVtt(std::string_view data, std::string_view fileLabel) {
    RawCaptions raw;
    Notes notes;
    const std::string text = decodedText(data, notes);
    const auto all = lines(text);
    if (all.empty() || !all.front().starts_with("WEBVTT") ||
        (all.front().size() > 6 && all.front()[6] != ' ' && all.front()[6] != '\t')) {
        return std::unexpected(parseError("not a WebVTT file (no WEBVTT header)"));
    }
    std::size_t i = 1;
    while (i < all.size() && !trim(all[i]).empty()) {
        ++i; // header lines
    }
    while (i < all.size()) {
        while (i < all.size() && trim(all[i]).empty()) {
            ++i;
        }
        if (i >= all.size()) {
            break;
        }
        const std::string_view first = all[i];
        if (first.starts_with("NOTE") || first.starts_with("STYLE") || first.starts_with("REGION")) {
            if (first.starts_with("STYLE") || first.starts_with("REGION")) {
                notes.add(ReportOutcome::Approximated, "WebVTT style sheets and regions are not kept");
            }
            while (i < all.size() && !trim(all[i]).empty()) {
                ++i;
            }
            continue;
        }
        std::size_t timing = i;
        if (first.find("-->") == std::string_view::npos) {
            timing = i + 1; // a cue identifier
        }
        if (timing >= all.size() || all[timing].find("-->") == std::string_view::npos) {
            notes.add(ReportOutcome::Dropped, "A block that is not a cue was left out");
            while (i < all.size() && !trim(all[i]).empty()) {
                ++i;
            }
            continue;
        }
        const std::string_view t = trim(all[timing]);
        const std::size_t arrow = t.find("-->");
        const auto start = parseClock(t.substr(0, arrow));
        const std::string_view rest = trim(t.substr(arrow + 3));
        const std::size_t space = rest.find_first_of(" \t");
        const auto end = parseClock(rest.substr(0, space));
        RawCue cue;
        cue.where = "line " + std::to_string(timing + 1);
        if (space != std::string_view::npos) {
            applyVttSettings(rest.substr(space), cue, notes);
        }
        std::string body;
        i = timing + 1;
        while (i < all.size() && !trim(all[i]).empty()) {
            if (!body.empty()) {
                body.push_back('\n');
            }
            body += vttPlainText(all[i], notes);
            ++i;
        }
        if (!start || !end) {
            notes.add(ReportOutcome::Dropped, "A cue with an unreadable timing line was left out");
            continue;
        }
        cue.start = *start;
        cue.end = *end;
        cue.text = std::move(body);
        raw.cues.push_back(std::move(cue));
        if (raw.cues.size() > kMaxCaptionCues) {
            break;
        }
    }
    notes.flush(raw.report, fileLabel);
    return raw;
}

namespace {

constexpr std::string_view kZeroWidthSpace = "\xE2\x80\x8B";

/// One payload line: WebVTT's three escapes; for SubRip a zero-width space breaks what players would interpret.
std::string escapeLine(std::string_view line, bool vtt) {
    std::string out;
    out.reserve(line.size() + 8);
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        const char after = i + 1 < line.size() ? line[i + 1] : '\0';
        if (vtt) {
            out += c == '&' ? std::string_view("&amp;") : c == '<' ? std::string_view("&lt;") : c == '>' ? std::string_view("&gt;")
                                                                                              : std::string_view(&line[i], 1);
            continue;
        }
        if (c == '-' && after == '-' && i + 2 < line.size() && line[i + 2] == '>') {
            out += "--";
            out += kZeroWidthSpace; // "--​>": not a timing arrow
            ++i;
            continue;
        }
        out += c;
        if ((c == '<' && (std::isalpha(static_cast<unsigned char>(after)) || after == '/')) || (c == '{' && after == '\\')) {
            out += kZeroWidthSpace; // "<​i>", "{​\\an8}": no tag, no override
        }
    }
    return out;
}

std::string payloadOf(std::string_view text, bool vtt) {
    std::string out;
    for (const std::string& line : textLines(text)) {
        if (!out.empty()) {
            out += '\n';
        }
        out += escapeLine(line, vtt);
    }
    return out;
}

} // namespace

std::string writeSrt(const std::vector<CaptionCue>& cues) {
    std::string out;
    int index = 0;
    for (const CaptionCue& cue : cues) {
        const std::string payload = payloadOf(cue.text, false);
        if (payload.empty() || cue.duration.value <= 0) {
            continue;
        }
        ++index;
        out += std::to_string(index) + "\n";
        out += clockTime(cue.start, ',') + " --> " + clockTime(cue.end(), ',') + "\n";
        out += payload + "\n\n";
    }
    return out;
}

std::string writeVtt(const std::vector<CaptionCue>& cues, const CaptionWriteOptions& options) {
    std::string out = "WEBVTT\n\n";
    std::string settings;
    if (options.placement == CaptionPlacement::Top) {
        settings += " line:10%";
    }
    if (options.alignment == CaptionAlign::Left) {
        settings += " align:start";
    } else if (options.alignment == CaptionAlign::Right) {
        settings += " align:end";
    }
    for (const CaptionCue& cue : cues) {
        const std::string payload = payloadOf(cue.text, true);
        if (payload.empty() || cue.duration.value <= 0) {
            continue;
        }
        out += clockTime(cue.start, '.') + " --> " + clockTime(cue.end(), '.') + settings + "\n";
        out += payload + "\n\n";
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// CEA-608 glue (SCC and MCC)

std::map<std::int64_t, std::pair<std::uint8_t, std::uint8_t>> cea608Pairs(const std::vector<CaptionCue>& cues,
                                                                          const CaptionWriteOptions& options, Rate rate,
                                                                          Notes& notes) {
    std::vector<cea608::Cue> in;
    std::size_t rewrapped = 0;
    for (const CaptionCue& cue : cues) {
        cea608::Cue c;
        const auto startFrame = ticksToFrames(cue.start + options.startTimecode, rate, Rounding::Nearest);
        const auto endFrame = ticksToFrames(cue.end() + options.startTimecode, rate, Rounding::Nearest);
        if (!startFrame || !endFrame) {
            continue;
        }
        c.startFrame = *startFrame;
        c.endFrame = std::max(*endFrame, c.startFrame + 1);
        // 608 rows hold 32 characters and captions use at most four rows.
        std::vector<std::string> lines = textLines(cue.text);
        bool fits = lines.size() <= 4;
        for (const std::string& l : lines) {
            fits = fits && captionDisplayLength(l) <= static_cast<std::size_t>(cea608::kColumns);
        }
        if (!fits) {
            const std::string flat = flattenCaptionText(cue.text, options.language);
            const auto wrapped = breakCaptionLines(flat, options.language, cea608::kColumns, 4, simpleLineBreaker());
            lines = wrapped ? *wrapped : std::vector<std::string>{flat};
            ++rewrapped;
        }
        c.lines = std::move(lines);
        c.top = options.placement == CaptionPlacement::Top;
        c.align = options.alignment == CaptionAlign::Left ? 0 : options.alignment == CaptionAlign::Right ? 2 : 1;
        in.push_back(std::move(c));
    }
    if (rewrapped > 0) {
        notes.add(ReportOutcome::Approximated, "Captions rewrapped to the 608 grid (32 characters, at most 4 rows)");
    }
    const cea608::Mode mode = options.display == Cea608Display::PopOn     ? cea608::Mode::PopOn
                              : options.display == Cea608Display::PaintOn ? cea608::Mode::PaintOn
                                                                          : cea608::Mode::RollUp;
    const int rows = options.display == Cea608Display::RollUp3 ? 3 : options.display == Cea608Display::RollUp4 ? 4 : 2;
    const cea608::EncodeResult r = cea608::encode(in, mode, rows);
    if (r.lateCaptions > 0) {
        notes.add(ReportOutcome::Approximated,
                  std::to_string(r.lateCaptions) + " captions show up to " + std::to_string(r.maxLateFrames) +
                      " frames late: 608 sends one character pair a frame and there was not time to load them");
    }
    if (r.replacedCharacters > 0) {
        notes.add(ReportOutcome::Approximated, std::to_string(r.replacedCharacters) +
                                                   " characters 608 cannot show were replaced by their closest letter or '?'");
    }
    if (r.clippedLines > 0) {
        notes.add(ReportOutcome::Approximated, "Lines longer than 32 characters were cut");
    }
    return r.pairs;
}

void cea608Cues(const std::vector<std::pair<std::int64_t, std::pair<std::uint8_t, std::uint8_t>>>& pairs, Rate rate,
                RawCaptions& out, Notes& notes) {
    cea608::Decoder decoder;
    std::int64_t last = 0;
    for (const auto& [frame, pair] : pairs) {
        decoder.push(frame, pair.first, pair.second);
        last = std::max(last, frame + 1);
    }
    const auto captions = decoder.finish(last);
    const cea608::DecodeStats& stats = decoder.stats();
    if (stats.parityErrors > 0) {
        notes.add(ReportOutcome::Dropped, std::to_string(stats.parityErrors) + " byte pairs with parity errors were skipped");
    }
    if (stats.otherChannel > 0) {
        notes.add(ReportOutcome::Dropped, "Data channel 2 (CC2) was not read: only CC1 is imported");
    }
    if (stats.rollUp > 0) {
        notes.add(ReportOutcome::Approximated, "Roll-up captions imported as one cue a line");
    }
    if (stats.paintOn > 0) {
        notes.add(ReportOutcome::Approximated, "Paint-on captions imported as cues of what was on screen");
    }
    bool styled = false;
    for (const cea608::Caption& c : captions) {
        RawCue cue;
        const auto start = framesToTicks(c.startFrame, rate, Rounding::Nearest);
        const auto end = framesToTicks(c.endFrame, rate, Rounding::Nearest);
        if (!start || !end) {
            continue;
        }
        cue.start = *start;
        cue.end = *end;
        for (const std::string& row : c.rows) {
            if (!cue.text.empty()) {
                cue.text.push_back('\n');
            }
            cue.text += row;
        }
        cue.placement = c.topRow <= 7 ? CaptionPlacement::Top : CaptionPlacement::Bottom;
        styled = styled || c.italic || c.underline || c.colour;
        cue.where = "frame " + std::to_string(c.startFrame);
        out.cues.push_back(std::move(cue));
    }
    if (styled) {
        notes.add(ReportOutcome::Approximated, "608 italics, underline and colours are not kept");
    }
    if (!captions.empty()) {
        notes.add(ReportOutcome::Approximated, "608 rows and columns map to the track's top or bottom placement");
    }
}

} // namespace detail::captions

// ---------------------------------------------------------------------------------------------------------------------
// Import and export

namespace {

/// Model cues from what a reader found: the timecode offset, order, overlaps and the model's text rules.
CaptionFileContents finishImport(RawCaptions raw, CaptionFileFormat format, const CaptionReadOptions& options,
                                 std::string_view fileLabel) {
    CaptionFileContents out;
    out.format = format;
    out.language = raw.language;
    out.title = raw.title;
    out.frameRate = raw.frameRate;
    out.dropFrame = raw.dropFrame;
    out.report = std::move(raw.report);
    Notes notes;
    std::stable_sort(raw.cues.begin(), raw.cues.end(), [](const RawCue& a, const RawCue& b) { return a.start < b.start; });
    if (raw.timecodeBased && options.startTimecode.value > 0 && !raw.cues.empty()) {
        if (raw.cues.front().start >= options.startTimecode) {
            for (RawCue& c : raw.cues) {
                c.start = c.start - options.startTimecode;
                c.end = c.end - options.startTimecode;
            }
        } else {
            notes.add(ReportOutcome::Approximated,
                      "The file's timecodes start before the sequence's start timecode: read as counting from 00:00:00:00");
        }
    }
    std::optional<CaptionPlacement> placement;
    std::optional<CaptionAlign> alignment;
    bool placementsDiffer = false;
    bool alignmentsDiffer = false;
    for (RawCue& c : raw.cues) {
        if (c.start.value < 0) {
            notes.add(ReportOutcome::Dropped, "Cues before the start of the sequence were left out");
            continue;
        }
        if (c.end > c.start + options.maxCueDuration) {
            c.end = c.start + options.maxCueDuration;
            notes.add(ReportOutcome::Approximated, "Cues longer than a minute were shortened to one");
        }
        if (!(c.end > c.start)) {
            notes.add(ReportOutcome::Dropped, "Cues without duration were left out");
            continue;
        }
        // Text: valid UTF-8 without control characters, lines trimmed, blank lines dropped, within the model's bound.
        std::string text;
        for (std::string line : textLines(c.text)) {
            std::string clean;
            for (const char ch : line) {
                const auto u = static_cast<unsigned char>(ch);
                if (u >= 0x20 || ch == '\t') {
                    clean.push_back(ch == '\t' ? ' ' : ch);
                }
            }
            const std::string_view t = trim(clean);
            if (t.empty()) {
                continue;
            }
            if (!text.empty()) {
                text.push_back('\n');
            }
            text += t;
        }
        if (!isValidUtf8(text)) {
            text = toUtf8Lenient(text);
        }
        if (text.size() > kMaxCaptionTextBytes) {
            std::size_t cut = kMaxCaptionTextBytes;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            text.resize(cut);
            notes.add(ReportOutcome::Approximated, "Cue text longer than 4096 bytes was cut");
        }
        if (text.empty() || !isValidCaptionText(text)) {
            if (!text.empty()) {
                notes.add(ReportOutcome::Dropped, "Cues with characters captions cannot hold were left out");
            }
            continue;
        }
        if (c.placement) {
            placementsDiffer = placementsDiffer || (placement && *placement != *c.placement);
            placement = c.placement;
        }
        if (c.alignment) {
            alignmentsDiffer = alignmentsDiffer || (alignment && *alignment != *c.alignment);
            alignment = c.alignment;
        }
        // Overlaps: a cue starting with the one before it joins it; one starting inside it cuts it short.
        if (!out.cues.empty()) {
            CaptionCue& prev = out.cues.back();
            if (c.start == prev.start) {
                prev.text += "\n" + text;
                prev.duration = std::max(prev.duration, c.end - prev.start);
                notes.add(ReportOutcome::Approximated, "Cues starting together were joined into one");
                continue;
            }
            if (c.start < prev.end()) {
                prev.duration = c.start - prev.start;
                notes.add(ReportOutcome::Approximated, "Overlapping cues were shortened to end where the next starts");
            }
        }
        if (out.cues.size() >= kMaxCaptionCues) {
            notes.add(ReportOutcome::Dropped, "Cues past the 100,000th were left out");
            break;
        }
        CaptionCue cue;
        cue.start = c.start;
        cue.duration = c.end - c.start;
        cue.text = std::move(text);
        out.cues.push_back(std::move(cue));
    }
    // Joined texts may exceed the bound again.
    for (CaptionCue& c : out.cues) {
        if (c.text.size() > kMaxCaptionTextBytes) {
            std::size_t cut = kMaxCaptionTextBytes;
            while (cut > 0 && (static_cast<unsigned char>(c.text[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            c.text.resize(cut);
        }
    }
    if (placementsDiffer) {
        notes.add(ReportOutcome::Approximated, "Cues placed at the top and at the bottom: the track keeps one placement (the last)");
    }
    if (alignmentsDiffer) {
        notes.add(ReportOutcome::Approximated, "Cues with different alignments: the track keeps one alignment (the last)");
    }
    out.placement = placement;
    out.alignment = alignment;
    SourceLocation where;
    where.file = std::string(fileLabel);
    out.report.add(ReportOutcome::Carried, ReportCategory::Caption,
                   std::to_string(out.cues.size()) + " cues read from " + std::string(displayName(format)), where);
    notes.flush(out.report, fileLabel);
    return out;
}

} // namespace

std::expected<CaptionFileContents, Error> readCaptionData(std::string_view data, CaptionFileFormat format,
                                                          const CaptionReadOptions& options, std::string_view fileLabel) {
    if (data.size() > kMaxCaptionFileBytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "caption file larger than 64 MB"});
    }
    std::expected<RawCaptions, Error> raw;
    switch (format) {
    case CaptionFileFormat::Srt:
        raw = readSrt(data, fileLabel);
        break;
    case CaptionFileFormat::WebVtt:
        raw = readVtt(data, fileLabel);
        break;
    case CaptionFileFormat::Scc:
        raw = readScc(data, options, fileLabel);
        break;
    case CaptionFileFormat::Mcc:
        raw = readMcc(data, options, fileLabel);
        break;
    case CaptionFileFormat::EbuStl:
        raw = readStl(data, options, fileLabel);
        break;
    case CaptionFileFormat::Ttml:
    case CaptionFileFormat::Dfxp:
    case CaptionFileFormat::SmpteTt:
    case CaptionFileFormat::Itt:
        raw = readTtml(data, options, fileLabel);
        break;
    }
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return finishImport(std::move(*raw), format, options, fileLabel);
}

std::expected<CaptionFileContents, Error> readCaptionFile(const std::filesystem::path& file, const CaptionReadOptions& options,
                                                          std::optional<CaptionFileFormat> given) {
    auto data = readFileLimited(file, kMaxCaptionFileBytes);
    if (!data) {
        return std::unexpected(data.error());
    }
    const std::string label = utf8FromPath(file.filename());
    const auto format = given ? given : detectCaptionFile(*data, label);
    if (!format) {
        return std::unexpected(Error{ErrorCode::Unsupported, label + " is not a caption file Wabcut reads"});
    }
    return readCaptionData(*data, *format, options, label);
}

std::expected<CaptionWriteResult, Error> writeCaptionData(const std::vector<CaptionCue>& cues, TimeRange range,
                                                          CaptionFileFormat format, const CaptionWriteOptions& options,
                                                          std::string_view fileLabel) {
    // Cues inside the range, cut to it, shifted to start at 0.
    std::vector<CaptionCue> inRange;
    for (const CaptionCue& cue : cues) {
        const Ticks start = std::max(cue.start, range.start);
        const Ticks end = std::min(cue.end(), range.end());
        if (!(start < end) || textLines(cue.text).empty()) {
            continue;
        }
        CaptionCue c = cue;
        c.start = start - range.start;
        c.duration = end - start;
        inRange.push_back(std::move(c));
    }
    CaptionWriteResult result;
    result.cues = inRange.size();
    Notes notes;
    std::expected<std::string, Error> data;
    switch (format) {
    case CaptionFileFormat::Srt:
        data = writeSrt(inRange);
        if (options.placement != CaptionPlacement::Bottom) {
            notes.add(ReportOutcome::Approximated, "SubRip has no position: players show the cues at the bottom");
        }
        break;
    case CaptionFileFormat::WebVtt:
        data = writeVtt(inRange, options);
        break;
    case CaptionFileFormat::Scc:
        data = writeScc(inRange, options, notes);
        break;
    case CaptionFileFormat::Mcc:
        data = writeMcc(inRange, options, notes);
        break;
    case CaptionFileFormat::EbuStl:
        data = writeStl(inRange, options, notes);
        break;
    case CaptionFileFormat::Ttml:
    case CaptionFileFormat::Dfxp:
    case CaptionFileFormat::SmpteTt:
    case CaptionFileFormat::Itt:
        data = writeTtml(inRange, format, options, notes);
        break;
    }
    if (!data) {
        return std::unexpected(data.error());
    }
    result.data = std::move(*data);
    SourceLocation where;
    where.file = std::string(fileLabel);
    result.report.add(ReportOutcome::Carried, ReportCategory::Caption,
                      std::to_string(result.cues) + " cues written as " + std::string(displayName(format)), where);
    notes.flush(result.report, fileLabel);
    return result;
}

std::map<std::int64_t, std::vector<std::array<std::uint8_t, 2>>> cea608ForVideo(const std::vector<CaptionCue>& cues,
                                                                                 TimeRange range, Rate videoRate,
                                                                                 const CaptionWriteOptions& options,
                                                                                 ImportReport* report) {
    std::vector<CaptionCue> inRange;
    for (const CaptionCue& cue : cues) {
        const Ticks start = std::max(cue.start, range.start);
        const Ticks end = std::min(cue.end(), range.end());
        if (!(start < end) || textLines(cue.text).empty()) {
            continue;
        }
        CaptionCue c = cue;
        c.start = start - range.start;
        c.duration = end - start;
        inRange.push_back(std::move(c));
    }
    // 608 sends one byte pair per field-1 frame of the NTSC rate; 30 / 60 fps video keeps 30.
    const bool integer30 = videoRate == kRate30 || videoRate == kRate60;
    const Rate slotRate = integer30 ? kRate30 : kRate29_97;
    CaptionWriteOptions o = options;
    o.startTimecode = Ticks{};
    Notes notes;
    const auto pairs = cea608Pairs(inRange, o, slotRate, notes);
    std::map<std::int64_t, std::vector<std::array<std::uint8_t, 2>>> out;
    for (const auto& [slot, pair] : pairs) {
        const auto frame = ticksToFrames(framesToTicks(slot, slotRate), videoRate, Rounding::Floor);
        if (frame && *frame >= 0) {
            out[*frame].push_back({pair.first, pair.second});
        }
    }
    if (report) {
        notes.flush(*report, "embedded captions");
    }
    return out;
}

} // namespace wabcut::interchange
