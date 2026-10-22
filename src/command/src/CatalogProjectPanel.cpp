// Project panel commands (0.2, P.3: Premiere's Project panel and Metadata panel): Search Bins (bin.createSearch /
// bin.setSearch), the Freeform view (freeform.*), XMP (media.readXmp / media.fileMetadata) and bringing
// items in from another project (project.importItems). Model: ProjectOps.h, Merge.h; files: interchange/Xmp.h.
// Writing XMP sidecars stays in the app (Library.writeXmp): it writes next to the project's media, paths no parameter
// names, so a host that confines file commands to folders (MCP --allow-files) could not check it.

#include "Catalog.h"

#include <wabcut/interchange/Xmp.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace wabcut::command::detail {

namespace {

Json boundedText(std::string description, std::size_t maxBytes, bool required = false) {
    Json s = schema::string(std::move(description));
    s["maxLength"] = static_cast<std::int64_t>(maxBytes);
    if (required) {
        s["minLength"] = 1;
    }
    return s;
}

Json binIdParam(std::string description) { return schema::integer(std::move(description) + " A bin id.", 1); }

/// The "bin" param: a bin of the project; absent (or 0) = the root.
BinId binOf(ExecutionContext& c, std::string_view key) {
    if (!c.has(key) || c.integer(key) == 0) {
        return BinId{};
    }
    const BinId bin{static_cast<std::uint64_t>(c.integer(key))};
    if (!c.state().findBin(bin)) {
        fail(ErrorCode::NotFound, "bin " + std::to_string(bin.value) + " does not exist", Json{{"path", "/" + std::string(key)}});
    }
    return bin;
}

CommandOutput stateOutput(ProjectState state, Json data = nullptr, std::vector<std::uint64_t> created = {}) {
    CommandOutput out;
    out.state = std::move(state);
    out.data = std::move(data);
    out.createdIds = std::move(created);
    return out;
}

CommandOutput readOutput(ExecutionContext& c, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    return out;
}

// ---- Search Bins ----

Json searchParams() {
    std::vector<std::string> fields{""};
    for (const std::string_view f : kBinSearchFields) {
        fields.emplace_back(f);
    }
    std::vector<std::string> types{""};
    for (const std::string_view t : kBinSearchTypes) {
        types.emplace_back(t);
    }
    return schema::object(
        {{"text", boundedText("What to find (case-insensitive); empty finds everything.", BinSearch::kMaxTextBytes)},
         {"field", schema::enumeration(fields, "Where to look: \"\" (all: name, comment, description, clip metadata, tape "
                                               "name, path) or one property.")},
         {"type", schema::enumeration(types, "Only this kind of item (\"\" = any).")},
         {"label", schema::integer("Only this label colour (0..15; -1 = any).", -1, 15)},
         {"good", schema::boolean("Only media marked Good.")},
         {"offline", schema::boolean("Only offline media.")},
         {"generated", schema::boolean("Only AI-generated media.")},
         {"transcribed", schema::boolean("Only transcribed media.")},
         {"proxy", schema::boolean("Only media with a proxy.")},
         {"unused", schema::boolean("Only media no sequence uses.")},
         {"duplicates", schema::boolean("Only media that shares its file with another item.")},
         {"from", boundedText("File date from (YYYY-MM-DD, inclusive).", 10)},
         {"to", boundedText("File date to (YYYY-MM-DD, inclusive).", 10)}},
        {}, "The query.");
}

BinSearch searchValue(const Json& j) {
    BinSearch s;
    if (!j.is_object()) {
        return s;
    }
    const auto text = [&](const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string(); };
    const auto flag = [&](const char* key) { return j.contains(key) && j[key].is_boolean() && j[key].get<bool>(); };
    s.text = text("text");
    s.field = text("field");
    s.type = text("type");
    s.label = j.contains("label") && j["label"].is_number_integer() ? static_cast<std::int32_t>(j["label"].get<std::int64_t>()) : -1;
    s.good = flag("good");
    s.offline = flag("offline");
    s.generated = flag("generated");
    s.transcribed = flag("transcribed");
    s.proxy = flag("proxy");
    s.unused = flag("unused");
    s.duplicates = flag("duplicates");
    s.from = text("from");
    s.to = text("to");
    if (const auto problem = binSearchProblem(s)) {
        fail(ErrorCode::InvalidParams, *problem, Json{{"path", "/search"}});
    }
    return s;
}

// ---- Freeform ----

std::string_view kindName(FreeformPlacement::Kind kind) {
    switch (kind) {
    case FreeformPlacement::Kind::Media:
        return "media";
    case FreeformPlacement::Kind::Sequence:
        return "sequence";
    case FreeformPlacement::Kind::Bin:
        return "bin";
    }
    return "media";
}

Json arrangementJson(const FreeformArrangement& a) {
    Json items = Json::array();
    for (const FreeformPlacement& p : a.items) {
        items.push_back(Json{{"kind", std::string(kindName(p.kind))}, {"id", p.id}, {"x", p.x}, {"y", p.y}, {"size", p.size}});
    }
    return Json{{"scale", a.scale}, {"items", std::move(items)}};
}

Json placementsParam(std::string description) {
    return schema::array(schema::object({{"kind", schema::enumeration({"media", "sequence", "bin"}, "What it is.")},
                                         {"id", schema::integer("Its id.", 1)},
                                         {"x", schema::number("Left edge (view units).", -kFreeformMaxCoordinate, kFreeformMaxCoordinate)},
                                         {"y", schema::number("Top edge (view units).", -kFreeformMaxCoordinate, kFreeformMaxCoordinate)},
                                         {"size", schema::number("Its own size (0.25 .. 4, default 1).", kFreeformMinSize, kFreeformMaxSize)}},
                                        {"kind", "id"}),
                         std::move(description));
}

std::vector<FreeformPlacement> placementsValue(const Json& list) {
    std::vector<FreeformPlacement> out;
    if (!list.is_array()) {
        return out;
    }
    for (const Json& j : list) {
        FreeformPlacement p;
        const std::string kind = j.value("kind", std::string("media"));
        p.kind = kind == "sequence" ? FreeformPlacement::Kind::Sequence
                 : kind == "bin"    ? FreeformPlacement::Kind::Bin
                                    : FreeformPlacement::Kind::Media;
        p.id = j.value("id", std::uint64_t{0});
        p.x = j.value("x", 0.0);
        p.y = j.value("y", 0.0);
        p.size = j.value("size", 1.0);
        out.push_back(p);
    }
    return out;
}

FreeformArrangement currentArrangement(const ProjectState& state, BinId bin) {
    const FreeformView* view = state.findFreeform(bin);
    return view ? view->current : FreeformArrangement{};
}

// ---- Media files ----

const MediaItem& mediaWithFile(ExecutionContext& c, MediaId id) {
    const MediaItem* item = c.state().findMedia(id);
    if (item->isGenerator() || item->locator.path.empty()) {
        fail(ErrorCode::EditRejected, "'" + item->name + "' has no file");
    }
    return *item;
}

/// Another project's items with their files found where that project is (absolute path, else its relative path).
ProjectState resolvedSource(ProjectState source, const std::filesystem::path& file) {
    for (MediaPtr& m : source.media) {
        if (m->isGenerator()) {
            continue;
        }
        MediaItem item = *m;
        const auto found = resolveMediaPath(item.locator, file);
        std::error_code ec;
        bool online = found.has_value();
        if (found && item.locator.fingerprint.isKnown()) {
            const auto fingerprint = fingerprintFile(*found);
            online = fingerprint && fingerprint->sameContentAs(item.locator.fingerprint);
        }
        if (online) {
            const std::filesystem::path absolute = std::filesystem::absolute(*found, ec);
            item.locator.path = pathToUtf8((ec ? *found : absolute).lexically_normal());
        }
        item.locator.offline = !online || item.madeOffline;
        item.locator.relativePath.clear(); // made again for this project when it is saved
        m = std::make_shared<const MediaItem>(std::move(item));
    }
    return source;
}

} // namespace

