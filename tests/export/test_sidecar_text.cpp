// Codex wave-3 P2-10 (docs/internal/reviews-codex-wave3.md): caption sidecars carry plain text. Text that looks like markup
// ("<name>", "&", "<i>", "{\an8}") must reach a player as those characters, and nothing inside a cue (blank lines, a
// line that looks like a timing line) may end the cue or start another. The generated SRT and WebVTT files are parsed
// back here the way a lenient player reads them (tags interpreted, any timing-looking line starts a cue) and compared
// with the cues as written. Failed before the fix: "<name>" vanished as a tag and the injected line became a cue.

#include <wabcut/export/CaptionSidecar.h>

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::exporter;

namespace {

Ticks ms(std::int64_t v) {
    return Ticks{v * kTicksPerSecond / 1000};
}

struct ParsedCue {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::string text; ///< the displayed text: tags interpreted away, entities decoded, lines joined by '\n'
};

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

const std::regex kTiming(R"((\d\d):(\d\d):(\d\d)[,.](\d\d\d)\s*-->\s*(\d\d):(\d\d):(\d\d)[,.](\d\d\d).*)");

std::int64_t toMs(const std::smatch& m, int first) {
    return ((std::stoll(m[first].str()) * 60 + std::stoll(m[first + 1].str())) * 60 + std::stoll(m[first + 2].str())) * 1000 +
           std::stoll(m[first + 3].str());
}

/// What a player shows of a payload line. WebVTT: every "<...>" is a tag (the spec), entities are decoded. SRT has no
/// escapes; players interpret the HTML-like tags they know (b, i, u, s, font) and SSA override blocks ("{\an8}").
std::string displayed(std::string line, bool vtt) {
    if (vtt) {
        line = std::regex_replace(line, std::regex(R"(<[^>]*>)"), "");
    } else {
        line = std::regex_replace(line, std::regex(R"(</?(b|i|u|s|font)\b[^>]*>)", std::regex::icase), "");
        line = std::regex_replace(line, std::regex(R"(\{\\[^}]*\})"), "");
    }
    for (const auto& [entity, text] : {std::pair{"&lt;", "<"}, std::pair{"&gt;", ">"}, std::pair{"&nbsp;", "\xC2\xA0"},
                                       std::pair{"&amp;", "&"}}) {
        if (vtt) {
            line = std::regex_replace(line, std::regex(entity), text);
        }
    }
    // Zero-width spaces are invisible.
    for (std::size_t at; (at = line.find("\xE2\x80\x8B")) != std::string::npos;) {
        line.erase(at, 3);
    }
    return line;
}

/// Blocks are separated by blank lines; within a block any timing-looking line starts a new cue (lenient parsers).
std::vector<ParsedCue> parse(const std::string& text, bool vtt) {
    std::vector<ParsedCue> cues;
    std::vector<std::string> all = lines(text);
    std::size_t i = 0;
    if (vtt) {
        REQUIRE(!all.empty());
        CHECK(all[0] == "WEBVTT");
        i = 1;
    }
    bool inCue = false;
    for (; i < all.size(); ++i) {
        const std::string& line = all[i];
        std::smatch m;
        if (line.empty()) {
            inCue = false;
            continue;
        }
        if (std::regex_match(line, m, kTiming)) {
            cues.push_back({toMs(m, 1), toMs(m, 5), {}});
            inCue = true;
            continue;
        }
        if (!inCue) {
            continue; // an SRT index or a VTT cue id before the timing line
        }
        std::string& t = cues.back().text;
        t += (t.empty() ? "" : "\n") + displayed(line, vtt);
    }
    return cues;
}

/// The cue text as it should be shown: line ends normalised, blank (or blank-looking) lines dropped.
std::string expected(const std::string& text) {
    std::string out;
    for (const std::string& line : lines(text)) {
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        out += (out.empty() ? "" : "\n") + line;
    }
    return out;
}

} // namespace

TEST_CASE("Caption sidecars (wave-3 P2-10): plain text survives SRT and VTT; nothing ends a cue or makes another",
          "[export][captions][wave3]") {
    CaptionTrack track;
    track.id = CaptionTrackId{1};
    track.enabled = true;
    track.cues = {
        {ms(0), ms(2000), "Use <name> & stuff\n\n00:00:07.000 --> 00:00:08.000\ninjected"},
        {ms(3000), ms(1000), "<i>not italic</i> {\\an8} a --> b & c&amp;d"},
        {ms(5000), ms(1000), "Line one\r\nLine two\r\n   \r\n\r\n3\r\n00:00:09,000 --> 00:00:10,000\r\nstill this cue"},
        {ms(7000), ms(500), "1 < 2 > 0"},
    };
    for (const CaptionExport format : {CaptionExport::Srt, CaptionExport::Vtt}) {
        const bool vtt = format == CaptionExport::Vtt;
        INFO((vtt ? "VTT" : "SRT"));
        const std::string text = captionSidecarText(track, {ms(500), ms(9000)}, format);
        INFO(text);
        const std::vector<ParsedCue> cues = parse(text, vtt);
        REQUIRE(cues.size() == track.cues.size());
        for (std::size_t i = 0; i < cues.size(); ++i) {
            const CaptionCue& cue = track.cues[i];
            const std::int64_t start = std::max<std::int64_t>(0, cue.start.value * 1000 / kTicksPerSecond - 500);
            CHECK(cues[i].startMs == start);
            CHECK(cues[i].endMs == cue.end().value * 1000 / kTicksPerSecond - 500);
            CHECK(cues[i].text == expected(cue.text));
        }
    }
    // Ordinary text is untouched (the existing exact-output expectations hold).
    CaptionTrack plain = track;
    plain.cues = {{ms(0), ms(1500), "Line one\nLine two"}};
    CHECK(captionSidecarText(plain, {ms(500), ms(500)}, CaptionExport::Vtt) ==
          "WEBVTT\n\n00:00:00.000 --> 00:00:00.500\nLine one\nLine two\n\n");
    CHECK(captionSidecarText(plain, {ms(500), ms(500)}, CaptionExport::Srt) ==
          "1\n00:00:00,000 --> 00:00:00,500\nLine one\nLine two\n\n");
}
