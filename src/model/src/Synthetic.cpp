// Synthetic media and adjustment layers (C.15): descriptors, new items, checks and the edits that make them.

#include <wabcut/model/Synthetic.h>

#include <wabcut/model/ChangeSet.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut {

namespace {

using U = ParamUnit;

constexpr std::array<EffectParamInfo, 3> kColorMatteParams{{
    {"color.r", "Red", U::Fraction, 1.0, 0.0, 1.0, false},
    {"color.g", "Green", U::Fraction, 1.0, 0.0, 1.0, false},
    {"color.b", "Blue", U::Fraction, 1.0, 0.0, 1.0, false},
}};

constexpr std::array<SyntheticDescriptor, 3> kKinds{{
    {SyntheticKind::BlackVideo, kBlackVideoGenerator, "blackVideo", "Black Video", {}, "Opaque black: a slug or a base layer."},
    {SyntheticKind::ColorMatte, kColorMatteGenerator, "colorMatte", "Color Matte", kColorMatteParams,
     "A solid colour frame (color.r, color.g, color.b: 0..1, the sequence's encoding): a background for text, a flash."},
    {SyntheticKind::TransparentVideo, kTransparentVideoGenerator, "transparentVideo", "Transparent Video", {},
     "A fully transparent frame: a layer of its own for effects that draw (outlines, overlays)."},
}};

bool validFormat(const SyntheticFormat& f) {
    return f.width >= 1 && f.width <= 16384 && f.height >= 1 && f.height <= 16384 && f.frameRate.isValid();
}

/// Checks `params` against `infos`: known, constant, finite, in range; sorted and unique (after sorting when asked).
std::optional<std::string> paramsProblem(std::vector<EffectParam>& params, std::span<const EffectParamInfo> infos,
                                         bool sortFirst) {
    if (sortFirst) {
        std::sort(params.begin(), params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const EffectParam& p = params[i];
        if (i > 0 && !(params[i - 1].key < p.key)) {
            return "parameter '" + p.key.substr(0, 64) + "' is listed twice (or the list is not sorted)";
        }
        const auto info = std::find_if(infos.begin(), infos.end(), [&](const EffectParamInfo& in) { return in.key == p.key; });
        if (info == infos.end()) {
            return "unknown parameter '" + p.key.substr(0, 64) + "'";
        }
        if (!p.value.isConstant()) {
            return "parameter '" + p.key + "' cannot be keyframed";
        }
        const double v = p.value.constant();
        if (!std::isfinite(v) || v < info->minValue || v > info->maxValue) {
            return "parameter '" + p.key + "' is outside " + std::to_string(info->minValue) + ".." +
                   std::to_string(info->maxValue);
        }
    }
    return std::nullopt;
}

MediaItem generatorItem(std::string_view kind, std::vector<EffectParam> params, const SyntheticFormat& f, AlphaMode alpha,
                        std::string name) {
    MediaItem item;
    item.name = std::move(name);
    item.generator = GeneratorSpec{std::string(kind), {}, {}, {}, std::move(params)};
    item.info.isStillImage = true; // unbounded: clips of it can be any length
    VideoStreamInfo v;
    v.index = 0;
    v.codec = std::string(kind);
    v.width = f.width;
    v.height = f.height;
    v.frameRate = f.frameRate;
    v.alpha = alpha;
    item.info.video.push_back(std::move(v));
    return item;
}

} // namespace

std::span<const SyntheticDescriptor> syntheticKinds() { return kKinds; }

const SyntheticDescriptor* findSynthetic(std::string_view kindOrShortName) {
    for (const SyntheticDescriptor& d : kKinds) {
        if (d.generator == kindOrShortName || d.shortName == kindOrShortName) {
            return &d;
        }
    }
    return nullptr;
}

const SyntheticDescriptor* syntheticOf(const MediaItem& item) {
    return item.generator ? findSynthetic(item.generator->kind) : nullptr;
}

bool isAdjustmentLayer(const MediaItem& item) { return item.generator && item.generator->kind == kAdjustmentGenerator; }

std::expected<MediaItem, Error> syntheticMediaItem(SyntheticKind kind, std::vector<EffectParam> params,
                                                   const SyntheticFormat& format, std::string name) {
    const auto d = std::find_if(kKinds.begin(), kKinds.end(), [&](const SyntheticDescriptor& x) { return x.kind == kind; });
    if (d == kKinds.end()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "unknown synthetic kind"});
    }
    if (!validFormat(format)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a synthetic item needs a frame size of 1..16384 and a positive rate"});
    }
    if (const auto problem = paramsProblem(params, d->params, true)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::string(d->displayName) + ": " + *problem});
    }
    const AlphaMode alpha = kind == SyntheticKind::TransparentVideo ? AlphaMode::Straight : AlphaMode::None;
    return generatorItem(d->generator, std::move(params), format, alpha,
                         name.empty() ? std::string(d->displayName) : std::move(name));
}

