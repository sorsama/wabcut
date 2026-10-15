#include <wabcut/model/Timecode.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

namespace wabcut {

namespace {

constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kMinutesPerHour = 60;
constexpr std::int64_t kSecondsPerHour = kSecondsPerMinute * kMinutesPerHour;
constexpr std::size_t kMaxFieldDigits = 9;

int digitCount(std::int64_t value) {
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

/// Width of the frames field: two digits, more for rates above 100 fps.
int framesFieldWidth(std::int64_t base) { return base > 100 ? digitCount(base - 1) : 2; }

void appendPadded(std::string& out, std::int64_t value, int width) {
    std::array<char, 24> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    const auto length = static_cast<int>(result.ptr - buffer.data());
    for (int i = length; i < width; ++i) {
        out.push_back('0');
    }
    out.append(buffer.data(), result.ptr);
}

bool isSeparator(char c) { return c == ':' || c == ';' || c == '.' || c == ','; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

Error parseError(std::string_view text, std::string_view why) {
    std::string message = "invalid timecode \"";
    message += text;
    message += "\": ";
    message += why;
    return {ErrorCode::ParseError, std::move(message)};
}

} // namespace

std::int64_t timecodeBase(Rate rate) {
    if (!rate.isValid()) {
        return 0;
    }
    const std::int64_t base = (static_cast<std::int64_t>(rate.num) + rate.den / 2) / rate.den;
    return base > 0 ? base : 1;
}

std::int64_t dropFramesPerMinute(Rate rate) {
    // NTSC-family rates: 30000/1001 drops 2, 60000/1001 drops 4, 120000/1001 drops 8.
    if (rate.den == 1001 && rate.num > 0 && rate.num % 30000 == 0) {
        return (rate.num / 1000) / 15;
    }
    return 0;
}

std::string framesToTimecode(std::int64_t frame, Rate rate, bool dropFrame) {
    if (!rate.isValid()) {
        return "--:--:--:--";
    }
    const std::int64_t base = timecodeBase(rate);
    const std::int64_t drop = dropFrame ? dropFramesPerMinute(rate) : 0;
    const bool negative = frame < 0;
    std::int64_t label = negative ? -frame : frame;

    if (drop > 0) {
        // Convert a real frame count into the label count by re-inserting the skipped labels.
        const std::int64_t framesPerMinute = base * kSecondsPerMinute - drop;
        const std::int64_t framesPer10Minutes = base * kSecondsPerMinute * 10 - drop * 9;
        const std::int64_t tens = label / framesPer10Minutes;
        const std::int64_t rest = label % framesPer10Minutes;
        label += drop * 9 * tens;
        if (rest > drop) {
            label += drop * ((rest - drop) / framesPerMinute);
        }
    }

    const std::int64_t frames = label % base;
    const std::int64_t totalSeconds = label / base;
    const std::int64_t seconds = totalSeconds % kSecondsPerMinute;
    const std::int64_t minutes = (totalSeconds / kSecondsPerMinute) % kMinutesPerHour;
    const std::int64_t hours = totalSeconds / kSecondsPerHour;

    std::string out;
    out.reserve(16);
    if (negative) {
        out.push_back('-');
    }
    appendPadded(out, hours, 2);
    out.push_back(':');
    appendPadded(out, minutes, 2);
    out.push_back(':');
    appendPadded(out, seconds, 2);
    out.push_back(drop > 0 ? ';' : ':');
    appendPadded(out, frames, framesFieldWidth(base));
    return out;
}

std::string ticksToTimecode(Ticks t, Rate rate, bool dropFrame) {
    if (!rate.isValid()) {
        return framesToTimecode(0, rate, dropFrame);
    }
    return framesToTimecode(ticksToFrames(t, rate), rate, dropFrame);
}

std::expected<std::int64_t, Error> parseTimecodeFrames(std::string_view text, Rate rate, bool dropFrame) {
    if (!rate.isValid()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid frame rate"});
    }
    const std::int64_t base = timecodeBase(rate);
    const std::int64_t drop = dropFrame ? dropFramesPerMinute(rate) : 0;

    std::string_view s = text;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    bool negative = false;
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        negative = s.front() == '-';
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return std::unexpected(parseError(text, "empty"));
    }

    // Split into digit fields, most significant first.
    std::vector<std::string_view> fields;
    bool hasSeparator = false;
    std::size_t fieldStart = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || isSeparator(s[i])) {
            hasSeparator = hasSeparator || i < s.size();
            fields.push_back(s.substr(fieldStart, i - fieldStart));
            fieldStart = i + 1;
        } else if (!isDigit(s[i])) {
            return std::unexpected(parseError(text, "unexpected character"));
        }
    }
    if (!hasSeparator) {
        // Bare digits: packed from the right, frames first (Premiere's timecode entry).
        const std::string_view digits = fields.front();
        fields.clear();
        std::size_t end = digits.size();
        std::size_t width = static_cast<std::size_t>(framesFieldWidth(base));
        while (end > 0 && fields.size() < 3) {
            const std::size_t begin = end > width ? end - width : 0;
            fields.insert(fields.begin(), digits.substr(begin, end - begin));
            end = begin;
            width = 2;
        }
        if (end > 0) {
            fields.insert(fields.begin(), digits.substr(0, end));
        }
    }
    if (fields.size() > 4) {
        return std::unexpected(parseError(text, "too many fields"));
    }

