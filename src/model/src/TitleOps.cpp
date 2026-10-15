// Title clips: generator media item + clip + style effect.

#include <wabcut/model/TitleOps.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace wabcut::edit {

namespace {

constexpr std::size_t kMaxNameCodePoints = 40;

/// The first non-empty line of `text`, at most kMaxNameCodePoints code points; "Title" when there is none.
std::string titleName(std::string_view text) {
    std::string_view line;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        line = text.substr(0, nl);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (!line.empty() || nl == std::string_view::npos) {
            break;
        }
        text = text.substr(nl + 1);
    }
    if (line.empty()) {
        return "Title";
    }
    std::size_t codePoints = 0;
    std::size_t end = 0;
    while (end < line.size() && codePoints < kMaxNameCodePoints) {
        ++end;
        while (end < line.size() && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80) {
            ++end;
        }
        ++codePoints;
    }
    std::string name(line.substr(0, end));
    if (end < line.size()) {
        name += "\xE2\x80\xA6"; // ...
    }
    return name;
}

/// The title item `id`, or why it cannot be edited as one.
std::expected<std::size_t, Error> titleIndex(const ProjectState& state, MediaId id) {
    const auto it = std::lower_bound(state.media.begin(), state.media.end(), id,
                                     [](const MediaPtr& m, MediaId value) { return m->id < value; });
    if (it == state.media.end() || (*it)->id != id) {
        return std::unexpected(detail::notFound("media " + std::to_string(id.value) + " does not exist"));
    }
    if (!(*it)->isTitle()) {
        return std::unexpected(detail::invalidArgument("media '" + (*it)->name + "' is not a title"));
    }
    return static_cast<std::size_t>(it - state.media.begin());
}

/// `state` with the title item at `index` replaced by `item`, as an edit result. A title shown by a clip on a
/// locked track cannot change (locked tracks must look the same before and after an edit).
EditOutcome replaceTitle(const ProjectState& state, std::size_t index, MediaItem item) {
    if (item == *state.media[index]) {
        return detail::unchanged(state);
    }
    for (const SequencePtr& seq : state.sequences) {
        std::optional<std::string> locked;
        seq->forEachTrack([&](const Track& t) {
            if (!locked && t.locked &&
                std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.media == item.id; })) {
                locked = detail::trackLabel(*seq, t.id);
            }
        });
        if (locked) {
            return std::unexpected(detail::invalidArgument("title '" + item.name + "' is shown on locked track " + *locked +
                                                           " in sequence '" + seq->name + "'"));
        }
    }
    EditResult result;
    result.state = state;
    result.state.media[index] = std::make_shared<const MediaItem>(std::move(item));
    result.changes = diff(state, result.state);
    return result;
}

} // namespace

bool isValidUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) {
                return false;
            }
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        constexpr std::uint32_t kMinimum[] = {0, 0x80, 0x800, 0x10000};
        if (cp < kMinimum[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n + 1;
    }
    return true;
}

MediaItem titleMediaItem(std::string text, const TitleFont& font, std::int32_t width, std::int32_t height, Rate rate) {
    MediaItem item;
    item.name = titleName(text);
    item.generator = GeneratorSpec{std::string(kTitleGenerator), std::move(text), font.family, font.style};
    item.info.isStillImage = true; // unbounded: clips of it can be any length
    VideoStreamInfo video;
    video.index = 0;
    video.codec = std::string(kTitleGenerator);
    video.width = width;
    video.height = height;
    video.frameRate = rate;
    video.alpha = AlphaMode::Straight;
    item.info.video.push_back(std::move(video));
    return item;
}

EditOutcome addTitle(const ProjectState& state, SequenceId sequenceId, TrackId trackId, Ticks at, Ticks duration,
                     std::string text, std::vector<EffectParam> style, TitleFont font, BinId bin) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Track* track = (*seq)->findTrack(trackId);
    if (!track) {
        return std::unexpected(detail::notFound("track " + std::to_string(trackId.value) + " does not exist"));
    }
    if (track->kind != TrackKind::Video) {
        return std::unexpected(detail::invalidArgument("a title goes on a video track"));
    }
    if (track->locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, trackId) + " is locked"));
    }
    if (duration.value <= 0 || at.value < 0) {
        return std::unexpected(detail::invalidArgument("a title needs a start >= 0 and a positive duration"));
    }
    if (!isValidUtf8(text)) {
        return std::unexpected(detail::invalidArgument("the title text is not UTF-8"));
    }
    if (font.family.empty()) {
        return std::unexpected(detail::invalidArgument("a title needs a font family"));
    }
    MediaItem item = titleMediaItem(std::move(text), font, (*seq)->width, (*seq)->height, (*seq)->frameRate);
    item.bin = bin;
    auto media = addMedia(state, std::move(item));
    if (!media) {
        return std::unexpected(media.error());
    }
    const SourceClip source{media->id, TimeRange{Ticks{}, duration}, {SourcePatch{StreamRef{StreamKind::Video, 0}, trackId}}};
    auto placed = overwriteEdit(media->state, sequenceId, source, at);
    if (!placed) {
        return std::unexpected(placed.error());
    }
    if (placed->placed.size() != 1) {
        return std::unexpected(detail::invalidArgument("the title could not be placed"));
    }
    const ClipId clip = placed->placed.front();
    auto styled = addEffect(placed->state, sequenceId, std::span(&clip, 1), std::string(kTitleStyleEffect), std::move(style));
    if (!styled) {
        return std::unexpected(styled.error());
    }
    EditResult result = std::move(*placed);
    result.state = std::move(styled->state);
    result.changes = diff(state, result.state);
    result.createdEffects = std::move(styled->createdEffects);
    result.createdMedia = media->id;
    return result;
}

EditOutcome setTitleText(const ProjectState& state, MediaId title, std::string text) {
    const auto index = titleIndex(state, title);
    if (!index) {
        return std::unexpected(index.error());
    }
    if (!isValidUtf8(text)) {
        return std::unexpected(detail::invalidArgument("the title text is not UTF-8"));
    }
    MediaItem item = *state.media[*index];
    item.generator->text = std::move(text);
    return replaceTitle(state, *index, std::move(item));
}

EditOutcome setTitleFont(const ProjectState& state, MediaId title, std::optional<std::string> family,
                         std::optional<std::string> style) {
    const auto index = titleIndex(state, title);
    if (!index) {
        return std::unexpected(index.error());
    }
    if (family && family->empty()) {
        return std::unexpected(detail::invalidArgument("a title needs a font family"));
    }
    MediaItem item = *state.media[*index];
    if (family) {
        item.generator->fontFamily = std::move(*family);
    }
    if (style) {
        item.generator->fontStyle = std::move(*style);
    }
    return replaceTitle(state, *index, std::move(item));
}

} // namespace wabcut::edit
