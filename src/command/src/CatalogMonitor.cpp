// P.2 (E6): Premiere's Source and Program Monitor commands.
//   clip.setMotion        Motion (Position, Scale, Rotation, Anchor Point) set together at one time, as the Program
//                         Monitor's direct manipulation does: one undo step, keyed where the parameter is animated.
//   sequence.exportFrame  Export Frame from the Program Monitor: the sequence frame rendered as an export renders it,
//                         written as DPX, JPEG, PNG, TIFF, Targa, BMP, GIF or OpenEXR (the host's FrameExportServices),
//                         optionally imported into the project (one undo step).
//   source.exportFrame    The same from the Source Monitor: a media item's frame at a source time, at its own size.

#include "Catalog.h"

#include <wabcut/command/FrameServices.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::command::detail {

namespace {

const Clip& clipOf(ExecutionContext& c, ClipId id) { return c.sequence().findClip(id)->clip(); }

/// The Motion parameters by command key, in the order clip.setMotion reports them.
struct MotionParam {
    const char* key;
    ParamId id;
    std::optional<double> edit::MotionChange::*field;
};
constexpr std::array kMotionParams{
    MotionParam{"positionX", ParamId::PositionX, &edit::MotionChange::positionX},
    MotionParam{"positionY", ParamId::PositionY, &edit::MotionChange::positionY},
    MotionParam{"scale", ParamId::Scale, &edit::MotionChange::scale},
    MotionParam{"rotation", ParamId::Rotation, &edit::MotionChange::rotation},
    MotionParam{"anchorX", ParamId::AnchorX, &edit::MotionChange::anchorX},
    MotionParam{"anchorY", ParamId::AnchorY, &edit::MotionChange::anchorY},
};

Json motionJson(const edit::MotionValues& v) {
    return Json{{"positionX", v.positionX}, {"positionY", v.positionY}, {"scale", v.scale},
                {"rotation", v.rotation},   {"anchorX", v.anchorX},     {"anchorY", v.anchorY}};
}

/// Export Frame's formats (Premiere's list) and their file extensions; the first extension is the one added.
struct StillFormatName {
    const char* name;
    std::vector<std::string> extensions;
};
const std::vector<StillFormatName>& stillFormatNames() {
    static const std::vector<StillFormatName> kNames{
        {"dpx", {".dpx"}},   {"jpeg", {".jpg", ".jpeg"}}, {"png", {".png"}}, {"tiff", {".tif", ".tiff"}},
        {"targa", {".tga"}}, {"bmp", {".bmp"}},           {"gif", {".gif"}}, {"openexr", {".exr"}},
    };
    return kNames;
}

std::vector<std::string> stillFormatList() {
    std::vector<std::string> out;
    for (const StillFormatName& f : stillFormatNames()) {
        out.emplace_back(f.name);
    }
    return out;
}

/// The params both Export Frame commands take besides where the frame is.
std::vector<std::pair<const char*, Json>> exportFrameParams() {
    Json path = schema::string("The file to write (UTF-8 path). The format's extension is added when it has another one; "
                               "an existing file is replaced.");
    path["minLength"] = 1;
    path["maxLength"] = 4096;
    return {{"format", schema::enumeration(stillFormatList(), "The image format (Premiere's Export Frame list). Default png.")},
            {"path", path},
            {"importIntoProject",
             schema::boolean("Premiere's \"Import into project\": the still is added to the project as a media item.", false)},
            {"bin", schema::integer("With importIntoProject: the bin it goes into (default the root bin).", 1)}};
}

/// Renders `time` of `sequence` in `render` (a state of its own for the Source Monitor's frame) through the host's
/// FrameExportServices, writes it, and with "importIntoProject" adds the file to the context's project.
CommandOutput exportFrameOf(ExecutionContext& c, const ProjectState& render, SequenceId sequence, Ticks time) {
    auto* services = findService<FrameExportServices>(c.services());
    const std::string format = c.string("format", "png");
    const auto known = std::find_if(stillFormatNames().begin(), stillFormatNames().end(),
                                    [&](const StillFormatName& f) { return format == f.name; });
    if (known == stillFormatNames().end()) {
        fail(ErrorCode::InvalidParams, "unknown format " + format, Json{{"path", "/format"}});
    }
    std::filesystem::path file = pathFromUtf8(c.string("path"));
    std::string extension = pathToUtf8(file.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (std::find(known->extensions.begin(), known->extensions.end(), extension) == known->extensions.end()) {
        file += pathFromUtf8(known->extensions.front()); // "Frame.png" as DPX becomes "Frame.png.dpx"
    }
    const bool import = c.boolean("importIntoProject", false);
    BinId bin;
    if (c.has("bin")) {
        bin = BinId{static_cast<std::uint64_t>(c.integer("bin"))};
        if (!c.state().findBin(bin)) {
            fail(ErrorCode::NotFound, "no bin " + std::to_string(bin.value), Json{{"path", "/bin"}});
        }
    }
    if (!services || !services->writer()) {
        fail(ErrorCode::InvalidRequest, "Export Frame needs a host that renders frames (the app, wabcut-cli)");
    }
    if (std::find(services->formats().begin(), services->formats().end(), format) == services->formats().end()) {
        fail(ErrorCode::InvalidParams, "this host cannot write " + format, Json{{"path", "/format"}});
    }
    if (import && !services->describer()) {
        fail(ErrorCode::InvalidRequest, "this host cannot import the still into the project");
    }
    CommandOutput out;
    out.state = c.state();
    Json data{{"path", pathToUtf8(file)}, {"format", format}, {"written", false}};
    if (c.dryRun()) {
        out.data = std::move(data);
        return out;
    }
    auto written = services->writer()(render, sequence, time, file, format);
    if (!written) {
        fail(ErrorCode::EditRejected, "Export Frame failed: " + written.error().message);
    }
    data["written"] = true;
    data["width"] = written->width;
    data["height"] = written->height;
    if (import) {
        auto item = services->describer()(written->file);
        if (!item) {
            fail(ErrorCode::EditRejected, "the still was written but cannot be imported: " + item.error().message);
        }
        item->bin = bin;
        auto added = edit::addMedia(c.state(), std::move(*item));
        if (!added) {
            fail(ErrorCode::EditRejected, "the still was written but cannot be imported: " + added.error().message);
        }
        out.state = std::move(added->state);
        out.createdIds.push_back(added->id.value);
        data["media"] = added->id.value;
    }
    out.data = std::move(data);
    return out;
}

/// The Source Monitor's picture of `media` at source time `t`: a project of one sequence of the media's size and rate
/// with the frame on V1 at 0, as the Source Monitor plays it.
std::pair<ProjectState, SequenceId> sourceFrameProject(const MediaItem& media, Ticks t) {
    const VideoStreamInfo* v = media.info.video.empty() ? nullptr : &media.info.video.front();
    if (!v || v->width <= 0 || v->height <= 0) {
        fail(ErrorCode::InvalidParams, media.name + " has no picture", Json{{"path", "/media"}});
    }
    MediaItem copy = media;
    copy.markIn.reset();
    copy.markOut.reset();
    copy.subclip.reset();
    auto withMedia = edit::addMedia(ProjectState{}, copy);
    if (!withMedia) {
        fail(ErrorCode::EditRejected, withMedia.error().message);
    }
    edit::SequenceSettings settings;
    settings.name = media.name;
    settings.width = v->width;
    settings.height = v->height;
    if (v->frameRate.isValid() && v->frameRate.isExact()) {
        settings.frameRate = v->frameRate;
    }
    auto withSequence = edit::addSequence(withMedia->state, settings);
    if (!withSequence) {
        fail(ErrorCode::EditRejected, withSequence.error().message);
    }
    const Sequence& seq = *withSequence->state.findSequence(withSequence->id);
    const MediaItem& placed = *withSequence->state.findMedia(withMedia->id);
    const Ticks frame = framesToTicks(1, settings.frameRate, Rounding::Ceil).value_or(Ticks{kTicksPerSecond / 25});
    const Ticks start = media.info.isStillImage ? Ticks{} : snapToFrame(t, settings.frameRate);
    edit::SourceClip clip{withMedia->id, TimeRange{start, frame}, edit::defaultSourcePatches(seq, placed)};
    auto edited = edit::overwriteEdit(withSequence->state, withSequence->id, clip, Ticks{});
    if (!edited) {
        fail(ErrorCode::InvalidParams, "no frame of " + media.name + " there: " + edited.error().message, Json{{"path", "/time"}});
    }
    return {std::move(edited->state), withSequence->id};
}

} // namespace

void registerMonitorCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"clip.setMotion", 1, "Motion",
         "Sets a video clip's Motion - Position, Scale, Rotation, Anchor Point - in one step, as dragging the clip in the "
         "Program Monitor does (move, scale by a handle, rotate outside a corner, move the anchor point). Each value "
         "given is in the model's unit: position and anchor in pixels from the centre (sequence pixels for position, "
         "source pixels for the anchor), scale as a factor (1 = 100%), rotation in degrees clockwise. A parameter "
         "whose stopwatch is on gets a keyframe at \"time\" (the key there changes, else a new one continues the "
         "curve's shape); a constant one takes the value. Parameters left out keep their values.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The video clip.")},
                       {"time", timeParam("Where animated parameters are keyed (inside the clip). Required when one "
                                          "of the parameters given is animated; default the clip's start.")},
                       {"positionX", schema::number("Position X: the anchor's offset from the frame's centre, in "
                                                    "sequence pixels (right is positive).")},
                       {"positionY", schema::number("Position Y: the anchor's offset from the frame's centre, in "
                                                    "sequence pixels (down is positive).")},
                       {"scale", schema::number("Scale as a factor (1 = 100%).", 0.0, 100.0)},
                       {"rotation", schema::number("Rotation in degrees, clockwise.")},
                       {"anchorX", schema::number("Anchor Point X: offset from the clip frame's centre, in source "
                                                  "pixels.")},
                       {"anchorY", schema::number("Anchor Point Y: offset from the clip frame's centre, in source "
                                                  "pixels.")}},
                      {"clip"}),
         Json{{"clip", 42}, {"time", "00:00:02:00"}, {"positionX", 320.0}, {"positionY", -180.0}, {"scale", 0.5}},
         {"motion.set"},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             const Clip& clip = clipOf(c, id);
             edit::MotionChange change;
             std::string animated;
             for (const MotionParam& p : kMotionParams) {
                 if (c.has(p.key)) {
                     change.*(p.field) = c.number(p.key);
                     if (clip.properties()[p.id].isCurve() && animated.empty()) {
                         animated = p.key;
                     }
                 }
             }
             if (change.empty()) {
                 fail(ErrorCode::InvalidParams, "give at least one of positionX, positionY, scale, rotation, anchorX, anchorY");
             }
             Ticks at = clip.start();
             if (c.has("time")) {
                 at = c.time("time");
                 if (at < clip.start() || at >= clip.end()) {
                     fail(ErrorCode::InvalidParams,
                          "the time lies outside the clip (" + c.timecode(clip.start()) + " to " + c.timecode(clip.end()) + ")",
                          Json{{"path", "/time"}});
                 }
             } else if (!animated.empty()) {
                 fail(ErrorCode::InvalidParams, animated + " is animated: give the \"time\" to key it at",
                      Json{{"path", "/time"}});
             }
             const Ticks curveTime = clip.curveTimeAt(at);
             edit::EditResult result = c.take(edit::setMotion(c.state(), c.sequenceId(), id, curveTime, change));
             const Sequence* seq = result.state.findSequence(c.sequenceId());
             const auto located = seq ? seq->findClip(id) : std::nullopt;
             Json data = located ? motionJson(edit::motionAt(located->clip(), curveTime)) : Json::object();
             data["clip"] = id.value;
             data["time"] = timeJson(c, at);
             return edited(std::move(result), std::move(data));
         }});

    Json sequenceFrame = withSequence({{"time", timeParam("The frame to export (snapped to the sequence's frames).")}},
                                      {"time", "path"});
    for (auto& [key, value] : exportFrameParams()) {
        sequenceFrame["properties"][key] = std::move(value);
    }
    addBuiltin(
        registry,
        command::touchesFiles(
            {"sequence.exportFrame", 1, "Export Frame",
             "Premiere's Export Frame (the Program Monitor's camera button, Ctrl+Shift+E): renders the sequence frame at "
             "\"time\" as an export renders it (original media, the export colour transform) at the sequence's size and "
             "writes it to \"path\" as DPX (10-bit), JPEG, PNG, TIFF, Targa, BMP, GIF or OpenEXR (half float, linear). "
             "With \"importIntoProject\" the still is added to the project as a media item (one undo step); otherwise the "
             "project does not change. Needs a host that renders frames (the app, wabcut-cli); a dry run writes nothing.",
             Capability::EditProject, std::move(sequenceFrame),
             Json{{"time", "00:00:10:00"}, {"format", "png"}, {"path", "C:/Stills/Thumbnail.png"}, {"importIntoProject", true}},
             {"frame.export"},
             [](ExecutionContext& c) {
                 const Ticks at = c.time("time");
                 if (at.value < 0) {
                     fail(ErrorCode::InvalidParams, "the time is before the sequence's start", Json{{"path", "/time"}});
                 }
                 return exportFrameOf(c, c.state(), c.sequenceId(), at);
             }},
            {"path"}));

    Json sourceFrame = withSequence({{"media", mediaParam("The media item the Source Monitor shows.")},
                                     {"time", sourceTimeParam("The source frame to export (the media's time).")}},
                                    {"media", "time", "path"});
    for (auto& [key, value] : exportFrameParams()) {
        sourceFrame["properties"][key] = std::move(value);
    }
    addBuiltin(
        registry,
        command::touchesFiles(
            {"source.exportFrame", 1, "Export Frame (Source Monitor)",
             "Premiere's Export Frame from the Source Monitor: the frame of a media item at a source time, at the media's "
             "own frame size, written to \"path\" in the chosen format (as sequence.exportFrame); with "
             "\"importIntoProject\" the still joins the project (one undo step). Needs a host that renders frames.",
             Capability::EditProject, std::move(sourceFrame),
             Json{{"media", 3}, {"time", "00:00:04:12"}, {"format", "jpeg"}, {"path", "C:/Stills/Frame.jpg"}}, {},
             [](ExecutionContext& c) {
                 const MediaId id = c.media("media");
                 const MediaItem* item = c.state().findMedia(id);
                 if (!item) {
                     fail(ErrorCode::NotFound, "no media " + std::to_string(id.value), Json{{"path", "/media"}});
                 }
                 const Rate rate = sourceTimecodeRate(c.state(), *item);
                 const std::optional<Ticks> t = sourceTimeValue(c.params().at("time"), "/time", rate);
                 if (!t || t->value < 0) {
                     fail(ErrorCode::InvalidParams, "give the source time to export", Json{{"path", "/time"}});
                 }
                 auto [state, sequence] = sourceFrameProject(*item, *t);
                 return exportFrameOf(c, state, sequence, Ticks{});
             }},
            {"path"}));
}

} // namespace wabcut::command::detail
