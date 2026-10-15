#pragma once

// What an import kept, approximated and lost.
//
// Every interchange format can describe things Wabcut's model cannot hold yet (transitions, retimes,
// effects, nested sequences, ...). An import never fails or guesses silently because of that: each
// element it read is listed here as Carried (in the timeline exactly), Approximated (in the timeline,
// but changed; the reason says how) or Dropped (not in the timeline; the reason says why), with the
// place in the source file it came from. `reason` is plain English meant for the user, unlike
// Error::message.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::interchange {

enum class ReportOutcome : std::uint8_t {
    /// Imported exactly.
    Carried,
    /// Imported, but changed to fit the model (e.g. a dissolve imported as a cut).
    Approximated,
    /// Not imported.
    Dropped,
};

enum class ReportCategory : std::uint8_t {
    Clip,
    Gap,
    Transition,
    Speed,
    Effect,
    Marker,
    Track,
    SequenceSetting,
    AudioMapping,
    /// Media references: file paths, relinking, declared durations.
    Media,
    /// Motion graphics (C.13): Lottie layers, shapes, keyframes, text and masks.
    Graphic,
};

inline constexpr std::size_t kReportOutcomeCount = 3;
inline constexpr std::size_t kReportCategoryCount = 11;

/// Stable lower-case names ("carried", "sequence-setting", ...) used by the CLI and JSON output.
std::string_view toString(ReportOutcome outcome);
std::string_view toString(ReportCategory category);

/// Where an entry came from in the imported file.
struct SourceLocation {
    /// The imported file (as given to importFile, or the name passed to importData).
    std::string file;
    /// Format-specific path to the element: an XML element path ("/xmeml/sequence[1]/media/video/track[2]/
    /// clipitem[3]"), a JSON pointer ("/tracks/children/0/children/4") or an EDL event ("event 012").
    std::string element;
    /// 1-based line, or 0 when the format has no useful line (JSON).
    std::int32_t line = 0;

    bool operator==(const SourceLocation&) const = default;
};

struct ReportEntry {
    ReportOutcome outcome = ReportOutcome::Carried;
    ReportCategory category = ReportCategory::Clip;
    /// Human-readable explanation, e.g. "Cross Dissolve (30 frames) imported as a cut".
    std::string reason;
    SourceLocation location;
    /// Name of the sequence the entry belongs to; empty for project-level entries (media).
    std::string sequence;

    bool operator==(const ReportEntry&) const = default;
};

class ImportReport {
public:
    void add(ReportEntry entry) { m_entries.push_back(std::move(entry)); }
    void add(ReportOutcome outcome, ReportCategory category, std::string reason, SourceLocation location,
             std::string sequence = {});
    /// Appends every entry of `other`.
    void append(const ImportReport& other);

    const std::vector<ReportEntry>& entries() const { return m_entries; }
    bool empty() const { return m_entries.empty(); }
    std::size_t size() const { return m_entries.size(); }

    std::size_t count(ReportOutcome outcome) const;
    std::size_t count(ReportOutcome outcome, ReportCategory category) const;
    /// Entries with the given outcome (and category), in report order.
    std::vector<const ReportEntry*> filter(ReportOutcome outcome) const;
    std::vector<const ReportEntry*> filter(ReportOutcome outcome, ReportCategory category) const;

private:
    std::vector<ReportEntry> m_entries;
};

} // namespace wabcut::interchange