void registerProjectPanelCommands(CommandRegistry& registry) {
    // ---- Search Bins ----
    addBuiltin(registry,
               {"bin.createSearch", 1, "Create Search Bin",
                "Premiere's Create Search Bin: a bin named \"name\" in \"parent\" (default the root) that lists, live, the "
                "bins, sequences and media items of the whole project its \"search\" finds. It holds nothing itself. "
                "Returns its id.",
                Capability::EditProject,
                withSequence({{"name", boundedText("The search bin's name.", BinSearch::kMaxTextBytes, true)},
                              {"search", searchParams()},
                              {"parent", binIdParam("The bin it goes in.")}},
                             {"name", "search"}),
                Json{{"name", "Good takes"}, {"search", Json{{"good", true}}}}, {},
                [](ExecutionContext& c) {
                    auto created = c.take(edit::addSearchBin(c.state(), c.string("name"), searchValue(c.params()["search"]),
                                                             binOf(c, "parent")));
                    return stateOutput(std::move(created.state), Json{{"bin", created.id.value}}, {created.id.value});
                }});
    addBuiltin(registry,
               {"bin.setSearch", 1, "Edit Search Bin", "Replaces the query of a search bin.", Capability::EditProject,
                withSequence({{"bin", binIdParam("The search bin.")}, {"search", searchParams()}}, {"bin", "search"}),
                Json{{"bin", 12}, {"search", Json{{"text", "interview"}, {"field", "scene"}}}}, {},
                [](ExecutionContext& c) {
                    return stateOutput(c.take(edit::setBinSearch(c.state(), binOf(c, "bin"), searchValue(c.params()["search"]))));
                }});

    // ---- Freeform view ----
    addBuiltin(registry,
               {"freeform.get", 1, "Freeform View",
                "A bin's Freeform view: its clip size, where each item is (back to front) and its saved layouts.",
                Capability::Read, withSequence({{"bin", binIdParam("The bin (absent: the root).")}}), Json::object(), {},
                [](ExecutionContext& c) {
                    const BinId bin = binOf(c, "bin");
                    const FreeformView* view = c.state().findFreeform(bin);
                    Json layouts = Json::array();
                    if (view) {
                        for (const FreeformLayout& l : view->layouts) {
                            layouts.push_back(Json{{"name", l.name}, {"arrangement", arrangementJson(l.arrangement)}});
                        }
                    }
                    return readOutput(c, Json{{"bin", bin.value},
                                              {"arrangement", arrangementJson(currentArrangement(c.state(), bin))},
                                              {"layouts", std::move(layouts)}});
                }});
    addBuiltin(registry,
               {"freeform.set", 1, "Arrange in Freeform View",
                "Places items of a bin in its Freeform view (Premiere's Freeform view: thumbnails anywhere, overlapping "
                "ones stacked; later items over earlier ones). \"items\" replaces the arrangement unless \"merge\" (default "
                "true) keeps the other items where they are, moving the named ones to the front. \"scale\" is the clip size.",
                Capability::EditProject,
                withSequence({{"bin", binIdParam("The bin (absent: the root).")},
                              {"items", placementsParam("Where the items go.")},
                              {"merge", schema::boolean("Keep the items not named (default true).")},
                              {"scale", schema::number("The clip size (0.25 .. 4).", kFreeformMinSize, kFreeformMaxSize)}}),
                Json{{"items", Json::array({Json{{"kind", "media"}, {"id", 3}, {"x", 40}, {"y", 0}}})}}, {},
                [](ExecutionContext& c) {
                    const BinId bin = binOf(c, "bin");
                    FreeformArrangement a = currentArrangement(c.state(), bin);
                    const std::vector<FreeformPlacement> named = placementsValue(c.params().value("items", Json::array()));
                    if (!c.boolean("merge", true)) {
                        a.items.clear();
                    }
                    for (const FreeformPlacement& p : named) {
                        std::erase_if(a.items, [&](const FreeformPlacement& x) { return x.kind == p.kind && x.id == p.id; });
                        a.items.push_back(p);
                    }
                    if (c.has("scale")) {
                        a.scale = c.number("scale");
                    }
                    return stateOutput(c.take(edit::setFreeformArrangement(c.state(), bin, std::move(a))));
                }});
    addBuiltin(registry,
               {"freeform.resetToGrid", 1, "Reset to Grid",
                "Premiere's Reset to Grid: every item of the bin placed row by row in the chosen order (\"by\": name, "
                "label or type; bins, sequences and media keep their groups), \"columns\" per row, at size 1.",
                Capability::EditProject,
                withSequence({{"bin", binIdParam("The bin (absent: the root).")},
                              {"by", schema::enumeration({"name", "label", "type"}, "The order (default name).")},
                              {"columns", schema::integer("Items per row (default 5).", 1, 100)}}),
                Json{{"by", "label"}, {"columns", 4}}, {},
                [](ExecutionContext& c) {
                    const BinId bin = binOf(c, "bin");
                    std::vector<FreeformPlacement> items = edit::freeformItemsOf(c.state(), bin);
                    const std::string by = c.string("by", "name");
                    const auto labelOf = [&](const FreeformPlacement& p) {
                        const MediaItem* m = p.kind == FreeformPlacement::Kind::Media ? c.state().findMedia(MediaId{p.id}) : nullptr;
                        return m ? m->colorIndex : -1;
                    };
                    const auto typeOf = [&](const FreeformPlacement& p) {
                        const MediaItem* m = p.kind == FreeformPlacement::Kind::Media ? c.state().findMedia(MediaId{p.id}) : nullptr;
                        if (!m) {
                            return std::string(kindName(p.kind));
                        }
                        return m->isGenerator() ? std::string("title")
                               : m->info.isStillImage ? std::string("still")
                               : m->info.video.empty() ? std::string("audio")
                                                       : std::string("video");
                    };
                    if (by == "label") {
                        std::stable_sort(items.begin(), items.end(), [&](const auto& a, const auto& b) { return labelOf(a) < labelOf(b); });
                    } else if (by == "type") {
                        std::stable_sort(items.begin(), items.end(), [&](const auto& a, const auto& b) { return typeOf(a) < typeOf(b); });
                    }
                    const double scale = currentArrangement(c.state(), bin).scale;
                    FreeformArrangement a = edit::freeformGrid(items, static_cast<int>(c.integer("columns", 5)), scale);
                    return stateOutput(c.take(edit::setFreeformArrangement(c.state(), bin, std::move(a))));
                }});
    addBuiltin(registry,
               {"freeform.alignToGrid", 1, "Align to Grid",
                "Premiere's Align to Grid: the named items (all when none) snapped to the nearest grid cell of the view's "
                "clip size; items on one cell stack.",
                Capability::EditProject,
                withSequence({{"bin", binIdParam("The bin (absent: the root).")}, {"items", placementsParam("The items to align.")}}),
                Json::object(), {},
                [](ExecutionContext& c) {
                    const BinId bin = binOf(c, "bin");
                    const std::vector<FreeformPlacement> named = placementsValue(c.params().value("items", Json::array()));
                    FreeformArrangement a = edit::alignFreeformToGrid(currentArrangement(c.state(), bin), named);
                    return stateOutput(c.take(edit::setFreeformArrangement(c.state(), bin, std::move(a))));
                }});
    addBuiltin(registry,
               {"freeform.saveLayout", 1, "Save as New Layout",
                "Saves the bin's Freeform arrangement under \"name\" (replacing a layout of that name).", Capability::EditProject,
                withSequence({{"bin", binIdParam("The bin (absent: the root).")},
                              {"name", boundedText("The layout's name.", FreeformLayout::kMaxNameBytes, true)}},
                             {"name"}),
                Json{{"name", "Storyboard"}}, {},
                [](ExecutionContext& c) {
                    return stateOutput(c.take(edit::saveFreeformLayout(c.state(), binOf(c, "bin"), c.string("name"))));
                }});
    addBuiltin(registry,
               {"freeform.restoreLayout", 1, "Restore Layout", "Arranges the bin's Freeform view as saved layout \"name\".",
                Capability::EditProject,
                withSequence({{"bin", binIdParam("The bin (absent: the root).")},
                              {"name", boundedText("The layout's name.", FreeformLayout::kMaxNameBytes, true)}},
                             {"name"}),
                Json{{"name", "Storyboard"}}, {},
                [](ExecutionContext& c) {
                    return stateOutput(c.take(edit::restoreFreeformLayout(c.state(), binOf(c, "bin"), c.string("name"))));
                }});
    addBuiltin(registry,
               {"freeform.removeLayout", 1, "Delete Layout", "Deletes the bin's saved Freeform layout \"name\".",
                Capability::EditProject,
                withSequence({{"bin", binIdParam("The bin (absent: the root).")},
                              {"name", boundedText("The layout's name.", FreeformLayout::kMaxNameBytes, true)}},
                             {"name"}),
                Json{{"name", "Storyboard"}}, {},
                [](ExecutionContext& c) {
                    return stateOutput(c.take(edit::removeFreeformLayout(c.state(), binOf(c, "bin"), c.string("name"))));
                }});

    // ---- XMP and file metadata ----
    addBuiltin(registry,
               {"media.fileMetadata", 1, "File Metadata",
                "The XMP of a media item's file (the Metadata panel's File section): a sidecar \"<name>.xmp\" first, else "
                "the packet inside the file, each property with its schema.",
                Capability::Read, withSequence({{"media", mediaParam("The media item.")}}, {"media"}), Json{{"media", 3}}, {},
                [](ExecutionContext& c) {
                    const MediaItem& item = mediaWithFile(c, c.media("media"));
                    const std::filesystem::path file = pathFromUtf8(item.locator.path);
                    Json xmp = Json::array();
                    Json source = nullptr;
                    if (auto packet = interchange::readMediaXmp(file); packet && *packet) {
                        source = Json{{"kind", (*packet)->source == interchange::XmpSource::Sidecar ? "sidecar" : "embedded"},
                                      {"file", pathToUtf8((*packet)->file)}};
                        for (const interchange::XmpProperty& p : (*packet)->properties) {
                            xmp.push_back(Json{{"group", p.group}, {"prefix", p.prefix}, {"name", p.name},
                                               {"label", p.label}, {"value", p.value}});
                        }
                    }
                    return readOutput(c, Json{{"media", item.id.value}, {"xmpSource", std::move(source)}, {"xmp", std::move(xmp)}});
                }});
    addBuiltin(registry,
               {"media.readXmp", 1, "Read XMP Metadata",
                "Fills the clip metadata of media items from the XMP of their files (Premiere's links: Scene, Shot, Take, "
                "Log Note, Good, Client, Tape Name, Comment, Description and the camera fields). \"overwrite\" (default "
                "true) replaces what is set; false only fills empty fields. Items without XMP stay. One undo step.",
                Capability::EditProject,
                withSequence({{"media", schema::array(mediaParam("A media item."), "The media items.", 1)},
                              {"overwrite", schema::boolean("Replace fields that are set (default true).")}},
                             {"media"}),
                Json{{"media", Json::array({3, 4})}}, {},
                [](ExecutionContext& c) {
                    const bool overwrite = c.boolean("overwrite", true);
                    ProjectState state = c.state();
                    Json read = Json::array();
                    for (const MediaId id : c.mediaList("media")) {
                        const MediaItem* item = state.findMedia(id);
                        if (item->isGenerator() || item->locator.path.empty()) {
                            continue;
                        }
                        auto packet = interchange::readMediaXmp(pathFromUtf8(item->locator.path));
                        if (!packet || !*packet) {
                            continue;
                        }
                        const interchange::XmpClipFields f = interchange::clipFieldsFromXmp((*packet)->properties);
                        edit::MediaAttributeChange change;
                        const auto pick = [&](const std::optional<std::string>& value, const std::string& current,
                                              std::optional<std::string>& into) {
                            if (value && (overwrite || current.empty())) {
                                into = *value;
                            }
                        };
                        pick(f.scene, item->metadata.scene, change.scene);
                        pick(f.shot, item->metadata.shot, change.shot);
                        pick(f.take, item->metadata.take, change.take);
                        pick(f.logNote, item->metadata.logNote, change.logNote);
                        pick(f.client, item->metadata.client, change.client);
                        pick(f.comment, item->comment, change.comment);
                        pick(f.description, item->description, change.description);
                        pick(f.tapeName, item->timecode ? item->timecode->reelName : std::string(), change.tapeName);
                        pick(f.cameraAngle, item->metadata.cameraAngle, change.cameraAngle);
                        pick(f.cameraLabel, item->metadata.cameraLabel, change.cameraLabel);
                        pick(f.cameraModel, item->metadata.cameraModel, change.cameraModel);
                        pick(f.shotDate, item->metadata.shotDate, change.shotDate);
                        pick(f.shotLocation, item->metadata.shotLocation, change.shotLocation);
                        if (f.good && (overwrite || !item->metadata.good)) {
                            change.good = *f.good;
                        }
                        state = c.take(edit::setMediaAttributes(state, std::vector<MediaId>{id}, change));
                        read.push_back(Json{{"media", id.value}, {"file", pathToUtf8((*packet)->file)}});
                    }
                    return stateOutput(std::move(state), Json{{"read", std::move(read)}});
                }});

    // ---- Another project's items ----
    addBuiltin(registry,
               command::touchesFiles(
                   {"project.importItems", 1, "Import from Project",
                    "Premiere's import of a project (or of selected sequences): copies of another project's bins (with "
                    "everything in them), sequences (with what they play: media, nested and multicam sequences, LUTs, "
                    "speakers, transcript corrections, redactions, review notes, templates and styles this project lacks) "
                    "and media items, in \"into\" (default the root) or a new bin \"binName\". Media this project already "
                    "has (same file) is reused, so no item is duplicated; files are found where the other project is. "
                    "\"everything\" brings the whole project. The copies remember the other project and their id there "
                    "(importSource: Reveal in Source Project, relinks offered). One undo step.",
                    Capability::EditProject,
                    withSequence({{"path", boundedText("The other project file (UTF-8 path).", 4096, true)},
                                  {"everything", schema::boolean("The whole project (default false).")},
                                  {"bins", schema::array(schema::integer("A bin id there.", 1), "Bins there.")},
                                  {"sequences", schema::array(schema::integer("A sequence id there.", 1), "Sequences there.")},
                                  {"media", schema::array(schema::integer("A media id there.", 1), "Media items there.")},
                                  {"into", binIdParam("The bin here they go in.")},
                                  {"binName", boundedText("A new bin here that holds them.", 256)},
                                  {"reuseMedia", schema::boolean("Reuse media this project has (default true).")}},
                                 {"path"}),
                    Json{{"path", "D:/Productions/Doc/Interviews.wabcut"}, {"sequences", Json::array({12})}}, {},
                    [](ExecutionContext& c) {
                        const std::filesystem::path file = pathFromUtf8(c.string("path"));
                        auto loaded = io::loadProject(file);
                        if (!loaded) {
                            fail(ErrorCode::EditRejected, "cannot read " + c.string("path") + ": " + loaded.error().message);
                        }
                        const ProjectState source = resolvedSource(std::move(*loaded), file);
                        edit::ProjectImportSelection selection;
                        selection.everything = c.boolean("everything", false);
                        const auto ids = [&](const char* key, auto make) {
                            using Id = decltype(make(0));
                            std::vector<Id> out;
                            if (c.has(key)) {
                                for (const Json& v : c.params()[key]) {
                                    out.push_back(make(v.get<std::uint64_t>()));
                                }
                            }
                            return out;
                        };
                        selection.bins = ids("bins", [](std::uint64_t v) { return BinId{v}; });
                        selection.sequences = ids("sequences", [](std::uint64_t v) { return SequenceId{v}; });
                        selection.media = ids("media", [](std::uint64_t v) { return MediaId{v}; });
                        edit::ProjectImportOptions options;
                        options.into = binOf(c, "into");
                        options.binName = c.string("binName");
                        options.reuseMedia = c.boolean("reuseMedia", true);
                        std::error_code ec;
                        const std::filesystem::path absolute = std::filesystem::absolute(file, ec);
                        options.sourceProject = pathToUtf8((ec ? file : absolute).lexically_normal());
                        auto imported = edit::importProjectItems(c.state(), source, selection, options);
                        if (!imported) {
                            fail(imported.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::EditRejected,
                                 imported.error().message);
                        }
                        Json data{{"bin", imported->bin.value},
                                  {"sequences", Json::array()},
                                  {"media", Json::array()},
                                  {"bins", Json::array()},
                                  {"reusedMedia", imported->reusedMedia},
                                  {"notes", imported->notes}};
                        std::vector<std::uint64_t> created;
                        for (const SequenceId s : imported->sequences) {
                            data["sequences"].push_back(s.value);
                            created.push_back(s.value);
                        }
                        for (const MediaId m : imported->media) {
                            data["media"].push_back(m.value);
                            created.push_back(m.value);
                        }
                        for (const BinId b : imported->bins) {
                            data["bins"].push_back(b.value);
                            created.push_back(b.value);
                        }
                        CommandOutput out = stateOutput(std::move(imported->state), std::move(data), std::move(created));
                        for (const std::string& note : imported->notes) {
                            out.warnings.push_back(note);
                        }
                        return out;
                    }},
                   {"path"}));
}

} // namespace wabcut::command::detail
