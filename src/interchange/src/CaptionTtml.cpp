// Timed Text (CaptionFiles.h): W3C TTML1, DFXP, SMPTE-TT (ST 2052-1) and Apple iTunes Timed Text (iTT), all one
// vocabulary: <tt> with timing parameters, regions and styles in <head>, and <p> cues in <body>/<div>.

#include "CaptionFormats.h"

#include "TextUtil.h"

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <pugixml.hpp>

namespace wabcut::interchange::detail::captions {

namespace {

constexpr std::size_t kMaxDepth = 32;

std::string_view localName(const pugi::xml_node& node) {
    const std::string_view name = node.name();
    const std::size_t colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

/// An attribute by its local name (any prefix: tts:origin, ttp:frameRate, xml:id).
std::string_view attr(const pugi::xml_node& node, std::string_view local) {
    for (const pugi::xml_attribute a : node.attributes()) {
        std::string_view name = a.name();
        const std::size_t colon = name.find(':');
        if ((colon == std::string_view::npos ? name : name.substr(colon + 1)) == local) {
            return a.value();
        }
    }
    return {};
}

struct TimingParameters {
    double frameRate = 30.0;          ///< effective frames a second (frameRate x multiplier)
    std::int64_t frameRateNum = 30;   ///< exact effective rate
    std::int64_t frameRateDen = 1;
    std::int64_t subFrameRate = 1;
    std::int64_t tickRate = 1;
    bool smpte = false;               ///< ttp:timeBase="smpte": clock times are timecodes
    bool drop = false;
};

/// A TTML time expression -> ticks. Clock time: hh:mm:ss, hh:mm:ss.fraction, hh:mm:ss:frames(.subframes);
/// offset time: a number and h / m / s / ms / f / t.
std::optional<Ticks> parseTime(std::string_view text, const TimingParameters& p) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    const Rate rate{static_cast<std::int32_t>(p.frameRateNum), static_cast<std::int32_t>(p.frameRateDen)};
    if (text.find(':') != std::string_view::npos) {
        std::vector<std::string_view> parts;
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t colon = std::min(text.find(':', pos), text.size());
            parts.push_back(text.substr(pos, colon - pos));
            if (colon >= text.size()) {
                break;
            }
            pos = colon + 1;
        }
        if (parts.size() == 4) { // hh:mm:ss:ff(.sub)
            const std::string_view ff = parts[3].substr(0, parts[3].find('.'));
            const auto h = parseInt64(parts[0]);
            const auto m = parseInt64(parts[1]);
            const auto s = parseInt64(parts[2]);
            const auto f = parseInt64(ff);
            if (!h || !m || !s || !f || *m >= 60 || *s >= 60 || *h > 999 || *f < 0 || !rate.isValid()) {
                return std::nullopt;
            }
            if (p.smpte && p.drop && supportsDropFrame(rate)) {
                const auto frames = parseTimecodeFrames(std::string(parts[0]) + ":" + std::string(parts[1]) + ":" +
                                                            std::string(parts[2]) + ";" + std::string(ff),
                                                        rate, true);
                return frames ? framesToTicks(*frames, rate, Rounding::Nearest) : std::nullopt;
            }
            const std::int64_t base = std::max<std::int64_t>(1, timecodeBase(rate));
            return framesToTicks((*h * 3600 + *m * 60 + *s) * base + *f, rate, Rounding::Nearest);
        }
        if (parts.size() == 3) { // hh:mm:ss(.fraction)
            const auto h = parseInt64(parts[0]);
            const auto m = parseInt64(parts[1]);
            const auto s = parseDouble(parts[2]);
            if (!h || !m || !s || *m >= 60 || *s < 0 || *s >= 61 || *h > 999) {
                return std::nullopt;
            }
            const double seconds = static_cast<double>(*h * 3600 + *m * 60) + *s;
            return Ticks{static_cast<std::int64_t>(std::llround(seconds * 1000.0)) * (kTicksPerSecond / 1000)};
        }
        return std::nullopt;
    }
    std::size_t unit = 0;
    while (unit < text.size() && (std::isdigit(static_cast<unsigned char>(text[unit])) || text[unit] == '.')) {
        ++unit;
    }
    const auto value = parseDouble(text.substr(0, unit));
    const std::string_view metric = text.substr(unit);
    if (!value || *value < 0 || *value > 1e9) {
        return std::nullopt;
    }
    double seconds = 0.0;
    if (metric == "h") {
        seconds = *value * 3600.0;
    } else if (metric == "m") {
        seconds = *value * 60.0;
    } else if (metric == "s") {
        seconds = *value;
    } else if (metric == "ms") {
        seconds = *value / 1000.0;
    } else if (metric == "f") {
        return framesToTicks(static_cast<std::int64_t>(std::llround(*value)), rate, Rounding::Nearest);
    } else if (metric == "t") {
        seconds = *value / static_cast<double>(std::max<std::int64_t>(1, p.tickRate));
    } else {
        return std::nullopt;
    }
    if (seconds > 1e7) {
        return std::nullopt;
    }
    return Ticks{static_cast<std::int64_t>(std::llround(seconds * 1000.0)) * (kTicksPerSecond / 1000)};
}

struct RegionInfo {
    std::optional<CaptionPlacement> placement;
    std::optional<CaptionAlign> alignment;
};

std::optional<CaptionAlign> alignOf(std::string_view textAlign) {
    if (textAlign == "left" || textAlign == "start") {
        return CaptionAlign::Left;
    }
    if (textAlign == "right" || textAlign == "end") {
        return CaptionAlign::Right;
    }
    if (textAlign == "center") {
        return CaptionAlign::Center;
    }
    return std::nullopt;
}

RegionInfo regionOf(const pugi::xml_node& region) {
    RegionInfo r;
    const std::string_view displayAlign = attr(region, "displayAlign");
    const std::string_view origin = attr(region, "origin");
    if (!origin.empty()) {
        const auto words = splitWords(origin);
        if (words.size() == 2 && words[1].ends_with('%')) {
            if (const auto y = parseDouble(words[1].substr(0, words[1].size() - 1))) {
                r.placement = *y < 40.0 && displayAlign != "after" ? CaptionPlacement::Top : CaptionPlacement::Bottom;
            }
        }
    }
    if (!r.placement && !displayAlign.empty()) {
        r.placement = displayAlign == "before" ? CaptionPlacement::Top : CaptionPlacement::Bottom;
    }
    const std::string id = toLowerAscii(attr(region, "id"));
    if (!r.placement && (id == "top" || id == "bottom")) {
        r.placement = id == "top" ? CaptionPlacement::Top : CaptionPlacement::Bottom;
    }
    r.alignment = alignOf(attr(region, "textAlign"));
    for (const pugi::xml_node style : region.children()) {
        if (localName(style) == "style" && !r.alignment) {
            r.alignment = alignOf(attr(style, "textAlign"));
        }
    }
    return r;
}

struct Reader {
    TimingParameters timing;
    std::map<std::string, RegionInfo, std::less<>> regions;
    std::map<std::string, std::optional<CaptionAlign>, std::less<>> styles;
    RawCaptions& raw;
    Notes& notes;
    bool styled = false;

