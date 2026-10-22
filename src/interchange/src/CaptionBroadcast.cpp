// Broadcast caption files (CaptionFiles.h): Scenarist SCC and MacCaption MCC (CEA-608 in the file), and EBU N19
// subtitles (.stl, EBU Tech 3264).

#include "CaptionFormats.h"

#include "AudioPost.h"
#include "TextUtil.h"

#include <wabcut/model/CaptionText.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace wabcut::interchange::detail::captions {

namespace {

std::vector<std::string_view> splitLinesKeepEmpty(std::string_view text) {
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

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// The NTSC rate SCC and MCC captions run at: 29.97, or 30 when the caller says so.
Rate captionRate(std::optional<Rate> requested) {
    return requested && *requested == kRate30 ? kRate30 : kRate29_97;
}

constexpr std::string_view kMccHeader =
    "File Format=MacCaption_MCC V1.0\n"
    "\n"
    "///////////////////////////////////////////////////////////////////////////////////\n"
    "// Computer Prompting & Captioning Company\n"
    "// Ancillary Data Packet Transfer File\n"
    "//\n"
    "// Permission to generate this format is granted provided that\n"
    "//   1. This ANC Transfer file format is used on an as-is basis and no warranty is given, and\n"
    "//   2. This entire descriptive information text is included in a generated .mcc file.\n"
    "//\n"
    "// General file format:\n"
    "//   HH:MM:SS:FF(tab)[Hexadecimal ANC data in groups of 2 characters]\n"
    "//     Hexadecimal data starts with the Ancillary Data Packet DID (Data ID defined in S291M)\n"
    "//       and concludes with the Check Sum following the User Data Words.\n"
    "//     Each time code line must contain at most one complete ancillary data packet.\n"
    "//     To transfer additional ANC Data successive lines may contain identical time code.\n"
    "//     Time Code Rate=[24, 25, 30, 30DF, 50, 60]\n"
    "//\n"
    "//   ANC data bytes may be represented by one ASCII character according to the following schema:\n"
    "//     G  FAh 00h 00h\n"
    "//     H  2 x (FAh 00h 00h)\n"
    "//     I  3 x (FAh 00h 00h)\n"
    "//     J  4 x (FAh 00h 00h)\n"
    "//     K  5 x (FAh 00h 00h)\n"
    "//     L  6 x (FAh 00h 00h)\n"
    "//     M  7 x (FAh 00h 00h)\n"
    "//     N  8 x (FAh 00h 00h)\n"
    "//     O  9 x (FAh 00h 00h)\n"
    "//     P  FBh 80h 80h\n"
    "//     Q  FCh 80h 80h\n"
    "//     R  FDh 80h 80h\n"
    "//     S  96h 69h\n"
    "//     T  61h 01h\n"
    "//     U  E1h 00h 00h 00h\n"
    "//     Z  00h\n"
    "//\n"
    "///////////////////////////////////////////////////////////////////////////////////\n";

/// MCC's one-character aliases for common byte runs.
std::string mccEncode(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    std::size_t i = 0;
    auto at = [&](std::size_t k) { return i + k < bytes.size() ? bytes[i + k] : 0xFFFF; };
    while (i < bytes.size()) {
        if (at(0) == 0xFA && at(1) == 0 && at(2) == 0) {
            int run = 0;
            while (run < 9 && i + 3 * static_cast<std::size_t>(run) + 2 < bytes.size() &&
                   bytes[i + 3 * static_cast<std::size_t>(run)] == 0xFA && bytes[i + 3 * static_cast<std::size_t>(run) + 1] == 0 &&
                   bytes[i + 3 * static_cast<std::size_t>(run) + 2] == 0) {
                ++run;
            }
            out.push_back(static_cast<char>('G' + run - 1));
            i += 3 * static_cast<std::size_t>(run);
            continue;
        }
        if ((at(0) == 0xFB || at(0) == 0xFC || at(0) == 0xFD) && at(1) == 0x80 && at(2) == 0x80) {
            out.push_back(static_cast<char>('P' + (bytes[i] - 0xFB)));
            i += 3;
            continue;
        }
        if (at(0) == 0x96 && at(1) == 0x69) {
            out.push_back('S');
            i += 2;
            continue;
        }
        if (at(0) == 0x61 && at(1) == 0x01) {
            out.push_back('T');
            i += 2;
            continue;
        }
        if (at(0) == 0) {
            out.push_back('Z');
            ++i;
            continue;
        }
        static constexpr char kHex[] = "0123456789ABCDEF";
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0xF]);
        ++i;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> mccDecode(std::string_view text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= 'G' && c <= 'O') {
            for (int k = 0; k <= c - 'G'; ++k) {
                out.insert(out.end(), {0xFA, 0x00, 0x00});
            }
        } else if (c >= 'P' && c <= 'R') {
            out.insert(out.end(), {static_cast<std::uint8_t>(0xFB + (c - 'P')), 0x80, 0x80});
        } else if (c == 'S') {
            out.insert(out.end(), {0x96, 0x69});
        } else if (c == 'T') {
            out.insert(out.end(), {0x61, 0x01});
        } else if (c == 'U') {
            out.insert(out.end(), {0xE1, 0x00, 0x00, 0x00});
        } else if (c == 'Z') {
            out.push_back(0x00);
        } else if (c == ' ' || c == '\t') {
            continue;
        } else {
            const int hi = hexValue(c);
            const int lo = i + 1 < text.size() ? hexValue(text[i + 1]) : -1;
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
            ++i;
        }
        if (out.size() > 4096) {
            return std::nullopt;
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// SCC

std::expected<RawCaptions, Error> readScc(std::string_view data, const CaptionReadOptions& options, std::string_view fileLabel) {
    RawCaptions raw;
    Notes notes;
    if (data.starts_with("\xEF\xBB\xBF")) {
        data.remove_prefix(3);
    }
    const auto all = splitLinesKeepEmpty(data);
    std::size_t first = 0;
    while (first < all.size() && trim(all[first]).empty()) {
        ++first;
    }
    if (first >= all.size() || !trim(all[first]).starts_with("Scenarist_SCC")) {
        return std::unexpected(parseError("not a Scenarist SCC file (no Scenarist_SCC header)"));
    }
    const Rate rate = captionRate(options.frameRate);
    std::vector<std::pair<std::int64_t, std::pair<std::uint8_t, std::uint8_t>>> pairs;
    bool drop = false;
    std::size_t badLines = 0;
    for (std::size_t i = first + 1; i < all.size(); ++i) {
        const std::string_view line = trim(all[i]);
        if (line.empty()) {
            continue;
        }
        const auto words = splitWords(line);
        if (words.empty()) {
            continue;
        }
        const bool df = words[0].find(';') != std::string_view::npos || words[0].find('.') != std::string_view::npos;
        drop = drop || df;
        const auto frame = parseTimecodeFrames(words[0], rate, df && supportsDropFrame(rate));
        if (!frame || *frame < 0) {
            ++badLines;
            continue;
        }
        std::int64_t at = *frame;
        for (std::size_t w = 1; w < words.size(); ++w) {
            const std::string_view word = words[w];
            if (word.size() != 4) {
                ++badLines;
                break;
            }
            const int a = hexValue(word[0]) * 16 + hexValue(word[1]);
            const int b = hexValue(word[2]) * 16 + hexValue(word[3]);
            if (hexValue(word[0]) < 0 || hexValue(word[1]) < 0 || hexValue(word[2]) < 0 || hexValue(word[3]) < 0) {
                ++badLines;
                break;
            }
            pairs.push_back({at++, {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b)}});
        }
        if (pairs.size() > 20'000'000) {
            break;
        }
    }
    if (badLines > 0) {
        notes.add(ReportOutcome::Dropped, "Lines or words that are not SCC data were skipped");
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    raw.frameRate = rate;
    raw.dropFrame = drop;
    raw.timecodeBased = true;
    cea608Cues(pairs, rate, raw, notes);
    notes.flush(raw.report, fileLabel);
    return raw;
}

std::expected<std::string, Error> writeScc(const std::vector<CaptionCue>& cues, const CaptionWriteOptions& options, Notes& notes) {
    const Rate rate = captionRate(options.frameRate);
    if (!(options.frameRate == kRate29_97) && !(options.frameRate == kRate30)) {
        notes.add(ReportOutcome::Approximated, "SCC captions are timed at 29.97 fps (the sequence's frame rate is not NTSC)");
    }
    const bool drop = options.dropFrame && rate == kRate29_97;
    const auto pairs = cea608Pairs(cues, options, rate, notes);
    std::string out = "Scenarist_SCC V1.0";
    std::int64_t expected = -1;
    for (const auto& [frame, pair] : pairs) {
        char word[8];
        std::snprintf(word, sizeof word, "%02x%02x", pair.first, pair.second);
        if (frame != expected) {
            out += "\n\n" + framesToTimecode(frame, rate, drop) + "\t" + word;
        } else {
            out += std::string(" ") + word;
        }
        expected = frame + 1;
    }
    out += "\n\n";
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// MCC

std::expected<RawCaptions, Error> readMcc(std::string_view data, const CaptionReadOptions& options, std::string_view fileLabel) {
    RawCaptions raw;
    Notes notes;
    if (data.starts_with("\xEF\xBB\xBF")) {
        data.remove_prefix(3);
    }
    const auto all = splitLinesKeepEmpty(data);
    if (all.empty() || !trim(all.front()).starts_with("File Format=MacCaption_MCC")) {
        return std::unexpected(parseError("not a MacCaption MCC file"));
    }
    Rate rate = captionRate(options.frameRate);
    bool drop = true;
    std::vector<std::pair<std::int64_t, std::pair<std::uint8_t, std::uint8_t>>> pairs;
    std::size_t badLines = 0;
    std::size_t otherPackets = 0;
    std::size_t badChecksums = 0;
    for (std::size_t i = 1; i < all.size(); ++i) {
        const std::string_view line = trim(all[i]);
        if (line.empty() || line.starts_with("//")) {
            continue;
        }
        if (line.starts_with("Time Code Rate=")) {
            const std::string_view value = trim(line.substr(15));
            drop = value.ends_with("DF");
            const auto fps = parseInt64(value.substr(0, value.size() - (drop ? 2 : 0)));
            if (fps && *fps == 30) {
                rate = drop ? kRate29_97 : kRate30;
            } else if (fps && *fps == 60) {
                rate = drop ? kRate59_94 : kRate60;
            } else if (fps && (*fps == 24 || *fps == 25 || *fps == 50)) {
                rate = Rate{static_cast<std::int32_t>(*fps), 1};
            }
            continue;
        }
        if (line.find('=') != std::string_view::npos && line.find('\t') == std::string_view::npos) {
            continue; // UUID=, Creation Program=, ...
        }
        const std::size_t tab = line.find_first_of("\t ");
        if (tab == std::string_view::npos) {
            ++badLines;
            continue;
        }
        std::string_view tc = line.substr(0, tab);
        if (const std::size_t dot = tc.find('.'); dot != std::string_view::npos) {
            if (tc.substr(dot + 1).starts_with("1")) {
                continue; // field 2
            }
            tc = tc.substr(0, dot);
        }
        const auto frame = parseTimecodeFrames(tc, rate, drop && supportsDropFrame(rate));
        const auto bytes = mccDecode(trim(line.substr(tab + 1)));
        if (!frame || !bytes || bytes->size() < 4) {
            ++badLines;
            continue;
        }
        const std::vector<std::uint8_t>& anc = *bytes;
        if (anc[0] != 0x61 || anc[1] != 0x01) {
            ++otherPackets;
            continue;
        }
        const std::size_t dc = anc[2];
        if (anc.size() < 3 + dc + 1) {
            ++badLines;
            continue;
        }
        std::uint8_t sum = 0;
        for (std::size_t k = 0; k < 3 + dc; ++k) {
            sum = static_cast<std::uint8_t>(sum + anc[k]);
        }
        if (sum != anc[3 + dc]) {
            ++badChecksums;
        }
        // The CDP: 96 69, length, frame rate, flags, sequence counter, [time code section], cc_data section.
        const std::uint8_t* cdp = anc.data() + 3;
        if (dc < 7 || cdp[0] != 0x96 || cdp[1] != 0x69) {
            ++badLines;
            continue;
        }
        std::size_t at = 7;
        const std::uint8_t flags = cdp[4];
        if ((flags & 0x80) != 0) {
            if (at + 5 > dc || cdp[at] != 0x71) {
                ++badLines;
                continue;
            }
            at += 5;
        }
        if ((flags & 0x40) == 0 || at + 2 > dc || cdp[at] != 0x72) {
            continue; // no caption data in this packet
        }
        const std::size_t count = cdp[at + 1] & 0x1F;
        at += 2;
        for (std::size_t k = 0; k < count && at + 3 <= dc; ++k, at += 3) {
            const std::uint8_t header = cdp[at];
            if ((header & 0x04) != 0 && (header & 0x03) == 0) { // valid, NTSC field 1
                pairs.push_back({*frame, {cdp[at + 1], cdp[at + 2]}});
            }
        }
        if (pairs.size() > 20'000'000) {
            break;
        }
    }
    if (badLines > 0) {
        notes.add(ReportOutcome::Dropped, "Lines that are not MCC caption packets were skipped");
    }
    if (otherPackets > 0) {
        notes.add(ReportOutcome::Dropped, "Ancillary packets other than captions (CDPs) were skipped");
    }
    if (badChecksums > 0) {
        notes.add(ReportOutcome::Approximated, "Packets with wrong checksums were read anyway");
    }
    notes.add(ReportOutcome::Dropped, "CEA-708 caption services in the packets are not read (the 608 captions are)");
    raw.frameRate = rate;
    raw.dropFrame = drop;
    raw.timecodeBased = true;
    cea608Cues(pairs, rate, raw, notes);
    notes.flush(raw.report, fileLabel);
    return raw;
}

std::expected<std::string, Error> writeMcc(const std::vector<CaptionCue>& cues, const CaptionWriteOptions& options, Notes& notes) {
    const Rate rate = captionRate(options.frameRate);
    if (!(options.frameRate == kRate29_97) && !(options.frameRate == kRate30)) {
        notes.add(ReportOutcome::Approximated, "MCC captions are written at 29.97 fps (the sequence's frame rate is not NTSC)");
    }
    const bool drop = options.dropFrame && rate == kRate29_97;
    const auto pairs = cea608Pairs(cues, options, rate, notes);
    std::string text;
    for (const CaptionCue& c : cues) {
        text += std::to_string(c.start.value) + "/" + std::to_string(c.duration.value) + "/" + c.text + "\n";
    }
    const auto id = audiopost::stableId("mcc\n" + text);
    char uuid[40];
    std::snprintf(uuid, sizeof uuid, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", id[0], id[1], id[2],
                  id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
    std::string out(kMccHeader);
    out += "\nUUID=" + std::string(uuid) + "\n";
    out += "Creation Program=" + (options.creationProgram.empty() ? std::string("Wabcut") : options.creationProgram) + "\n";
    if (!options.creationDate.empty()) {
        out += "Creation Date=" + options.creationDate + "\n";
    }
    if (!options.creationTime.empty()) {
        out += "Creation Time=" + options.creationTime + "\n";
    }
    out += std::string("Time Code Rate=") + (rate == kRate30 ? "30" : drop ? "30DF" : "30") + "\n\n";
    if (pairs.empty()) {
        return out;
    }
    const std::uint8_t frameRateByte = rate == kRate30 ? 0x5F : 0x4F;
    const std::int64_t first = pairs.begin()->first;
    const std::int64_t last = pairs.rbegin()->first;
    std::uint16_t sequence = 0;
    for (std::int64_t frame = first; frame <= last; ++frame) {
        const auto it = pairs.find(frame);
        std::vector<std::uint8_t> cdp{0x96, 0x69, 0x00, frameRateByte, 0x43, static_cast<std::uint8_t>(sequence >> 8),
                                      static_cast<std::uint8_t>(sequence & 0xFF), 0x72, static_cast<std::uint8_t>(0xE0 | 20)};
        cdp.push_back(0xFC); // NTSC field 1, valid
        cdp.push_back(it != pairs.end() ? it->second.first : 0x80);
        cdp.push_back(it != pairs.end() ? it->second.second : 0x80);
        cdp.insert(cdp.end(), {0xFD, 0x80, 0x80}); // field 2: nothing
        for (int k = 0; k < 18; ++k) {
            cdp.insert(cdp.end(), {0xFA, 0x00, 0x00}); // no CEA-708 data
        }
        cdp.insert(cdp.end(), {0x74, static_cast<std::uint8_t>(sequence >> 8), static_cast<std::uint8_t>(sequence & 0xFF), 0x00});
        cdp[2] = static_cast<std::uint8_t>(cdp.size());
        std::uint8_t sum = 0;
        for (const std::uint8_t b : cdp) {
            sum = static_cast<std::uint8_t>(sum + b);
        }
        cdp.back() = static_cast<std::uint8_t>(-sum); // the CDP's bytes sum to 0 modulo 256
        std::vector<std::uint8_t> anc{0x61, 0x01, static_cast<std::uint8_t>(cdp.size())};
        anc.insert(anc.end(), cdp.begin(), cdp.end());
        std::uint8_t checksum = 0;
        for (const std::uint8_t b : anc) {
            checksum = static_cast<std::uint8_t>(checksum + b);
        }
        anc.push_back(checksum);
        std::string tc = framesToTimecode(frame, rate, drop);
        std::replace(tc.begin(), tc.end(), ';', ':'); // MCC writes ':' in drop-frame timecodes too
        out += tc + "\t" + mccEncode(anc) + "\n";
        ++sequence;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// EBU STL (EBU Tech 3264): a 1024-byte General Subtitle Information block, then 128-byte Text and Timing Information
// blocks. Text is ISO 6937 (character code table 00), a non-spacing diacritic before its letter.

namespace {

/// ISO 6937 0xA0-0xBF and 0xD0-0xFF (0: unassigned or a diacritic).
constexpr std::array<char32_t, 96> kIso6937High = {
    0xA0,   0xA1, 0xA2, 0xA3, 0x24,   0xA5, 0x23,   0xA7, 0xA4,   0x2018, 0x201C, 0xAB, 0x2190, 0x2191, 0x2192, 0x2193, // A0
    0xB0,   0xB1, 0xB2, 0xB3, 0xD7,   0xB5, 0xB6,   0xB7, 0xF7,   0x2019, 0x201D, 0xBB, 0xBC,   0xBD,   0xBE,   0xBF,   // B0
    0,      0,    0,    0,    0,      0,    0,      0,    0,      0,      0,      0,    0,      0,      0,      0,      // C0
    0x2015, 0xB9, 0xAE, 0xA9, 0x2122, 0x266A, 0xAC, 0xA6, 0,      0,      0,      0,    0x215B, 0x215C, 0x215D, 0x215E, // D0
    0x2126, 0xC6, 0x110, 0xAA, 0x126, 0,    0x132,  0x13F, 0x141, 0xD8,   0x152,  0xBA, 0xDE,   0x166,  0x14A,  0x149,  // E0
    0x138,  0xE6, 0x111, 0xF0, 0x127, 0x131, 0x133, 0x140, 0x142, 0xF8,   0x153,  0xDF, 0xFE,   0x167,  0x14B,  0xAD,   // F0
};

/// Non-spacing diacritics 0xC1-0xCF: the letters each combines with and the precomposed results.
struct Diacritic {
    std::uint8_t code;
    std::string_view bases;
    std::u32string_view composed;
};
constexpr Diacritic kDiacritics[] = {
    {0xC1, "AEIOUaeiou", U"ÀÈÌÒÙàèìòù"},                     // grave
    {0xC2, "AEIOUYaeiouyCcGgLlNnRrSsZz",
     U"ÁÉÍÓÚÝáéíóúýĆćǴǵĹĺŃń"
     U"ŔŕŚśŹź"},                                                                   // acute
    {0xC3, "AEIOUaeiouCcGgHhJjSsWwYy",
     U"ÂÊÎÔÛâêîôûĈĉĜĝĤĥĴĵŜŝ"
     U"ŴŵŶŷ"},                                                                               // circumflex
    {0xC4, "ANOanoIiUu", U"ÃÑÕãñõĨĩŨũ"},                     // tilde
    {0xC5, "AaEeIiOoUu", U"ĀāĒēĪīŌōŪū"},                     // macron
    {0xC6, "AaGgUu", U"ĂăĞğŬŭ"},                                                 // breve
    {0xC7, "CcEeGgIZz", U"ĊċĖėĠġİŻż"},                            // dot above
    {0xC8, "AEIOUaeiouyY", U"ÄËÏÖÜäëïöüÿŸ"},       // diaeresis
    {0xCA, "AaUu", U"ÅåŮů"},                                                               // ring
    {0xCB, "CcGkKlLnNrRsStT", U"ÇçĢķĶļĻņŅŗŖşŞţŢ"}, // cedilla
    {0xCD, "OoUu", U"ŐőŰű"},                                                               // double acute
    {0xCE, "AaEeIiUu", U"ĄąĘęĮįŲų"},                                   // ogonek
    {0xCF, "CcDdEeLlNnRrSsTtZz",
     U"ČčĎďĚěĽľŇňŘřŠšŤťŽž"}, // caron
};

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::u32string toCodePoints(std::string_view text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + len > text.size()) {
            len = 1;
        }
        char32_t cp = len == 1 ? c : static_cast<char32_t>(c & (0x7F >> len));
        for (std::size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

/// ISO 6937 bytes of `text` (one line), and how many characters had no code.
std::string toIso6937(std::string_view text, std::size_t& replaced) {
    std::string out;
    for (const char32_t ch : toCodePoints(text)) {
        if (ch >= 0x20 && ch < 0x7F && ch != 0x24) {
            out.push_back(static_cast<char>(ch));
            continue;
        }
        if (ch == 0x24) {
            out.push_back(static_cast<char>(0xA4)); // '$' sits at 0xA4 in ISO 6937
            continue;
        }
        bool done = false;
        for (std::size_t i = 0; i < kIso6937High.size() && !done; ++i) {
            if (kIso6937High[i] == ch && ch != 0 && !(i >= 0x20 && i < 0x30)) {
                out.push_back(static_cast<char>(0xA0 + i));
                done = true;
            }
        }
        for (const Diacritic& d : kDiacritics) {
            if (done) {
                break;
            }
            const std::size_t at = d.composed.find(ch);
            if (at != std::u32string_view::npos) {
                out.push_back(static_cast<char>(d.code));
                out.push_back(d.bases[at]);
                done = true;
            }
        }
        if (!done) {
            out.push_back('?');
            ++replaced;
        }
    }
    return out;
}

/// Text of a TTI text field: ISO 6937 to UTF-8, 0x8A between lines, control codes dropped.
std::string fromIso6937(std::string_view field, bool& styled) {
    std::string out;
    for (std::size_t i = 0; i < field.size(); ++i) {
        const auto b = static_cast<std::uint8_t>(field[i]);
        if (b == 0x8F) {
            break; // unused space to the end
        }
        if (b == 0x8A) {
            out.push_back('\n');
            continue;
        }
        if (b >= 0x80 && b <= 0x85) {
            styled = true; // italics, underline, boxing on / off
            continue;
        }
        if (b < 0x20 || (b >= 0x86 && b < 0xA0)) {
            if (b <= 0x07 || (b >= 0x10 && b <= 0x17) || b == 0x1C || b == 0x1D) {
                styled = true; // teletext colours and backgrounds
            }
            if (b == 0x20 || b == 0x0B || b == 0x0A || b == 0x0D) {
                continue;
            }
            continue;
        }
        if (b >= 0xC1 && b <= 0xCF) {
            if (i + 1 >= field.size()) {
                break;
            }
            const char base = field[i + 1];
            ++i;
            char32_t composed = static_cast<unsigned char>(base);
            for (const Diacritic& d : kDiacritics) {
                if (d.code == b) {
                    const std::size_t at = d.bases.find(base);
                    if (at != std::string_view::npos) {
                        composed = d.composed[at];
                    }
                }
            }
            appendUtf8(out, composed);
            continue;
        }
        if (b >= 0xA0) {
            const char32_t ch = kIso6937High[b - 0xA0];
            if (ch != 0) {
                appendUtf8(out, ch);
            }
            continue;
        }
        out.push_back(static_cast<char>(b));
    }
    return out;
}

/// EBU language codes (Tech 3264 appendix 3) of the most common ISO 639-1 codes.
constexpr std::pair<std::string_view, std::string_view> kLanguages[] = {
    {"en", "09"}, {"fr", "0F"}, {"de", "08"}, {"es", "0A"}, {"it", "15"}, {"nl", "1D"}, {"pt", "21"}, {"sv", "28"},
    {"da", "07"}, {"fi", "27"}, {"no", "1E"}, {"pl", "20"}, {"cs", "05"}, {"hu", "17"}, {"tr", "2C"}, {"el", "43"},
    {"ru", "56"}, {"ar", "41"}, {"ja", "69"}, {"zh", "75"}, {"ko", "6B"}, {"th", "5D"}, {"hr", "06"}, {"ro", "22"},
};

std::string fixed(std::string_view text, std::size_t size, char pad = ' ') {
    std::string out(text.substr(0, size));
    out.resize(size, pad);
    return out;
}

} // namespace

std::expected<RawCaptions, Error> readStl(std::string_view data, const CaptionReadOptions& options, std::string_view fileLabel) {
    RawCaptions raw;
    Notes notes;
    if (data.size() < 1024 || data.substr(3, 3) != "STL" || data.substr(8, 3) != ".01") {
        return std::unexpected(parseError("not an EBU STL file (no GSI block)"));
    }
    const std::string_view gsi = data.substr(0, 1024);
    const std::string_view dfc = gsi.substr(3, 8);
    Rate rate = dfc.substr(3, 2) == "25" ? kRate25 : kRate30;
    if (options.frameRate && (*options.frameRate == kRate29_97 || *options.frameRate == kRate30) && rate == kRate30) {
        rate = *options.frameRate;
    }
    if (gsi.substr(12, 2) != "00") {
        notes.add(ReportOutcome::Approximated, "Character code tables other than Latin (00) are read as Latin");
    }
    const std::string lc = toLowerAscii(gsi.substr(14, 2));
    for (const auto& [iso, ebu] : kLanguages) {
        if (toLowerAscii(ebu) == lc) {
            raw.language = std::string(iso);
        }
    }
    raw.title = std::string(trim(toUtf8Lenient(gsi.substr(16, 32))));
    // Time code start of programme (HHMMSSFF): times count from it.
    std::int64_t programmeStart = 0;
    if (const std::string_view tcp = gsi.substr(256, 8);
        std::all_of(tcp.begin(), tcp.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        const auto v = parseTimecodeFrames(std::string(tcp.substr(0, 2)) + ":" + std::string(tcp.substr(2, 2)) + ":" +
                                               std::string(tcp.substr(4, 2)) + ":" + std::string(tcp.substr(6, 2)),
                                           rate, false);
        programmeStart = v.value_or(0);
    }
    const std::size_t blocks = (data.size() - 1024) / 128;
    bool styled = false;
    std::string pending;
    std::optional<RawCue> building;
    for (std::size_t k = 0; k < blocks && raw.cues.size() <= kMaxCaptionCues; ++k) {
        const std::string_view tti = data.substr(1024 + k * 128, 128);
        const auto u = [&](std::size_t at) { return static_cast<std::uint8_t>(tti[at]); };
        const std::uint8_t ebn = u(3);
        const bool comment = u(15) == 1;
        auto frames = [&](std::size_t at) {
            return ((static_cast<std::int64_t>(u(at)) * 60 + u(at + 1)) * 60 + u(at + 2)) * timecodeBase(rate) + u(at + 3);
        };
        if (comment) {
            continue;
        }
        if (ebn >= 0xF0 && ebn != 0xFF) {
            continue; // user data blocks
        }
        if (!building) {
            RawCue cue;
            const std::int64_t in = frames(5) - (options.startTimecode.value > 0 ? 0 : programmeStart);
            const std::int64_t out = frames(9) - (options.startTimecode.value > 0 ? 0 : programmeStart);
            cue.start = framesToTicks(in, rate, Rounding::Nearest).value_or(Ticks{});
            cue.end = framesToTicks(out, rate, Rounding::Nearest).value_or(Ticks{});
            const std::uint8_t vp = u(13);
            cue.placement = vp > 0 && vp <= 11 ? CaptionPlacement::Top : CaptionPlacement::Bottom;
            const std::uint8_t jc = u(14);
            cue.alignment = jc == 1 ? CaptionAlign::Left : jc == 3 ? CaptionAlign::Right : CaptionAlign::Center;
            cue.where = "TTI block " + std::to_string(k + 1);
            building = std::move(cue);
        }
        building->text += fromIso6937(tti.substr(16, 112), styled);
        if (ebn == 0xFF) {
            raw.cues.push_back(std::move(*building));
            building.reset();
        }
    }
    if (building) {
        raw.cues.push_back(std::move(*building));
    }
    if (styled) {
        notes.add(ReportOutcome::Approximated, "Italics, underline, boxing and teletext colours are not kept");
    }
    raw.frameRate = rate;
    raw.timecodeBased = options.startTimecode.value > 0;
    if (programmeStart > 0 && options.startTimecode.value == 0) {
        notes.add(ReportOutcome::Approximated, "Times count from the programme's start timecode (" +
                                                   framesToTimecode(programmeStart, rate, false) + ")");
    }
    notes.flush(raw.report, fileLabel);
    return raw;
}

std::expected<std::string, Error> writeStl(const std::vector<CaptionCue>& cues, const CaptionWriteOptions& options, Notes& notes) {
    const bool is25 = options.frameRate == kRate25 || options.frameRate == kRate50;
    const Rate rate = is25 ? kRate25 : options.frameRate == kRate30 ? kRate30 : kRate29_97;
    if (!(options.frameRate == kRate25) && !(options.frameRate == kRate30) && !(options.frameRate == kRate29_97)) {
        notes.add(ReportOutcome::Approximated,
                  std::string("EBU STL is timed at 25 or 30 fps: written at ") + (is25 ? "25" : "30") + " fps");
    }
    // Text: at most 3 rows of at most 40 characters (Tech 3264's teletext limits).
    std::vector<std::vector<std::string>> texts;
    std::size_t rewrapped = 0;
    for (const CaptionCue& c : cues) {
        std::vector<std::string> lines = textLines(c.text);
        bool fits = lines.size() <= 3;
        for (const std::string& l : lines) {
            fits = fits && captionDisplayLength(l) <= 40;
        }
        if (!fits) {
            const auto wrapped =
                breakCaptionLines(flattenCaptionText(c.text, options.language), options.language, 40, 3, simpleLineBreaker());
            lines = wrapped ? *wrapped : std::vector<std::string>{flattenCaptionText(c.text, options.language).substr(0, 40)};
            ++rewrapped;
        }
        texts.push_back(std::move(lines));
    }
    if (rewrapped > 0) {
        notes.add(ReportOutcome::Approximated, "Subtitles rewrapped to EBU STL's 40 characters and 3 rows");
    }
    std::size_t replaced = 0;
    std::vector<std::string> blocks;
    std::uint16_t number = 0;
    auto tc = [&](Ticks t, std::string& out) {
        const std::int64_t f = ticksToFrames(t + options.startTimecode, rate, Rounding::Nearest).value_or(0);
        const std::int64_t base = timecodeBase(rate);
        const std::int64_t frames = f % base;
        const std::int64_t secs = f / base;
        out.push_back(static_cast<char>((secs / 3600) % 24));
        out.push_back(static_cast<char>((secs / 60) % 60));
        out.push_back(static_cast<char>(secs % 60));
        out.push_back(static_cast<char>(frames));
    };
    for (std::size_t i = 0; i < cues.size(); ++i) {
        std::string field;
        for (std::size_t l = 0; l < texts[i].size(); ++l) {
            if (l > 0) {
                field.push_back(static_cast<char>(0x8A));
                field.push_back(static_cast<char>(0x8A)); // double-height rows: a blank row between
            }
            field += toIso6937(texts[i][l], replaced);
        }
        // Split over extension blocks of 112 bytes.
        std::vector<std::string> parts;
        for (std::size_t at = 0; at < field.size() || parts.empty(); at += 112) {
            parts.push_back(field.substr(at, 112));
        }
        ++number;
        for (std::size_t p = 0; p < parts.size(); ++p) {
            std::string b;
            b.push_back(0); // subtitle group
            b.push_back(static_cast<char>(number & 0xFF));
            b.push_back(static_cast<char>(number >> 8));
            b.push_back(static_cast<char>(p + 1 == parts.size() ? 0xFF : p)); // extension block number
            b.push_back(0);                                                     // cumulative status
            tc(cues[i].start, b);
            tc(cues[i].end(), b);
            const int rows = static_cast<int>(texts[i].size());
            b.push_back(static_cast<char>(options.placement == CaptionPlacement::Top ? 1 : 23 - 2 * (rows - 1) - 1)); // vertical
            b.push_back(static_cast<char>(options.alignment == CaptionAlign::Left    ? 1
                                          : options.alignment == CaptionAlign::Right ? 3
                                                                                     : 2)); // justification
            b.push_back(0);                                                                 // not a comment
            std::string text = parts[p];
            text.resize(112, static_cast<char>(0x8F));
            b += text;
            blocks.push_back(std::move(b));
        }
    }
    if (replaced > 0) {
        notes.add(ReportOutcome::Approximated,
                  std::to_string(replaced) + " characters EBU STL's Latin table lacks were written as '?'");
    }
    std::string lc = "09";
    for (const auto& [iso, ebu] : kLanguages) {
        if (iso == toLowerAscii(options.language)) {
            lc = std::string(ebu);
        }
    }
    std::string date = "000000";
    if (options.creationDate.size() >= 10) { // "YYYY-MM-DD"
        date = options.creationDate.substr(2, 2) + options.creationDate.substr(5, 2) + options.creationDate.substr(8, 2);
    }
    auto tcp = [&](Ticks t) {
        const std::int64_t f = ticksToFrames(t + options.startTimecode, rate, Rounding::Nearest).value_or(0);
        const std::int64_t base = timecodeBase(rate);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02lld%02lld%02lld%02lld", static_cast<long long>(f / base / 3600 % 24),
                      static_cast<long long>(f / base / 60 % 60), static_cast<long long>(f / base % 60),
                      static_cast<long long>(f % base));
        return std::string(buf);
    };
    char counts[16];
    std::string gsi;
    gsi += "850";                                    // code page
    gsi += is25 ? "STL25.01" : "STL30.01";           // disk format
    gsi += "1";                                      // display standard: teletext level 1
    gsi += "00";                                     // character code table: Latin
    gsi += lc;                                       // language
    gsi += fixed(options.title, 32);                 // original programme title
    gsi += fixed("", 32) + fixed("", 32) + fixed("", 32) + fixed("", 32) + fixed("", 32); // episode, translations, translator
    gsi += fixed("", 16);                            // subtitle list reference
    gsi += date + date + "00";                       // creation date, revision date, revision number
    std::snprintf(counts, sizeof counts, "%05zu%05zu%03d", blocks.size(), cues.size(), 1);
    gsi += counts;                                   // TTI blocks, subtitles, groups
    gsi += "40";                                     // characters a row
    gsi += "23";                                     // rows
    gsi += "1";                                      // timecode status
    gsi += tcp(Ticks{});                             // start of programme
    gsi += cues.empty() ? tcp(Ticks{}) : tcp(cues.front().start); // first in-cue
    gsi += "11";                                     // disks: 1 of 1
    gsi += fixed("", 3);                             // country of origin
    gsi += fixed(options.creationProgram, 32);       // publisher
    gsi += fixed("", 32) + fixed("", 32);            // editor
    gsi += fixed("", 75);                            // spare
    gsi += fixed("", 576);                           // user-defined area
    std::string out = std::move(gsi);
    for (const std::string& b : blocks) {
        out += b;
    }
    return out;
}

} // namespace wabcut::interchange::detail::captions