    // Right-aligned: [hours, minutes, seconds, frames].
    std::array<std::int64_t, 4> values{};
    const std::size_t offset = 4 - fields.size();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::string_view field = fields[i];
        if (field.empty()) {
            return std::unexpected(parseError(text, "empty field"));
        }
        if (field.size() > kMaxFieldDigits) {
            return std::unexpected(parseError(text, "field too long"));
        }
        std::int64_t value = 0;
        const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
        if (result.ec != std::errc{} || result.ptr != field.data() + field.size()) {
            return std::unexpected(parseError(text, "not a number"));
        }
        values[offset + i] = value;
    }
    std::int64_t hours = values[0];
    std::int64_t minutes = values[1];
    std::int64_t seconds = values[2];
    std::int64_t frames = values[3];

    // Every field below the leading one must be in range; the leading one may carry over ("90:00" is
    // 90 seconds), which is normalized before the drop-frame check.
    if (offset < 3 && frames >= base) {
        return std::unexpected(parseError(text, "frames out of range"));
    }
    if (offset < 2 && seconds >= kSecondsPerMinute) {
        return std::unexpected(parseError(text, "seconds out of range"));
    }
    if (offset < 1 && minutes >= kMinutesPerHour) {
        return std::unexpected(parseError(text, "minutes out of range"));
    }
    if (offset == 3) {
        seconds += frames / base;
        frames %= base;
    }
    if (offset >= 2) {
        minutes += seconds / kSecondsPerMinute;
        seconds %= kSecondsPerMinute;
    }
    if (offset >= 1) {
        hours += minutes / kMinutesPerHour;
        minutes %= kMinutesPerHour;
    }

    const std::int64_t totalMinutes = hours * kMinutesPerHour + minutes;
    if (drop > 0 && seconds == 0 && frames < drop && totalMinutes % 10 != 0) {
        return std::unexpected(parseError(text, "this frame number does not exist in drop-frame timecode"));
    }

    std::int64_t frame = ((hours * kSecondsPerHour + minutes * kSecondsPerMinute + seconds) * base) + frames;
    if (drop > 0) {
        frame -= drop * (totalMinutes - totalMinutes / 10);
    }
    return negative ? -frame : frame;
}

std::expected<Ticks, Error> parseTimecode(std::string_view text, Rate rate, bool dropFrame) {
    auto frames = parseTimecodeFrames(text, rate, dropFrame);
    if (!frames) {
        return std::unexpected(std::move(frames.error()));
    }
    return framesToTicks(*frames, rate);
}

} // namespace wabcut
