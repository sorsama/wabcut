// Vertical and square versions of a gaming edit (C.16, GamingOps.h): a copy at 1080x1920 or 1080x1080 laid out with the
// C.15 layouts (or the host's Auto Reframe), captions moved into the version's safe area.

#include <wabcut/model/GamingOps.h>

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Merge.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cctype>
#include <memory>

namespace wabcut {

namespace {

using edit::detail::invalidArgument;
using edit::detail::notFound;

std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool hasMedia(const ProjectState& state, const Clip& clip) {
    const MediaItem* item = state.findMedia(clip.media);
    return item && !item->isGenerator();
}

/// Index of the gameplay track: the lowest video track holding media.
std::optional<std::size_t> gameplayIndex(const ProjectState* state, const Sequence& seq) {
    for (std::size_t i = 0; i < seq.videoTracks.size(); ++i) {
        const Track& t = *seq.videoTracks[i];
        const bool media = std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) {
            return state ? hasMedia(*state, c) : c.media.isValid();
        });
        if (media) {
            return i;
        }
    }
    return std::nullopt;
}

bool facecamName(std::string_view name) {
    const std::string lower = asciiLower(name);
    for (const char* word : {"facecam", "webcam", "cam", "camera", "face"}) {
        if (lower.find(word) != std::string::npos) {
            return true;
        }
    }
    return lower.find("\xE0\xB8\x81\xE0\xB8\xA5\xE0\xB9\x89\xE0\xB8\xAD\xE0\xB8\x87") != std::string::npos; // "กล้อง"
}

/// The clip on `track` overlapping `range` the most, if any.
const Clip* bestOverlap(const Track& track, TimeRange range) {
    const Clip* best = nullptr;
    Ticks bestOverlapLength{};
    for (const Clip& c : track.clips) {
        const Ticks s = std::max(c.start(), range.start);
        const Ticks e = std::min(c.end(), range.end());
        if (e > s && e - s > bestOverlapLength) {
            best = &c;
            bestOverlapLength = e - s;
        }
    }
    return best;
}

std::pair<int, int> versionSize(VersionFormat format) { return format == VersionFormat::Shorts ? std::pair{1080, 1920} : std::pair{1080, 1080}; }

} // namespace

std::string_view toString(VersionFormat format) { return format == VersionFormat::Shorts ? "shorts" : "square"; }

std::optional<VersionFormat> versionFormatFromString(std::string_view text) {
    if (text == "shorts") {
        return VersionFormat::Shorts;
    }
    if (text == "square") {
        return VersionFormat::Square;
    }
    return std::nullopt;
}

std::string_view toString(VersionMethod method) {
    switch (method) {
    case VersionMethod::Auto: return "auto";
    case VersionMethod::Layout: return "layout";
    case VersionMethod::Reframe: return "reframe";
    case VersionMethod::BlurFill: return "blurFill";
    }
    return "auto";
}

std::optional<VersionMethod> versionMethodFromString(std::string_view text) {
    for (const VersionMethod m : {VersionMethod::Auto, VersionMethod::Layout, VersionMethod::Reframe, VersionMethod::BlurFill}) {
        if (toString(m) == text) {
            return m;
        }
    }
    return std::nullopt;
}

std::optional<TrackId> detectFacecamTrack(const Sequence& seq) {
    const auto game = gameplayIndex(nullptr, seq);
    if (!game) {
        return std::nullopt;
    }
    for (std::size_t i = *game + 1; i < seq.videoTracks.size(); ++i) {
        const Track& t = *seq.videoTracks[i];
        if (!t.clips.empty() && facecamName(t.name)) {
            return t.id;
        }
    }
    // A track whose clips carry a layout (C.15 picture-in-picture) and play other media than the gameplay's.
    std::vector<std::uint64_t> gameMedia;
    for (const Clip& c : seq.videoTracks[*game]->clips) {
        gameMedia.push_back(c.media.value);
    }
    for (std::size_t i = *game + 1; i < seq.videoTracks.size(); ++i) {
        const Track& t = *seq.videoTracks[i];
        for (const Clip& c : t.clips) {
            const bool laidOut = std::any_of(c.effects().begin(), c.effects().end(),
                                             [](const EffectInstance& e) { return e.findData(kLayoutDataKey) != nullptr; });
            if (laidOut && c.media.isValid() && std::find(gameMedia.begin(), gameMedia.end(), c.media.value) == gameMedia.end()) {
                return t.id;
            }
        }
    }
    return std::nullopt;
}

CaptionStyle versionCaptionStyle(const CaptionStyle& style, VersionFormat format) {
    CaptionStyle out = style;
    if (format == VersionFormat::Shorts) {
        // Above the platforms' bottom controls (title, description) and clear of their side buttons.
        out.placement = CaptionPlacement::Custom;
        out.positionX = ParamValue(0.5);
        out.positionY = ParamValue(0.68);
        out.maxWidth = std::min(out.maxWidth, 0.8);
        out.safeArea = std::max(out.safeArea, 0.1);
        out.fontSize = std::min(out.fontSize, 72.0 / 1080.0);
    } else {
        out.placement = CaptionPlacement::Bottom;
        out.safeArea = std::max(out.safeArea, 0.08);
        out.maxWidth = std::min(out.maxWidth, 0.9);
    }
    return out;
}

