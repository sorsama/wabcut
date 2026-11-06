// 0.7 (VR / 360 video): vr.info and vr.reorient, and the helpers the sequence and footage commands share for a VR format
// (CatalogEditing.cpp: sequence.setSettings "vr", media.interpretFootage "vr"). A VR sequence or item is an
// equirectangular picture, mono or stereoscopic (model/Vr.h).

#include "Catalog.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Vr.h>

#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

std::vector<std::string> projectionNames() {
    return {std::string(toString(VrProjection::None)), std::string(toString(VrProjection::Equirectangular360)),
            std::string(toString(VrProjection::Equirectangular180))};
}

std::vector<std::string> stereoNames() {
    return {std::string(toString(StereoLayout::Mono)), std::string(toString(StereoLayout::TopBottom)),
            std::string(toString(StereoLayout::SideBySide))};
}

} // namespace

Json vrParam(std::string description) {
    Json properties = Json::object();
    properties["projection"] = schema::enumeration(projectionNames(), "none = a flat picture; equirect360 the whole sphere "
                                                                      "(2:1 for one eye); equirect180 the front half (VR180).");
    properties["stereo"] = schema::enumeration(stereoNames(), "Where the eyes are in the frame: mono, topBottom (left eye "
                                                              "on top) or sideBySide (left eye on the left). Default mono.");
    return Json{{"type", Json::array({"object", "null"})},
                {"properties", std::move(properties)},
                {"required", Json::array({"projection"})},
                {"description", std::move(description)}};
}

VrFormat vrFormatValue(const Json& value, const std::string& path) {
    if (!value.is_object()) {
        fail(ErrorCode::InvalidParams, "a VR format is an object with a \"projection\"", Json{{"path", path}});
    }
    VrFormat format;
    const std::string projection = value.value("projection", std::string());
    const auto parsedProjection = vrProjectionFromString(projection);
    if (!parsedProjection) {
        fail(ErrorCode::InvalidParams, "\"projection\" is none, equirect360 or equirect180", Json{{"path", path + "/projection"}});
    }
    format.projection = *parsedProjection;
    if (value.contains("stereo")) {
        const std::string stereo = value["stereo"].is_string() ? value["stereo"].get<std::string>() : std::string();
        const auto parsedStereo = stereoLayoutFromString(stereo);
        if (!parsedStereo) {
            fail(ErrorCode::InvalidParams, "\"stereo\" is mono, topBottom or sideBySide", Json{{"path", path + "/stereo"}});
        }
        format.stereo = *parsedStereo;
    }
    if (const auto problem = vrFormatProblem(format)) {
        fail(ErrorCode::InvalidParams, *problem, Json{{"path", path}});
    }
    return format;
}

Json vrFormatJson(const VrFormat& format) {
    return Json{{"projection", std::string(toString(format.projection))},
                {"stereo", std::string(toString(format.stereo))},
                {"isVr", format.isVr()}};
}

void registerVrCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"vr.info", 1, "VR Video Info",
         "Whether the sequence is VR / 360 video (its projection and eye layout: sequence.setSettings \"vr\" sets it) and, "
         "with \"media\", what that item's picture is: \"file\" is what its spherical metadata says, \"interpretation\" "
         "Interpret Footage's setting (null: none), \"effective\" the one in use and \"suits\" whether the frame size "
         "fits the projection (a 360 eye is 2:1, a 180 eye 1:1).",
         Capability::Read, withSequence({{"media", mediaParam("A media item.")}}), Json{{"media", 3}}, {},
         [](ExecutionContext& c) {
             Json data{{"sequence", vrFormatJson(c.sequence().vr)}};
             if (c.has("media")) {
                 const MediaItem& item = *c.state().findMedia(c.media("media"));
                 const VrFormat file = item.info.hasVideo() ? item.info.video.front().vr : VrFormat{};
                 const VrFormat effective = mediaVr(item);
                 Json media{{"file", vrFormatJson(file)}, {"effective", vrFormatJson(effective)}};
                 media["interpretation"] = item.interpretation && item.interpretation->vr ? vrFormatJson(*item.interpretation->vr)
                                                                                           : Json(nullptr);
                 if (item.info.hasVideo()) {
                     const VideoStreamInfo& v = item.info.video.front();
                     media["suits"] = vrAspectSuits(effective, v.width, v.height);
                 }
                 data["media"] = std::move(media);
             }
             CommandOutput out;
             out.state = c.state();
             out.data = std::move(data);
             return out;
         }});
    addBuiltin(
        registry,
        {"vr.reorient", 1, "VR Reorient",
         "Adds the VR Reorient effect to 360 / VR180 clips and sets it: \"yaw\" turns the sphere to the right, \"pitch\" "
         "up, \"roll\" tilts it to the right (degrees; keyframe them with clip.addKeyframe), so a different part of the "
         "scene lands straight ahead. The projection and eye layout default to the clip's (its footage's, else the "
         "sequence's); a flat clip needs \"projection\". One undo step.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("360 / VR180 clips.")},
                       {"yaw", schema::number("Degrees to the right (-3600 to 3600).", -3600.0, 3600.0)},
                       {"pitch", schema::number("Degrees up (-90 to 90).", -90.0, 90.0)},
                       {"roll", schema::number("Degrees of tilt to the right (-3600 to 3600).", -3600.0, 3600.0)},
                       {"projection", schema::enumeration({std::string(toString(VrProjection::Equirectangular360)),
                                                           std::string(toString(VrProjection::Equirectangular180))},
                                                          "The picture's projection. Default: the clip's.")},
                       {"stereo", schema::enumeration(stereoNames(), "The picture's eye layout. Default: the clip's.")}},
                      {"clips"}),
         Json{{"clips", Json::array({42})}, {"yaw", 90}}, {},
         [](ExecutionContext& c) {
             const std::vector<ClipId> clips = c.clips("clips");
             const Sequence& seq = c.sequence();
             const auto clipLocation = seq.findClip(clips.front());
             VrFormat format = seq.vr;
             if (clipLocation) {
                 if (const MediaItem* item = c.state().findMedia(clipLocation->clip().media)) {
                     if (mediaVr(*item).isVr()) {
                         format = mediaVr(*item);
                     }
                 }
             }
             if (c.has("projection")) {
                 format.projection = *vrProjectionFromString(c.string("projection"));
             }
             if (c.has("stereo")) {
                 format.stereo = *stereoLayoutFromString(c.string("stereo"));
             }
             if (!format.isVr()) {
                 fail(ErrorCode::InvalidParams,
                      "the clip is not VR video (Interpret Footage sets it): give \"projection\" (equirect360 or equirect180)",
                      Json{{"path", "/projection"}});
             }
             std::vector<EffectParam> params;
             for (const char* key : {"yaw", "pitch", "roll"}) {
                 if (c.has(key)) {
                     params.push_back(EffectParam{key, ParamValue(c.number(key))});
                 }
             }
             params.push_back(EffectParam{"projection", ParamValue(format.projection == VrProjection::Equirectangular180 ? 1.0 : 0.0)});
             params.push_back(EffectParam{"stereo", ParamValue(static_cast<double>(static_cast<int>(format.stereo)))});
             return edited(c.take(edit::addEffect(c.state(), c.sequenceId(), clips, "wabcut.vr.reorient", params, std::nullopt)));
         }});
}

} // namespace wabcut::command::detail