    /// Text of a <p> (or <span>): text nodes with XML whitespace collapsed, <br/> as a new line.
    void text(const pugi::xml_node& node, std::string& out, std::size_t depth) {
        if (depth > kMaxDepth) {
            return;
        }
        for (const pugi::xml_node child : node.children()) {
            if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
                std::string_view t = child.value();
                for (const char c : t) {
                    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
                    if (space) {
                        if (!out.empty() && out.back() != ' ' && out.back() != '\n') {
                            out.push_back(' ');
                        }
                    } else {
                        out.push_back(c);
                    }
                }
                continue;
            }
            if (child.type() != pugi::node_element) {
                continue;
            }
            const std::string_view name = localName(child);
            if (name == "br") {
                while (!out.empty() && out.back() == ' ') {
                    out.pop_back();
                }
                out.push_back('\n');
            } else if (name == "span") {
                if (!attr(child, "begin").empty() || !attr(child, "end").empty()) {
                    notes.add(ReportOutcome::Approximated, "Timed spans inside cues are shown for the whole cue");
                }
                if (!attr(child, "fontStyle").empty() || !attr(child, "fontWeight").empty() ||
                    !attr(child, "color").empty() || !attr(child, "textDecoration").empty() ||
                    !attr(child, "style").empty() || !attr(child, "backgroundColor").empty()) {
                    styled = true;
                }
                text(child, out, depth + 1);
            } else if (name != "metadata" && name != "set") {
                text(child, out, depth + 1);
            }
        }
    }

