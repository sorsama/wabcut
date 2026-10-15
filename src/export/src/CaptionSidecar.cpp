#include <wabcut/export/CaptionSidecar.h>

#include <wabcut/playback/video/CaptionRedaction.h>
#include <wabcut/playback/video/Evaluate.h>

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <string_view>

namespace wabcut::exporter {

static_assert(kRedactedWordToken == playback::kRedactedWordToken);

namespace {

std::string timestamp(Ticks t, char separator) {
    const std::int64_t ms = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(static_cast<long double>(t.value) * 1000.0L / kTicksPerSecond + 0.5L));
    return std::format("{:02}:{:02}:{:02}{}{:03}", ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, separator, ms % 1000);
}

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
            out += kZeroWidthSpace; // "--\u200B>": not a timing arrow
            ++i;
            continue;
        }
        out += c;
        if ((c == '<' && (std::isalpha(static_cast<unsigned char>(after)) || after == '/')) || (c == '{' && after == '\\')) {
            out += kZeroWidthSpace; // "<\u200Bi>", "{\u200B\\an8}": no tag, no override
        }
    }
    return out;
}

/// The cue's payload: its lines (CR LF, CR and LF all end a line), blank ones dropped, each escaped.
std::string payloadOf(std::string_view text, bool vtt) {
    std::string out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(start, end - start);
        start = end + (end + 1 < text.size() && text[end] == '\r' && text[end + 1] == '\n' ? 2 : 1);
        if (line.find_first_not_of(" \t") == std::string_view::npos) {
            continue;
        }
        if (!out.empty()) {
            out += '\n';
        }
        out += escapeLine(line, vtt);
    }
    return out;
}

} // namespace

const CaptionTrack* captionTrackToExport(const Sequence& sequence, const CaptionExportSettings& captions) {
    if (captions.track) {
        for (const CaptionTrack& t : sequence.captionTracks) {
            if (t.id.value == *captions.track && t.enabled) {
                return &t;
            }
        }
    }
    for (const CaptionTrack& t : sequence.captionTracks) {
        if (t.enabled) {
            return &t;
        }
    }
    return nullptr;
}

std::string captionSidecarText(const CaptionTrack& track, TimeRange range, CaptionExport format) {
    const bool vtt = format == CaptionExport::Vtt;
    std::string out = vtt ? "WEBVTT\n\n" : "";
    int index = 0;
    for (const CaptionCue& cue : track.cues) {
        const Ticks start = std::max(cue.start, range.start);
        const Ticks end = std::min(cue.end(), range.end());
        const std::string payload = payloadOf(cue.text, vtt);
        if (!(start < end) || payload.empty()) {
            continue;
        }
        ++index;
        if (!vtt) {
            out += std::to_string(index) + "\n";
        }
        const char separator = vtt ? '.' : ',';
        out += timestamp(start - range.start, separator) + " --> " + timestamp(end - range.start, separator) + "\n";
        out += payload + "\n\n";
    }
    return out;
}

std::expected<std::filesystem::path, Error> writeCaptionSidecar(const ProjectState& project, const Sequence& sequence,
                                                                const CaptionExportSettings& captions, TimeRange range,
                                                                const std::filesystem::path& output) {
    if (captions.mode != CaptionExport::Srt && captions.mode != CaptionExport::Vtt) {
        return std::filesystem::path{};
    }
    const CaptionTrack* track = captionTrackToExport(sequence, captions);
    if (!track) {
        return std::filesystem::path{};
    }
    std::filesystem::path file = output;
    file.replace_extension(captions.mode == CaptionExport::Vtt ? ".vtt" : ".srt");
    // Words the soundtrack bleeps or mutes are not written either (Codex wave-4 P1-3).
    const std::vector<TimeRange> redacted = playback::redactedSpeechRanges(project, sequence);
    const std::string text = captionSidecarText(
        playback::redactCaptionTrack(*track, redacted, captions.words.get(), captions.redactedWord), range, captions.mode);
    std::filesystem::path temp = file;
    temp += ".wabcut-partial";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write the captions to " + pathToUtf8(file)});
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write the captions to " + pathToUtf8(file)});
    }
    return file;
}

} // namespace wabcut::exporter
