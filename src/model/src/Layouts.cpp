// Layout presets (C.15, gaming): facecam picture-in-picture and vertical shorts layouts as plain clip edits (Layouts.h).

#include <wabcut/model/Layouts.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/ProjectOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cmath>

namespace wabcut {

namespace {

struct Picture {
    double width = 0.0;  ///< display pixels (pixel aspect applied)
    double height = 0.0;
};

/// The display size of what a video clip shows: its media stream, or its nested sequence's frame.
std::optional<Picture> pictureOf(const ProjectState& state, const Clip& clip) {
    if (clip.isNested()) {
        const Sequence* nested = state.findSequence(clip.sequence);
        if (!nested || nested->width <= 0 || nested->height <= 0) {
            return std::nullopt;
        }
        return Picture{static_cast<double>(nested->width), static_cast<double>(nested->height)};
    }
    const MediaItem* item = state.findMedia(clip.media);
    if (!item || clip.stream.kind != StreamKind::Video || clip.stream.index < 0 ||
        static_cast<std::size_t>(clip.stream.index) >= item->info.video.size()) {
        return std::nullopt;
    }
    const VideoStreamInfo& v = item->info.video[static_cast<std::size_t>(clip.stream.index)];
    if (v.width <= 0 || v.height <= 0) {
        return std::nullopt;
    }
    const double par = v.sampleAspect.isValid() ? static_cast<double>(v.sampleAspect.num) / v.sampleAspect.den : 1.0;
    const bool sideways = v.rotationDegrees == 90 || v.rotationDegrees == 270;
    const double w = static_cast<double>(v.width) * par;
    const double h = static_cast<double>(v.height);
    return sideways ? Picture{h, w} : Picture{w, h};
}

bool hasLayoutTag(const EffectInstance& e) { return e.findData(kLayoutDataKey) != nullptr; }

/// Builds the effects a layout adds, each tagged so a later layout replaces it.
class StackBuilder {
public:
    explicit StackBuilder(edit::detail::SequenceEditor& ed) : m_ed(ed) {}

