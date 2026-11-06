#pragma once

// Rights and clearance of a media item (0.7): what a documentary maker keeps for every third-party clip, still and
// track - who it came from, the licence, where and until when it may be used, what it cost, how it must be credited
// and whether it is cleared. The record belongs to the media item (every clip of it, and every subclip of it, shares
// it), is saved with the project only when set, and feeds the Rights and Clearance Log, the Media Usage Report's
// flags and the end-credits roll.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut {

/// Where the clearance of an item stands. Unset: nobody has said (the item may be the editor's own material).
enum class ClearanceStatus : std::uint8_t {
    Unset,
    Cleared,     ///< the licence is paid for / granted and in the file
    Pending,     ///< asked for, not granted yet: must not go out
    FairUse,     ///< used without a licence on a fair use / fair dealing argument
    NotRequired, ///< the editor's own, public domain, or a licence that needs no clearance
};

/// "" (unset), "cleared", "pending", "fairUse", "notRequired": the names project files and commands use.
std::string_view toString(ClearanceStatus status);
/// The status named `name` (the names above), or nullopt.
std::optional<ClearanceStatus> clearanceStatusFromName(std::string_view name);

struct MediaRights {
    static constexpr std::size_t kMaxFieldBytes = 512;  ///< source, licence, reference, territory, credit line
    static constexpr std::size_t kMaxNotesBytes = 4096; ///< notes
    /// A cost is kept in hundredths of its currency: at most this much (so sums of 100,000 items cannot overflow).
    static constexpr std::int64_t kMaxCostMinor = 100'000'000'000'000LL;

    std::string source;    ///< who supplied or owns it ("British Pathe", "Getty Images", "A. Sharma")
    std::string licence;   ///< the terms ("Royalty free, all media, 10 years")
    std::string reference; ///< the licence / asset / invoice number
    std::string territory; ///< where it may be shown ("Worldwide", "UK and Ireland")
    std::string expiry;    ///< the last day it may be shown: "YYYY-MM-DD", empty = does not expire / not known
    std::optional<std::int64_t> costMinor; ///< what it cost, in hundredths of `currency`
    std::string currency;  ///< three upper-case letters ("USD", "EUR"), or empty
    std::string creditLine; ///< as it must appear in the credits ("Archive footage courtesy of British Pathe")
    ClearanceStatus status = ClearanceStatus::Unset;
    std::string notes;

    bool isEmpty() const {
        return source.empty() && licence.empty() && reference.empty() && territory.empty() && expiry.empty() &&
               !costMinor && currency.empty() && creditLine.empty() && status == ClearanceStatus::Unset && notes.empty();
    }
    bool operator==(const MediaRights&) const = default;
};

/// "YYYY-MM-DD" with a real calendar day (year 1900..2999).
bool isIsoCalendarDate(std::string_view text);
/// Days from 1970-01-01 to the ISO date `text`, or nullopt when it is not one.
std::optional<std::int64_t> isoDateToDays(std::string_view text);
/// The ISO date of a day count since 1970-01-01.
std::string isoDateFromDays(std::int64_t days);

/// Why `rights` cannot be saved (a field too long or not UTF-8 or with control characters, a date that is not
/// YYYY-MM-DD, a currency that is not three letters, a negative or absurd cost), or nullopt.
std::optional<std::string> rightsProblem(const MediaRights& rights);

/// A cost typed by a person -> hundredths: "250", "250.5", "1,250.00", "1.250,00", "$ 99.99". Digits and the
/// separators between them; the last '.' or ',' followed by one or two digits is the decimal point. Nullopt when it is
/// not a non-negative amount that fits kMaxCostMinor.
std::optional<std::int64_t> parseCostMinor(std::string_view text);
/// Hundredths -> "1250.00" (always two decimals, no thousands separator).
std::string formatCostMinor(std::int64_t minor);

/// How a record stands on the day `asOf` ("YYYY-MM-DD"; usually the delivery or air date).
struct RightsAssessment {
    bool pending = false;     ///< status Pending
    bool statusUnset = false; ///< the record has details but nobody said whether it is cleared
    bool expired = false;     ///< expiry before `asOf`
    bool expiresSoon = false; ///< expiry within `soonDays` after `asOf`
    std::int64_t daysLeft = 0; ///< days from `asOf` to expiry (negative once expired); 0 when there is no expiry

    /// Uncleared material: asked for but not granted, or never said.
    bool uncleared() const { return pending || statusUnset; }
    bool needsAttention() const { return uncleared() || expired || expiresSoon; }
};
/// A record that is empty has nothing to flag. `asOf` that is not a date: no expiry flags. `soonDays` 0 = no
/// "expires soon" flag.
RightsAssessment assessRights(const MediaRights& rights, std::string_view asOf, int soonDays = 30);

/// The words for the flags of `assessment`, in the order they matter ("Not cleared", "Status not set", "Licence
/// expired", "Licence expires in 12 days"); empty when there is nothing to say.
std::string rightsFlagText(const MediaRights& rights, const RightsAssessment& assessment);

} // namespace wabcut
