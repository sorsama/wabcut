// 0.2 (P.3): the Project panel's new parts - clip metadata columns and the Metadata panel, Search Bins, the Freeform
// view, XMP (sidecars and MP4s), Productions and their settings, several projects open, project locks, importing from
// another project and where the copies came from. Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProductionBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/editor/PreviewRender.h>
#include <wabcut/editor/Production.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QQuickItem>
#include <QUrl>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

struct Rig {
    Timeline tl;
    ProjectController controller;
    LibraryBridge library;

    explicit Rig(std::string_view picture = "    V1 tgt |AaaaBbbb|\n    A1 tgt |AaaaBbbb|\n") : tl(picture) {
        controller.resetProject(tl.state, tl.seq);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setAnalysisProbe({});
        library.setController(&controller);
    }
    QString key(char letter) const { return mediaKey(tl.media(letter)); }
    const MediaItem& item(char letter) const { return *controller.snapshot()->findMedia(tl.media(letter)); }
    std::size_t undo() { return controller.session().undoCount(); }
    QStringList keys() const {
        QStringList out;
        for (int r = 0; r < library.items()->rowCount(); ++r)
            out << library.items()->keyOf(r);
        return out;
    }
};

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(QDir::toNativeSeparators(outputDir()).toStdU16String()) / "project-panel" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QUrl url(const fs::path& p) { return QUrl::fromLocalFile(QString::fromStdU16String(p.u16string())); }

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

void writeFile(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << bytes;
}

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string be32(std::uint32_t v) {
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}

/// An ISO-BMFF box.
std::string box(const std::string& type, const std::string& payload) {
    return be32(static_cast<std::uint32_t>(8 + payload.size())) + type + payload;
}

