#include <wabcut/model/ImageSequence.h>

#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/RawSourceSettings.h>
#include <wabcut/model/SpannedClip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <system_error>

namespace wabcut {

namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool isSeparator(char c) { return c == '/' || c == '\\'; }

/// '%' doubled, as FFmpeg's frame file names want literal percent signs.
std::string escapePercent(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += c;
        if (c == '%') {
            out += '%';
        }
    }
    return out;
}

/// A non-negative decimal integer filling all of `text`, at most `max`.
std::optional<std::int64_t> parseNumber(std::string_view text, std::int64_t max) {
    if (text.empty() || text.size() > 12) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < 0 || value > max) {
        return std::nullopt;
    }
    return value;
}

bool validRate(Rate rate) {
    return rate.isValid() && rate.num <= 1'000'000 && rate.den <= 1'000'000 &&
           static_cast<std::int64_t>(rate.num) <= static_cast<std::int64_t>(kMaxImageSequenceRate) * rate.den;
}

std::string extensionOf(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string{} : std::string(name.substr(dot));
}

} // namespace

std::filesystem::path ImageSequenceSpec::framePath(std::int64_t number) const {
    std::string digitsText = std::to_string(number);
    if (static_cast<int>(digitsText.size()) < digits) {
        digitsText.insert(0, static_cast<std::size_t>(digits) - digitsText.size(), '0');
    }
    return directory / pathFromUtf8(prefix + digitsText + suffix);
}

bool isImageSequenceExtension(std::string_view extension) {
    // 0.2: ".dng" - CinemaDNG, a camera RAW recording as numbered DNG frames (media/RawDevelop.h).
    static constexpr std::array<std::string_view, 10> kExtensions{".png", ".jpg", ".jpeg", ".tif", ".tiff",
                                                                  ".dpx", ".tga", ".exr",  ".bmp", ".dng"};
    const std::string lower = lowered(extension);
    return std::ranges::find(kExtensions, std::string_view(lower)) != kExtensions.end();
}

std::expected<ImageSequenceSpec, Error> findImageSequence(const std::filesystem::path& frame, Rate frameRate) {
    if (!validRate(frameRate)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the frame rate must be above 0 and at most 1000 fps"});
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(frame, ec).lexically_normal();
    if (ec || !std::filesystem::is_regular_file(absolute, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "File not found: " + pathToUtf8(frame)});
    }
    const std::string name = pathToUtf8(absolute.filename());
    const std::string extension = extensionOf(name);
    if (!isImageSequenceExtension(extension)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     name + " is not a still image an image sequence can be made of (PNG, JPEG, "
                                            "TIFF, DPX, Targa, OpenEXR or BMP)"});
    }
    const std::string_view stem = std::string_view(name).substr(0, name.size() - extension.size());
    std::size_t end = stem.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(stem[end - 1])) == 0) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(stem[start - 1])) != 0) {
        --start;
    }
    if (start == end) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, name + " has no frame number"});
    }
    if (end - start > static_cast<std::size_t>(kMaxImageSequenceDigits)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, name + ": the frame number is too long"});
    }
    ImageSequenceSpec spec;
    spec.directory = absolute.parent_path();
    spec.prefix = std::string(stem.substr(0, start));
    spec.suffix = std::string(stem.substr(end)) + extension;
    spec.digits = static_cast<int>(end - start);
    spec.first = parseNumber(stem.substr(start, end - start), kMaxImageSequenceNumber).value_or(0);
    spec.frameRate = frameRate;
    if (imageSequencePattern(spec).size() > kMaxImageSequencePathBytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the image sequence's path is too long"});
    }
    // The run of numbered files from the chosen one up to the first gap (Premiere: the selected file onwards).
    while (spec.count < kMaxImageSequenceFrames && spec.first + spec.count <= kMaxImageSequenceNumber &&
           std::filesystem::is_regular_file(spec.framePath(spec.first + spec.count), ec)) {
        ++spec.count;
    }
    return spec;
}

std::string imageSequencePattern(const ImageSequenceSpec& spec) {
    std::string directory = pathToUtf8(spec.directory);
    if (!directory.empty() && !isSeparator(directory.back())) {
        directory += static_cast<char>(std::filesystem::path::preferred_separator);
    }
    return escapePercent(directory) + escapePercent(spec.prefix) + "%0" + std::to_string(spec.digits) + "d" +
           escapePercent(spec.suffix);
}

std::string imageSequencePath(const ImageSequenceSpec& spec) {
    return imageSequencePattern(spec) + "?start=" + std::to_string(spec.first) + "&rate=" +
           std::to_string(spec.frameRate.num) + ":" + std::to_string(spec.frameRate.den);
}

