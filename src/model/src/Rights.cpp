#include <wabcut/model/Rights.h>

#include <wabcut/model/Plural.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/TitleOps.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace wabcut {

namespace {

// Howard Hinnant's civil-date algorithms (days since 1970-01-01 <-> year, month, day), proleptic Gregorian.
constexpr std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr void civilFromDays(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

constexpr bool isLeap(std::int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

constexpr unsigned daysInMonth(std::int64_t y, unsigned m) {
    constexpr std::array<unsigned, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && isLeap(y) ? 29U : kDays[m - 1];
}

bool digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// Text a person typed into a field: UTF-8, no control characters (a new line is fine in `allowNewlines` fields).
bool plainText(std::string_view text, bool allowNewlines) {
    if (!edit::isValidUtf8(text)) {
        return false;
    }
    return std::none_of(text.begin(), text.end(), [&](char c) {
        const auto u = static_cast<unsigned char>(c);
        return (u < 0x20 && !(allowNewlines && (c == '\n' || c == '\t'))) || u == 0x7F;
    });
}

} // namespace

std::string_view toString(ClearanceStatus status) {
    switch (status) {
    case ClearanceStatus::Unset:
        return "";
    case ClearanceStatus::Cleared:
        return "cleared";
    case ClearanceStatus::Pending:
        return "pending";
    case ClearanceStatus::FairUse:
        return "fairUse";
    case ClearanceStatus::NotRequired:
        return "notRequired";
    }
    return "";
}

std::optional<ClearanceStatus> clearanceStatusFromName(std::string_view name) {
    for (const ClearanceStatus s : {ClearanceStatus::Unset, ClearanceStatus::Cleared, ClearanceStatus::Pending,
                                    ClearanceStatus::FairUse, ClearanceStatus::NotRequired}) {
        if (toString(s) == name) {
            return s;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> isoDateToDays(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-' || !digits(text.substr(0, 4)) ||
        !digits(text.substr(5, 2)) || !digits(text.substr(8, 2))) {
        return std::nullopt;
    }
    const auto number = [&](std::size_t at, std::size_t n) {
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            v = v * 10 + (text[at + i] - '0');
        }
        return v;
    };
    const int y = number(0, 4);
    const int m = number(5, 2);
    const int d = number(8, 2);
    if (y < 1900 || y > 2999 || m < 1 || m > 12 || d < 1 || d > static_cast<int>(daysInMonth(y, static_cast<unsigned>(m)))) {
        return std::nullopt;
    }
    return daysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

bool isIsoCalendarDate(std::string_view text) { return isoDateToDays(text).has_value(); }

std::string isoDateFromDays(std::int64_t days) {
    std::int64_t y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civilFromDays(days, y, m, d);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%04lld-%02u-%02u", static_cast<long long>(y), m, d);
    return buffer;
}

std::optional<std::string> rightsProblem(const MediaRights& r) {
    for (const std::string* field : {&r.source, &r.licence, &r.reference, &r.territory, &r.creditLine}) {
        if (field->size() > MediaRights::kMaxFieldBytes) {
            return "a rights field is longer than " + std::to_string(MediaRights::kMaxFieldBytes) + " bytes";
        }
        if (!plainText(*field, false)) {
            return "a rights field is not plain UTF-8 text";
        }
    }
    if (r.notes.size() > MediaRights::kMaxNotesBytes) {
        return "the rights notes are longer than " + std::to_string(MediaRights::kMaxNotesBytes) + " bytes";
    }
    if (!plainText(r.notes, true)) {
        return "the rights notes are not plain UTF-8 text";
    }
    if (!r.expiry.empty() && !isIsoCalendarDate(r.expiry)) {
        return "the expiry must be a date written YYYY-MM-DD";
    }
    if (!r.currency.empty() &&
        (r.currency.size() != 3 || !std::all_of(r.currency.begin(), r.currency.end(), [](char c) { return c >= 'A' && c <= 'Z'; }))) {
        return "the currency must be three capital letters (USD, EUR, GBP)";
    }
    if (r.costMinor && (*r.costMinor < 0 || *r.costMinor > MediaRights::kMaxCostMinor)) {
        return "the cost must be between 0 and " + formatCostMinor(MediaRights::kMaxCostMinor);
    }
    return std::nullopt;
}

std::optional<std::int64_t> parseCostMinor(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (!(c >= '0' && c <= '9') && c != '.' && c != ',' && c != ' ' && c != '\'') {
            return std::nullopt;
        }
    }
    // The decimal point is the last '.' or ',' with one or two digits after it; any other separator groups thousands.
    std::size_t point = std::string_view::npos;
    for (std::size_t i = text.size(); i-- > 0;) {
        if (text[i] == '.' || text[i] == ',') {
            point = i;
            break;
        }
    }
    std::string_view whole = text;
    std::string_view fraction;
    if (point != std::string_view::npos) {
        const std::string_view after = text.substr(point + 1);
        if (digits(after) && after.size() <= 2) {
            whole = text.substr(0, point);
            fraction = after;
        } else if (!after.empty() && !(digits(after) && after.size() == 3)) {
            return std::nullopt; // "12.3456" or "1,2,x": not an amount
        } else if (after.empty()) {
            whole = text.substr(0, point); // "12."
        }
    }
    // The whole part: digits, grouped in threes by '.', ',', ' ' or an apostrophe ("1,250,000", "1.250.000", "1 250").
    std::int64_t major = 0;
    bool any = false;
    std::size_t group = 0;     // digits in the current group
    std::size_t groups = 0;    // groups seen so far (including the current one)
    std::size_t firstSize = 0; // digits in the first group
    const auto endGroup = [&] {
        if (groups > 1 && group != 3) {
            return false;
        }
        return true;
    };
    for (const char c : whole) {
        if (c >= '0' && c <= '9') {
            if (group == 0) {
                ++groups;
            }
            ++group;
            if (groups == 1) {
                firstSize = group;
            }
            any = true;
            major = major * 10 + (c - '0');
            if (major > MediaRights::kMaxCostMinor) {
                return std::nullopt;
            }
        } else {
            if (group == 0 || !endGroup()) {
                return std::nullopt; // a leading or doubled separator, or a short group
            }
            group = 0;
        }
    }
    if (!any && fraction.empty()) {
        return std::nullopt;
    }
    if (groups > 1 && (group != 3 || firstSize > 3)) {
        return std::nullopt;
    }
    std::int64_t minor = 0;
    if (!fraction.empty()) {
        minor = (fraction[0] - '0') * 10 + (fraction.size() > 1 ? fraction[1] - '0' : 0);
    }
    const std::int64_t total = major * 100 + minor;
    if (major > MediaRights::kMaxCostMinor / 100 || total > MediaRights::kMaxCostMinor) {
        return std::nullopt;
    }
    return total;
}

std::string formatCostMinor(std::int64_t minor) {
    const bool negative = minor < 0;
    const std::int64_t v = negative ? -minor : minor;
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%s%lld.%02lld", negative ? "-" : "", static_cast<long long>(v / 100),
                  static_cast<long long>(v % 100));
    return buffer;
}

RightsAssessment assessRights(const MediaRights& rights, std::string_view asOf, int soonDays) {
    RightsAssessment a;
    if (rights.isEmpty()) {
        return a;
    }
    a.pending = rights.status == ClearanceStatus::Pending;
    a.statusUnset = rights.status == ClearanceStatus::Unset;
    const auto expiry = isoDateToDays(rights.expiry);
    const auto today = isoDateToDays(asOf);
    if (expiry && today) {
        a.daysLeft = *expiry - *today;
        a.expired = a.daysLeft < 0;
        a.expiresSoon = !a.expired && soonDays > 0 && a.daysLeft <= soonDays;
    }
    return a;
}

const MediaRights* effectiveRights(const ProjectState& state, const MediaItem& item) {
    const MediaItem* at = &item;
    // A subclip without a record of its own stands for its master's; the chain is short, but a damaged project could
    // loop (a subclip whose master is itself or a cycle): stop after as many steps as there are items.
    for (std::size_t steps = 0; at && steps <= state.media.size(); ++steps) {
        if (at->rights && !at->rights->isEmpty()) {
            return &*at->rights;
        }
        at = at->subclip ? state.findMedia(at->subclip->master) : nullptr;
    }
    return nullptr;
}

std::string rightsFlagText(const MediaRights& rights, const RightsAssessment& a) {
    std::string out;
    const auto add = [&](const std::string& text) {
        if (!out.empty()) {
            out += "; ";
        }
        out += text;
    };
    if (a.pending) {
        add("Not cleared yet");
    }
    if (a.statusUnset) {
        add("Clearance status not set");
    }
    if (a.expired) {
        add("Licence expired on " + rights.expiry);
    } else if (a.expiresSoon) {
        add(a.daysLeft == 0 ? std::string("Licence expires today") : "Licence expires in " + counted(a.daysLeft, "day"));
    }
    return out;
}

} // namespace wabcut