namespace edit {

std::expected<VersionResult, Error> makeVersion(const ProjectState& state, SequenceId sequenceId, const VersionOptions& options) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& source = **found;
    if (source.width <= source.height) {
        return std::unexpected(invalidArgument("a version is made from a landscape (16:9) sequence"));
    }
    if (const auto problem = layoutOptionsProblem(options.layout)) {
        return std::unexpected(invalidArgument("layout: " + *problem));
    }
    const auto [width, height] = versionSize(options.format);
    const std::string name = !options.name.empty() ? options.name
                                                   : source.name + (options.format == VersionFormat::Shorts ? " (Shorts)" : " (Square)");

    // The facecam: an explicit track, a region of the capture, or a detected track.
    std::optional<std::size_t> camIndex;
    if (options.facecamTrack) {
        const auto pos = source.trackPosition(*options.facecamTrack);
        if (!pos) {
            return std::unexpected(notFound("track " + std::to_string(options.facecamTrack->value) + " does not exist"));
        }
        if (pos->kind != TrackKind::Video) {
            return std::unexpected(invalidArgument("the facecam track must be a video track"));
        }
        camIndex = pos->index;
    } else if (!options.facecamRegion) {
        if (const auto detected = detectFacecamTrack(source)) {
            camIndex = source.trackPosition(*detected)->index;
        }
    }
    const bool hasCam = camIndex.has_value() || options.facecamRegion.has_value();
    VersionMethod method = options.method;
    if (method == VersionMethod::Auto) {
        method = hasCam ? VersionMethod::Layout : options.reframe ? VersionMethod::Reframe : VersionMethod::BlurFill;
    }
    if (method == VersionMethod::Layout && !hasCam) {
        return std::unexpected(invalidArgument("the layout method needs a facecam: a facecam track or a facecam region"));
    }
    if (method == VersionMethod::Reframe && !options.reframe) {
        return std::unexpected(invalidArgument("this host has no Auto Reframe"));
    }

    VersionResult result;
    result.method = method;
    ProjectState working;
    SequenceId copyId;
    if (method == VersionMethod::Reframe) {
        auto reframed = options.reframe(state, sequenceId, width, height, name);
        if (!reframed) {
            return std::unexpected(reframed.error());
        }
        working = std::move(reframed->state);
        copyId = reframed->sequence;
        result.notes = std::move(reframed->notes);
    } else {
        auto copied = copySequenceFrom(state, state, sequenceId, name);
        if (!copied) {
            return std::unexpected(copied.error());
        }
        working = std::move(copied->state);
        copyId = copied->id;
        Sequence resized = *working.findSequence(copyId);
        resized.width = width;
        resized.height = height;
        working = working.withSequence(std::make_shared<const Sequence>(std::move(resized)));

        const Sequence& copy = *working.findSequence(copyId);
        const auto game = gameplayIndex(&working, copy);
        if (!game) {
            return std::unexpected(invalidArgument("the sequence has no gameplay: no video clip of media"));
        }
        if (camIndex && *camIndex <= *game) {
            return std::unexpected(invalidArgument("the facecam track must be above the gameplay's"));
        }
        if (camIndex && *camIndex < copy.videoTracks.size()) {
            result.facecamTrack = copy.videoTracks[*camIndex]->id;
        }
        // The gameplay clips, by id (layouts add tracks and copies as they go).
        std::vector<ClipId> gameplay;
        for (const Clip& c : copy.videoTracks[*game]->clips) {
            if (hasMedia(working, c)) {
                gameplay.push_back(c.id);
            }
        }
        for (const ClipId id : gameplay) {
            const Sequence& now = *working.findSequence(copyId);
            const auto loc = now.findClip(id);
            if (!loc) {
                continue;
            }
            LayoutRequest request;
            request.gameplay = id;
            request.options = options.layout;
            if (options.facecamRegion) {
                request.options.facecamRegion = options.facecamRegion;
            }
            if (result.facecamTrack.isValid()) {
                if (const Clip* cam = bestOverlap(*now.findTrack(result.facecamTrack), loc->clip().timelineRange)) {
                    request.facecam = cam->id;
                }
            }
            const bool cam = request.facecam.isValid() || request.options.facecamRegion.has_value();
            if (method == VersionMethod::Layout && options.format == VersionFormat::Shorts && cam) {
                request.preset = LayoutPreset::VerticalSplit;
            } else {
                request.preset = LayoutPreset::VerticalBlurFill;
                if (method == VersionMethod::Layout && !cam) {
                    result.notes.push_back("clip " + std::to_string(id.value) + " has no facecam over it: blur fill");
                }
            }
            auto laid = applyLayout(working, copyId, request);
            if (!laid) {
                result.notes.push_back("clip " + std::to_string(id.value) + " was not laid out: " + laid.error().message);
                continue;
            }
            working = std::move(laid->edit.state);
            ++result.clipsLaidOut;
        }
    }

    // Captions into the version's safe area.
    if (options.captions) {
        const std::vector<CaptionTrack> tracks = working.findSequence(copyId)->captionTracks;
        for (const CaptionTrack& t : tracks) {
            auto styled = setCaptionStyle(working, copyId, t.id, versionCaptionStyle(t.style, options.format));
            if (!styled) {
                result.notes.push_back("caption track \"" + t.name + "\" kept its style: " + styled.error().message);
                continue;
            }
            working = std::move(styled->state);
            ++result.captionTracks;
        }
    }
    result.sequence = copyId;
    result.changes = diff(state, working);
    result.state = std::move(working);
    return result;
}

} // namespace edit

} // namespace wabcut