std::optional<ImageSequenceSpec> parseImageSequencePath(std::string_view utf8) {
    utf8 = withoutRawSourceSettings(utf8); // 0.2 RAW Source Settings ride after the path (a CinemaDNG sequence)
    const std::size_t question = utf8.rfind('?');
    if (question == std::string_view::npos || question > kMaxImageSequencePathBytes) {
        return std::nullopt;
    }
    // The query: exactly "start=<n>&rate=<num>:<den>".
    const std::string_view query = utf8.substr(question + 1);
    constexpr std::string_view kStart = "start=";
    constexpr std::string_view kRate = "&rate=";
    const std::size_t rateAt = query.find(kRate);
    if (!query.starts_with(kStart) || rateAt == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view rateText = query.substr(rateAt + kRate.size());
    const std::size_t colon = rateText.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto first = parseNumber(query.substr(kStart.size(), rateAt - kStart.size()), kMaxImageSequenceNumber);
    const auto num = parseNumber(rateText.substr(0, colon), 1'000'000);
    const auto den = parseNumber(rateText.substr(colon + 1), 1'000'000);
    if (!first || !num || !den) {
        return std::nullopt;
    }
    ImageSequenceSpec spec;
    spec.first = *first;
    spec.frameRate = Rate{static_cast<std::int32_t>(*num), static_cast<std::int32_t>(*den)};
    if (!validRate(spec.frameRate)) {
        return std::nullopt;
    }
    // The pattern: literal text ('%%' is one '%') around exactly one "%0<digits>d".
    const std::string_view pattern = utf8.substr(0, question);
    std::string before;
    std::string after;
    bool found = false;
    for (std::size_t i = 0; i < pattern.size();) {
        const char c = pattern[i];
        if (c != '%') {
            (found ? after : before) += c;
            ++i;
            continue;
        }
        if (i + 1 < pattern.size() && pattern[i + 1] == '%') {
            (found ? after : before) += '%';
            i += 2;
            continue;
        }
        // "%0<1..9>d"
        if (found || i + 3 >= pattern.size() || pattern[i + 1] != '0' ||
            std::isdigit(static_cast<unsigned char>(pattern[i + 2])) == 0 || pattern[i + 2] == '0' ||
            pattern[i + 3] != 'd') {
            return std::nullopt;
        }
        spec.digits = pattern[i + 2] - '0';
        found = true;
        i += 4;
    }
    if (!found || std::ranges::any_of(after, isSeparator) || !isImageSequenceExtension(extensionOf(after))) {
        return std::nullopt;
    }
    const auto separator = std::ranges::find_if(before.rbegin(), before.rend(), isSeparator);
    const std::size_t nameStart = static_cast<std::size_t>(before.rend() - separator);
    spec.directory = pathFromUtf8(before.substr(0, nameStart)).parent_path(); // "C:\shots\" -> "C:\shots"
    spec.prefix = before.substr(nameStart);
    spec.suffix = std::move(after);
    return spec;
}

std::filesystem::path mediaFileToCheck(const std::filesystem::path& path) {
    const std::string utf8 = pathToUtf8(path);
    if (utf8.find('?') == std::string::npos) {
        return path; // a plain file (no file name holds '?')
    }
    if (const auto sequence = parseImageSequencePath(utf8)) {
        return sequence->firstFramePath();
    }
    if (const auto spanned = parseSpannedClipPath(utf8)) {
        return spanned->firstPath();
    }
    if (const auto layered = parseLayeredFilePath(utf8)) {
        return layered->file;
    }
    if (const std::string_view bare = withoutRawSourceSettings(utf8); bare.size() != utf8.size()) {
        return pathFromUtf8(std::string(bare)); // a RAW file's Source Settings
    }
    return path;
}

std::string relinkedMediaPath(std::string_view recorded, const std::filesystem::path& file) {
    if (const std::string_view bare = withoutRawSourceSettings(recorded); bare.size() != recorded.size()) {
        // The RAW Source Settings stay with the item wherever its file is found.
        return withRawSourceSettings(relinkedMediaPath(bare, file), rawSourceSettingsOf(recorded));
    }
    if (recorded.find('?') != std::string_view::npos) {
        if (auto sequence = parseImageSequencePath(recorded)) {
            // The same numbering and rate, from the file found (its own number is the new first frame).
            const std::string name = pathToUtf8(file.filename());
            const std::size_t end = name.find_last_of("0123456789");
            if (end != std::string::npos) {
                std::size_t begin = end;
                while (begin > 0 && std::isdigit(static_cast<unsigned char>(name[begin - 1])) != 0) {
                    --begin;
                }
                const auto number = parseNumber(std::string_view(name).substr(begin, end + 1 - begin), kMaxImageSequenceNumber);
                if (number) {
                    sequence->directory = file.parent_path();
                    sequence->prefix = name.substr(0, begin);
                    sequence->suffix = name.substr(end + 1);
                    sequence->first = *number;
                    return imageSequencePath(*sequence);
                }
            }
        } else if (auto spanned = parseSpannedClipPath(recorded)) {
            spanned->directory = file.parent_path(); // the other parts are looked for next to it
            spanned->parts.front().name = pathToUtf8(file.filename());
            if (std::string path = spannedClipPath(*spanned); !path.empty()) {
                return path;
            }
        } else if (auto layered = parseLayeredFilePath(recorded)) {
            layered->file = file;
            if (std::string path = layeredFilePath(*layered); !path.empty()) {
                return path;
            }
        }
    }
    return pathToUtf8(file);
}

bool mediaFileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(mediaFileToCheck(path), ec);
}

} // namespace wabcut
