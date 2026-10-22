#include "MetadataWriter.h"

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <system_error>

namespace wabcut::exporter::detail {

namespace {

std::int64_t toMs(Ticks t) {
    return static_cast<std::int64_t>(static_cast<long double>(t.value) * 1000.0L / kTicksPerSecond + 0.5L);
}

std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
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
            // Control characters other than tab / line ends are not allowed in XML 1.0.
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                out += ' ';
            } else {
                out += c;
            }
        }
    }
    return out;
}

/// Adobe's xmpDM time format of a rate ("30Timecode", "2997DropTimecode", ...).
std::string timeFormatOf(Rate rate, bool drop) {
    const double fps = rate.toDouble();
    const bool ntsc = rate.den == 1001;
    if (ntsc && std::abs(fps - 29.97) < 0.01) {
        return drop ? "2997DropTimecode" : "2997NonDropTimecode";
    }
    if (ntsc && std::abs(fps - 59.94) < 0.01) {
        return drop ? "5994DropTimecode" : "5994NonDropTimecode";
    }
    if (ntsc && std::abs(fps - 23.976) < 0.01) {
        return "23976Timecode";
    }
    return std::format("{}Timecode", std::lround(fps));
}

/// xmpDM's frame rate of a rate ("f30", "f30000s1001").
std::string frameRateOf(Rate rate) {
    return rate.den == 1 ? std::format("f{}", rate.num) : std::format("f{}s{}", rate.num, rate.den);
}

} // namespace

std::vector<ChapterMark> chapterMarks(const Sequence& sequence, TimeRange range) {
    std::vector<const Marker*> chapters;
    for (const Marker& m : sequence.markers) {
        if (m.type == MarkerType::Chapter && m.start >= range.start && m.start < range.end()) {
            chapters.push_back(&m);
        }
    }
    std::sort(chapters.begin(), chapters.end(), [](const Marker* a, const Marker* b) { return a->start < b->start; });
    std::vector<ChapterMark> out;
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        const Marker& m = *chapters[i];
        Ticks end = i + 1 < chapters.size() ? chapters[i + 1]->start : range.end();
        if (m.duration.value > 0) {
            end = std::min(end, m.end());
        }
        ChapterMark c;
        c.startMs = toMs(m.start - range.start);
        c.endMs = toMs(std::min(end, range.end()) - range.start);
        c.title = !m.name.empty() ? m.name : !m.comment.empty() ? m.comment : std::format("Chapter {}", i + 1);
        if (c.title.size() > 1024) {
            c.title.resize(1024);
        }
        if (c.endMs > c.startMs) {
            out.push_back(std::move(c));
        }
    }
    return out;
}

Ticks exportStartTimecode(const ExportSettings& settings, const Sequence& sequence, TimeRange range) {
    if (settings.metadata.startTimecode) {
        return *settings.metadata.startTimecode;
    }
    const Ticks t = sequence.startTimecode + range.start;
    return std::min(t, kMaxStartTimecode);
}

std::string timecodeText(Ticks t, Rate rate, bool dropFrame) {
    const std::int64_t frame = ticksToFrames(t, rate, Rounding::Nearest).value_or(0);
    return framesToTimecode(frame, rate, dropFrame && supportsDropFrame(rate));
}

