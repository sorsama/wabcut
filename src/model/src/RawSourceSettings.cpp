#include <wabcut/model/RawSourceSettings.h>

#include "SequenceEditor.h"

#include <wabcut/model/ChangeSet.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <memory>
#include <utility>

namespace wabcut {

namespace {

constexpr std::string_view kKey = "raw=";
constexpr std::string_view kExposure = "exposure:";
constexpr std::string_view kWhite = ",white:";

/// A whole number of at most `maxDigits` digits, optionally signed, and nothing else.
std::optional<int> parseInt(std::string_view text, int maxDigits) {
    const std::size_t digits = text.size() - (!text.empty() && (text[0] == '-' || text[0] == '+') ? 1 : 0);
    if (digits == 0 || digits > static_cast<std::size_t>(maxDigits)) {
        return std::nullopt;
    }
    if (text[0] == '+') {
        text.remove_prefix(1);
    }
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/// Stops in hundredths: "[+-]<digit>.<two digits>".
std::optional<double> parseStops(std::string_view text) {
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos || text.size() - dot != 3) {
        return std::nullopt;
    }
    const auto whole = parseInt(text.substr(0, dot), 1);
    const auto hundredths = parseInt(text.substr(dot + 1), 2);
    if (!whole || !hundredths || *hundredths < 0 || text[dot + 1] == '-' || text[dot + 1] == '+') {
        return std::nullopt;
    }
    const bool negative = text[0] == '-';
    const double value = (std::abs(*whole) * 100 + *hundredths) / 100.0;
    return negative ? -value : value;
}

/// The settings of a suffix's text (after "raw="), or nullopt when it is not one.
std::optional<RawSourceSettings> parseSettings(std::string_view text) {
    if (!text.starts_with(kExposure)) {
        return std::nullopt;
    }
    text.remove_prefix(kExposure.size());
    RawSourceSettings settings;
    const std::size_t white = text.find(kWhite);
    const auto stops = parseStops(text.substr(0, white));
    if (!stops) {
        return std::nullopt;
    }
    settings.exposure = *stops;
    if (white != std::string_view::npos) {
        const std::string_view rest = text.substr(white + kWhite.size());
        const std::size_t colon = rest.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const auto kelvin = parseInt(rest.substr(0, colon), 5);
        const auto tint = parseInt(rest.substr(colon + 1), 4);
        if (!kelvin || !tint) {
            return std::nullopt;
        }
        settings.white = RawWhiteBalance{*kelvin, *tint};
    }
    if (!validRawSourceSettings(settings) || settings.isDefault()) {
        return std::nullopt;
    }
    return settings;
}

/// Where the suffix (its '?' or '&') starts, and its settings; nullopt when the path carries none.
std::optional<std::pair<std::size_t, RawSourceSettings>> suffixOf(std::string_view utf8) {
    const std::size_t at = utf8.rfind(kKey);
    if (at == std::string_view::npos || at == 0 || (utf8[at - 1] != '?' && utf8[at - 1] != '&')) {
        return std::nullopt;
    }
    // "&raw=" only after a path's own query.
    if (utf8[at - 1] == '&' && utf8.substr(0, at - 1).find('?') == std::string_view::npos) {
        return std::nullopt;
    }
    auto settings = parseSettings(utf8.substr(at + kKey.size()));
    if (!settings) {
        return std::nullopt;
    }
    return std::pair{at - 1, *settings};
}

} // namespace

bool validRawSourceSettings(const RawSourceSettings& settings) {
    if (!std::isfinite(settings.exposure) || std::abs(settings.exposure) > kMaxRawExposureStops + 1e-9) {
        return false;
    }
    return !settings.white ||
           (settings.white->temperature >= kMinRawTemperature && settings.white->temperature <= kMaxRawTemperature &&
            std::abs(settings.white->tint) <= kMaxRawTint);
}

RawSourceSettings rawSourceSettingsOf(std::string_view utf8) {
    const auto suffix = suffixOf(utf8);
    return suffix ? suffix->second : RawSourceSettings{};
}

std::string_view withoutRawSourceSettings(std::string_view utf8) {
    const auto suffix = suffixOf(utf8);
    return suffix ? utf8.substr(0, suffix->first) : utf8;
}

std::string withRawSourceSettings(std::string_view utf8, const RawSourceSettings& settings) {
    if (!validRawSourceSettings(settings)) {
        return {};
    }
    std::string out(withoutRawSourceSettings(utf8));
    const long hundredths = std::lround(settings.exposure * 100.0);
    if (hundredths == 0 && !settings.white) {
        return out;
    }
    out += out.find('?') == std::string::npos ? '?' : '&';
    out += std::format("{}{}{}{}.{:02}", kKey, kExposure, hundredths < 0 ? "-" : "+", std::abs(hundredths) / 100,
                       std::abs(hundredths) % 100);
    if (settings.white) {
        out += std::format("{}{}:{}", kWhite, settings.white->temperature, settings.white->tint);
    }
    return out;
}

bool isRawMedia(const MediaInfo& info) {
    return info.hasVideo() && info.video.front().pixelFormat.starts_with("bayer");
}

namespace edit {

EditOutcome setRawSourceSettings(const ProjectState& state, std::span<const MediaId> media,
                                 const RawSourceSettings& settings) {
    if (!validRawSourceSettings(settings)) {
        return std::unexpected(detail::invalidArgument(std::format(
            "RAW Source Settings must be within {} stops of exposure, {} to {} K and a tint of -{} to +{}",
            kMaxRawExposureStops, kMinRawTemperature, kMaxRawTemperature, kMaxRawTint, kMaxRawTint)));
    }
    if (media.empty()) {
        return std::unexpected(detail::invalidArgument("no media item to set RAW Source Settings on"));
    }
    ProjectState next = state;
    for (const MediaId id : media) {
        const auto it = std::lower_bound(next.media.begin(), next.media.end(), id,
                                         [](const MediaPtr& m, MediaId value) { return m->id < value; });
        if (it == next.media.end() || (*it)->id != id) {
            return std::unexpected(detail::notFound("media " + std::to_string(id.value) + " does not exist"));
        }
        const MediaItem& old = **it;
        if (old.isGenerator() || !isRawMedia(old.info)) {
            return std::unexpected(detail::invalidArgument(
                "'" + old.name + "' is not camera RAW: Source Settings develop ProRes RAW and CinemaDNG clips"));
        }
        auto item = std::make_shared<MediaItem>(old);
        item->locator.path = withRawSourceSettings(old.locator.path, settings);
        if (!old.locator.relativePath.empty()) {
            item->locator.relativePath = withRawSourceSettings(old.locator.relativePath, settings);
        }
        item->info.path = item->locator.path;
        if (item->locator.path != old.locator.path) {
            item->locator.proxy.reset(); // developed with the old settings
        }
        *it = std::move(item);
    }
    EditResult out;
    out.changes = diff(state, next);
    out.state = std::move(next);
    return out;
}

} // namespace edit

} // namespace wabcut