    EffectInstance make(std::string_view effect, std::initializer_list<std::pair<const char*, double>> params) {
        EffectInstance e;
        e.id = m_ed.newEffectId();
        e.effect = std::string(effect);
        for (const auto& [key, value] : params) {
            e.set(key, ParamValue(value));
        }
        e.setData(EffectData{std::string(kLayoutDataKey), std::string("layout")});
        return e;
    }

private:
    edit::detail::SequenceEditor& m_ed;
};

bool isFull(const NormalizedRect& r) {
    constexpr double kEps = 1e-9;
    return r.x <= kEps && r.y <= kEps && r.x + r.width >= 1.0 - kEps && r.y + r.height >= 1.0 - kEps;
}

/// What one clip of the layout becomes: Motion, and the tagged effects replacing the previous layout's.
struct ClipLook {
    RegionPlacement place;
    bool crop = false;
    bool roundedMask = false;
    bool circle = false;
    double roundness = 0.0;
    double borderWidth = 0.0;
    std::array<double, 3> borderColor{1.0, 1.0, 1.0};
    bool shadow = false;
    double blur = 0.0;
};

void applyLook(edit::detail::SequenceEditor& ed, Clip& clip, const ClipLook& look) {
    ClipContent content = clip.content();
    content.properties.set(ParamId::Scale, look.place.scale);
    content.properties.set(ParamId::PositionX, look.place.positionX);
    content.properties.set(ParamId::PositionY, look.place.positionY);
    content.properties.set(ParamId::AnchorX, look.place.anchorX);
    content.properties.set(ParamId::AnchorY, look.place.anchorY);
    content.properties.set(ParamId::Rotation, 0.0);
    EffectStack stack;
    for (const EffectInstance& e : content.effects) {
        if (!hasLayoutTag(e)) {
            stack.push_back(e);
        }
    }
    StackBuilder make(ed);
    const NormalizedRect& s = look.place.shown;
    if (look.blur > 0.0) {
        stack.push_back(make.make("wabcut.blur.gaussian", {{"radius", look.blur}, {"repeatEdges", 1.0}}));
    }
    if (look.crop) {
        stack.push_back(make.make("wabcut.crop", {{"left", std::clamp(s.x, 0.0, 1.0)},
                                                  {"top", std::clamp(s.y, 0.0, 1.0)},
                                                  {"right", std::clamp(1.0 - s.x - s.width, 0.0, 1.0)},
                                                  {"bottom", std::clamp(1.0 - s.y - s.height, 0.0, 1.0)}}));
    }
    if (look.roundedMask) {
        EffectInstance maskEffect = make.make("wabcut.mask", {});
        Mask m;
        m.id = 1;
        m.name = look.circle ? "Layout circle" : "Layout rounded corners";
        m.shape = look.circle ? MaskShape::Ellipse : MaskShape::Rectangle;
        m.set("center.x", ParamValue(s.x + s.width / 2.0));
        m.set("center.y", ParamValue(s.y + s.height / 2.0));
        m.set("size.x", ParamValue(s.width));
        m.set("size.y", ParamValue(s.height));
        if (!look.circle) {
            m.set("roundness", ParamValue(std::clamp(look.roundness, 0.0, 1.0)));
        }
        maskEffect.masks.push_back(std::move(m));
        stack.push_back(std::move(maskEffect));
    }
    if (look.borderWidth > 0.0) {
        stack.push_back(make.make(kBorderEffect, {{"width", look.borderWidth},
                                                  {"color.r", look.borderColor[0]},
                                                  {"color.g", look.borderColor[1]},
                                                  {"color.b", look.borderColor[2]},
                                                  {"opacity", 1.0}}));
    }
    if (look.shadow) {
        stack.push_back(make.make("wabcut.dropShadow",
                                  {{"opacity", 0.6}, {"angle", 135.0}, {"distance", 6.0}, {"softness", 18.0}}));
    }
    content.effects = std::move(stack);
    clip.setContent(std::move(content));
}

/// The facecam box of a PiP (canvas fractions) for a source region of `aspect` (width / height).
NormalizedRect pipBox(const LayoutOptions& o, double aspect, double canvasW, double canvasH) {
    const double m = o.margin * canvasH;
    double w = o.size * canvasW;
    double h = w / (o.circle ? 1.0 : aspect);
    const double maxH = std::max(1.0, canvasH - 2.0 * m);
    if (h > maxH) {
        w *= maxH / h;
        h = maxH;
    }
    const bool left = o.corner == LayoutCorner::TopLeft || o.corner == LayoutCorner::BottomLeft;
    const bool top = o.corner == LayoutCorner::TopLeft || o.corner == LayoutCorner::TopRight;
    const double x = left ? m : canvasW - m - w;
    const double y = top ? m : canvasH - m - h;
    return {x / canvasW, y / canvasH, w / canvasW, h / canvasH};
}

/// A free unlocked video track above video index `above` over `range`, else nullopt.
std::optional<TrackId> freeTrackAbove(const Sequence& seq, std::size_t above, TimeRange range,
                                      std::span<const TrackId> taken) {
    for (std::size_t i = above + 1; i < seq.videoTracks.size(); ++i) {
        const Track& t = *seq.videoTracks[i];
        if (t.locked || std::find(taken.begin(), taken.end(), t.id) != taken.end()) {
            continue;
        }
        const bool busy = std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.timelineRange.overlaps(range); });
        if (!busy) {
            return t.id;
        }
    }
    return std::nullopt;
}

} // namespace

std::string_view toString(LayoutPreset preset) {
    switch (preset) {
    case LayoutPreset::PictureInPicture: return "pip";
    case LayoutPreset::VerticalSplit: return "verticalSplit";
    case LayoutPreset::VerticalBlurFill: return "verticalBlurFill";
    }
    return "pip";
}