std::string xmpPacket(const ExportSettings& settings, const Sequence& sequence, TimeRange range, Rate rate,
                      std::string_view creatorTool) {
    const ExportMetadata& m = settings.metadata;
    const bool drop = sequence.dropFrame && supportsDropFrame(rate);
    const std::string title = m.title.empty() ? sequence.name : m.title;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::string x;
    x += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
    x += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"" + xmlEscape(creatorTool) + "\">\n";
    x += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
    x += "  <rdf:Description rdf:about=\"\"\n";
    x += "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
    x += "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n";
    x += "    xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\"\n";
    x += "    xmp:CreatorTool=\"" + xmlEscape(creatorTool) + "\"\n";
    x += std::format("    xmp:CreateDate=\"{:%Y-%m-%dT%H:%M:%SZ}\"\n", now);
    x += std::format("    xmpDM:videoFrameRate=\"{:.6f}\">\n", rate.toDouble());
    x += "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">" + xmlEscape(title) + "</rdf:li></rdf:Alt></dc:title>\n";
    if (!m.description.empty()) {
        x += "   <dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">" + xmlEscape(m.description) +
             "</rdf:li></rdf:Alt></dc:description>\n";
    }
    x += "   <xmpDM:startTimeCode rdf:parseType=\"Resource\">\n";
    x += "    <xmpDM:timeFormat>" + timeFormatOf(rate, drop) + "</xmpDM:timeFormat>\n";
    x += "    <xmpDM:timeValue>" + timecodeText(exportStartTimecode(settings, sequence, range), rate, drop) +
         "</xmpDM:timeValue>\n";
    x += "   </xmpDM:startTimeCode>\n";
    if (m.xmpMarkers) {
        std::string markers;
        for (const Marker& marker : sequence.markers) {
            if (marker.start < range.start || marker.start >= range.end()) {
                continue;
            }
            const std::int64_t start = ticksToFrames(marker.start - range.start, rate, Rounding::Nearest).value_or(0);
            const std::int64_t duration = ticksToFrames(marker.duration, rate, Rounding::Nearest).value_or(0);
            markers += "        <rdf:li rdf:parseType=\"Resource\">\n";
            markers += std::format("         <xmpDM:startTime>{}</xmpDM:startTime>\n", start);
            if (duration > 0) {
                markers += std::format("         <xmpDM:duration>{}</xmpDM:duration>\n", duration);
            }
            if (!marker.name.empty()) {
                markers += "         <xmpDM:name>" + xmlEscape(marker.name) + "</xmpDM:name>\n";
            }
            if (!marker.comment.empty()) {
                markers += "         <xmpDM:comment>" + xmlEscape(marker.comment) + "</xmpDM:comment>\n";
            }
            switch (marker.type) {
            case MarkerType::Chapter:
                markers += "         <xmpDM:type>Chapter</xmpDM:type>\n";
                break;
            case MarkerType::Segmentation:
                markers += "         <xmpDM:type>Segmentation</xmpDM:type>\n";
                break;
            case MarkerType::WebLink:
                markers += "         <xmpDM:type>WebLink</xmpDM:type>\n";
                markers += "         <xmpDM:location>" + xmlEscape(marker.url) + "</xmpDM:location>\n";
                if (!marker.target.empty()) {
                    markers += "         <xmpDM:target>" + xmlEscape(marker.target) + "</xmpDM:target>\n";
                }
                break;
            case MarkerType::Comment:
                break;
            }
            markers += "        </rdf:li>\n";
        }
        if (!markers.empty()) {
            x += "   <xmpDM:Tracks>\n    <rdf:Bag>\n     <rdf:li rdf:parseType=\"Resource\">\n";
            x += "      <xmpDM:trackName>Markers</xmpDM:trackName>\n";
            x += "      <xmpDM:frameRate>" + frameRateOf(rate) + "</xmpDM:frameRate>\n";
            x += "      <xmpDM:markers>\n       <rdf:Seq>\n" + markers + "       </rdf:Seq>\n      </xmpDM:markers>\n";
            x += "     </rdf:li>\n    </rdf:Bag>\n   </xmpDM:Tracks>\n";
        }
    }
    x += "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>";
    return x;
}

std::expected<void, Error> appendXmpBox(const std::filesystem::path& file, const std::string& xmp) {
    static constexpr std::array<std::uint8_t, 16> kXmpUuid = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                                                             0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};
    const std::uint64_t size = 8 + kXmpUuid.size() + xmp.size();
    if (size > 0xFFFFFFFFull) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The XMP is too large"});
    }
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot add the XMP to " + pathToUtf8(file)});
    }
    const std::array<char, 8> header = {static_cast<char>((size >> 24) & 0xFF), static_cast<char>((size >> 16) & 0xFF),
                                        static_cast<char>((size >> 8) & 0xFF), static_cast<char>(size & 0xFF),
                                        'u', 'u', 'i', 'd'};
    out.write(header.data(), header.size());
    out.write(reinterpret_cast<const char*>(kXmpUuid.data()), static_cast<std::streamsize>(kXmpUuid.size()));
    out.write(xmp.data(), static_cast<std::streamsize>(xmp.size()));
    if (!out) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot add the XMP to " + pathToUtf8(file)});
    }
    return {};
}

std::filesystem::path xmpSidecarPath(const std::filesystem::path& output) {
    std::filesystem::path p = output;
    p.replace_extension(".xmp");
    return p;
}

std::expected<void, Error> writeTextFile(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::path temp = file;
    temp += ".wabcut-partial";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + pathToUtf8(file)});
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + pathToUtf8(file)});
    }
    return {};
}

} // namespace wabcut::exporter::detail
