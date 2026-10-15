// Source Monitor marks stored on media items (MediaItem::markIn / markOut).

#include <wabcut/model/EditOps.h>

#include <wabcut/model/ChangeSet.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <memory>
#include <string>

namespace wabcut::edit {

std::optional<Ticks> sourceMarkExtent(const MediaItem& media) {
    std::optional<Ticks> extent;
    const auto take = [&](StreamKind kind, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            if (const auto e = media.sourceExtent({kind, static_cast<std::int32_t>(i)})) {
                extent = extent ? std::max(*extent, *e) : *e;
            }
        }
    };
    take(StreamKind::Video, media.info.video.size());
    take(StreamKind::Audio, media.info.audio.size());
    return extent;
}

SourceMarks storedSourceMarks(const MediaItem& media, bool* clamped) {
    SourceMarks marks{media.markIn, media.markOut};
    bool changed = false;
    // A restricted subclip's marks stay inside its range.
    if (media.subclip && media.subclip->restricted) {
        const TimeRange range = media.subclip->range;
        if (marks.in && (*marks.in < range.start || *marks.in >= range.end())) {
            marks.in = std::clamp(*marks.in, range.start, range.end() - Ticks{1});
            changed = true;
        }
        if (marks.out && (*marks.out <= range.start || *marks.out > range.end())) {
            marks.out = std::clamp(*marks.out, range.start + Ticks{1}, range.end());
            changed = true;
        }
    }
    if (const auto extent = sourceMarkExtent(media)) {
        if (marks.out && *marks.out > *extent) {
            marks.out = *extent;
            changed = true;
        }
        if (marks.in && *marks.in >= (marks.out ? *marks.out : *extent)) {
            marks.in.reset();
            changed = true;
        }
    }
    if (clamped) {
        *clamped = changed;
    }
    return marks;
}

EditOutcome setSourceMarks(const ProjectState& state, MediaId id, SourceMarks marks) {
    const auto it = std::lower_bound(state.media.begin(), state.media.end(), id,
                                     [](const MediaPtr& m, MediaId value) { return m->id < value; });
    if (it == state.media.end() || (*it)->id != id) {
        return std::unexpected(detail::notFound("media " + std::to_string(id.value) + " does not exist"));
    }
    const MediaItem& media = **it;
    if (marks.in && marks.in->value < 0) {
        return std::unexpected(detail::invalidArgument("the source In is before the start of the media"));
    }
    if (marks.out && marks.out->value <= 0) {
        return std::unexpected(detail::invalidArgument("the source Out must come after the start of the media"));
    }
    if (marks.in && marks.out && *marks.out <= *marks.in) {
        return std::unexpected(detail::invalidArgument("the source Out must come after the source In"));
    }
    if (const auto extent = sourceMarkExtent(media);
        extent && ((marks.in && *marks.in >= *extent) || (marks.out && *marks.out > *extent))) {
        return std::unexpected(detail::invalidArgument("the source marks are past the end of '" + media.name + "'"));
    }
    if (media.subclip && media.subclip->restricted) {
        const TimeRange range = media.subclip->range;
        if ((marks.in && (*marks.in < range.start || *marks.in >= range.end())) ||
            (marks.out && (*marks.out <= range.start || *marks.out > range.end()))) {
            return std::unexpected(detail::invalidArgument("the source marks are outside the subclip '" + media.name + "'"));
        }
    }
    if (media.markIn == marks.in && media.markOut == marks.out) {
        return detail::unchanged(state);
    }
    MediaItem item = media;
    item.markIn = marks.in;
    item.markOut = marks.out;
    EditResult result;
    result.state = state;
    result.state.media[static_cast<std::size_t>(it - state.media.begin())] =
        std::make_shared<const MediaItem>(std::move(item));
    result.changes = diff(state, result.state);
    return result;
}

} // namespace wabcut::edit