std::optional<LayoutPreset> layoutPresetFromString(std::string_view text) {
    for (const LayoutPreset p : {LayoutPreset::PictureInPicture, LayoutPreset::VerticalSplit, LayoutPreset::VerticalBlurFill}) {
        if (toString(p) == text) {
            return p;
        }
    }
    return std::nullopt;
}

std::string_view toString(LayoutCorner corner) {
    switch (corner) {
    case LayoutCorner::TopLeft: return "topLeft";
    case LayoutCorner::TopRight: return "topRight";
    case LayoutCorner::BottomLeft: return "bottomLeft";
    case LayoutCorner::BottomRight: return "bottomRight";
    }
    return "bottomRight";
}

std::optional<LayoutCorner> layoutCornerFromString(std::string_view text) {
    for (const LayoutCorner c : {LayoutCorner::TopLeft, LayoutCorner::TopRight, LayoutCorner::BottomLeft, LayoutCorner::BottomRight}) {
        if (toString(c) == text) {
            return c;
        }
    }
    return std::nullopt;
}

std::optional<std::string> layoutOptionsProblem(const LayoutOptions& o) {
    const auto in = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!in(o.size, 0.05, 1.0)) {
        return "size must be 0.05..1 (a fraction of the frame width)";
    }
    if (!in(o.margin, 0.0, 0.4)) {
        return "margin must be 0..0.4 (a fraction of the frame height)";
    }
    if (!in(o.cornerRadius, 0.0, 0.5)) {
        return "cornerRadius must be 0..0.5 (a fraction of the box's shorter side)";
    }
    if (!in(o.borderWidth, 0.0, 100.0)) {
        return "borderWidth must be 0..100 pixels";
    }
    for (const double c : o.borderColor) {
        if (!in(c, 0.0, 1.0)) {
            return "borderColor channels must be 0..1";
        }
    }
    if (!in(o.split, 0.2, 0.8)) {
        return "split must be 0.2..0.8 (the gameplay's share of the height)";
    }
    if (!in(o.blur, 0.0, 500.0)) {
        return "blur must be 0..500 pixels";
    }
    if (!in(o.zoom, 1.0, 4.0)) {
        return "zoom must be 1..4";
    }
    if (o.facecamRegion) {
        const NormalizedRect& r = *o.facecamRegion;
        if (!in(r.x, 0.0, 1.0) || !in(r.y, 0.0, 1.0) || !in(r.width, 0.01, 1.0) || !in(r.height, 0.01, 1.0) ||
            r.x + r.width > 1.0 + 1e-9 || r.y + r.height > 1.0 + 1e-9) {
            return "facecamRegion must lie inside the gameplay frame (fractions 0..1, at least 0.01 wide and high)";
        }
    }
    return std::nullopt;
}

RegionPlacement placeRegion(double sourceWidth, double sourceHeight, const NormalizedRect& region, const NormalizedRect& target,
                            double canvasWidth, double canvasHeight) {
    double rx = region.x * sourceWidth;
    double ry = region.y * sourceHeight;
    double rw = std::max(region.width * sourceWidth, 1e-9);
    double rh = std::max(region.height * sourceHeight, 1e-9);
    const double tx = target.x * canvasWidth;
    const double ty = target.y * canvasHeight;
    const double tw = std::max(target.width * canvasWidth, 1e-9);
    const double th = std::max(target.height * canvasHeight, 1e-9);
    // Trim the region to the target's aspect around its centre (centre crop), so it fills the target exactly.
    if (rw / rh > tw / th) {
        const double w = rh * tw / th;
        rx += (rw - w) / 2.0;
        rw = w;
    } else {
        const double h = rw * th / tw;
        ry += (rh - h) / 2.0;
        rh = h;
    }
    RegionPlacement p;
    p.scale = tw / rw;
    p.anchorX = rx + rw / 2.0 - sourceWidth / 2.0;
    p.anchorY = ry + rh / 2.0 - sourceHeight / 2.0;
    p.positionX = tx + tw / 2.0 - canvasWidth / 2.0;
    p.positionY = ty + th / 2.0 - canvasHeight / 2.0;
    p.shown = {rx / sourceWidth, ry / sourceHeight, rw / sourceWidth, rh / sourceHeight};
    return p;
}