/// A second of 48 kHz mono 16-bit PCM at `hz`: a real file the probe reads (relinking describes the file).
bool writeWav(const fs::path& file, double hz) {
    constexpr std::uint32_t kWavRate = 48000;
    std::string data;
    for (std::uint32_t i = 0; i < kWavRate; ++i) {
        const auto s = static_cast<std::int16_t>(std::lround(0.4 * 32767.0 * std::sin(2.0 * std::numbers::pi * hz * i / kWavRate)));
        data.push_back(static_cast<char>(s & 0xff));
        data.push_back(static_cast<char>((s >> 8) & 0xff));
    }
    const auto le32 = [](std::uint32_t v) {
        return std::string{static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
    };
    const auto le16 = [](std::uint16_t v) { return std::string{static_cast<char>(v), static_cast<char>(v >> 8)}; };
    const std::string bytes = "RIFF" + le32(static_cast<std::uint32_t>(36 + data.size())) + "WAVEfmt " + le32(16) + le16(1) +
                              le16(1) + le32(kWavRate) + le32(kWavRate * 2) + le16(2) + le16(16) + "data" +
                              le32(static_cast<std::uint32_t>(data.size())) + data;
    writeFile(file, bytes);
    return fs::file_size(file) == bytes.size();
}

/// A saved project with one sequence playing a media item whose file is `file` (on an audio track with `audio`).
ProjectState projectPlaying(const fs::path& file, const std::string& sequenceName, bool audio = false) {
    Timeline t(audio ? "    A1 tgt |Aaaa|\n" : "    V1 tgt |Aaaa|\n");
    ProjectState s = t.state;
    for (MediaPtr& m : s.media) {
        if (m->id == t.media('A')) {
            MediaItem item = *m;
            item.locator.path = pathToUtf8(file);
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    Sequence seq = *s.findSequence(t.seq);
    seq.name = sequenceName;
    return s.withSequence(std::make_shared<const Sequence>(std::move(seq)));
}

} // namespace

TEST_CASE("Library: clip metadata columns, inline edits and Good, one undo step each", "[app][library][metadata]") {
    Rig r;
    const auto columns = r.library.allColumns();
    QStringList ids;
    for (const QVariant& c : columns)
        ids << c.toMap().value(QStringLiteral("id")).toString();
    for (const char* id : {"scene", "shot", "take", "logNote", "good", "client", "tapeName"})
        CHECK(ids.contains(QLatin1String(id)));
    CHECK(r.library.columns().size() == 12); // hidden until chosen

    const auto before = r.undo();
    REQUIRE(r.library.setMetadata({r.key('A'), r.key('B')}, QStringLiteral("scene"), QStringLiteral("12")));
    REQUIRE(r.library.setMetadata({r.key('A')}, QStringLiteral("good"), true));
    REQUIRE(r.library.setMetadata({r.key('A')}, QStringLiteral("tapeName"), QStringLiteral("A001")));
    CHECK(r.undo() == before + 3);
    CHECK(r.item('A').metadata.scene == "12");
    CHECK(r.item('B').metadata.scene == "12");
    CHECK(r.item('A').metadata.good);
    CHECK(r.item('A').timecode->reelName == "A001");
    CHECK_FALSE(r.library.setMetadata({r.key('A')}, QStringLiteral("password"), QStringLiteral("x")));

    r.library.setColumnVisible(QStringLiteral("scene"), true);
    r.library.setColumnVisible(QStringLiteral("good"), true);
    CHECK(r.library.cellText(r.key('A'), QStringLiteral("scene")) == QStringLiteral("12"));
    CHECK(r.library.cellText(r.key('A'), QStringLiteral("good")) == QStringLiteral("Good"));
    CHECK(r.library.cellText(r.key('A'), QStringLiteral("tapeName")) == QStringLiteral("A001"));
    const int row = r.library.items()->rowOf(r.key('A'));
    CHECK(r.library.items()->data(r.library.items()->index(row, 0), LibraryModel::GoodRole).toBool());

    // Find looks in the clip metadata; the Metadata panel's map has it.
    r.library.setFindText(QStringLiteral("A001"));
    CHECK(r.keys() == QStringList{r.key('A')});
    r.library.setFindText({});
    const QVariantMap clip = r.library.clipMetadata(r.key('A'));
    CHECK(clip.value(QStringLiteral("scene")).toString() == QStringLiteral("12"));
    CHECK(clip.value(QStringLiteral("good")).toBool());
    CHECK(r.controller.undo());
    CHECK_FALSE(r.item('A').timecode.has_value());
}

TEST_CASE("Library: a search bin lists what its query finds, live, across bins", "[app][library][searchbin]") {
    Rig r;
    const QString selects = r.library.newBin(QStringLiteral("Selects"));
    REQUIRE(r.library.moveItems({r.key('B')}, selects));
    REQUIRE(r.library.setMetadata({r.key('B')}, QStringLiteral("good"), true));

    // From the Find bar: the query moves into the new bin.
    r.library.setFilters(QVariantMap{});
    r.library.setFindText(QStringLiteral("B"));
    const auto before = r.undo();
    const QString search = r.library.createSearchBin();
    REQUIRE_FALSE(search.isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK_FALSE(r.library.filtering());
    CHECK(r.library.details(search).value(QStringLiteral("searchBin")).toBool());
    CHECK(r.library.searchOf(search).value(QStringLiteral("text")).toString() == QStringLiteral("B"));

    // Opened, it lists B from its bin, never itself.
    r.library.openBin(search);
    CHECK(r.library.searchBinOpen());
    CHECK(r.keys().contains(r.key('B')));
    CHECK_FALSE(r.keys().contains(search));

    // Edit Search Bin: Good items only, by scene; live as metadata changes.
    REQUIRE(r.library.editSearchBin(search, QVariantMap{{QStringLiteral("good"), true}, {QStringLiteral("field"), QStringLiteral("scene")},
                                                        {QStringLiteral("text"), QStringLiteral("7")}}));
    CHECK(r.keys().isEmpty());
    REQUIRE(r.library.setMetadata({r.key('B')}, QStringLiteral("scene"), QStringLiteral("7")));
    CHECK(r.keys() == QStringList{r.key('B')});
    REQUIRE(r.library.setMetadata({r.key('A')}, QStringLiteral("scene"), QStringLiteral("7"))); // not Good
    CHECK(r.keys() == QStringList{r.key('B')});

    // A new bin made while a search bin is open goes next to it, not into it.
    const QString made = r.library.newBin(QStringLiteral("Next to it"));
    REQUIRE_FALSE(made.isEmpty());
    CHECK_FALSE(r.controller.snapshot()->findBin(*parseBinKey(made))->parent.isValid());
    // Its card counts what it finds.
    r.library.openBin({});
    const int row = r.library.items()->rowOf(search);
    REQUIRE(row >= 0);
    CHECK(r.library.items()->data(r.library.items()->index(row, 0), LibraryModel::CountRole).toInt() == 1);
    CHECK(r.library.items()->data(r.library.items()->index(row, 0), LibraryModel::SearchBinRole).toBool());
    // Items cannot be moved into it.
    CHECK_FALSE(r.library.moveItems({r.key('C')}, search));
}

TEST_CASE("Library: the Freeform view keeps places, stacks and layouts in the project", "[app][library][freeform]") {
    Rig r;
    r.library.setView(QStringLiteral("freeform"));
    CHECK(r.library.view() == QStringLiteral("freeform"));
    REQUIRE(r.library.freeformAvailable());
    QVariantMap places = r.library.freeform();
    CHECK(places.size() == r.library.items()->rowCount()); // every item has a place before any edit
    const auto at = [&](const QString& k) { return r.library.freeform().value(k).toMap(); };

    // Move A onto B: one undo step, A on top (a stack).
    const QVariantMap b = at(r.key('B'));
    const QVariantMap a = at(r.key('A'));
    const auto before = r.undo();
    REQUIRE(r.library.moveFreeform({r.key('A')}, b.value("x").toDouble() - a.value("x").toDouble() + 8,
                                   b.value("y").toDouble() - a.value("y").toDouble() + 8));
    CHECK(r.undo() == before + 1);
    CHECK(at(r.key('A')).value("z").toInt() > at(r.key('B')).value("z").toInt());
    const QStringList stack = r.library.freeformStackOf(r.key('B'));
    CHECK(stack.contains(r.key('A')));
    CHECK(stack.back() == r.key('A'));
    // Ctrl+wheel brings the next card of the stack to the front.
    CHECK(r.library.cycleFreeformStack(r.key('A'), 1) == r.key('B'));
    CHECK(at(r.key('B')).value("z").toInt() > at(r.key('A')).value("z").toInt());

    // Clip size, a saved layout, Reset to Grid, Restore Layout.
    REQUIRE(r.library.setFreeformScale(1.5));
    CHECK(r.library.freeformScale() == 1.5);
    REQUIRE(r.library.setFreeformSize({r.key('A')}, 2.0));
    CHECK(at(r.key('A')).value("size").toDouble() == 2.0);
    REQUIRE(r.library.saveFreeformLayout(QStringLiteral("Storyboard")));
    CHECK(r.library.freeformLayouts() == QStringList{QStringLiteral("Storyboard")});
    const double stackedX = at(r.key('A')).value("x").toDouble();
    REQUIRE(r.library.freeformResetToGrid(QStringLiteral("name"), 4));
    CHECK(at(r.key('A')).value("size").toDouble() == 1.0);
    REQUIRE(r.library.restoreFreeformLayout(QStringLiteral("storyboard")));
    CHECK(at(r.key('A')).value("x").toDouble() == stackedX);
    REQUIRE(r.library.freeformAlignToGrid(false));
    const double cell = r.library.freeformCellWidth() * 1.5;
    CHECK(std::fmod(at(r.key('A')).value("x").toDouble(), cell) == 0.0);
    // Saved with the project.
    const FreeformView* view = r.controller.snapshot()->findFreeform(BinId{});
    REQUIRE(view);
    CHECK(view->layouts.size() == 1);
    CHECK(view->current.scale == 1.5);
    REQUIRE(r.library.removeFreeformLayout(QStringLiteral("Storyboard")));
    CHECK(r.library.freeformLayouts().isEmpty());

    // While Find lists items, there is nothing to arrange.
    r.library.setFindText(QStringLiteral("A"));
    CHECK_FALSE(r.library.freeformAvailable());
    CHECK_FALSE(r.library.moveFreeform({r.key('A')}, 10, 10));
}

TEST_CASE("Library: XMP sidecars carry clip metadata out and back in", "[app][library][xmp]") {
    const fs::path dir = scratch("xmp");
    Rig r;
    // Give A and B files.
    ProjectState s = *r.controller.snapshot();
    for (MediaPtr& m : s.media) {
        if (m->id == r.tl.media('A') || m->id == r.tl.media('B')) {
            const fs::path file = dir / (m->name + ".mov");
            std::ofstream(file, std::ios::binary) << std::string(256, 'x');
            MediaItem item = *m;
            item.locator.path = pathToUtf8(file);
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    REQUIRE(r.controller.session().commit("files", s));
    REQUIRE(r.library.setMetadata({r.key('A')}, QStringLiteral("scene"), QStringLiteral("21")));
    REQUIRE(r.library.setMetadata({r.key('A')}, QStringLiteral("logNote"), QStringLiteral("Keeper")));
    CHECK(r.library.writeXmp({r.key('A')}) == 1);
    CHECK(fs::exists(dir / "A.xmp"));
    const QVariantMap file = r.library.fileMetadata(r.key('A'));
    CHECK(file.value(QStringLiteral("xmpSource")).toString() == QStringLiteral("sidecar"));
    CHECK_FALSE(file.value(QStringLiteral("xmp")).toList().isEmpty());

    fs::copy_file(dir / "A.xmp", dir / "B.xmp");
    const auto before = r.undo();
    CHECK(r.library.readXmp({r.key('B'), r.key('C')}) == 1); // C has no file
    CHECK(r.undo() == before + 1);
    CHECK(r.item('B').metadata.scene == "21");
    CHECK(r.item('B').metadata.logNote == "Keeper");
}

TEST_CASE("Productions: open projects next to each other, import in one undo step, locks", "[app][production]") {
    resetSettings();
    const fs::path dir = scratch("production");
    const fs::path footage = dir / "Footage" / "interview.mov";
    fs::create_directories(footage.parent_path());
    std::ofstream(footage, std::ios::binary) << std::string(512, 'i');

    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    ProductionBridge production(&lifecycle);
    REQUIRE(production.makeProduction(url(dir / "Doc"), QStringLiteral("Doc")));
    CHECK(production.isOpen());
    CHECK(production.name() == QStringLiteral("Doc"));

    // Two projects in it: the one edited and the one browsed next to it.
    const QUrl assembly = production.newProject({}, QStringLiteral("Assembly"));
    REQUIRE_FALSE(assembly.isEmpty());
    const fs::path interviews = dir / "Doc" / "Interviews.wabcut";
    REQUIRE(io::saveProject(projectPlaying(footage, "Interview cut"), interviews, io::SaveOptions{0}));
    production.refresh();
    REQUIRE(production.entries().size() == 2);

    REQUIRE(lifecycle.open(assembly));
    CHECK_FALSE(lifecycle.readOnly());
    CHECK_FALSE(lifecycle.productionFolder().isEmpty());
    CHECK(fs::exists(editor::projectLockPath(fs::path(assembly.toLocalFile().toStdU16String())))); // locked for us
    bool current = false;
    for (const QVariant& e : production.entries())
        current = current || e.toMap().value(QStringLiteral("current")).toBool();
    CHECK(current);

    REQUIRE(production.browse(url(interviews)));
    CHECK(production.browsingName() == QStringLiteral("Interviews"));
    QString sequenceKey;
    for (const QVariant& v : production.browseItems()) {
        if (v.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("sequence"))
            sequenceKey = v.toMap().value(QStringLiteral("key")).toString();
    }
    REQUIRE_FALSE(sequenceKey.isEmpty());
    const std::size_t sequences = controller.snapshot()->sequences.size();
    const auto undo = controller.session().undoCount();
    const QVariantMap imported = production.importItems({sequenceKey}, QStringLiteral("From Interviews"));
    CHECK(imported.value(QStringLiteral("ok")).toBool());
    CHECK(controller.session().undoCount() == undo + 1);
    CHECK(controller.snapshot()->sequences.size() == sequences + 1);
    bool found = false;
    for (const MediaPtr& m : controller.snapshot()->media)
        found = found || (fs::path(pathFromUtf8(m->locator.path)) == footage && !m->locator.offline);
    CHECK(found); // the copy plays the same file, found where it is
    // Importing again reuses the media (no duplicate item).
    const std::size_t media = controller.snapshot()->media.size();
    CHECK(production.importItems({sequenceKey}).value(QStringLiteral("reusedMedia")).toInt() == 1);
    CHECK(controller.snapshot()->media.size() == media);

    // Another computer has Interviews open: it opens read-only, and Save is refused.
    {
        std::ofstream lock(editor::projectLockPath(interviews), std::ios::binary);
        lock << R"({"format": "wabcut-project-lock", "user": "alex", "host": "EDIT-BAY-2", "pid": 77, "since": "2026-10-16T09:00:00Z"})";
    }
    REQUIRE(lifecycle.open(url(interviews)));
    CHECK(lifecycle.readOnly());
    CHECK(lifecycle.lockOwner() == QStringLiteral("alex on EDIT-BAY-2"));
    CHECK(lifecycle.windowTitle().contains(QStringLiteral("read-only")));
    CHECK_FALSE(lifecycle.save());
    // Opening it released our lock on Assembly.
    CHECK_FALSE(fs::exists(editor::projectLockPath(fs::path(assembly.toLocalFile().toStdU16String()))));
    // Save As to another file makes it ours.
    REQUIRE(lifecycle.saveAs(url(dir / "Doc" / "Interviews (mine).wabcut")));
    CHECK_FALSE(lifecycle.readOnly());
}

TEST_CASE("Library: the File section edits XMP in the sidecar, or in an MP4 itself when chosen", "[app][library][xmp]") {
    const fs::path dir = scratch("xmp-edit");
    Rig r;
    const fs::path mp4 = dir / "A.mp4";
    const std::string original =
        box("ftyp", "isom") + box("moov", box("mvhd", std::string(100, '\0'))) + box("mdat", std::string(4000, 'v'));
    writeFile(mp4, original);
    const fs::path wav = dir / "B.wav";
    writeFile(wav, "RIFF" + std::string(60, '\0'));
    ProjectState s = *r.controller.snapshot();
    for (MediaPtr& m : s.media) {
        if (m->id == r.tl.media('A') || m->id == r.tl.media('B')) {
            MediaItem item = *m;
            item.locator.path = pathToUtf8(m->id == r.tl.media('A') ? mp4 : wav);
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    REQUIRE(r.controller.session().commit("files", s));
    const QString dm = QStringLiteral("http://ns.adobe.com/xmp/1.0/DynamicMedia/");
    const QString dc = QStringLiteral("http://purl.org/dc/elements/1.1/");

    // Off (the default): into the sidecar, the media untouched; a linked property sets the clip field, one undo step.
    CHECK_FALSE(r.library.embedXmp());
    auto before = r.undo();
    REQUIRE(r.library.setFileMetadata(r.key('A'), dm, QStringLiteral("xmpDM"), QStringLiteral("scene"), QStringLiteral("77")));
    CHECK(r.undo() == before + 1);
    CHECK(r.item('A').metadata.scene == "77");
    CHECK(fs::exists(dir / "A.xmp"));
    CHECK(readFile(mp4) == original);
    QVariantMap file = r.library.fileMetadata(r.key('A'));
    CHECK(file.value(QStringLiteral("writesTo")).toString() == QStringLiteral("sidecar"));
    bool editable = false;
    for (const QVariant& v : file.value(QStringLiteral("xmp")).toList()) {
        const QVariantMap p = v.toMap();
        if (p.value(QStringLiteral("property")).toString() == QStringLiteral("scene")) {
            editable = p.value(QStringLiteral("editable")).toBool();
            CHECK(p.value(QStringLiteral("value")).toString() == QStringLiteral("77"));
            CHECK(p.value(QStringLiteral("ns")).toString() == dm);
        }
    }
    CHECK(editable);
    // Not linked: only the file changes. A structure's field is not set here.
    before = r.undo();
    REQUIRE(r.library.setFileMetadata(r.key('A'), dc, QStringLiteral("dc"), QStringLiteral("rights"), QStringLiteral("(c) 2026")));
    CHECK(r.undo() == before);
    CHECK_FALSE(r.library.setFileMetadata(r.key('A'), dm, QStringLiteral("xmpDM"), QStringLiteral("startTimecode/timeValue"),
                                          QStringLiteral("x")));

    // On: into the MP4 itself (a checked copy replaced it), and the item is pointed at the file again.
    fs::remove(dir / "A.xmp");
    r.library.setEmbedXmp(true);
    QStringList relinked;
    r.library.setRelinker([&](const QString& key, const fs::path&) {
        relinked << key;
        return true;
    });
    CHECK(r.library.fileMetadata(r.key('A')).value(QStringLiteral("writesTo")).toString() == QStringLiteral("file"));
    before = r.undo();
    REQUIRE(r.library.setFileMetadata(r.key('A'), dm, QStringLiteral("xmpDM"), QStringLiteral("cameraModel"), QStringLiteral("FX3")));
    CHECK(r.undo() == before + 1);
    CHECK(r.item('A').metadata.cameraModel == "FX3");
    CHECK(relinked == QStringList{r.key('A')});
    CHECK_FALSE(fs::exists(dir / "A.xmp"));
    const std::string after = readFile(mp4);
    CHECK(after.size() > original.size());
    CHECK(after.substr(0, original.size()) == original); // nothing before the new box moved
    CHECK(r.library.fileMetadata(r.key('A')).value(QStringLiteral("xmpSource")).toString() == QStringLiteral("embedded"));
    // Write XMP goes into the MP4 too; the WAV keeps a sidecar.
    CHECK(r.library.writeXmp({r.key('A'), r.key('B')}) == 2);
    CHECK(fs::exists(dir / "B.xmp"));
    CHECK_FALSE(fs::exists(dir / "A.xmp"));
    CHECK(r.library.fileMetadata(r.key('B')).value(QStringLiteral("writesTo")).toString() == QStringLiteral("sidecar"));

    // Status and the camera fields are columns and Metadata panel fields.
    QStringList ids;
    for (const QVariant& c : r.library.allColumns())
        ids << c.toMap().value(QStringLiteral("id")).toString();
    for (const char* id : {"status", "cameraAngle", "cameraLabel", "cameraModel", "shotDate", "shotLocation"})
        CHECK(ids.contains(QLatin1String(id)));
    REQUIRE(r.library.setMetadata({r.key('A')}, QStringLiteral("status"), QStringLiteral("Approved")));
    REQUIRE(r.library.setMetadata({r.key('A')}, QStringLiteral("shotLocation"), QStringLiteral("Harbour")));
    r.library.setColumnVisible(QStringLiteral("status"), true);
    r.library.setColumnVisible(QStringLiteral("cameraModel"), true);
    CHECK(r.library.cellText(r.key('A'), QStringLiteral("status")) == QStringLiteral("Approved"));
    CHECK(r.library.cellText(r.key('A'), QStringLiteral("cameraModel")) == QStringLiteral("FX3"));
    CHECK(r.library.clipMetadata(r.key('A')).value(QStringLiteral("shotLocation")).toString() == QStringLiteral("Harbour"));
    r.library.setFindText(QStringLiteral("Harbour"));
    CHECK(r.keys() == QStringList{r.key('A')});
    r.library.setFindText({});
    // What Add property offers.
    bool offered = false;
    for (const QVariant& v : r.library.xmpFieldChoices())
        offered = offered || v.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("xmpDM:shotLocation");
    CHECK(offered);
    r.library.setEmbedXmp(false);
}

TEST_CASE("Productions: several projects stay open, each with its own undo history and unsaved changes", "[app][production][open]") {
    resetSettings();
    const fs::path dir = scratch("open-several");
    const fs::path footage = dir / "interview.mov";
    writeFile(footage, std::string(512, 'i'));
    const fs::path first = dir / "First.wabcut";
    const fs::path second = dir / "Second.wabcut";
    REQUIRE(io::saveProject(projectPlaying(footage, "First cut"), first, io::SaveOptions{0}));
    REQUIRE(io::saveProject(projectPlaying(footage, "Second cut"), second, io::SaveOptions{0}));

    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    lifecycle.setLockProjects(true);
    REQUIRE(lifecycle.open(url(first)));
    const auto rename = [&](const std::string& name) {
        const MediaId media = controller.snapshot()->media.front()->id;
        return static_cast<bool>(controller.session().apply("Rename", [&](const ProjectState& s) {
            edit::MediaAttributeChange c;
            c.name = name;
            return edit::setMediaAttributes(s, std::vector<MediaId>{media}, c);
        }));
    };
    REQUIRE(rename("Interview"));
    CHECK(lifecycle.dirty());
    controller.setPlayhead(Ticks{controller.currentSequence()->duration.value / 2});
    const Ticks firstPlayhead = controller.playhead();

    // The second opens next to it: the first stays loaded (autosaved when left, still locked).
    REQUIRE(lifecycle.openAlongside(url(second)));
    REQUIRE(lifecycle.openProjects().size() == 2);
    CHECK(lifecycle.activeProject() == 1);
    CHECK(controller.snapshot()->sequences.front()->name == "Second cut");
    CHECK_FALSE(lifecycle.dirty());
    CHECK_FALSE(controller.session().canUndo());
    CHECK(lifecycle.openProjects()[0].toMap().value(QStringLiteral("dirty")).toBool());
    CHECK(fs::exists(io::autosavePath(first)));
    CHECK(fs::exists(editor::projectLockPath(first)));
    CHECK(fs::exists(editor::projectLockPath(second)));
    CHECK(lifecycle.firstDirtyProject() == 0);
    REQUIRE(rename("Second take"));

    // Back to the first at once: its unsaved change, its undo step, its playhead.
    REQUIRE(lifecycle.switchTo(0));
    CHECK(controller.snapshot()->media.front()->name == "Interview");
    CHECK(controller.snapshot()->sequences.front()->name == "First cut");
    CHECK(lifecycle.dirty());
    CHECK(controller.session().undoLabel() == "Rename");
    CHECK(controller.playhead() == firstPlayhead);
    CHECK(lifecycle.windowTitle().contains(QStringLiteral("First")));
    CHECK(lifecycle.openProjects()[1].toMap().value(QStringLiteral("dirty")).toBool());
    CHECK(fs::exists(io::autosavePath(second)));
    REQUIRE(controller.session().undo());
    CHECK_FALSE(lifecycle.dirty());
    CHECK(lifecycle.firstDirtyProject() == 1);

    // Already open: File > Open and Open next to this one switch to it (nothing is read again).
    REQUIRE(lifecycle.open(url(second)));
    CHECK(lifecycle.activeProject() == 1);
    CHECK(lifecycle.openProjects().size() == 2);
    CHECK(controller.snapshot()->media.front()->name == "Second take");
    REQUIRE(lifecycle.openAlongside(url(first)));
    CHECK(lifecycle.activeProject() == 0);
    CHECK(lifecycle.indexOfProject(url(second)) == 1);
    CHECK(lifecycle.openProjectState(second)->media.front()->name == "Second take");
    CHECK_FALSE(lifecycle.saveAs(url(second))); // never over a project open next to it

    // Not in the middle of a drag.
    controller.beginGesture(QStringLiteral("Drag"));
    CHECK_FALSE(lifecycle.switchTo(1));
    controller.endGesture();

    // A file that does not open: nothing changes.
    const ProjectSnapshot shown = controller.snapshot();
    CHECK_FALSE(lifecycle.openAlongside(url(dir / "missing.wabcut")));
    CHECK(lifecycle.openProjects().size() == 2);
    CHECK(lifecycle.activeProject() == 0);
    CHECK(controller.snapshot() == shown);
    CHECK(lifecycle.windowTitle().contains(QStringLiteral("First")));

    // Closing one the panels do not show: its lock and autosaves go.
    REQUIRE(lifecycle.closeProject(1));
    CHECK(lifecycle.openProjects().size() == 1);
    CHECK_FALSE(fs::exists(editor::projectLockPath(second)));
    CHECK_FALSE(fs::exists(io::autosavePath(second)));
    CHECK_FALSE(lifecycle.closeProject(0)); // the only one open
    // Closing the one shown: the other comes in.
    REQUIRE(lifecycle.openAlongside(url(second)));
    REQUIRE(lifecycle.closeProject(lifecycle.activeProject()));
    CHECK(lifecycle.activeProject() == 0);
    CHECK(controller.snapshot()->sequences.front()->name == "First cut");
    CHECK(fs::exists(editor::projectLockPath(first)));
    lifecycle.setLockProjects(false);
}

TEST_CASE("Productions: shared scratch disks and copies that know their source project", "[app][production][settings]") {
    resetSettings();
    const fs::path dir = scratch("production-settings");
    const fs::path footage = dir / "Footage" / "interview.wav";
    const fs::path graded = dir / "Footage" / "interview graded.wav";
    REQUIRE(writeWav(footage, 440.0));
    REQUIRE(writeWav(graded, 660.0));

    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    ProductionBridge production(&lifecycle);
    QStringList shownInLibrary;
    production.setShowItem([&](const QString& key) { shownInLibrary << key; });
    REQUIRE(production.makeProduction(url(dir / "Doc"), QStringLiteral("Doc")));
    CHECK(production.settings().value(QStringLiteral("previews")).toString().isEmpty());

    // Production Settings: a folder inside the production is kept relative; the media cache follows at once.
    const QString appCache = controller.mediaCacheDirectory();
    const fs::path cache = dir / "Fast" / "Media Cache";
    REQUIRE(production.setSettings({{QStringLiteral("previews"), url(dir / "Doc" / "Previews")}, {QStringLiteral("mediaCache"), url(cache)}}));
    CHECK(production.settings().value(QStringLiteral("previews")).toString() == QStringLiteral("Previews"));
    CHECK(fs::path(production.settings().value(QStringLiteral("mediaCache")).toString().toStdU16String()) == cache.lexically_normal());
    const QUrl assembly = production.newProject({}, QStringLiteral("Assembly"));
    REQUIRE(lifecycle.open(assembly));
    CHECK(fs::path(controller.mediaCacheDirectory().toStdU16String()).lexically_normal() == cache.lexically_normal());
    CHECK(editor::previewFolderFor(fs::path(assembly.toLocalFile().toStdU16String())) ==
          (dir / "Doc" / "Previews" / "Assembly Preview Files").lexically_normal());
    REQUIRE(production.setSettings({{QStringLiteral("mediaCache"), QString()}}));
    CHECK(controller.mediaCacheDirectory() == appCache);
    CHECK(production.settings().value(QStringLiteral("previews")).toString() == QStringLiteral("Previews")); // kept

    // Copies remember their source project and item.
    const fs::path interviews = dir / "Doc" / "Interviews.wabcut";
    REQUIRE(io::saveProject(projectPlaying(footage, "Interview cut", true), interviews, io::SaveOptions{0}));
    REQUIRE(production.browse(url(interviews)));
    QString browsedSequence;
    for (const QVariant& v : production.browseItems()) {
        if (v.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("sequence"))
            browsedSequence = v.toMap().value(QStringLiteral("key")).toString();
    }
    REQUIRE_FALSE(browsedSequence.isEmpty());
    REQUIRE(production.importItems({browsedSequence}).value(QStringLiteral("ok")).toBool());
    MediaId copyId;
    SequenceId copySequence;
    for (const MediaPtr& m : controller.snapshot()->media) {
        if (m->importSource) {
            copyId = m->id;
            CHECK(fs::path(pathFromUtf8(m->importSource->project)) == interviews.lexically_normal());
        }
    }
    for (const SequencePtr& q : controller.snapshot()->sequences) {
        if (q->importSource)
            copySequence = q->id;
    }
    REQUIRE(copyId.isValid());
    REQUIRE(copySequence.isValid());
    const QString copyKey = mediaKey(copyId);
    const QVariantMap source = production.sourceOf(copyKey);
    CHECK(source.value(QStringLiteral("projectName")).toString() == QStringLiteral("Interviews"));
    CHECK_FALSE(source.value(QStringLiteral("open")).toBool());
    CHECK(production.sourceOf(sequenceKey(copySequence)).value(QStringLiteral("key")).toString() == browsedSequence);
    CHECK(production.sourceOf(mediaKey(MediaId{987654})).isEmpty());

    // Reveal in Source Project, not open here: it is browsed, the item pointed at.
    production.stopBrowsing();
    QVariantMap revealed = production.revealSource(copyKey);
    CHECK(revealed.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(revealed.value(QStringLiteral("switched")).toBool());
    CHECK(production.browsingName() == QStringLiteral("Interviews"));
    const QString sourceKey = source.value(QStringLiteral("key")).toString();
    CHECK(production.revealedKey() == sourceKey);

    // The source project, open next to this one, relinks its media to the graded file (not saved yet).
    REQUIRE(lifecycle.openAlongside(url(interviews)));
    REQUIRE(lifecycle.relink(sourceKey, url(graded), true));
    REQUIRE(lifecycle.switchTo(0));
    // The copy is offered the same relink, read from the source as it is in memory; one undo step.
    const QVariantList offers = production.relinkOffers();
    REQUIRE(offers.size() == 1);
    CHECK(offers.front().toMap().value(QStringLiteral("key")).toString() == copyKey);
    CHECK(fs::path(offers.front().toMap().value(QStringLiteral("to")).toString().toStdU16String()) == graded.lexically_normal());
    const auto undo = controller.session().undoCount();
    CHECK(production.applyRelinkOffers() == 1);
    CHECK(controller.session().undoCount() == undo + 1);
    CHECK(fs::path(pathFromUtf8(controller.snapshot()->findMedia(copyId)->locator.path)) == graded.lexically_normal());
    CHECK(production.relinkOffers().isEmpty());

    // Reveal again: the source is open now, so it comes into the panels with the item shown.
    revealed = production.revealSource(copyKey);
    CHECK(revealed.value(QStringLiteral("switched")).toBool());
    CHECK(lifecycle.activeProject() == 1);
    CHECK(shownInLibrary == QStringList{sourceKey});
    // The production lists both as open.
    int open = 0;
    for (const QVariant& e : production.entries())
        open += e.toMap().value(QStringLiteral("open")).toBool() ? 1 : 0;
    CHECK(open == 2);
}

TEST_CASE("Shell: the Freeform view, the Metadata panel and the Production panel load", "[app][library][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(library);
    REQUIRE(controller);
    library->setPersistent(false);

    library->setView(QStringLiteral("freeform"));
    spin(50);
    QQuickItem* freeform = findNamed(shell.window->contentItem(), QStringLiteral("libraryFreeform"));
    REQUIRE(freeform);
    CHECK(freeform->isVisible());
    const MediaPtr first = controller->snapshot()->media.front();
    const QString key = mediaKey(first->id);
    REQUIRE(waitFor([&] { return findNamed(freeform, QStringLiteral("libraryFreeformCell_") + key) != nullptr; }));

    // The Metadata panel shows the selection and edits it.
    library->select(key, LibraryBridge::Replace);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("metadata"))));
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] { return (panel = findNamed(shell.window->contentItem(), QStringLiteral("metadataPanel"))) != nullptr; }));
    QQuickItem* scene = nullptr;
    REQUIRE(waitFor([&] { return (scene = findNamed(panel, QStringLiteral("metadata_sceneField"))) != nullptr; }));
    scene->setProperty("text", QStringLiteral("42"));
    QMetaObject::invokeMethod(scene, "accepted");
    REQUIRE(waitFor([&] { return controller->snapshot()->findMedia(first->id)->metadata.scene == "42"; }));

    // The File section: its rows edit the sidecar, Add property is there.
    REQUIRE(waitFor([&] { return findNamed(panel, QStringLiteral("metadataAddProperty")) != nullptr; }));

    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("production"))));
    QQuickItem* productionPanel = nullptr;
    REQUIRE(waitFor([&] {
        return (productionPanel = findNamed(shell.window->contentItem(), QStringLiteral("productionPanel"))) != nullptr;
    }));

    // Several projects open: Main.qml opens one next to the other, the panel lists both, and closing one with unsaved
    // changes asks first (Discard closes it; the other comes into the panels).
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(file);
    const fs::path dir = scratch("shell-open");
    const fs::path footage = dir / "clip.mov";
    writeFile(footage, std::string(256, 'c'));
    REQUIRE(io::saveProject(projectPlaying(footage, "One"), dir / "One.wabcut", io::SaveOptions{0}));
    REQUIRE(io::saveProject(projectPlaying(footage, "Two"), dir / "Two.wabcut", io::SaveOptions{0}));
    REQUIRE(file->open(url(dir / "One.wabcut")));
    QMetaObject::invokeMethod(shell.window.get(), "openProjectFile", Q_ARG(QVariant, QVariant(url(dir / "Two.wabcut"))));
    REQUIRE(file->openProjects().size() == 2);
    REQUIRE(waitFor([&] { return findNamed(productionPanel, QStringLiteral("openProjectClose_1")) != nullptr; }));
    const MediaId media = controller->snapshot()->media.front()->id;
    REQUIRE(controller->session().apply("Rename", [&](const ProjectState& s) {
        edit::MediaAttributeChange c;
        c.name = "Renamed";
        return edit::setMediaAttributes(s, std::vector<MediaId>{media}, c);
    }));
    REQUIRE(file->dirty());
    QMetaObject::invokeMethod(shell.window.get(), "closeOpenProject", Q_ARG(QVariant, QVariant(1)));
    QObject* unsaved = shell.object("unsavedChangesDialog");
    REQUIRE(unsaved);
    REQUIRE(waitFor([&] { return unsaved->property("visible").toBool(); }));
    CHECK(file->openProjects().size() == 2); // nothing closed before the answer
    QMetaObject::invokeMethod(unsaved, "discardChosen");
    QMetaObject::invokeMethod(unsaved, "close");
    CHECK(file->openProjects().size() == 1);
    CHECK(controller->snapshot()->sequences.front()->name == "One");

    // Quitting asks about every open project with unsaved changes in turn, the one in the panels first; Cancel stops.
    const auto renameFirst = [&](const char* name) {
        const MediaId m = controller->snapshot()->media.front()->id;
        return controller->session().apply("Rename", [&](const ProjectState& s) {
            edit::MediaAttributeChange c;
            c.name = name;
            return edit::setMediaAttributes(s, std::vector<MediaId>{m}, c);
        });
    };
    const auto readyToClose = [&] {
        QVariant ready;
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "readyToClose", Q_RETURN_ARG(QVariant, ready)));
        return ready.toBool();
    };
    REQUIRE(renameFirst("One changed"));
    QMetaObject::invokeMethod(shell.window.get(), "openProjectFile", Q_ARG(QVariant, QVariant(url(dir / "Two.wabcut"))));
    REQUIRE(file->openProjects().size() == 2);
    REQUIRE(renameFirst("Two changed"));
    REQUIRE(file->activeProject() == 1);
    CHECK_FALSE(readyToClose());
    REQUIRE(waitFor([&] { return unsaved->property("visible").toBool(); }));
    CHECK(file->activeProject() == 1); // the one shown first
    QMetaObject::invokeMethod(unsaved, "discardChosen");
    QMetaObject::invokeMethod(unsaved, "close");
    // Two is closed; One (unsaved) is the next question.
    CHECK(file->openProjects().size() == 1);
    CHECK(controller->snapshot()->sequences.front()->name == "One");
    CHECK_FALSE(readyToClose());
    REQUIRE(waitFor([&] { return unsaved->property("visible").toBool(); }));
    QMetaObject::invokeMethod(unsaved, "close"); // Cancel: nothing closes, the changes stay
    CHECK(file->dirty());
    CHECK(file->openProjects().size() == 1);
    REQUIRE(controller->session().undo());
    CHECK_FALSE(file->dirty());
    CHECK(readyToClose()); // nothing unsaved left
    library->setView(QStringLiteral("grid"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.filter(QStringLiteral("qml")).isEmpty());
}
