// 0.2 (P.3): the Project panel's commands - clip metadata (media.setAttributes), Search Bins (bin.createSearch /
// setSearch), the Freeform view (freeform.*), XMP (media.readXmp / fileMetadata) and project.importItems.

#include "CommandHarness.h"

#include <wabcut/interchange/Xmp.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "project-panel-commands" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

/// Points media `id` of the harness's project at `file` (one undo step through the session).
void giveFile(CommandHarness& h, MediaId id, const fs::path& file) {
    {
        std::ofstream out(file, std::ios::binary);
        out << std::string(512, 'x');
    }
    ProjectState s = *h.state();
    for (MediaPtr& m : s.media) {
        if (m->id == id) {
            MediaItem item = *m;
            item.locator.path = pathToUtf8(file);
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    REQUIRE(h.session.commit("file", s));
}

} // namespace

TEST_CASE("media.setAttributes sets Premiere's clip metadata", "[command][library][metadata]") {
    CommandHarness h("    V1 tgt |Aaaa|\n");
    const MediaId a = h.tl.media('A');
    h.ok("media.setAttributes", Json{{"media", Json::array({a.value})}, {"scene", "12"}, {"shot", "3"}, {"take", "2"},
                                     {"logNote", "Laughs"}, {"client", "Northwind"}, {"good", true}, {"tapeName", "A001"}});
    const MediaItem& m = *h.state()->findMedia(a);
    CHECK(m.metadata.scene == "12");
    CHECK(m.metadata.good);
    CHECK(m.timecode->reelName == "A001");
    CHECK(h.session.undoCount() == 1);
    CHECK(h.errorOf("media.setAttributes", Json{{"media", Json::array({a.value})},
                                                {"scene", std::string(ClipMetadata::kMaxFieldBytes + 1, 'x')}}) ==
          "EditRejected");
}

TEST_CASE("bin.createSearch and bin.setSearch make and edit search bins", "[command][library][searchbin]") {
    CommandHarness h("    V1 tgt |Aaaa|\n");
    const auto made = h.ok("bin.createSearch", Json{{"name", "Good takes"}, {"search", Json{{"good", true}, {"text", "int"}, {"field", "scene"}}}});
    const BinId bin{made.data["bin"].get<std::uint64_t>()};
    const Bin* b = h.state()->findBin(bin);
    REQUIRE(b);
    REQUIRE(b->search);
    CHECK(b->search->good);
    CHECK(b->search->field == "scene");
    h.ok("bin.setSearch", Json{{"bin", bin.value}, {"search", Json{{"type", "audio"}}}});
    CHECK(h.state()->findBin(bin)->search->type == "audio");
    CHECK_FALSE(h.state()->findBin(bin)->search->good);
    CHECK(h.errorOf("bin.createSearch", Json{{"name", "x"}, {"search", Json{{"field", "secret"}}}}) == "InvalidParams");
    CHECK(h.errorOf("bin.move", Json{{"target", bin.value}, {"media", Json::array({h.tl.media('A').value})}}) == "EditRejected");
}

TEST_CASE("freeform commands arrange, align, reset and keep layouts", "[command][library][freeform]") {
    CommandHarness h("    V1 tgt |AaaaBbbb|\n");
    const std::uint64_t a = h.tl.media('A').value;
    const std::uint64_t b = h.tl.media('B').value;
    h.ok("freeform.set", Json{{"items", Json::array({Json{{"kind", "media"}, {"id", a}, {"x", 30}, {"y", 5}},
                                                     Json{{"kind", "media"}, {"id", b}, {"x", 33}, {"y", 8}, {"size", 2}}})},
                              {"scale", 1.5}});
    auto view = h.ok("freeform.get");
    CHECK(view.data["arrangement"]["scale"] == 1.5);
    REQUIRE(view.data["arrangement"]["items"].size() == 2);
    CHECK(view.data["arrangement"]["items"][1]["id"] == b); // B on top of A: a stack

    // Merge (default): A moves to the front, B stays.
    h.ok("freeform.set", Json{{"items", Json::array({Json{{"kind", "media"}, {"id", a}, {"x", 500}, {"y", 0}}})}});
    view = h.ok("freeform.get");
    CHECK(view.data["arrangement"]["items"][1]["id"] == a);
    CHECK(view.data["arrangement"]["items"][0]["x"] == 33.0);

    h.ok("freeform.saveLayout", Json{{"name", "Story"}});
    h.ok("freeform.alignToGrid");
    view = h.ok("freeform.get");
    for (const Json& item : view.data["arrangement"]["items"]) {
        const double cellW = edit::kFreeformCellWidth * 1.5;
        CHECK(std::fmod(item["x"].get<double>(), cellW) == 0.0);
    }
    h.ok("freeform.resetToGrid", Json{{"by", "label"}, {"columns", 2}});
    view = h.ok("freeform.get");
    CHECK(view.data["arrangement"]["items"].size() == h.state()->media.size() + h.state()->sequences.size());
    h.ok("freeform.restoreLayout", Json{{"name", "story"}});
    view = h.ok("freeform.get");
    CHECK(view.data["arrangement"]["items"].size() == 2);
    CHECK(view.data["layouts"].size() == 1);
    h.ok("freeform.removeLayout", Json{{"name", "Story"}});
    CHECK(h.ok("freeform.get").data["layouts"].empty());
    CHECK(h.errorOf("freeform.restoreLayout", Json{{"name", "Story"}}) == "NotFound");
    CHECK(h.errorOf("freeform.set", Json{{"items", Json::array({Json{{"kind", "media"}, {"id", a}, {"size", 9}}})}}) ==
          "InvalidParams");
}

TEST_CASE("media.readXmp and media.fileMetadata read clip metadata from a sidecar", "[command][library][xmp]") {
    const fs::path dir = scratch("xmp");
    CommandHarness h("    V1 tgt |AaaaBbbb|\n");
    const MediaId a = h.tl.media('A');
    const MediaId b = h.tl.media('B');
    giveFile(h, a, dir / "A.mov");
    giveFile(h, b, dir / "B.mov");
    h.ok("media.setAttributes", Json{{"media", Json::array({a.value})}, {"scene", "7"}, {"good", true}, {"logNote", "Nice"}});
    // The app writes sidecars (Library.writeXmp); here the writer itself.
    REQUIRE(interchange::writeXmpSidecar(*h.state()->findMedia(a)));
    CHECK(fs::exists(dir / "A.xmp"));
    // Copy A's sidecar to B and read it there.
    fs::copy_file(dir / "A.xmp", dir / "B.xmp");
    const auto read = h.ok("media.readXmp", Json{{"media", Json::array({b.value})}});
    CHECK(read.data["read"].size() == 1);
    const MediaItem& item = *h.state()->findMedia(b);
    CHECK(item.metadata.scene == "7");
    CHECK(item.metadata.good);
    CHECK(item.metadata.logNote == "Nice");
    const auto meta = h.ok("media.fileMetadata", Json{{"media", b.value}});
    CHECK(meta.data["xmpSource"]["kind"] == "sidecar");
    CHECK_FALSE(meta.data["xmp"].empty());
    // Not overwriting keeps what is set.
    h.ok("media.setAttributes", Json{{"media", Json::array({b.value})}, {"scene", "mine"}});
    h.ok("media.readXmp", Json{{"media", Json::array({b.value})}, {"overwrite", false}});
    CHECK(h.state()->findMedia(b)->metadata.scene == "mine");
    // Writing sidecars is not a command (it writes where no parameter says).
    CHECK_FALSE(command::builtinCommands().knows("media.writeXmp"));
}

TEST_CASE("project.importItems brings another project's sequence and its media in one step", "[command][library][import]") {
    const fs::path dir = scratch("import");
    // The other project: a sequence of A with A's file next to it.
    Timeline other("    V1 tgt |Aaaa|\n");
    ProjectState source = other.state;
    {
        std::ofstream out(dir / "A.mov", std::ios::binary);
        out << std::string(512, 'a');
    }
    for (MediaPtr& m : source.media) {
        if (m->id == other.media('A')) {
            MediaItem item = *m;
            item.locator.path = "Z:/gone/A.mov";
            item.locator.relativePath = "A.mov";
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    const fs::path file = dir / "Other.wabcut";
    REQUIRE(io::saveProject(source, file));

    CommandHarness h("    V1 tgt |Zzzz|\n");
    const std::size_t sequences = h.state()->sequences.size();
    const auto result = h.ok("project.importItems", Json{{"path", pathToUtf8(file)},
                                                         {"sequences", Json::array({other.seq.value})},
                                                         {"binName", "Other"}});
    CHECK(h.state()->sequences.size() == sequences + 1);
    CHECK(h.session.undoCount() == 1);
    REQUIRE(result.data["media"].size() == 1);
    const MediaItem* a = h.state()->findMedia(MediaId{result.data["media"][0].get<std::uint64_t>()});
    REQUIRE(a);
    CHECK_FALSE(a->locator.offline); // found next to the other project by its relative path
    CHECK(fs::path(pathFromUtf8(a->locator.path)).filename() == "A.mov");
    // The copy remembers where it came from.
    REQUIRE(a->importSource.has_value());
    CHECK(fs::path(pathFromUtf8(a->importSource->project)) == fs::absolute(file).lexically_normal());
    CHECK(a->importSource->id == other.media('A').value);
    CHECK(h.state()->findBin(BinId{result.data["bin"].get<std::uint64_t>()})->name == "Other");
    CHECK(h.errorOf("project.importItems", Json{{"path", pathToUtf8(dir / "missing.wabcut")}, {"everything", true}}) ==
          "EditRejected");
    CHECK(h.errorOf("project.importItems", Json{{"path", pathToUtf8(file)}, {"sequences", Json::array({987654})}}) ==
          "NotFound");
}