namespace edit {

std::expected<LayoutResult, Error> applyLayout(const ProjectState& state, SequenceId sequenceId, const LayoutRequest& request) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const LayoutOptions& o = request.options;
    if (const auto problem = layoutOptionsProblem(o)) {
        return std::unexpected(detail::invalidArgument("layout: " + *problem));
    }
    const auto gameLoc = (*seq)->findClip(request.gameplay);
    if (!gameLoc) {
        return std::unexpected(detail::notFound("clip " + std::to_string(request.gameplay.value) + " is not in this sequence"));
    }
    const Clip& game = gameLoc->clip();
    const auto gamePos = (*seq)->trackPosition(gameLoc->track->id);
    if (gamePos->kind != TrackKind::Video) {
        return std::unexpected(detail::invalidArgument("the gameplay must be a video clip"));
    }
    if (gameLoc->track->locked) {
        return std::unexpected(detail::invalidArgument("the gameplay's track is locked"));
    }
    const auto gamePicture = pictureOf(state, game);
    if (!gamePicture) {
        return std::unexpected(detail::invalidArgument("the gameplay clip has no picture size"));
    }
    std::optional<Sequence::ClipLocation> camLoc;
    if (request.facecam.isValid()) {
        camLoc = (*seq)->findClip(request.facecam);
        if (!camLoc) {
            return std::unexpected(detail::notFound("clip " + std::to_string(request.facecam.value) + " is not in this sequence"));
        }
        const auto camPos = (*seq)->trackPosition(camLoc->track->id);
        if (camPos->kind != TrackKind::Video || camPos->index <= gamePos->index) {
            return std::unexpected(detail::invalidArgument("the facecam must be a video clip on a track above the gameplay's"));
        }
        if (camLoc->track->locked) {
            return std::unexpected(detail::invalidArgument("the facecam's track is locked"));
        }
        if (!camLoc->clip().timelineRange.overlaps(game.timelineRange)) {
            return std::unexpected(detail::invalidArgument("the facecam clip does not overlap the gameplay clip"));
        }
        if (!pictureOf(state, camLoc->clip())) {
            return std::unexpected(detail::invalidArgument("the facecam clip has no picture size"));
        }
    }
    const bool regionCam = !camLoc && o.facecamRegion.has_value();
    const bool hasCam = camLoc.has_value() || regionCam;
    if (!hasCam && request.preset != LayoutPreset::VerticalBlurFill) {
        return std::unexpected(detail::invalidArgument("this layout needs a facecam: a facecam clip or a facecamRegion"));
    }

