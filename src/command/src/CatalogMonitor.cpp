// P.2 (E6): Premiere's Source and Program Monitor commands.
//   clip.setMotion        Motion (Position, Scale, Rotation, Anchor Point) set together at one time, as the Program
//                         Monitor's direct manipulation does: one undo step, keyed where the parameter is animated.
//   sequence.exportFrame  Export Frame from the Program Monitor: the sequence frame rendered as an export renders it,
//                         written as DPX, JPEG, PNG, TIFF, Targa, BMP, GIF or OpenEXR (the host's FrameExportServices),
//                         optionally imported into the project (one undo step).
//   source.exportFrame    The same from the Source Monitor: a media item's frame at a source time, at its own size.
//   P1 (#23): both take a size (fit or fill), a JPEG quality and a size cap (a YouTube thumbnail: 1280 x 720 under 2 MB);
//   sequence.exportFramesAtMarkers writes a still at each marker (or a highlight's peak), named after it, optionally
//   imported into a "Thumbnails" bin.

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

/// P1 (#23): the size and quality params every Export Frame command takes.
std::vector<std::pair<const char*, Json>> stillOptionParams() {
    return {{"width", schema::integer("Scale the frame to this width (with height; default the frame's own size). "
                                      "1280 x 720 is a YouTube thumbnail.",
                                      16, 16384)},
            {"height", schema::integer("Scale the frame to this height (with width).", 16, 16384)},
            {"fit", schema::enumeration({"fit", "fill"}, "With a size of another shape: \"fit\" keeps the whole frame with "
                                                         "black bars (default), \"fill\" crops its centre.")},
            {"quality", schema::integer("JPEG quality, 1-100 (default: the highest).", 1, 100)},
            {"maxBytes", schema::integer("JPEG: the most the file may weigh, in bytes (YouTube thumbnails: 2000000); the "
                                         "quality goes down until it fits.",
                                         10000)}};
}

/// The still options of the params (both width and height, or neither).
FrameExportServices::Options stillOptionsOf(ExecutionContext& c) {
    FrameExportServices::Options options;
    if (c.has("width") != c.has("height")) {
        fail(ErrorCode::InvalidParams, "give both width and height, or neither", Json{{"path", c.has("width") ? "/height" : "/width"}});
    }
    if (c.has("width")) {
        options.width = static_cast<int>(c.integer("width"));
        options.height = static_cast<int>(c.integer("height"));
        if (options.width < 16 || options.height < 16 || options.width > 16384 || options.height > 16384) {
            fail(ErrorCode::InvalidParams, "the still's size must be 16..16384 pixels a side", Json{{"path", "/width"}});
        }
    }
    options.fill = c.string("fit", "fit") == "fill";
    if (c.has("quality")) {
        options.quality = static_cast<int>(std::clamp<std::int64_t>(c.integer("quality"), 1, 100));
    }
    if (c.has("maxBytes")) {
        options.maxBytes = std::max<std::int64_t>(10000, c.integer("maxBytes"));
    }
    return options;
}

/// The params both Export Frame commands take besides where the frame is.
std::vector<std::pair<const char*, Json>> exportFrameParams() {
    Json path = schema::string("The file to write (UTF-8 path). The format's extension is added when it has another one; "
                               "an existing file is replaced.");
    path["minLength"] = 1;
    path["maxLength"] = 4096;
    std::vector<std::pair<const char*, Json>> params{
        {"format", schema::enumeration(stillFormatList(), "The image format (Premiere's Export Frame list). Default png.")},
        {"path", path},
        {"importIntoProject",
         schema::boolean("Premiere's \"Import into project\": the still is added to the project as a media item.", false)},
        {"bin", schema::integer("With importIntoProject: the bin it goes into (default the root bin).", 1)}};
    for (auto& option : stillOptionParams()) {
        params.push_back(std::move(option));
    }
    return params;
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
    const FrameExportServices::Options options = stillOptionsOf(c);
    CommandOutput out;
    out.state = c.state();
    Json data{{"path", pathToUtf8(file)}, {"format", format}, {"written", false}};
    if (c.dryRun()) {
        out.data = std::move(data);
        return out;
    }
    auto written = services->writer()(render, sequence, time, file, format, options);
    if (!written) {
        fail(ErrorCode::EditRejected, "Export Frame failed: " + written.error().message);
    }
    data["written"] = true;
    data["width"] = written->width;
    data["height"] = written->height;
    // P1 (#23): the file's size, the JPEG quality it took, and whether it stayed over a size cap.
    data["bytes"] = written->bytes;
    if (written->quality > 0) {
        data["quality"] = written->quality;
    }
    if (options.maxBytes > 0) {
        data["overLimit"] = written->bytes > options.maxBytes;
    }
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

/// P1 (#23): a marker's name as a file name: characters file systems refuse replaced, at most 80 bytes, never a Windows
/// device name; "" when nothing is left.
std::string stillFileName(std::string name) {
    name = name.substr(0, name.find('\n'));
    for (char& ch : name) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 32 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) {
            ch = '_';
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    while (!name.empty() && name.front() == ' ') {
        name.erase(name.begin());
    }
    if (name.size() > 80) {
        std::size_t cut = 80;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) {
            --cut; // not inside a UTF-8 character
        }
        name.resize(cut);
    }
    std::string upper = name.substr(0, name.find('.'));
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    static const std::array<std::string_view, 22> kDevices{"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                                           "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                                           "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (std::find(kDevices.begin(), kDevices.end(), upper) != kDevices.end()) {
        name.insert(name.begin(), '_');
    }
    return name;
}

