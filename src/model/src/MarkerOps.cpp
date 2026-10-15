// Markers 2.0 (M5i): validation, marker types, updating a sequence marker, and replacing the markers of a sequence
// or of a media item (clip markers, in media time).

#include <wabcut/model/EditOps.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/TitleOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace wabcut {

namespace {

constexpr std::array<std::pair<MarkerType, std::string_view>, 4> kMarkerTypeNames{{{MarkerType::Comment, "comment"},
                                                                                  {MarkerType::Chapter, "chapter"},
                                                                                  {MarkerType::Segmentation, "segmentation"},
                                                                                  {MarkerType::WebLink, "webLink"}}};

bool startsWithNoCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<std::string> markerProblem(const Marker& marker) {
    if (marker.start.value < 0 || marker.duration.value < 0) {
        return "a marker cannot start before 0 or have a negative duration";
    }
    if (marker.start.value > Ticks::max().value - marker.duration.value) {
        return "the marker ends too late";
    }
    if (marker.name.size() > kMaxMarkerTextBytes || marker.comment.size() > kMaxMarkerTextBytes) {
        return "a marker name or comment is longer than " + std::to_string(kMaxMarkerTextBytes) + " bytes";
    }
    if (marker.url.size() > kMaxMarkerUrlBytes || marker.target.size() > kMaxMarkerUrlBytes) {
        return "a marker URL or frame target is longer than " + std::to_string(kMaxMarkerUrlBytes) + " bytes";
    }
    if (!marker.url.empty() && !startsWithNoCase(marker.url, "http://") && !startsWithNoCase(marker.url, "https://")) {
        return "a marker URL must start with http:// or https://";
    }
    if (const MarkerHighlight* h = marker.highlight ? &*marker.highlight : nullptr) { // C.15
        if (!std::isfinite(h->score) || h->score < 0.0 || h->score > 1.0) {
            return "a highlight score must be 0..1";
        }
        if (h->reasons.size() > MarkerHighlight::kMaxReasons) {
            return "a highlight has more than " + std::to_string(MarkerHighlight::kMaxReasons) + " reasons";
        }
        for (const std::string& reason : h->reasons) {
            if (reason.empty() || reason.size() > MarkerHighlight::kMaxReasonBytes || !edit::isValidUtf8(reason)) {
                return "a highlight reason must be 1.." + std::to_string(MarkerHighlight::kMaxReasonBytes) + " bytes of UTF-8";
            }
        }
        if (h->source.empty() || h->source.size() > MarkerHighlight::kMaxSourceBytes || !edit::isValidUtf8(h->source)) {
            return "a highlight source must be 1.." + std::to_string(MarkerHighlight::kMaxSourceBytes) + " bytes of UTF-8";
        }
        if (h->peak < marker.start || h->peak > marker.end()) {
            return "a highlight's peak must lie inside its marker";
        }
    }
    return std::nullopt;
}

std::string_view toString(HighlightConfidence confidence) {
    switch (confidence) {
    case HighlightConfidence::Low: return "low";
    case HighlightConfidence::Medium: return "medium";
    case HighlightConfidence::High: return "high";
    }
    return "medium";
}

std::optional<HighlightConfidence> highlightConfidenceFromString(std::string_view text) {
    for (const HighlightConfidence c : {HighlightConfidence::Low, HighlightConfidence::Medium, HighlightConfidence::High}) {
        if (toString(c) == text) {
            return c;
        }
    }
    return std::nullopt;
}

std::string_view markerTypeName(MarkerType type) {
    for (const auto& [t, name] : kMarkerTypeNames) {
        if (t == type) {
            return name;
        }
    }
    return "comment";
}

std::optional<MarkerType> markerTypeFromName(std::string_view name) {
    for (const auto& [t, n] : kMarkerTypeNames) {
        if (n == name) {
            return t;
        }
    }
    return std::nullopt;
}

namespace edit {

using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

namespace {

/// Checks every marker and the count, and sorts them by start (stable, so equal starts keep their order).
std::optional<Error> prepare(std::vector<Marker>& markers) {
    if (markers.size() > kMaxMarkers) {
        return invalidArgument("more than " + std::to_string(kMaxMarkers) + " markers");
    }
    for (const Marker& m : markers) {
        if (const auto problem = markerProblem(m)) {
            return invalidArgument(*problem);
        }
    }
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
    return std::nullopt;
}

} // namespace

EditOutcome updateMarker(const ProjectState& state, SequenceId sequenceId, std::size_t index, Marker marker) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto& current = (*seq)->markers;
    if (index >= current.size()) {
        return std::unexpected(notFound("there is no marker " + std::to_string(index)));
    }
    if (const auto problem = markerProblem(marker)) {
        return std::unexpected(invalidArgument(*problem));
    }
    if (current[index] == marker) {
        return detail::unchanged(state);
    }
    std::vector<Marker> markers = current;
    markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(index));
    // A moved marker goes after the others at its start, as addMarker places it.
    const auto at = std::upper_bound(markers.begin(), markers.end(), marker.start,
                                     [](Ticks t, const Marker& m) { return t < m.start; });
    markers.insert(at, std::move(marker));
    SequenceEditor ed(state, **seq);
    ed.header().markers = std::move(markers);
    return ed.finish();
}

EditOutcome setSequenceMarkers(const ProjectState& state, SequenceId sequenceId, std::vector<Marker> markers) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (const auto error = prepare(markers)) {
        return std::unexpected(*error);
    }
    if ((*seq)->markers == markers) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    ed.header().markers = std::move(markers);
    return ed.finish();
}

EditOutcome setMediaMarkers(const ProjectState& state, MediaId id, std::vector<Marker> markers) {
    const auto it = std::lower_bound(state.media.begin(), state.media.end(), id,
                                     [](const MediaPtr& m, MediaId value) { return m->id < value; });
    if (it == state.media.end() || (*it)->id != id) {
        return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
    }
    if (const auto error = prepare(markers)) {
        return std::unexpected(*error);
    }
    if ((*it)->markers == markers) {
        return detail::unchanged(state);
    }
    MediaItem item = **it;
    item.markers = std::move(markers);
    EditResult result;
    result.state = state;
    result.state.media[static_cast<std::size_t>(it - state.media.begin())] =
        std::make_shared<const MediaItem>(std::move(item));
    result.changes = diff(state, result.state);
    return result;
}

} // namespace edit

} // namespace wabcut