    // Tracks for the copies of the gameplay (a sharp foreground, a facecam cut from the gameplay), above the gameplay;
    // new tracks on top when none is free over its range.
    ProjectState working = state;
    const bool needForeground = request.preset == LayoutPreset::VerticalBlurFill;
    std::vector<TrackId> taken;
    const auto allocate = [&](std::size_t above) -> std::expected<TrackId, Error> {
        const Sequence& s = *working.findSequence(sequenceId);
        if (const auto free = freeTrackAbove(s, above, game.timelineRange, taken)) {
            taken.push_back(*free);
            return *free;
        }
        auto added = addTrack(working, sequenceId, TrackKind::Video);
        if (!added) {
            return std::unexpected(added.error());
        }
        working = std::move(added->state);
        taken.push_back(added->id);
        return added->id;
    };
    TrackId foregroundTrack;
    TrackId camTrack;
    if (needForeground) {
        auto t = allocate(gamePos->index);
        if (!t) {
            return std::unexpected(t.error());
        }
        foregroundTrack = *t;
    }
    if (needForeground && camLoc) {
        // C.16: the facecam must stay on top of the sharp gameplay. When the foreground copy had to go above the facecam's
        // track, the facecam moves to a free track above the copy (a new one when none is free).
        const Sequence& s = *working.findSequence(sequenceId);
        const std::size_t foregroundIndex = s.trackPosition(foregroundTrack)->index;
        const std::size_t camIndex = s.trackPosition(camLoc->track->id)->index;
        if (camIndex < foregroundIndex) {
            std::optional<TrackId> above = freeTrackAbove(s, foregroundIndex, camLoc->clip().timelineRange, taken);
            if (!above) {
                auto added = addTrack(working, sequenceId, TrackKind::Video);
                if (!added) {
                    return std::unexpected(added.error());
                }
                working = std::move(added->state);
                above = added->id;
            }
            taken.push_back(*above);
            const std::size_t aboveIndex = working.findSequence(sequenceId)->trackPosition(*above)->index;
            const ClipId cam = request.facecam;
            auto moved = edit::moveClips(working, sequenceId, std::span<const ClipId>(&cam, 1), Ticks{},
                                         edit::TrackShift{static_cast<int>(aboveIndex - camIndex), 0}, edit::MoveMode::Overwrite,
                                         edit::LinkMode::IgnoreLinks);
            if (!moved) {
                return std::unexpected(moved.error());
            }
            working = std::move(moved->state);
        }
    }
    if (regionCam) {
        const std::size_t above = needForeground ? working.findSequence(sequenceId)->trackPosition(foregroundTrack)->index
                                                 : gamePos->index;
        auto t = allocate(above);
        if (!t) {
            return std::unexpected(t.error());
        }
        camTrack = *t;
    }

    const Sequence& seqNow = *working.findSequence(sequenceId);
    const double cw = seqNow.width;
    const double ch = seqNow.height;
    detail::SequenceEditor ed(working, seqNow);
    LayoutResult result;
    result.gameplay = request.gameplay;

    // The gameplay's link group holds the copies too, so the layout moves as one.
    LinkGroupId group = game.linkGroup;
    if (!group.isValid() && (needForeground || regionCam)) {
        group = ed.newLinkGroup();
    }
    const Clip original = game;
    const auto insertCopy = [&](TrackId track) {
        Clip copy = original;
        copy.id = ed.newClipId();
        copy.linkGroup = group;
        EffectStack effects;
        for (const EffectInstance& e : copy.effects()) {
            EffectInstance fresh = e;
            fresh.id = ed.newEffectId();
            effects.push_back(std::move(fresh));
        }
        copy.setEffects(std::move(effects));
        Track& t = ed.mutableTrack(track);
        const std::size_t at = t.clips.lowerBoundStart(copy.start());
        const ClipId id = copy.id;
        t.clips.insert(at, std::move(copy));
        return id;
    };
    const auto mutableOf = [&](ClipId id) -> Clip& {
        for (const TrackId t : ed.trackIds()) {
            const Track& track = ed.track(t);
            for (std::size_t i = 0; i < track.clips.size(); ++i) {
                if (track.clips[i].id == id) {
                    return ed.mutableClip(t, i);
                }
            }
        }
        return ed.mutableClip(gameLoc->track->id, 0); // unreachable: ids come from this sequence
    };

