#include <wabcut/interchange/ImportReport.h>

#include <algorithm>

namespace wabcut::interchange {

std::string_view toString(ReportOutcome outcome) {
    switch (outcome) {
    case ReportOutcome::Carried:
        return "carried";
    case ReportOutcome::Approximated:
        return "approximated";
    case ReportOutcome::Dropped:
        return "dropped";
    }
    return "unknown";
}

std::string_view toString(ReportCategory category) {
    switch (category) {
    case ReportCategory::Clip:
        return "clip";
    case ReportCategory::Gap:
        return "gap";
    case ReportCategory::Transition:
        return "transition";
    case ReportCategory::Speed:
        return "speed";
    case ReportCategory::Effect:
        return "effect";
    case ReportCategory::Marker:
        return "marker";
    case ReportCategory::Track:
        return "track";
    case ReportCategory::SequenceSetting:
        return "sequence-setting";
    case ReportCategory::AudioMapping:
        return "audio-mapping";
    case ReportCategory::Media:
        return "media";
    case ReportCategory::Graphic:
        return "graphic";
    }
    return "unknown";
}

void ImportReport::add(ReportOutcome outcome, ReportCategory category, std::string reason,
                       SourceLocation location, std::string sequence) {
    m_entries.push_back({outcome, category, std::move(reason), std::move(location), std::move(sequence)});
}

void ImportReport::append(const ImportReport& other) {
    m_entries.insert(m_entries.end(), other.m_entries.begin(), other.m_entries.end());
}

std::size_t ImportReport::count(ReportOutcome outcome) const {
    return static_cast<std::size_t>(std::count_if(
        m_entries.begin(), m_entries.end(), [&](const ReportEntry& e) { return e.outcome == outcome; }));
}

std::size_t ImportReport::count(ReportOutcome outcome, ReportCategory category) const {
    return static_cast<std::size_t>(
        std::count_if(m_entries.begin(), m_entries.end(),
                      [&](const ReportEntry& e) { return e.outcome == outcome && e.category == category; }));
}

std::vector<const ReportEntry*> ImportReport::filter(ReportOutcome outcome) const {
    std::vector<const ReportEntry*> out;
    for (const auto& e : m_entries) {
        if (e.outcome == outcome) {
            out.push_back(&e);
        }
    }
    return out;
}

std::vector<const ReportEntry*> ImportReport::filter(ReportOutcome outcome, ReportCategory category) const {
    std::vector<const ReportEntry*> out;
    for (const auto& e : m_entries) {
        if (e.outcome == outcome && e.category == category) {
            out.push_back(&e);
        }
    }
    return out;
}

} // namespace wabcut::interchange
