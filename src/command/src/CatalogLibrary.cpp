// Built-in library commands (C.11, over the M5k ops in ProjectOps.h): bins (bin.create / rename / move / remove) and
// media metadata and subclips (media.setAttributes, media.makeSubclip, media.setSubclip). Library organisation only:
// sequences' content never changes. Source times are ticks or a timecode in the media's own rate.

#include "Catalog.h"

#include <wabcut/command/SourceServices.h>
#include <wabcut/model/ProjectOps.h>

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
         "description and poster frame (a source time, or null to clear it). Fields not given stay. One undo step for "
         "any number of items.",
         Capability::EditProject,
         withSequence({{"media", schema::array(mediaParam("A media item."), "The media items.", 1)},
                       {"name", schema::string("The name (not empty).")},
                       {"colorIndex", schema::integer("Label colour, 0..15.", 0, 15)},
                       {"comment", schema::string("Comment.")},
                       {"description", schema::string("Description.")},
                       {"posterFrame", sourceTimeParam("Poster frame (media time); null clears it.")}},
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
}

} // namespace wabcut::command::detail