    // The facecam's source region and its look in a box (PiP, the blur fill's optional facecam).
    const Picture camPicture = camLoc ? *pictureOf(state, camLoc->clip()) : *gamePicture;
    const NormalizedRect camRegion = regionCam ? *o.facecamRegion : NormalizedRect{};
    const double camAspect = (camRegion.width * camPicture.width) / (camRegion.height * camPicture.height);
    const auto boxLook = [&](const NormalizedRect& box, bool rounded) {
        ClipLook look;
        look.place = placeRegion(camPicture.width, camPicture.height, camRegion, box, cw, ch);
        look.crop = true;
        look.circle = o.circle;
        look.roundedMask = o.circle || (rounded && o.cornerRadius > 0.0);
        look.roundness = o.cornerRadius * 2.0;
        look.borderWidth = o.borderWidth;
        look.borderColor = o.borderColor;
        look.shadow = o.shadow;
        return look;
    };

    ClipId camId = camLoc ? request.facecam : ClipId{};
    if (regionCam) {
        camId = insertCopy(camTrack);
    }
    switch (request.preset) {
    case LayoutPreset::PictureInPicture: {
        ClipLook gameLook;
        gameLook.place = placeRegion(gamePicture->width, gamePicture->height, NormalizedRect{}, NormalizedRect{}, cw, ch);
        gameLook.crop = !isFull(gameLook.place.shown);
        if (gameLoc->track->id.isValid()) {
            applyLook(ed, mutableOf(request.gameplay), gameLook);
        }
        applyLook(ed, mutableOf(camId), boxLook(pipBox(o, camAspect, cw, ch), true));
        break;
    }
    case LayoutPreset::VerticalSplit: {
        const NormalizedRect top{0.0, 0.0, 1.0, o.split};
        const NormalizedRect bottom{0.0, o.split, 1.0, 1.0 - o.split};
        const NormalizedRect camTop{0.0, 0.0, 1.0, 1.0 - o.split};
        const NormalizedRect gameBottom{0.0, 1.0 - o.split, 1.0, o.split};
        ClipLook gameLook;
        gameLook.place = placeRegion(gamePicture->width, gamePicture->height, NormalizedRect{},
                                     o.facecamOnTop ? gameBottom : top, cw, ch);
        gameLook.crop = true;
        applyLook(ed, mutableOf(request.gameplay), gameLook);
        ClipLook camLook;
        camLook.place = placeRegion(camPicture.width, camPicture.height, camRegion, o.facecamOnTop ? camTop : bottom, cw, ch);
        camLook.crop = true;
        applyLook(ed, mutableOf(camId), camLook);
        break;
    }
    case LayoutPreset::VerticalBlurFill: {
        ClipLook background;
        background.place = placeRegion(gamePicture->width, gamePicture->height, NormalizedRect{}, NormalizedRect{}, cw, ch);
        background.blur = o.blur;
        applyLook(ed, mutableOf(request.gameplay), background);
        // The sharp gameplay: fitted to the frame's width (zoom crops its sides), centred.
        const double regionW = 1.0 / o.zoom;
        const NormalizedRect middle{(1.0 - regionW) / 2.0, 0.0, regionW, 1.0};
        const double aspect = (regionW * gamePicture->width) / gamePicture->height;
        const double height = std::min(1.0, (cw / aspect) / ch);
        const NormalizedRect band{0.0, (1.0 - height) / 2.0, 1.0, height};
        const ClipId front = insertCopy(foregroundTrack);
        ClipLook sharp;
        sharp.place = placeRegion(gamePicture->width, gamePicture->height, middle, band, cw, ch);
        sharp.crop = !isFull(sharp.place.shown);
        applyLook(ed, mutableOf(front), sharp);
        result.foreground = front;
        if (camId.isValid()) {
            applyLook(ed, mutableOf(camId), boxLook(pipBox(o, camAspect, cw, ch), true));
        }
        break;
    }
    }
    if (group.isValid() && !game.linkGroup.isValid()) {
        mutableOf(request.gameplay).linkGroup = group;
    }
    result.facecam = camId;
    result.edit = ed.finish();
    result.edit.changes = diff(state, result.edit.state);
    return result;
}

} // namespace edit

} // namespace wabcut