    void walk(const pugi::xml_node& node, Ticks begin, std::optional<Ticks> end, RegionInfo region,
              std::optional<CaptionAlign> align, std::size_t depth) {
        if (depth > kMaxDepth) {
            notes.add(ReportOutcome::Dropped, "Elements nested deeper than 32 levels were skipped");
            return;
        }
        for (const pugi::xml_node child : node.children()) {
            if (child.type() != pugi::node_element) {
                continue;
            }
            const std::string_view name = localName(child);
            if (name != "div" && name != "p" && name != "body") {
                continue;
            }
            const auto b = parseTime(attr(child, "begin"), timing);
            const auto e = parseTime(attr(child, "end"), timing);
            const auto d = parseTime(attr(child, "dur"), timing);
            if ((!attr(child, "begin").empty() && !b) || (!attr(child, "end").empty() && !e) ||
                (!attr(child, "dur").empty() && !d)) {
                notes.add(ReportOutcome::Dropped, "Elements with unreadable times were left out");
                continue;
            }
            const Ticks childBegin = begin + b.value_or(Ticks{});
            std::optional<Ticks> childEnd = e ? std::optional<Ticks>(begin + *e) : d ? std::optional<Ticks>(childBegin + *d) : end;
            if (end && childEnd && *childEnd > *end) {
                childEnd = end;
            }
            RegionInfo childRegion = region;
            if (const std::string_view r = attr(child, "region"); !r.empty()) {
                if (const auto it = regions.find(r); it != regions.end()) {
                    childRegion = it->second;
                }
            }
            std::optional<CaptionAlign> childAlign = align;
            if (const std::string_view s = attr(child, "style"); !s.empty()) {
                for (const std::string_view id : splitWords(s)) {
                    if (const auto it = styles.find(id); it != styles.end() && it->second) {
                        childAlign = it->second;
                    }
                }
            }
            if (const auto a = alignOf(attr(child, "textAlign"))) {
                childAlign = a;
            }
            if (name != "p") {
                walk(child, childBegin, childEnd, childRegion, childAlign, depth + 1);
                continue;
            }
            if (!childEnd) {
                notes.add(ReportOutcome::Dropped, "Cues without an end time were left out");
                continue;
            }
            RawCue cue;
            cue.start = childBegin;
            cue.end = *childEnd;
            text(child, cue.text, depth + 1);
            if (!attr(child, "fontStyle").empty() || !attr(child, "color").empty() || !attr(child, "fontWeight").empty()) {
                styled = true;
            }
            cue.placement = childRegion.placement;
            cue.alignment = childAlign ? childAlign : childRegion.alignment;
            cue.where = "byte " + std::to_string(child.offset_debug());
            raw.cues.push_back(std::move(cue));
            if (raw.cues.size() > kMaxCaptionCues) {
                return;
            }
        }
    }
};

} // namespace