std::expected<MediaItem, Error> adjustmentMediaItem(const SyntheticFormat& format, std::string name) {
    if (!validFormat(format)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "an adjustment layer needs a frame size of 1..16384 and a positive rate"});
    }
    return generatorItem(kAdjustmentGenerator, {}, format, AlphaMode::Straight,
                         name.empty() ? std::string("Adjustment Layer") : std::move(name));
}

double syntheticParam(const MediaItem& item, std::string_view key, double fallback) {
    if (item.generator) {
        for (const EffectParam& p : item.generator->params) {
            if (p.key == key && p.value.isConstant()) {
                return p.value.constant();
            }
        }
    }
    if (const SyntheticDescriptor* d = syntheticOf(item)) {
        for (const EffectParamInfo& info : d->params) {
            if (info.key == key) {
                return info.defaultValue;
            }
        }
    }
    return fallback;
}

std::optional<std::string> syntheticProblem(const MediaItem& item) {
    if (!item.generator) {
        return std::nullopt;
    }
    const bool adjustment = item.generator->kind == kAdjustmentGenerator;
    const SyntheticDescriptor* d = syntheticOf(item);
    if (!adjustment && !d) {
        // Titles, graphics and unknown kinds are checked elsewhere (or kept as they are); parameters stay bounded.
        if (item.generator->params.size() > 64) {
            return "has more than 64 generator parameters";
        }
        return std::nullopt;
    }
    const std::string what = adjustment ? std::string("an adjustment layer") : "a " + std::string(d->displayName) + " item";
    if (item.info.video.size() != 1 || !item.info.audio.empty() || !item.info.isStillImage) {
        return "is " + what + " but not exactly one unbounded video stream";
    }
    std::vector<EffectParam> params = item.generator->params;
    if (adjustment) {
        return params.empty() ? std::nullopt : std::optional<std::string>("is an adjustment layer with parameters");
    }
    if (const auto problem = paramsProblem(params, d->params, false)) {
        return "is " + what + " with a " + *problem;
    }
    return std::nullopt;
}

namespace edit {

std::expected<Created<MediaId>, Error> addSyntheticMedia(const ProjectState& state, SyntheticKind kind,
                                                         std::vector<EffectParam> params, SequenceId sequence, BinId bin,
                                                         std::string name) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (bin.isValid() && !state.findBin(bin)) {
        return std::unexpected(detail::notFound("bin " + std::to_string(bin.value) + " does not exist"));
    }
    auto item = syntheticMediaItem(kind, std::move(params), SyntheticFormat{(*seq)->width, (*seq)->height, (*seq)->frameRate},
                                   std::move(name));
    if (!item) {
        return std::unexpected(item.error());
    }
    item->bin = bin;
    return addMedia(state, std::move(*item));
}

EditOutcome addAdjustmentLayer(const ProjectState& state, SequenceId sequenceId, TrackId trackId, Ticks at, Ticks duration,
                               MediaId media, BinId bin) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Track* track = (*seq)->findTrack(trackId);
    if (!track) {
        return std::unexpected(detail::notFound("track " + std::to_string(trackId.value) + " does not exist"));
    }
    if (track->kind != TrackKind::Video) {
        return std::unexpected(detail::invalidArgument("an adjustment layer goes on a video track"));
    }
    if (track->locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, trackId) + " is locked"));
    }
    if (duration.value <= 0 || at.value < 0) {
        return std::unexpected(detail::invalidArgument("an adjustment layer needs a start >= 0 and a positive duration"));
    }
    ProjectState next = state;
    MediaId created;
    if (media.isValid()) {
        const MediaItem* item = state.findMedia(media);
        if (!item) {
            return std::unexpected(detail::notFound("media " + std::to_string(media.value) + " does not exist"));
        }
        if (!isAdjustmentLayer(*item)) {
            return std::unexpected(detail::invalidArgument("media '" + item->name + "' is not an adjustment layer"));
        }
    } else {
        if (bin.isValid() && !state.findBin(bin)) {
            return std::unexpected(detail::notFound("bin " + std::to_string(bin.value) + " does not exist"));
        }
        auto item = adjustmentMediaItem(SyntheticFormat{(*seq)->width, (*seq)->height, (*seq)->frameRate});
        if (!item) {
            return std::unexpected(item.error());
        }
        item->bin = bin;
        auto added = addMedia(state, std::move(*item));
        if (!added) {
            return std::unexpected(added.error());
        }
        next = std::move(added->state);
        media = created = added->id;
    }
    const SourceClip source{media, TimeRange{Ticks{}, duration}, {SourcePatch{StreamRef{StreamKind::Video, 0}, trackId}}};
    auto placed = overwriteEdit(next, sequenceId, source, at);
    if (!placed) {
        return std::unexpected(placed.error());
    }
    if (placed->placed.size() != 1) {
        return std::unexpected(detail::invalidArgument("the adjustment layer could not be placed"));
    }
    EditResult result = std::move(*placed);
    result.changes = diff(state, result.state);
    result.createdMedia = created;
    return result;
}

} // namespace edit

} // namespace wabcut