/// P1 (#23): sequence.exportFramesAtMarkers - one still per marker (or per highlight's peak), named after the marker.
CommandOutput exportFramesAtMarkers(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    auto* services = findService<FrameExportServices>(c.services());
    const std::string format = c.string("format", "jpeg");
    const auto known = std::find_if(stillFormatNames().begin(), stillFormatNames().end(),
                                    [&](const StillFormatName& f) { return format == f.name; });
    if (known == stillFormatNames().end()) {
        fail(ErrorCode::InvalidParams, "unknown format " + format, Json{{"path", "/format"}});
    }
    const std::filesystem::path folder = pathFromUtf8(c.string("folder"));
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
        fail(ErrorCode::NotFound, "no folder " + c.string("folder"), Json{{"path", "/folder"}});
    }
    const std::string which = c.string("markers", "all");
    std::optional<std::int64_t> color;
    if (c.has("colorIndex")) {
        color = c.integer("colorIndex");
    }
    const bool atPeak = c.string("at", "peak") == "peak";
    const bool import = c.boolean("importIntoProject", false);
    if (!services || !services->writer()) {
        fail(ErrorCode::InvalidRequest, "Export Frame needs a host that renders frames (the app, wabcut-cli)");
    }
    if (std::find(services->formats().begin(), services->formats().end(), format) == services->formats().end()) {
        fail(ErrorCode::InvalidParams, "this host cannot write " + format, Json{{"path", "/format"}});
    }
    if (import && !services->describer()) {
        fail(ErrorCode::InvalidRequest, "this host cannot import the stills into the project");
    }
    const FrameExportServices::Options options = stillOptionsOf(c);

    // The markers to export, in time order, with where each still is taken.
    struct Pick {
        std::size_t index;
        const Marker* marker;
        Ticks at;
    };
    std::vector<Pick> picks;
    for (std::size_t i = 0; i < seq.markers.size(); ++i) {
        const Marker& m = seq.markers[i];
        if ((which == "chapters" && m.type != MarkerType::Chapter) || (which == "highlights" && !m.highlight) ||
            (color && m.colorIndex != *color)) {
            continue;
        }
        const Ticks at = atPeak && m.highlight ? m.highlight->peak : m.start;
        picks.push_back({i, &m, snapToFrame(at, seq.frameRate)});
    }
    if (picks.empty()) {
        fail(ErrorCode::NotFound,
             which == "highlights" ? "the sequence has no highlight markers" : "the sequence has no markers to export",
             Json{{"path", "/markers"}});
    }

    CommandOutput out;
    out.state = c.state();
    Json stills = Json::array();
    std::vector<std::filesystem::path> taken;
    for (const Pick& p : picks) {
        std::string base = stillFileName(p.marker->name);
        if (base.empty()) {
            base = "Marker " + std::to_string(p.index + 1);
        }
        std::filesystem::path file = folder / pathFromUtf8(base + known->extensions.front());
        for (int n = 2; std::find(taken.begin(), taken.end(), file) != taken.end() && n < 10000; ++n) {
            file = folder / pathFromUtf8(base + " (" + std::to_string(n) + ")" + known->extensions.front());
        }
        taken.push_back(file);
        Json still{{"marker", p.index}, {"name", p.marker->name}, {"time", timeJson(c, p.at)}, {"path", pathToUtf8(file)}};
        stills.push_back(std::move(still));
    }
    Json data{{"folder", pathToUtf8(folder)}, {"format", format}, {"count", stills.size()}, {"written", false}};
    if (c.dryRun()) {
        data["stills"] = std::move(stills);
        out.data = std::move(data);
        return out;
    }
    ProjectState state = c.state();
    BinId bin;
    Json media = Json::array();
    std::size_t over = 0;
    for (std::size_t i = 0; i < picks.size(); ++i) {
        // Progress (and Cancel) between stills: "Still 3 of 12 (16%)".
        c.checkpoint("Still " + std::to_string(i + 1) + " of " + std::to_string(picks.size()) + " (" +
                     std::to_string(i * 100 / picks.size()) + "%)");
        const std::filesystem::path file = pathFromUtf8(stills[i]["path"].get<std::string>());
        auto written = services->writer()(c.state(), seq.id, picks[i].at, file, format, options);
        if (!written) {
            fail(ErrorCode::EditRejected, "Export Frame failed at " + stills[i]["time"]["timecode"].get<std::string>() + ": " +
                                              written.error().message);
        }
        stills[i]["bytes"] = written->bytes;
        if (options.maxBytes > 0 && written->bytes > options.maxBytes) {
            ++over;
        }
        if (!import) {
            continue;
        }
        if (!bin.isValid()) {
            // The stills go into a "Thumbnails" bin at the top (made when there is none), all in this one undo step.
            const std::string binName = c.string("binName", "Thumbnails");
            const auto existing = std::find_if(state.bins.begin(), state.bins.end(), [&](const Bin& b) {
                return !b.parent.isValid() && b.name == binName && !b.isSearch();
            });
            if (existing != state.bins.end()) {
                bin = existing->id;
            } else {
                auto made = edit::addBin(state, binName);
                if (!made) {
                    fail(ErrorCode::EditRejected, "cannot make the bin: " + made.error().message);
                }
                state = std::move(made->state);
                bin = made->id;
            }
        }
        auto item = services->describer()(written->file);
        if (!item) {
            fail(ErrorCode::EditRejected, "the still was written but cannot be imported: " + item.error().message);
        }
        item->bin = bin;
        auto added = edit::addMedia(state, std::move(*item));
        if (!added) {
            fail(ErrorCode::EditRejected, "the still was written but cannot be imported: " + added.error().message);
        }
        state = std::move(added->state);
        out.createdIds.push_back(added->id.value);
        media.push_back(added->id.value);
    }
    data["written"] = true;
    data["stills"] = std::move(stills);
    if (options.maxBytes > 0) {
        data["overLimit"] = over;
    }
    if (import) {
        data["media"] = std::move(media);
        data["bin"] = bin.value;
        out.state = std::move(state);
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

    // P1 (#23): thumbnail candidates - a still at each marker (or a highlight's peak) in one go.
    Json folder = schema::string("The folder the stills go into (UTF-8 path; it must exist). Each still is named after its "
                                 "marker (\"Marker 3\" when it has no name; \" (2)\" for a name used twice); an existing "
                                 "file is replaced.");
    folder["minLength"] = 1;
    folder["maxLength"] = 4096;
    Json atMarkers = withSequence(
        {{"folder", folder},
         {"format", schema::enumeration(stillFormatList(), "The image format. Default jpeg.")},
         {"markers", schema::enumeration({"all", "chapters", "highlights"},
                                         "Which markers: every one (default), chapter markers, or the markers a highlight "
                                         "finder wrote.")},
         {"colorIndex", schema::integer("Only the markers of this colour (Premiere's 8: 0 green .. 7 cyan).", 0, 7)},
         {"at", schema::enumeration({"peak", "start"}, "Where the still is taken: a highlight marker's peak (default; the "
                                                       "start for other markers) or every marker's start.")},
         {"importIntoProject",
          schema::boolean("The stills are added to the project, into a bin (made when missing), in one undo step.", false)},
         {"binName", schema::string("With importIntoProject: the top-level bin they go into. Default \"Thumbnails\".")}},
        {"folder"});
    for (auto& [key, value] : stillOptionParams()) {
        atMarkers["properties"][key] = std::move(value);
    }
    addBuiltin(
        registry,
        command::touchesFiles(
            {"sequence.exportFramesAtMarkers", 1, "Export Frames at Markers",
             "Writes one still per sequence marker - all of them, the chapter markers, the highlight markers, or one colour "
             "- rendered as Export Frame renders them (original media, the export colour transform), named after the "
             "marker, into \"folder\": thumbnail candidates. A highlight marker's still is taken at its peak. With "
             "\"width\" / \"height\" (1280 x 720 for YouTube), \"quality\" and \"maxBytes\" (2000000 keeps a YouTube "
             "thumbnail under 2 MB) as sequence.exportFrame. With \"importIntoProject\" the stills join the project in "
             "a \"Thumbnails\" bin, one undo step. Needs a host that renders frames; a dry run lists the files only.",
             Capability::EditProject, std::move(atMarkers),
             Json{{"folder", "C:/Thumbnails"}, {"markers", "highlights"}, {"format", "jpeg"}, {"width", 1280},
                  {"height", 720}, {"fit", "fill"}, {"maxBytes", 2000000}},
             {},
             [](ExecutionContext& c) { return exportFramesAtMarkers(c); }},
            {"folder"}));
}

} // namespace wabcut::command::detail