std::expected<RawCaptions, Error> readTtml(std::string_view data, const CaptionReadOptions& options, std::string_view fileLabel) {
    RawCaptions raw;
    Notes notes;
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed =
        doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata, pugi::encoding_auto);
    if (!parsed) {
        return std::unexpected(parseError(std::string("not a Timed Text file: ") + parsed.description()));
    }
    const pugi::xml_node tt = doc.document_element();
    if (localName(tt) != "tt") {
        return std::unexpected(parseError("not a Timed Text file (the root is not <tt>)"));
    }
    Reader reader{{}, {}, {}, raw, notes};
    TimingParameters& t = reader.timing;
    if (const auto fr = parseInt64(attr(tt, "frameRate")); fr && *fr > 0 && *fr <= 300) {
        t.frameRateNum = *fr;
    } else if (options.frameRate) {
        t.frameRateNum = options.frameRate->num;
        t.frameRateDen = options.frameRate->den;
    }
    if (const std::string_view mul = attr(tt, "frameRateMultiplier"); !mul.empty()) {
        const auto words = splitWords(mul);
        const auto n = words.size() == 2 ? parseInt64(words[0]) : std::nullopt;
        const auto d = words.size() == 2 ? parseInt64(words[1]) : std::nullopt;
        if (n && d && *n > 0 && *d > 0 && *n <= 100000 && *d <= 100000) {
            t.frameRateNum *= *n;
            t.frameRateDen *= *d;
        }
    }
    if (const auto r = Rate::make(t.frameRateNum, t.frameRateDen)) {
        t.frameRateNum = r->num;
        t.frameRateDen = r->den;
    }
    if (const auto tr = parseInt64(attr(tt, "tickRate")); tr && *tr > 0) {
        t.tickRate = *tr;
    } else if (!attr(tt, "frameRate").empty()) {
        t.tickRate = t.frameRateNum / std::max<std::int64_t>(1, t.frameRateDen);
    }
    t.smpte = attr(tt, "timeBase") == "smpte";
    t.drop = attr(tt, "dropMode").starts_with("drop");
    raw.language = std::string(attr(tt, "lang").substr(0, 2));
    raw.timecodeBased = t.smpte;
    if (!attr(tt, "frameRate").empty() || t.smpte) {
        raw.frameRate = Rate::make(t.frameRateNum, t.frameRateDen);
        raw.dropFrame = t.drop;
    }
    pugi::xml_node body;
    for (const pugi::xml_node child : tt.children()) {
        if (localName(child) == "head") {
            for (const pugi::xml_node section : child.children()) {
                const std::string_view name = localName(section);
                if (name == "layout") {
                    for (const pugi::xml_node region : section.children()) {
                        if (localName(region) == "region") {
                            reader.regions.emplace(std::string(attr(region, "id")), regionOf(region));
                        }
                    }
                } else if (name == "styling") {
                    for (const pugi::xml_node style : section.children()) {
                        if (localName(style) == "style") {
                            reader.styles.emplace(std::string(attr(style, "id")), alignOf(attr(style, "textAlign")));
                            if (attr(style, "fontStyle") == "italic" || attr(style, "fontWeight") == "bold" ||
                                !attr(style, "color").empty()) {
                                reader.styled = true;
                            }
                        }
                    }
                } else if (name == "metadata") {
                    for (const pugi::xml_node m : section.children()) {
                        if (localName(m) == "title") {
                            raw.title = std::string(trim(m.text().get()));
                        }
                    }
                }
            }
        } else if (localName(child) == "body") {
            body = child;
        }
    }
    if (!body) {
        notes.flush(raw.report, fileLabel);
        return raw; // no captions
    }
    RegionInfo bodyRegion;
    if (const auto it = reader.regions.find(attr(body, "region")); it != reader.regions.end()) {
        bodyRegion = it->second;
    }
    // The body is the outermost time container: walk its parent so its own begin / end / region apply.
    reader.walk(tt, Ticks{}, std::nullopt, bodyRegion, std::nullopt, 0);
    if (reader.styled) {
        notes.add(ReportOutcome::Approximated, "Fonts, colours, italics and other styles are not kept (cue text is plain)");
    }
    if (!reader.regions.empty()) {
        notes.add(ReportOutcome::Approximated, "Regions map to the track's top or bottom placement");
    }
    notes.flush(raw.report, fileLabel);
    return raw;
}

// ---------------------------------------------------------------------------------------------------------------------
// Writing

namespace {

std::string mediaTime(Ticks t) { return clockTime(t, '.'); }

std::string smpteTime(Ticks t, Rate rate, bool drop) {
    const std::int64_t frame = ticksToFrames(t, rate, Rounding::Nearest).value_or(0);
    std::string tc = framesToTimecode(frame, rate, drop);
    std::replace(tc.begin(), tc.end(), ';', ':');
    return tc;
}

} // namespace

