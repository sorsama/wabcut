// Built-in library commands (C.11, over the M5k ops in ProjectOps.h): bins (bin.create / rename / move / remove) and
// media metadata and subclips (media.setAttributes, media.makeSubclip, media.setSubclip), and image sequences (0.2:
// media.importImageSequence, read by the host's ImageSequenceServices), layered Photoshop files and camera clips (0.2:
// media.layeredFileInfo / importLayeredFile, media.browse / importCameraClips, read by MediaFormatServices). Library
// organisation only:
// sequences' content never changes. Source times are ticks or a timecode in the media's own rate.

#include "Catalog.h"

#include <wabcut/command/ImageSequenceServices.h>
#include <wabcut/command/MediaFormatServices.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

Json binParam(std::string description) { return schema::integer(std::move(description) + " A bin id.", 1); }

Json idsParam(std::string description) { return schema::array(schema::integer("An id.", 1), std::move(description)); }

BinId binValue(ExecutionContext& c, std::string_view key) {
    const BinId bin{static_cast<std::uint64_t>(c.integer(key))};
    if (!c.state().findBin(bin)) {
        fail(ErrorCode::NotFound, "bin " + std::to_string(bin.value) + " does not exist", Json{{"path", "/" + std::string(key)}});
    }
    return bin;
}

template <class Id>
std::vector<Id> idList(ExecutionContext& c, const char* key) {
    std::vector<Id> out;
    if (c.has(key)) {
        for (const Json& v : c.params()[key]) {
            out.push_back(Id{v.get<std::uint64_t>()});
        }
    }
    return out;
}

/// The subclip's range: "in" and "out" (exclusive) source times in the media's rate.
TimeRange rangeParam(ExecutionContext& c, const MediaItem& media) {
    const Rate rate = sourceTimecodeRate(c.state(), media);
    const auto in = sourceTimeValue(c.params()["in"], "/in", rate);
    const auto out = sourceTimeValue(c.params()["out"], "/out", rate);
    if (!in || !out || *out <= *in) {
        fail(ErrorCode::InvalidParams, "\"out\" must be after \"in\"", Json{{"path", "/out"}});
    }
    return {*in, *out - *in};
}

CommandOutput projectOutput(ProjectState state, Json data = nullptr, std::vector<std::uint64_t> created = {}) {
    CommandOutput out;
    out.state = std::move(state);
    out.data = std::move(data);
    out.createdIds = std::move(created);
    return out;
}

const MediaFormatServices::Readers& formatReaders(ExecutionContext& c) {
    const auto* services = findService<MediaFormatServices>(c.services());
    if (!services) {
        fail(ErrorCode::EditRejected, "this host cannot read media files");
    }
    return services->readers();
}

std::string pathParamValue(ExecutionContext& c, const char* key) {
    std::string path = c.string(key);
    if (path.empty() || path.size() > 4096) {
        fail(ErrorCode::InvalidParams, std::string("\"") + key + "\" must name a file", Json{{"path", std::string("/") + key}});
    }
    return path;
}

std::string fileNameOf(const std::string& utf8) {
    return pathToUtf8(pathFromUtf8(utf8).filename());
}

Json layerJson(const MediaFormatServices::Layer& l) {
    Json groups = Json::array();
    for (const std::string& g : l.groups) {
        groups.push_back(g);
    }
    return Json{{"index", l.index},         {"name", l.name},   {"visible", l.visible}, {"hasPixels", l.hasPixels},
                {"group", l.group},         {"left", l.left},   {"top", l.top},         {"right", l.right},
                {"bottom", l.bottom},       {"blendMode", l.blendMode}, {"groups", std::move(groups)}};
}

void registerMediaFormatCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        command::touchesFiles(
            {"media.layeredFileInfo", 1, "Layered File Info",
             "What Premiere's Import Layered File dialog lists for a Photoshop file (.psd / .psb): the document's size, "
             "bit depth (8, 16 or 32) and colour mode (rgb, cmyk - converted to RGB on import - or grayscale) and its "
             "layers bottom to top - index, name, visibility (its own eye and its groups'), whether it has pixels or is a "
             "group, its bounds in document pixels, blend mode and the groups holding it.",
             Capability::Read, withSequence({{"path", schema::string("The Photoshop file (UTF-8 path).")}}, {"path"}),
             Json{{"path", "D:/Design/Lower third.psd"}}, {},
             [](ExecutionContext& c) {
                 const auto& readers = formatReaders(c);
                 const std::string path = pathParamValue(c, "path");
                 if (!readers.readLayers) {
                     fail(ErrorCode::EditRejected, "this host cannot read layered files");
                 }
                 const MediaFormatServices::LayeredDocument doc = c.take(readers.readLayers(pathFromUtf8(path)));
                 Json layers = Json::array();
                 for (const auto& l : doc.layers) {
                     layers.push_back(layerJson(l));
                 }
                 CommandOutput out;
                 out.state = c.state();
                 out.data = Json{{"width", doc.width},          {"height", doc.height},
                                 {"depth", doc.depth},          {"colorMode", doc.colorMode},
                                 {"layers", std::move(layers)}};
                 return out;
             }},
            {"path"}));
    addBuiltin(
        registry,
        command::touchesFiles(
            {"media.importLayeredFile", 1, "Import Layered File",
             "Premiere's Import Layered File for a Photoshop file (.psd / .psb). \"as\": \"mergeAll\" (default: one "
             "picture of every visible layer), \"merged\" (one picture of the \"layers\" chosen), \"layers\" (Individual "
             "Layers: one item per chosen layer, named \"<layer>/<file>\", in a new bin named after the file) or "
             "\"sequence\" (the same items, plus a sequence of the document's size with one layer per video track, bottom "
             "layer on V1, each \"duration\" seconds long, default 5). \"footage\": \"document\" (default: each layer on "
             "the document's canvas, where it sits) or \"layer\" (Layer Size: cropped to the layer's pixels; in a "
             "sequence each clip is placed where the layer sits). \"layers\": layer indices from media.layeredFileInfo "
             "(default every visible layer). The items read the file each time they are opened, so saving it in "
             "Photoshop updates them. One undo step.",
             Capability::EditProject,
             withSequence({{"path", schema::string("The Photoshop file (UTF-8 path).")},
                           {"as", schema::enumeration({"mergeAll", "merged", "layers", "sequence"}, "How the layers import.")},
                           {"layers", schema::array(schema::integer("A layer index.", 0), "The layers (bottom = 0).")},
                           {"footage", schema::enumeration({"document", "layer"}, "Footage Dimensions.")},
                           {"duration", schema::number("Sequence: each layer's length in seconds.", 0.04, 86400.0)},
                           {"bin", binParam("The bin it goes in.")}},
                          {"path"}),
             Json{{"path", "D:/Design/Lower third.psd"}, {"as", "sequence"}, {"footage", "layer"}}, {},
             [](ExecutionContext& c) {
                 const auto& readers = formatReaders(c);
                 if (!readers.readLayers || !readers.describeLayered) {
                     fail(ErrorCode::EditRejected, "this host cannot read layered files");
                 }
                 const std::string path = pathParamValue(c, "path");
                 const std::string as = c.has("as") ? c.string("as") : std::string("mergeAll");
                 const bool documentSize = !c.has("footage") || c.string("footage") == "document";
                 const BinId parentBin = c.has("bin") ? binValue(c, "bin") : BinId{};
                 const MediaFormatServices::LayeredDocument doc = c.take(readers.readLayers(pathFromUtf8(path)));
                 std::vector<int> chosen;
                 if (c.has("layers")) {
                     for (const Json& v : c.params()["layers"]) {
                         const int index = v.get<int>();
                         if (index < 0 || index >= static_cast<int>(doc.layers.size())) {
                             fail(ErrorCode::InvalidParams, "the file has no layer " + std::to_string(index), Json{{"path", "/layers"}});
                         }
                         chosen.push_back(index);
                     }
                 }
                 const std::string fileName = fileNameOf(path);
                 ProjectState state = c.state();
                 std::vector<std::uint64_t> created;
                 Json items = Json::array();
                 if (as == "mergeAll" || as == "merged") {
                     LayeredFileSpec spec{pathFromUtf8(path), as == "merged" ? chosen : std::vector<int>{}, true};
                     MediaItem item = c.take(readers.describeLayered(spec, fileName));
                     item.bin = parentBin;
                     auto added = c.take(edit::addMedia(state, std::move(item)));
                     state = std::move(added.state);
                     created.push_back(added.id.value);
                     items.push_back(added.id.value);
                     return projectOutput(std::move(state), Json{{"media", added.id.value}, {"items", std::move(items)}}, created);
                 }
                 // Individual layers (and the sequence): the visible layers with pixels among those chosen.
                 std::vector<const MediaFormatServices::Layer*> layers;
                 for (const auto& l : doc.layers) {
                     const bool wanted = chosen.empty() ? true : std::ranges::find(chosen, l.index) != chosen.end();
                     if (wanted && l.visible && l.hasPixels && !l.group) {
                         layers.push_back(&l);
                     }
                 }
                 if (layers.empty()) {
                     fail(ErrorCode::EditRejected, fileName + " has no visible layer with pixels to import");
                 }
                 auto bin = c.take(edit::addBin(state, pathToUtf8(pathFromUtf8(path).stem()), parentBin));
                 state = std::move(bin.state);
                 created.push_back(bin.id.value);
                 std::vector<MediaId> media;
                 for (const auto* l : layers) {
                     LayeredFileSpec spec{pathFromUtf8(path), {l->index}, documentSize};
                     MediaItem item = c.take(readers.describeLayered(spec, l->name + "/" + fileName));
                     item.bin = bin.id;
                     auto added = c.take(edit::addMedia(state, std::move(item)));
                     state = std::move(added.state);
                     created.push_back(added.id.value);
                     items.push_back(added.id.value);
                     media.push_back(added.id);
                 }
                 Json data{{"bin", bin.id.value}, {"items", std::move(items)}};
                 if (as == "sequence") {
                     edit::SequenceSettings settings;
                     settings.name = pathToUtf8(pathFromUtf8(path).stem());
                     settings.width = std::clamp(doc.width, 16, 16384);
                     settings.height = std::clamp(doc.height, 16, 16384);
                     settings.videoTracks = static_cast<std::int32_t>(std::min<std::size_t>(media.size(), 99));
                     settings.audioTracks = 1;
                     settings.bin = bin.id;
                     auto seq = c.take(edit::addSequence(state, settings));
                     state = std::move(seq.state);
                     created.push_back(seq.id.value);
                     const double seconds = c.has("duration") ? c.params()["duration"].get<double>() : 5.0;
                     const Ticks length{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
                     for (std::size_t i = 0; i < media.size() && i < 99; ++i) {
                         const Sequence& s = *state.findSequence(seq.id);
                         edit::SourceClip source;
                         source.media = media[i];
                         source.sourceRange = {Ticks{}, length};
                         source.patches = {edit::SourcePatch{StreamRef{StreamKind::Video, 0}, s.videoTracks[i]->id, {}}};
                         edit::EditResult placed = c.take(edit::overwriteEdit(state, seq.id, source, Ticks{}));
                         state = std::move(placed.state);
                         if (!documentSize) { // Layer Size: the clip where its layer sits in the document
                             const auto* l = layers[i];
                             const Sequence& now = *state.findSequence(seq.id);
                             const ClipId clip = now.videoTracks[i]->clips.front().id;
                             state = c.take(edit::setParameter(state, seq.id, clip, edit::ParamRef::of(ParamId::PositionX),
                                                               ParamValue((l->left + l->right) / 2.0 - doc.width / 2.0)))
                                         .state;
                             state = c.take(edit::setParameter(state, seq.id, clip, edit::ParamRef::of(ParamId::PositionY),
                                                               ParamValue((l->top + l->bottom) / 2.0 - doc.height / 2.0)))
                                         .state;
                         }
                     }
                     data["sequence"] = seq.id.value;
                 }
                 return projectOutput(std::move(state), std::move(data), created);
             }},
            {"path"}));
    addBuiltin(
        registry,
        command::touchesFiles(
            {"media.browse", 1, "Media Browser",
             "Premiere's Media Browser listing of a folder: a camera card (AVCHD, Canon XF, P2, XDCAM EX / HD, XAVC) or a "
             "folder inside one lists the card's clips instead of its folders, a recording split into several files (a "
             "spanned clip, GoPro chapters) as one clip; any other folder its subfolders and media files. Each entry: "
             "folder or clip, name, path (a clip's first file), its files, the card, P2's audio channel files, bytes.",
             Capability::Read, withSequence({{"folder", schema::string("The folder (UTF-8 path).")}}, {"folder"}),
             Json{{"folder", "E:/"}}, {},
             [](ExecutionContext& c) {
                 const auto& readers = formatReaders(c);
                 if (!readers.browse) {
                     fail(ErrorCode::EditRejected, "this host cannot browse folders");
                 }
                 const std::string folder = pathParamValue(c, "folder");
                 const std::vector<MediaFormatServices::Entry> entries = c.take(readers.browse(pathFromUtf8(folder)));
                 Json list = Json::array();
                 for (const auto& e : entries) {
                     Json files = Json::array();
                     for (const auto& f : e.files) {
                         files.push_back(pathToUtf8(f));
                     }
                     list.push_back(Json{{"folder", e.folder}, {"name", e.name}, {"path", pathToUtf8(e.path)},
                                         {"card", e.card}, {"files", std::move(files)}, {"audioChannels", e.audioChannels},
                                         {"bytes", e.bytes}, {"spanned", e.files.size() > 1}});
                 }
                 CommandOutput out;
                 out.state = c.state();
                 out.data = Json{{"entries", std::move(list)}};
                 return out;
             }},
            {"folder"}));
    addBuiltin(
        registry,
        command::touchesFiles(
            {"media.importCameraClips", 1, "Import from Media Browser",
             "Imports clips as the Media Browser lists them (media.browse): each \"files\" entry is a clip's first file; "
             "a spanned clip imports as ONE item playing its parts in turn, a P2 clip as a merged clip of its picture and "
             "its audio channel files (Merge Clips). Into \"bin\" (default the root). \"copyTo\": a folder - "
             "Premiere's Ingest (Copy): each clip's files are copied into <copyTo>/<clip name>/ first and the copies "
             "imported, so the card can be taken out. Clip markers the host reads from the files (the app and the CLI: a "
             "recording's chapter markers, as File > Import adds them) come in with the items; \"markers\" counts them. "
             "One undo step.",
             Capability::EditProject,
             withSequence({{"files", schema::array(schema::string("A clip's first file (UTF-8 path)."), "The clips.", 1)},
                           {"bin", binParam("The bin they go in.")},
                           {"copyTo", schema::string("Ingest: copy the clips' files into this folder first.")}},
                          {"files"}),
             Json{{"files", Json::array({"E:/DCIM/100GOPRO/GX010042.MP4"})}}, {},
             [](ExecutionContext& c) {
                 const auto& readers = formatReaders(c);
                 if (!readers.describeClip) {
                     fail(ErrorCode::EditRejected, "this host cannot import camera clips");
                 }
                 const BinId bin = c.has("bin") ? binValue(c, "bin") : BinId{};
                 const std::string copyTo = c.has("copyTo") ? c.string("copyTo") : std::string{};
                 if (!copyTo.empty() && (copyTo.size() > 4096 || !readers.ingestClip)) {
                     fail(ErrorCode::EditRejected, "this host cannot copy camera clips");
                 }
                 ProjectState state = c.state();
                 std::vector<std::uint64_t> created;
                 Json imported = Json::array();
                 std::size_t markers = 0; // P0 round: the clip markers the items came in with (chapter markers)
                 for (const Json& f : c.params()["files"]) {
                     const std::string file = f.get<std::string>();
                     if (file.empty() || file.size() > 4096) {
                         fail(ErrorCode::InvalidParams, "a clip's file must be named", Json{{"path", "/files"}});
                     }
                     std::vector<MediaItem> items =
                         c.take(copyTo.empty() ? readers.describeClip(pathFromUtf8(file))
                                               : readers.ingestClip(pathFromUtf8(file), pathFromUtf8(copyTo)));
                     std::vector<MediaId> ids;
                     std::size_t clipMarkers = 0;
                     for (MediaItem& item : items) {
                         item.bin = bin;
                         clipMarkers += item.markers.size();
                         auto added = c.take(edit::addMedia(state, std::move(item)));
                         state = std::move(added.state);
                         created.push_back(added.id.value);
                         ids.push_back(added.id);
                     }
                     Json entry{{"file", file}, {"media", ids.empty() ? 0 : ids.front().value}};
                     if (clipMarkers > 0) {
                         entry["markers"] = clipMarkers;
                         markers += clipMarkers;
                     }
                     if (ids.size() > 1) { // P2: the picture and its audio channels as one merged clip
                         edit::MergeSpec spec;
                         spec.name = state.findMedia(ids.front())->name;
                         spec.bin = bin;
                         spec.video = ids.front();
                         spec.audio.assign(ids.begin() + 1, ids.end());
                         spec.sync = MulticamSync::InPoints;
                         auto merged = c.take(edit::mergeClips(state, spec));
                         state = std::move(merged.state);
                         created.push_back(merged.id.value);
                         entry["merged"] = merged.id.value;
                     }
                     imported.push_back(std::move(entry));
                 }
                 return projectOutput(std::move(state), Json{{"imported", std::move(imported)}, {"markers", markers}}, created);
             }},
            {"files", "copyTo"}));
}

} // namespace

void registerLibraryCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"bin.create", 1, "New Bin",
                "Adds a bin (in \"parent\", default the project's root). Returns its id.", Capability::EditProject,
                withSequence({{"name", schema::string("The bin's name.")}, {"parent", binParam("The bin it goes in.")}},
                             {"name"}),
                Json{{"name", "Interviews"}}, {},
                [](ExecutionContext& c) {
                    const BinId parent = c.has("parent") ? binValue(c, "parent") : BinId{};
                    auto created = c.take(edit::addBin(c.state(), c.string("name"), parent));
                    return projectOutput(std::move(created.state), Json{{"bin", created.id.value}}, {created.id.value});
                }});
    addBuiltin(registry,
               {"bin.rename", 1, "Rename Bin", "Renames a bin (the name may not be empty).", Capability::EditProject,
                withSequence({{"bin", binParam("The bin.")}, {"name", schema::string("The new name (not empty).")}},
                             {"bin", "name"}),
                Json{{"bin", 7}, {"name", "Selects"}}, {},
                [](ExecutionContext& c) {
                    return projectOutput(c.take(edit::renameBin(c.state(), binValue(c, "bin"), c.string("name"))));
                }});
    addBuiltin(registry,
               {"bin.move", 1, "Move to Bin",
                "Moves bins, media items and sequences into \"target\" (absent: the project's root). A bin cannot go into "
                "itself or one of its sub-bins.",
                Capability::EditProject,
                withSequence({{"target", binParam("The bin they go in.")},
                              {"bins", idsParam("Bins to move.")},
                              {"media", idsParam("Media items to move.")},
                              {"sequences", idsParam("Sequences to move.")}}),
                Json{{"target", 7}, {"media", Json::array({3, 4})}}, {},
                [](ExecutionContext& c) {
                    const BinId target = c.has("target") ? binValue(c, "target") : BinId{};
                    const auto bins = idList<BinId>(c, "bins");
                    const auto media = idList<MediaId>(c, "media");
                    const auto sequences = idList<SequenceId>(c, "sequences");
                    if (bins.empty() && media.empty() && sequences.empty()) {
                        fail(ErrorCode::InvalidParams, "name bins, media or sequences to move");
                    }
                    return projectOutput(c.take(edit::moveToBin(c.state(), target, bins, media, sequences)));
                }});
    addBuiltin(registry,
               {"bin.remove", 1, "Delete Bin",
                "Deletes bins with everything in them (sub-bins, media items, sequences). Refused while media inside is "
                "used by a sequence that is not deleted with it.",
                Capability::EditProject,
                withSequence({{"bins", schema::array(schema::integer("A bin id.", 1), "The bins.", 1)}}, {"bins"}),
                Json{{"bins", Json::array({7})}}, {},
                [](ExecutionContext& c) {
                    return projectOutput(c.take(edit::removeBins(c.state(), idList<BinId>(c, "bins"))));
                }});
    addBuiltin(
        registry,
        {"media.setAttributes", 1, "Media Attributes",
         "Library metadata of media items: name (new clips take it; existing clips keep theirs), label colour, comment, "
         "description, poster frame (a source time, or null to clear it) and (0.2) Premiere's clip metadata: scene, shot, "
         "take, log note, client, good, tape name, status and the camera fields (camera angle, label and model, shot date "
         "and location). Fields not given stay. One undo step for any number of items.",
         Capability::EditProject,
         withSequence({{"media", schema::array(mediaParam("A media item."), "The media items.", 1)},
                       {"name", schema::string("The name (not empty).")},
                       {"colorIndex", schema::integer("Label colour, 0..15.", 0, 15)},
                       {"comment", schema::string("Comment.")},
                       {"description", schema::string("Description.")},
                       {"posterFrame", sourceTimeParam("Poster frame (media time); null clears it.")},
                       {"scene", schema::string("Scene.")},
                       {"shot", schema::string("Shot.")},
                       {"take", schema::string("Take.")},
                       {"logNote", schema::string("Log Note.")},
                       {"client", schema::string("Client.")},
                       {"good", schema::boolean("Good (a circled take).")},
                       {"tapeName", schema::string("Tape Name (the reel name; empty clears it).")},
                       {"status", schema::string("Status.")},
                       {"cameraAngle", schema::string("Camera Angle.")},
                       {"cameraLabel", schema::string("Camera Label.")},
                       {"cameraModel", schema::string("Camera Model.")},
                       {"shotDate", schema::string("Shot Date (ISO-8601 text).")},
                       {"shotLocation", schema::string("Shot Location.")}},
                      {"media"}),
         Json{{"media", Json::array({3})}, {"colorIndex", 4}, {"comment", "Best take"}}, {},
         [](ExecutionContext& c) {
             const std::vector<MediaId> media = c.mediaList("media");
             edit::MediaAttributeChange change;
             if (c.has("name")) {
                 change.name = c.string("name");
             }
             if (c.has("colorIndex")) {
                 change.colorIndex = static_cast<std::int32_t>(c.integer("colorIndex"));
             }
             if (c.has("comment")) {
                 change.comment = c.string("comment");
             }
             if (c.has("description")) {
                 change.description = c.string("description");
             }
             for (const auto& [key, field] : {std::pair{"scene", &change.scene}, std::pair{"shot", &change.shot},
                                              std::pair{"take", &change.take}, std::pair{"logNote", &change.logNote},
                                              std::pair{"client", &change.client}, std::pair{"tapeName", &change.tapeName},
                                              std::pair{"status", &change.status}, std::pair{"cameraAngle", &change.cameraAngle},
                                              std::pair{"cameraLabel", &change.cameraLabel},
                                              std::pair{"cameraModel", &change.cameraModel},
                                              std::pair{"shotDate", &change.shotDate},
                                              std::pair{"shotLocation", &change.shotLocation}}) {
                 if (c.has(key)) {
                     *field = c.string(key);
                 }
             }
             if (c.has("good")) {
                 change.good = c.boolean("good", false);
             }
             if (c.params().contains("posterFrame")) {
                 const MediaItem* first = c.state().findMedia(media.front());
                 change.posterFrame = sourceTimeValue(c.params()["posterFrame"], "/posterFrame",
                                                      sourceTimecodeRate(c.state(), *first));
             }
             return projectOutput(c.take(edit::setMediaAttributes(c.state(), media, change)));
         }});
    addBuiltin(registry,
               {"media.makeSubclip", 1, "Make Subclip",
                "A new media item playing \"in\"..\"out\" (exclusive) of a media item's file, named \"name\", in \"bin\" "
                "(default the root). Restricted (default) keeps trims inside that range. Returns its id.",
                Capability::EditProject,
                withSequence({{"media", mediaParam("The master item.")},
                              {"in", sourceTimeParam("Start (media time).")},
                              {"out", sourceTimeParam("End, exclusive (media time).")},
                              {"name", schema::string("The subclip's name.")},
                              {"restricted", schema::boolean("Keep trims inside the range (default true).")},
                              {"bin", binParam("The bin it goes in.")}},
                             {"media", "in", "out", "name"}),
                Json{{"media", 3}, {"in", "00:00:02:00"}, {"out", "00:00:05:00"}, {"name", "Laugh"}}, {},
                [](ExecutionContext& c) {
                    const MediaId master = c.media("media");
                    const TimeRange range = rangeParam(c, *c.state().findMedia(master));
                    const BinId bin = c.has("bin") ? binValue(c, "bin") : BinId{};
                    auto created = c.take(edit::makeSubclip(c.state(), master, range, c.string("name"),
                                                            c.boolean("restricted", true), bin));
                    return projectOutput(std::move(created.state), Json{{"media", created.id.value}}, {created.id.value});
                }});
    addBuiltin(registry,
               {"media.setSubclip", 1, "Edit Subclip",
                "Changes a subclip's range and restriction (Premiere's Edit Subclip). Restricting is refused while a clip "
                "of it uses media outside the new range.",
                Capability::EditProject,
                withSequence({{"media", mediaParam("The subclip.")},
                              {"in", sourceTimeParam("Start (media time).")},
                              {"out", sourceTimeParam("End, exclusive (media time).")},
                              {"restricted", schema::boolean("Keep trims inside the range (default true).")}},
                             {"media", "in", "out"}),
                Json{{"media", 12}, {"in", "00:00:02:00"}, {"out", "00:00:06:00"}}, {},
                [](ExecutionContext& c) {
                    const MediaId subclip = c.media("media");
                    const TimeRange range = rangeParam(c, *c.state().findMedia(subclip));
                    return projectOutput(c.take(edit::setSubclip(c.state(), subclip, range, c.boolean("restricted", true))));
                }});
    addBuiltin(
        registry,
        command::touchesFiles(
            {"media.importImageSequence", 1, "Import Image Sequence",
             "Premiere's Import with \"Image Sequence\" ticked: the numbered stills starting at \"path\" (PNG, JPEG, TIFF, "
             "DPX, Targa, OpenEXR or BMP; that file and the ones numbered after it without a gap, e.g. shot_0001.png, "
             "shot_0002.png, ...) become one video media item playing one still per frame at \"frameRate\" (default "
             "29.97 fps, Premiere's Indeterminate Media Timebase), named after its first file, in \"bin\" (default the "
             "root). Returns its id, the frame count and the rate.",
             Capability::EditProject,
             withSequence({{"path", schema::string("The first still of the sequence (UTF-8 path).")},
                           {"frameRate", schema::number("Frames per second (23.976, 24, 25, 29.97, 30, 50, 59.94, 60, ...).",
                                                        0.01, kMaxImageSequenceRate)},
                           {"bin", binParam("The bin it goes in.")}},
                          {"path"}),
             Json{{"path", "D:/Renders/shot_0001.png"}, {"frameRate", 24}}, {},
             [](ExecutionContext& c) {
                 const auto* reader = findService<ImageSequenceServices>(c.services());
                 if (!reader) {
                     fail(ErrorCode::EditRejected, "this host cannot read image sequences");
                 }
                 const std::string path = c.string("path");
                 if (path.empty() || path.size() > 4096) {
                     fail(ErrorCode::InvalidParams, "\"path\" must name a file", Json{{"path", "/path"}});
                 }
                 const Rate rate = c.has("frameRate") ? imageSequenceRate(c.params()["frameRate"].get<double>())
                                                      : kDefaultImageSequenceRate;
                 const BinId bin = c.has("bin") ? binValue(c, "bin") : BinId{};
                 MediaItem item = c.take(reader->describe(pathFromUtf8(path), rate));
                 if (item.info.video.size() != 1) {
                     fail(ErrorCode::EditRejected, path + " is not an image sequence");
                 }
                 item.bin = bin;
                 const Rate itemRate = item.info.video.front().frameRate;
                 const std::int64_t frames = item.info.video.front().frameCount;
                 auto created = c.take(edit::addMedia(c.state(), std::move(item)));
                 return projectOutput(std::move(created.state),
                                      Json{{"media", created.id.value},
                                           {"frames", frames},
                                           {"frameRate", Json{{"num", itemRate.num}, {"den", itemRate.den}}}},
                                      {created.id.value});
             }},
            {"path"}));
    registerMediaFormatCommands(registry); // 0.2: layered files, the Media Browser
}

} // namespace wabcut::command::detail

namespace wabcut::command {

Rate imageSequenceRate(double fps) {
    if (!(fps > 0.0) || fps > kMaxImageSequenceRate) {
        return kDefaultImageSequenceRate;
    }
    for (const int whole : {24, 30, 48, 60, 120}) {
        if (std::abs(fps - whole * 1000.0 / 1001.0) < 0.005) {
            return Rate{whole * 1000, 1001};
        }
    }
    if (std::abs(fps - std::round(fps)) < 1e-6) {
        return Rate{static_cast<std::int32_t>(std::round(fps)), 1};
    }
    return Rate{static_cast<std::int32_t>(std::llround(fps * 1000.0)), 1000}.normalized();
}

} // namespace wabcut::command