std::string writeTtml(const std::vector<CaptionCue>& cues, CaptionFileFormat flavour, const CaptionWriteOptions& options,
                      Notes& notes) {
    const bool itt = flavour == CaptionFileFormat::Itt;
    const bool smpte = flavour == CaptionFileFormat::SmpteTt;
    const std::string lang = options.language.empty() ? std::string("en") : options.language;
    const Rate rate = options.frameRate.isValid() ? options.frameRate.normalized() : kRate29_97;
    const bool ntsc = rate.den == 1001;
    const std::int64_t base = timecodeBase(rate);
    const bool drop = itt && options.dropFrame && supportsDropFrame(rate);
    auto escape = [](std::string_view text) {
        std::string out;
        for (const char c : text) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out.push_back(c);
            }
        }
        return out;
    };
    std::string out = itt ? "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
                          : "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\" "
           "xmlns:tts=\"http://www.w3.org/ns/ttml#styling\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\"";
    if (smpte) {
        out += " xmlns:smpte=\"http://www.smpte-ra.org/schemas/2052-1/2010/smpte-tt\""
               " ttp:profile=\"http://www.smpte-ra.org/schemas/2052-1/2010/profiles/smpte-tt-full\"";
    } else if (flavour == CaptionFileFormat::Dfxp) {
        out += " ttp:profile=\"http://www.w3.org/ns/ttml/profile/dfxp-presentation\"";
    }
    out += " xml:lang=\"" + escape(lang) + "\" ttp:timeBase=\"" + (itt ? "smpte" : "media") + "\"";
    if (itt || smpte) {
        out += " ttp:frameRate=\"" + std::to_string(base) + "\"";
        if (ntsc) {
            out += " ttp:frameRateMultiplier=\"1000 1001\"";
        }
    }
    if (itt) {
        out += std::string(" ttp:dropMode=\"") + (drop ? "dropNTSC" : "nonDrop") + "\"";
    }
    out += ">\n  <head>\n";
    if (!options.title.empty()) {
        out += "    <metadata>\n      <ttm:title>" + escape(options.title) + "</ttm:title>\n    </metadata>\n";
    }
    const char* textAlign = options.alignment == CaptionAlign::Left    ? "left"
                            : options.alignment == CaptionAlign::Right ? "right"
                                                                       : "center";
    out += "    <styling>\n"
           "      <style xml:id=\"normal\" tts:fontFamily=\"sansSerif\" tts:fontWeight=\"normal\" tts:fontStyle=\"normal\" "
           "tts:color=\"white\" tts:fontSize=\"100%\"/>\n"
           "    </styling>\n"
           "    <layout>\n";
    out += std::string("      <region xml:id=\"top\" tts:origin=\"10% 10%\" tts:extent=\"80% 20%\" tts:textAlign=\"") +
           textAlign + "\" tts:displayAlign=\"before\"/>\n";
    out += std::string("      <region xml:id=\"bottom\" tts:origin=\"10% 70%\" tts:extent=\"80% 20%\" tts:textAlign=\"") +
           textAlign + "\" tts:displayAlign=\"after\"/>\n";
    out += "    </layout>\n  </head>\n";
    out += std::string("  <body style=\"normal\" region=\"") + (options.placement == CaptionPlacement::Top ? "top" : "bottom") +
           "\">\n    <div>\n";
    int index = 0;
    bool rounded = false;
    for (const CaptionCue& cue : cues) {
        const std::vector<std::string> lines = textLines(cue.text);
        if (lines.empty()) {
            continue;
        }
        ++index;
        out += "      <p";
        if (itt) {
            const Ticks start = cue.start + options.startTimecode;
            const Ticks end = cue.end() + options.startTimecode;
            rounded = rounded || snapToFrame(start, rate) != start || snapToFrame(end, rate) != end;
            out += " begin=\"" + smpteTime(start, rate, drop) + "\" end=\"" + smpteTime(end, rate, drop) + "\"";
        } else {
            out += " xml:id=\"caption" + std::to_string(index) + "\" begin=\"" + mediaTime(cue.start) + "\" end=\"" +
                   mediaTime(cue.end()) + "\"";
        }
        out += ">";
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) {
                out += "<br/>";
            }
            out += escape(lines[i]);
        }
        out += "</p>\n";
    }
    out += "    </div>\n  </body>\n</tt>\n";
    if (rounded) {
        notes.add(ReportOutcome::Approximated, "Cue times rounded to whole frames (iTT times are timecodes)");
    }
    return out;
}
} // namespace wabcut::interchange::detail::captions
