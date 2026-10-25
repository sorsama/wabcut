// 0.2 media formats in the app, offscreen; no window is shown:
//   * Field Options through the Editing bridge (one undo step) and its dialog from the clip menu's command;
//   * Import Layered File through the Library (merged, individual layers, sequence) and its dialog for dropped PSDs;
//   * the Media Browser: the listing of a folder with GoPro chapters (one clip), importing it, and the panel in the shell;
//     a clip shown in the Source Monitor before it is imported, the Ingest settings (Copy, Transcode, Create Proxies),
//     the metadata view and the hover-scrub strip (generated clips: tools/gen_test_media.py).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "../media/PsdWriter.h"

#include "BridgeIds.h"
#include "EditingBridge.h"
#include "ExportController.h"
#include "LibraryBridge.h"
#include "MediaBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SourceMonitorBridge.h"

#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/model/SpannedClip.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QScopeGuard>
#include <QSettings>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUrl>

#include <filesystem>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

std::filesystem::path writeLowerThird(const QTemporaryDir& dir) {
    using media::test::TestPsdLayer;
    std::vector<TestPsdLayer> layers(3);
    layers[0].name = "Backdrop";
    layers[0].width = 64;
    layers[0].height = 32;
    layers[1].name = "Logo";
    layers[1].left = 40;
    layers[1].top = 4;
    layers[1].width = 16;
    layers[1].height = 8;
    layers[2].name = "Notes";
    layers[2].hidden = true;
    const std::filesystem::path file = std::filesystem::path(dir.path().toStdWString()) / "Lower third.psd";
    media::test::writeFile(file, media::test::writePsd(64, 32, 8, layers));
    return file;
}

std::filesystem::path testMedia(const char* name) { return std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name; }

bool haveBrowserMedia() {
    return std::filesystem::is_regular_file(testMedia("GX010042.MP4")) &&
           std::filesystem::is_regular_file(testMedia("GX020042.MP4"));
}

QString qpath(const std::filesystem::path& p) { return QDir::fromNativeSeparators(QString::fromStdWString(p.wstring())); }

/// A folder to browse: a GoPro card's DCIM folder with one recording in two chapters (1 s each, 640x360 30 fps, AAC
/// stereo), a single clip beside it, and a text file.
struct BrowserFolder {
    QTemporaryDir dir;
    std::filesystem::path root;

    BrowserFolder() {
        REQUIRE(dir.isValid());
        root = std::filesystem::path(dir.path().toStdWString());
        std::filesystem::create_directories(root / "DCIM");
        std::filesystem::copy_file(testMedia("GX010042.MP4"), root / "DCIM" / "GX010042.MP4");
        std::filesystem::copy_file(testMedia("GX020042.MP4"), root / "DCIM" / "GX020042.MP4");
        std::filesystem::copy_file(testMedia("GX010042.MP4"), root / "interview.mp4");
        QFile notes(qpath(root / "notes.txt"));
        REQUIRE(notes.open(QIODevice::WriteOnly));
        notes.write("not media");
    }
    QString chapters() const { return qpath(root / "DCIM" / "GX010042.MP4"); }
    QString single() const { return qpath(root / "interview.mp4"); }
    QString notes() const { return qpath(root / "notes.txt"); }
};

/// The Library, Source Monitor and project of the Media Browser tests (no QML).
struct BrowserRig {
    Timeline tl{R"(
        V1 tgt |Aaaa|
    )"};
    ProjectController controller;
    SourceMonitorBridge source;
    LibraryBridge library;

    BrowserRig() {
        controller.resetProject(tl.state, tl.seq);
        source.setController(&controller);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setController(&controller);
        library.setSourceMonitor(&source);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    const MediaItem* item(const QString& key) const {
        const auto id = parseMediaKey(key);
        return id ? controller.snapshot()->findMedia(*id) : nullptr;
    }
};

} // namespace

TEST_CASE("Field Options: the Editing bridge sets them on the selected video clip in one undo step", "[app][fields]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    EditingBridge editing;
    editing.setController(&controller);
    const QString video = clipKey(tl.clip("V1@0"));
    const QVariantMap info = editing.fieldOptionsInfo(video);
    CHECK(info.value(QStringLiteral("available")).toBool());
    CHECK(info.value(QStringLiteral("processing")).toString() == QStringLiteral("none"));
    CHECK(info.value(QStringLiteral("fieldOrder")).toString() == QStringLiteral("progressive"));
    CHECK_FALSE(editing.fieldOptionsInfo(clipKey(tl.clip("A1@0"))).value(QStringLiteral("available")).toBool());
    const std::size_t before = controller.session().undoCount();
    controller.selectClip(video);
    REQUIRE(editing.applyFieldOptions(video, {{QStringLiteral("reverseFieldDominance"), true},
                                              {QStringLiteral("processing"), QStringLiteral("alwaysDeinterlace")}})
                .isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    const ClipFieldOptions& options = controller.currentSequence()->findClip(tl.clip("V1@0"))->clip().content().fieldOptions;
    CHECK(options.reverseFieldDominance);
    CHECK(options.processing == FieldProcessing::AlwaysDeinterlace);
    CHECK(EditingBridge::handlesCommand(QStringLiteral("clip.fieldOptions")));
}

TEST_CASE("Import Layered File: the Library imports a PSD's layers as one picture, a bin or a sequence", "[app][library][psd]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    LibraryBridge library;
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setController(&controller);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl psd = QUrl::fromLocalFile(QString::fromStdWString(writeLowerThird(dir).wstring()));
    CHECK(library.isLayeredFile(psd));
    CHECK_FALSE(library.isLayeredFile(QUrl::fromLocalFile(QStringLiteral("C:/clip.mp4"))));
    const QVariantMap info = library.layeredFileInfo(psd);
    REQUIRE(info.value(QStringLiteral("layers")).toList().size() == 3);
    CHECK(info.value(QStringLiteral("width")).toInt() == 64);

    const std::size_t before = controller.session().undoCount();
    const QString merged = library.importLayeredFile(psd, {{QStringLiteral("as"), QStringLiteral("mergeAll")}});
    REQUIRE_FALSE(merged.isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(library.isSelected(merged));
    const QString bin = library.importLayeredFile(
        psd, {{QStringLiteral("as"), QStringLiteral("sequence")}, {QStringLiteral("footage"), QStringLiteral("layer")}});
    REQUIRE_FALSE(bin.isEmpty());
    CHECK(controller.session().undoCount() == before + 2);
    CHECK(controller.snapshot()->sequences.size() == 2); // the timeline's and the layers' sequence
    // A missing file: a message, nothing changes.
    CHECK(library.importLayeredFile(QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.psd"))), {}).isEmpty());
    CHECK_FALSE(library.message().isEmpty());
    CHECK(controller.session().undoCount() == before + 2);
}

TEST_CASE("Media Browser: GoPro chapters list as one clip; folders first", "[app][library][media-browser]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    LibraryBridge library;
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setController(&controller);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    REQUIRE(QDir(dir.path()).mkpath(QStringLiteral("DCIM")));
    for (const char* name : {"GX010042.MP4", "GX020042.MP4", "interview.mov"}) {
        QFile f(dir.filePath(QString::fromLatin1(name)));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not really media");
    }
    const QVariantList entries = library.browseFolder(dir.path());
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].toMap().value(QStringLiteral("folder")).toBool());
    // 0.4: every row has its file URL (folder rows import through ProjectFile.importFiles).
    CHECK(entries[0].toMap().value(QStringLiteral("url")).toUrl() ==
          QUrl::fromLocalFile(entries[0].toMap().value(QStringLiteral("path")).toString()));
    const QVariantMap gopro = entries[1].toMap();
    CHECK(gopro.value(QStringLiteral("name")).toString() == QStringLiteral("GX010042"));
    CHECK(gopro.value(QStringLiteral("spanned")).toBool());
    CHECK(gopro.value(QStringLiteral("files")).toInt() == 2);
    CHECK(gopro.value(QStringLiteral("thumbnail")).toString().startsWith(QStringLiteral("image://")));
    CHECK(library.parentFolder(dir.path()) == QDir::fromNativeSeparators(QFileInfo(dir.path()).absolutePath()));
    CHECK_FALSE(library.browserPlaces().isEmpty());
    // Not media: the import says why and changes nothing.
    const std::size_t before = controller.session().undoCount();
    CHECK(library.importCameraClips({gopro.value(QStringLiteral("path")).toString()}).isEmpty());
    CHECK_FALSE(library.message().isEmpty());
    CHECK(controller.session().undoCount() == before);
}

TEST_CASE("Media formats in the shell: the Field Options dialog, Import Layered File and the Media Browser panel",
          "[app][qml][media-browser]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* editing = shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
    REQUIRE(controller);
    REQUIRE(editing);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const auto opened = [&](const char* name) {
        QObject* dialog = nullptr;
        const bool open = waitFor([&] {
            dialog = shell.window->findChild<QObject*>(QString::fromLatin1(name));
            return dialog && dialog->property("opened").toBool();
        }, 5000);
        INFO(name << ": " << join(takeMessages()));
        REQUIRE(open);
        return dialog;
    };
    // Clip > Video Options > Field Options...
    controller->selectClip(clipKey(tl.clip("V1@0")));
    REQUIRE(editing->runCommand(QStringLiteral("clip.fieldOptions")));
    QObject* fields = opened("fieldOptionsDialog");
    fields->setProperty("processing", QStringLiteral("flickerRemoval"));
    REQUIRE(QMetaObject::invokeMethod(fields, "apply"));
    CHECK(controller->currentSequence()->findClip(tl.clip("V1@0"))->clip().content().fieldOptions.processing ==
          FieldProcessing::FlickerRemoval);
    // A dropped PSD opens Import Layered File.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl psd = QUrl::fromLocalFile(QString::fromStdWString(writeLowerThird(dir).wstring()));
    QVariant ignored;
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "importUrls", Q_RETURN_ARG(QVariant, ignored),
                                      Q_ARG(QVariant, QVariant(QVariantList{QVariant(psd)}))));
    QObject* layered = opened("importLayeredFileDialog");
    layered->setProperty("importAs", QStringLiteral("layers"));
    const std::size_t before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(layered, "apply"));
    CHECK(layered->property("errorText").toString().isEmpty());
    CHECK(controller->session().undoCount() == before + 1);
    // Window > Media Browser (Shift+8).
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("window.mediaBrowser"))));
    CHECK(waitFor([&] { return shell.window->findChild<QObject*>(QStringLiteral("mediaBrowserPanel")) != nullptr; }, 5000));
    QObject* browser = shell.window->findChild<QObject*>(QStringLiteral("mediaBrowserPanel"));
    REQUIRE(browser);
    // Ingest Settings.
    QObject* ingest = browser->findChild<QObject*>(QStringLiteral("mediaBrowserIngestPopover"));
    REQUIRE(ingest);
    REQUIRE(QMetaObject::invokeMethod(ingest, "open"));
    CHECK(waitFor([&] { return ingest->property("opened").toBool(); }, 5000));
    REQUIRE(QMetaObject::invokeMethod(ingest, "close"));
    if (haveBrowserMedia()) {
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
        REQUIRE(library);
        REQUIRE(source);
        BrowserFolder folder;
        REQUIRE(QMetaObject::invokeMethod(browser, "openFolder", Q_ARG(QVariant, qpath(folder.root))));
        const QVariantList entries = browser->property("entries").toList();
        int clip = -1;
        for (int i = 0; i < entries.size(); ++i) {
            if (entries[i].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("interview"))
                clip = i;
        }
        REQUIRE(clip >= 0);
        QObject* list = browser->findChild<QObject*>(QStringLiteral("mediaBrowserList"));
        REQUIRE(list);
        list->setProperty("currentIndex", clip);
        // The metadata view of the current clip.
        browser->setProperty("showMetadata", true);
        QObject* metadata = browser->findChild<QObject*>(QStringLiteral("mediaBrowserMetadata"));
        REQUIRE(metadata);
        CHECK(waitFor([&] { return metadata->property("visible").toBool(); }, 5000));
        CHECK(browser->property("metadata").toMap().value(QStringLiteral("video")).toMap().value(QStringLiteral("width")).toInt() == 640);
        // Double-click: the Source Monitor shows it, not imported.
        const std::size_t items = controller->snapshot()->media.size();
        REQUIRE(QMetaObject::invokeMethod(browser, "activate", Q_ARG(QVariant, entries[clip])));
        CHECK(source->preview());
        CHECK(controller->snapshot()->media.size() == items);
        // Import with Ingest > Create Proxies: the proxies of the clips imported are made.
        library->setIngest(true);
        library->setIngestAction(QStringLiteral("proxies"));
        REQUIRE(QMetaObject::invokeMethod(browser, "importSelected"));
        CHECK(controller->snapshot()->media.size() == items + 1);
        CHECK(library->message() == QStringLiteral("Creating proxies of 1 clip(s)."));
        library->setIngest(false);
        source->unload();
    }
    INFO(join(takeMessages()));
    resetSettings();
}

TEST_CASE("Media Browser: a clip plays in the Source Monitor before it is imported and its first mark imports it",
          "[app][library][media-browser][source]") {
    if (!haveBrowserMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    BrowserFolder folder;
    BrowserRig r;
    const std::size_t media = r.controller.snapshot()->media.size();
    const std::size_t before = r.undo();
    // Shown, not imported: the project does not change.
    REQUIRE(r.source.loadFile(folder.chapters()));
    CHECK(r.source.loaded());
    CHECK(r.source.preview());
    CHECK(r.source.mediaKeyText().isEmpty());
    CHECK(r.source.name() == QStringLiteral("GX010042"));
    CHECK(r.source.kind() == QStringLiteral("video"));
    CHECK(r.source.recent().isEmpty());
    CHECK(r.controller.snapshot()->media.size() == media);
    CHECK(r.undo() == before);
    // The first mark imports it (one undo step: both chapters as one clip) and goes on with the item (the mark: another).
    r.source.markIn();
    CHECK_FALSE(r.source.preview());
    const MediaItem* item = r.item(r.source.mediaKeyText());
    REQUIRE(item);
    CHECK(item->name == "GX010042");
    CHECK(parseSpannedClipPath(item->locator.path).has_value());
    CHECK(item->markIn.has_value());
    CHECK(r.controller.snapshot()->media.size() == media + 1);
    CHECK(r.undo() == before + 2);
    CHECK(r.source.recent().size() == 1);
    // What is not media is not shown (the imported item stays loaded).
    CHECK_FALSE(r.source.loadFile(folder.notes()));
    CHECK_FALSE(r.source.message().isEmpty());
    CHECK(r.source.mediaKeyText() == mediaKey(item->id));
}

TEST_CASE("Media Browser Ingest: Copy needs a folder and imports the copies; the metadata view reads the clip",
          "[app][library][media-browser][ingest]") {
    if (!haveBrowserMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    BrowserFolder folder;
    BrowserRig r;
    const std::size_t before = r.undo();
    // Off: imports in place.
    CHECK_FALSE(r.library.ingest());
    r.library.setIngest(true);
    r.library.setIngestAction(QStringLiteral("bogus"));
    CHECK(r.library.ingestAction() == QStringLiteral("copy"));
    // Copy without a folder is refused.
    CHECK(r.library.importFromBrowser({folder.single()}).isEmpty());
    CHECK_FALSE(r.library.message().isEmpty());
    CHECK(r.undo() == before);
    QTemporaryDir to;
    REQUIRE(to.isValid());
    r.library.setIngestFolder(QUrl::fromLocalFile(to.path()).toString());
    CHECK(r.library.ingestFolder() == QDir::fromNativeSeparators(to.path()));
    const QString key = r.library.importFromBrowser({folder.single()});
    INFO(r.library.message().toStdString());
    REQUIRE_FALSE(key.isEmpty());
    CHECK(r.undo() == before + 1);
    const MediaItem* item = r.item(key);
    REQUIRE(item);
    // Into a folder of the clip's own (copyCameraClip), as a card's clip keeps its files together.
    const std::filesystem::path copied = std::filesystem::path(to.path().toStdWString()) / "interview" / "interview.mp4";
    CHECK(std::filesystem::is_regular_file(copied));
    CHECK(pathFromUtf8(item->locator.path).lexically_normal() == copied.lexically_normal());

    // The metadata view: what Probe reads, nothing imported.
    const QVariantMap info = r.library.browserFileInfo(folder.chapters());
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("GX010042"));
    CHECK(info.value(QStringLiteral("files")).toInt() == 2);
    CHECK(info.value(QStringLiteral("seconds")).toDouble() == Catch::Approx(2.0).margin(0.1));
    const QVariantMap video = info.value(QStringLiteral("video")).toMap();
    CHECK(video.value(QStringLiteral("codec")).toString() == QStringLiteral("h264"));
    CHECK(video.value(QStringLiteral("width")).toInt() == 640);
    CHECK(video.value(QStringLiteral("height")).toInt() == 360);
    CHECK(video.value(QStringLiteral("frameRate")).toDouble() == Catch::Approx(30.0));
    CHECK(video.value(QStringLiteral("fieldOrder")).toString() == QStringLiteral("progressive"));
    const QVariantList audio = info.value(QStringLiteral("audio")).toList();
    REQUIRE(audio.size() == 1);
    CHECK(audio.front().toMap().value(QStringLiteral("channels")).toInt() == 2);
    CHECK(audio.front().toMap().value(QStringLiteral("sampleRate")).toInt() == 48000);
    CHECK(r.undo() == before + 1);
    CHECK(r.library.browserFileInfo(folder.notes()).isEmpty());
    CHECK_FALSE(r.library.message().isEmpty());
}

TEST_CASE("Media Browser Ingest: Transcode imports the original and switches it to its transcoded file",
          "[app][library][media-browser][ingest]") {
    if (!haveBrowserMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    BrowserFolder folder;
    BrowserRig r;
    ExportController queue;
    queue.setProject(&r.controller);
    queue.setVideoSourceFactory([](const ProjectState&, const Sequence&, const exporter::VideoSourceFormat& format)
                                       -> std::expected<std::unique_ptr<exporter::IVideoFrameSource>, Error> {
        return exporter::makeSyntheticVideoSource(format);
    });
    r.library.setExport(&queue);
    QTemporaryDir to;
    REQUIRE(to.isValid());
    r.library.setIngest(true);
    r.library.setIngestAction(QStringLiteral("transcode"));
    r.library.setIngestFolder(to.path());
    r.library.setIngestPreset(QStringLiteral("prores-422-hq"));
    const std::size_t before = r.undo();
    const QString key = r.library.importFromBrowser({folder.single()});
    INFO(r.library.message().toStdString());
    REQUIRE_FALSE(key.isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.library.pendingTranscodes() == 1);
    // The original plays until the export queue has made the new file.
    CHECK(pathFromUtf8(r.item(key)->locator.path).filename() == std::filesystem::path("interview.mp4"));
    REQUIRE(waitFor([&] { return r.library.pendingTranscodes() == 0; }, 120000));
    INFO(r.library.message().toStdString());
    const std::filesystem::path made = std::filesystem::path(to.path().toStdWString()) / "interview.mov";
    CHECK(std::filesystem::is_regular_file(made));
    CHECK(pathFromUtf8(r.item(key)->locator.path).lexically_normal() == made.lexically_normal());
    CHECK(r.item(key)->info.video.front().codec == "prores");
    CHECK(r.undo() == before + 2); // Use Transcoded Media
    r.controller.session().undo();
    CHECK(pathFromUtf8(r.item(key)->locator.path).filename() == std::filesystem::path("interview.mp4"));
}

TEST_CASE("Media Browser hover scrub: a clip's strip is ten of its frames side by side", "[app][library][media-browser]") {
    if (!haveBrowserMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    BrowserFolder folder;
    BrowserRig r;
    const QVariantList entries = r.library.browseFolder(qpath(folder.root));
    QVariantMap clip;
    for (const QVariant& e : entries) {
        if (e.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("interview"))
            clip = e.toMap();
    }
    REQUIRE_FALSE(clip.isEmpty());
    const QString thumbnail = clip.value(QStringLiteral("thumbnail")).toString();
    const QString strip = clip.value(QStringLiteral("strip")).toString();
    CHECK(strip == thumbnail + QStringLiteral("/s10"));
    CHECK(r.library.browserStripFrames() == 10);
    QTemporaryDir cache;
    REQUIRE(cache.isValid());
    ThumbnailImageProvider provider(std::filesystem::path(cache.path().toStdWString()));
    const auto image = [&](const QString& url) {
        const QString prefix = QStringLiteral("image://%1/").arg(QLatin1String(ThumbnailImageProvider::kProviderId));
        std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(url.mid(prefix.size()), QSize(0, 36)));
        bool done = false;
        QObject::connect(response.get(), &QQuickImageResponse::finished, [&] { done = true; });
        const bool finished = waitFor([&] { return done; }, 60000);
        INFO(response->errorString().toStdString());
        REQUIRE(finished);
        std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
        return factory ? factory->image() : QImage();
    };
    const QImage one = image(thumbnail);
    const QImage frames = image(strip);
    REQUIRE_FALSE(one.isNull());
    REQUIRE_FALSE(frames.isNull());
    CHECK(one.height() == 36);
    CHECK(frames.height() == 36);
    CHECK(frames.width() == 10 * one.width());
    // A still has one picture: its strip repeats it, so the tiles still divide the strip evenly.
    QImage picture(64, 36, QImage::Format_RGB32);
    picture.fill(Qt::green);
    const QString still = QDir(cache.path()).filePath(QStringLiteral("still.png"));
    REQUIRE(picture.save(still));
    const QImage stillStrip = image(ThumbnailImageProvider::stripUrlFor(QDir::toNativeSeparators(still).toStdString(), 10));
    REQUIRE_FALSE(stillStrip.isNull());
    CHECK(stillStrip.width() == 640);
    CHECK(stillStrip.pixelColor(600, 18).green() > 200);
}

// 0.4: the Media Browser's places: Videos and Home, the game capture tools' folders that exist (an OBS profile's
// recording folder here, from a fake OBS config), the folders the last dialog imports came from, then the drives.
TEST_CASE("Media Browser: places list game capture folders and recent import folders", "[app][library][media-browser]") {
    QSettings().remove(QStringLiteral("import"));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString recordings = dir.filePath(QStringLiteral("obsrec"));
    REQUIRE(QDir().mkpath(recordings));
    const QString config = dir.filePath(QStringLiteral("config"));
    const QString profile = config + QStringLiteral("/obs-studio/basic/profiles/Untitled");
    REQUIRE(QDir().mkpath(profile));
    {
        QFile ini(profile + QStringLiteral("/basic.ini"));
        REQUIRE(ini.open(QIODevice::WriteOnly));
        ini.write("[General]\nName=Untitled\n\n[SimpleOutput]\nFilePath=");
        ini.write(QDir::toNativeSeparators(recordings).toUtf8());
        ini.write("\n");
    }
    // OBS's config folder comes from APPDATA (Windows) or XDG_CONFIG_HOME (Linux): both point at the fake one.
    const bool hadAppData = qEnvironmentVariableIsSet("APPDATA");
    const QByteArray appData = qgetenv("APPDATA");
    const bool hadXdg = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    qputenv("APPDATA", QDir::toNativeSeparators(config).toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", QDir::toNativeSeparators(config).toLocal8Bit());
    const auto restore = qScopeGuard([&] {
        if (hadAppData)
            qputenv("APPDATA", appData);
        else
            qunsetenv("APPDATA");
        if (hadXdg)
            qputenv("XDG_CONFIG_HOME", xdg);
        else
            qunsetenv("XDG_CONFIG_HOME");
        QSettings().remove(QStringLiteral("import"));
    });

    LibraryBridge library;
    library.setPersistent(false);
    const auto find = [](const QVariantList& places, const QString& name) {
        for (const QVariant& v : places) {
            if (v.toMap().value(QStringLiteral("name")).toString() == name)
                return v.toMap();
        }
        return QVariantMap{};
    };
    const auto indexOf = [](const QVariantList& places, const QString& kind) {
        for (qsizetype i = 0; i < places.size(); ++i) {
            if (places[i].toMap().value(QStringLiteral("kind")).toString() == kind)
                return i;
        }
        return qsizetype{-1};
    };
    QVariantList places = library.browserPlaces();
    REQUIRE_FALSE(places.isEmpty());
    CHECK(places.front().toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("folder"));
#ifndef Q_OS_MACOS
    const QVariantMap obs = find(places, QStringLiteral("OBS recordings"));
    REQUIRE_FALSE(obs.isEmpty());
    CHECK(QDir::cleanPath(obs.value(QStringLiteral("path")).toString()) == QDir::cleanPath(recordings));
    CHECK(obs.value(QStringLiteral("kind")).toString() == QStringLiteral("capture"));
#endif

    // A dialog import's folder is listed as a recent place, after the capture folders and before the drives.
    REQUIRE(QDir().mkpath(dir.filePath(QStringLiteral("a"))));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    lifecycle.rememberImportFolder({QUrl::fromLocalFile(dir.filePath(QStringLiteral("a/x.mp4")))});
    places = library.browserPlaces();
    const QVariantMap recent = find(places, QStringLiteral("Recent: a"));
    REQUIRE_FALSE(recent.isEmpty());
    CHECK(recent.value(QStringLiteral("kind")).toString() == QStringLiteral("recent"));
    CHECK(QDir::cleanPath(recent.value(QStringLiteral("path")).toString()) == QDir::cleanPath(dir.filePath(QStringLiteral("a"))));
    const qsizetype recentAt = indexOf(places, QStringLiteral("recent"));
    CHECK(recentAt > indexOf(places, QStringLiteral("folder")));
#ifndef Q_OS_MACOS
    CHECK(recentAt > indexOf(places, QStringLiteral("capture")));
#endif
    if (const qsizetype driveAt = indexOf(places, QStringLiteral("drive")); driveAt >= 0)
        CHECK(recentAt < driveAt);

    // Fix round 1 (D): an import from a drive's root (an SD card, a USB stick) is no "Recent: " row without a name:
    // the drive is listed anyway.
    const QString root = QStorageInfo(dir.path()).rootPath();
    REQUIRE_FALSE(root.isEmpty());
    lifecycle.rememberImportFolder({QUrl::fromLocalFile(QDir(root).filePath(QStringLiteral("clip.mp4")))});
    places = library.browserPlaces();
    QStringList names;
    int rootRows = 0;
    for (const QVariant& v : places) {
        const QVariantMap p = v.toMap();
        names << p.value(QStringLiteral("name")).toString();
        if (QDir::cleanPath(p.value(QStringLiteral("path")).toString()).compare(QDir::cleanPath(root), Qt::CaseInsensitive) == 0) {
            ++rootRows;
            CHECK(p.value(QStringLiteral("kind")).toString() == QStringLiteral("drive"));
        }
    }
    INFO(names.join(QStringLiteral(" | ")).toStdString());
    CHECK_FALSE(names.contains(QStringLiteral("Recent: ")));
    CHECK(rootRows == 1);
    CHECK_FALSE(find(places, QStringLiteral("Recent: a")).isEmpty()); // the folder before is still there
}

namespace {

/// A Media Browser panel made in the shell's engine (its own Component.onCompleted reads where to open), over the
/// window's content.
std::unique_ptr<QQuickItem> makeBrowser(Shell& shell, const QByteArray& panel = "MediaBrowserPanel {}") {
    QQmlComponent component(&shell.engine);
    component.setData("import Wabcut\n" + panel + "\n", QUrl(QStringLiteral("qrc:/d04-browser/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setSize(QSizeF(420, 600));
    return std::unique_ptr<QQuickItem>(item);
}

/// Runs `code` in Main.qml's context (its ids: docking, window, ...) and returns its value.
QVariant evaluateInWindow(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

/// Maximize Frame on the Media Browser's panel group (or back): its header has room for its tools beside the tabs.
void toggleMaximizeMediaBrowser(const Shell& shell) {
    evaluateInWindow(shell, QStringLiteral("docking.toggleMaximize(docking.groupOfPanel('mediaBrowser').group)"));
    spin(100);
}

QQuickItem* findItemNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItemNamed(child, name))
            return found;
    }
    return nullptr;
}

} // namespace

// 0.4: the Media Browser opens at the folder it showed last (Preferences.mediaBrowserLastFolder) while that folder is
// there, else at the first place (Videos); its places menu lists the capture tools' folders.
TEST_CASE("Media Browser: it reopens the folder it showed last", "[app][library][media-browser][qml][screenshots]") {
    resetSettings();
    clearMessages();
    QSettings().remove(QStringLiteral("import"));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString clips = dir.filePath(QStringLiteral("Clips"));
    REQUIRE(QDir().mkpath(clips + QStringLiteral("/Session 1")));
    // A fake OBS profile, so the places menu has an "OBS recordings" row (as in the places test above).
    const QString recordings = dir.filePath(QStringLiteral("obsrec"));
    REQUIRE(QDir().mkpath(recordings));
    const QString config = dir.filePath(QStringLiteral("config"));
    const QString profile = config + QStringLiteral("/obs-studio/basic/profiles/Untitled");
    REQUIRE(QDir().mkpath(profile));
    {
        QFile ini(profile + QStringLiteral("/basic.ini"));
        REQUIRE(ini.open(QIODevice::WriteOnly));
        ini.write("[General]\nName=Untitled\n\n[SimpleOutput]\nFilePath=");
        ini.write(QDir::toNativeSeparators(recordings).toUtf8());
        ini.write("\n");
    }
    const bool hadAppData = qEnvironmentVariableIsSet("APPDATA");
    const QByteArray appData = qgetenv("APPDATA");
    const bool hadXdg = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    qputenv("APPDATA", QDir::toNativeSeparators(config).toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", QDir::toNativeSeparators(config).toLocal8Bit());
    const auto restore = qScopeGuard([&] {
        if (hadAppData)
            qputenv("APPDATA", appData);
        else
            qunsetenv("APPDATA");
        if (hadXdg)
            qputenv("XDG_CONFIG_HOME", xdg);
        else
            qunsetenv("XDG_CONFIG_HOME");
        QSettings().remove(QStringLiteral("import"));
    });
    const auto same = [](const QString& a, const QString& b) {
        return QDir::cleanPath(QDir::fromNativeSeparators(a)) == QDir::cleanPath(QDir::fromNativeSeparators(b));
    };

    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    REQUIRE(shell.preferences);
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(library);
    library->setPersistent(false);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("window.mediaBrowser"))));
    QObject* panelObject = nullptr;
    REQUIRE(waitFor([&] {
        panelObject = shell.window->findChild<QObject*>(QStringLiteral("mediaBrowserPanel"));
        return panelObject != nullptr;
    }, 5000));
    auto* panel = qobject_cast<QQuickItem*>(panelObject);
    REQUIRE(panel);

    // A folder that opens is remembered.
    bool ok = false;
    REQUIRE(QMetaObject::invokeMethod(panel, "openFolder", Q_RETURN_ARG(bool, ok), Q_ARG(QVariant, clips)));
    CHECK(ok);
    CHECK(same(shell.preferences->property("mediaBrowserLastFolder").toString(), clips));

    // A new Media Browser opens there.
    {
        const auto again = makeBrowser(shell);
        CHECK(same(again->property("folder").toString(), clips));
        QQuickItem* location = findItemNamed(again.get(), QStringLiteral("mediaBrowserLocation"));
        REQUIRE(location);
        CHECK(same(location->property("text").toString(), clips));
        const QVariantList entries = again->property("entries").toList();
        REQUIRE(entries.size() == 1);
        CHECK(entries.front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Session 1"));
    }
    // Fix round 1 (D): a Media Browser made hidden (the window preloads its panels) reads no folder until it shows: a
    // folder on a share that does not answer would hold the window just after start-up.
    {
        const auto hidden = makeBrowser(shell, "MediaBrowserPanel { visible: false }");
        spin(50);
        CHECK(hidden->property("folder").toString().isEmpty());
        CHECK(hidden->property("entries").toList().isEmpty());
        hidden->setVisible(true);
        REQUIRE(waitFor([&] { return !hidden->property("folder").toString().isEmpty(); }, 5000));
        CHECK(same(hidden->property("folder").toString(), clips));
        CHECK(hidden->property("entries").toList().size() == 1);
    }

    // The places menu over the OBS recordings: Videos and Home, the capture folders, the folder the last import came
    // from, the drives (the screenshot, with the panel's group maximized so its header tools show beside the tabs).
    REQUIRE(QDir().mkpath(recordings + QStringLiteral("/2026-10-20 Ranked")));
    REQUIRE(QDir().mkpath(dir.filePath(QStringLiteral("Footage"))));
    auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(lifecycle);
    lifecycle->rememberImportFolder({QUrl::fromLocalFile(dir.filePath(QStringLiteral("Footage/x.mp4")))});
    toggleMaximizeMediaBrowser(shell);
    REQUIRE(QMetaObject::invokeMethod(panel, "openFolder", Q_RETURN_ARG(bool, ok), Q_ARG(QVariant, recordings)));
    CHECK(ok);
    QObject* placesButton = panel->findChild<QObject*>(QStringLiteral("mediaBrowserPlaces"));
    if (!placesButton)
        placesButton = findItemNamed(panel, QStringLiteral("mediaBrowserPlaces"));
    REQUIRE(placesButton);
    QObject* placesMenu = panel->findChild<QObject*>(QStringLiteral("mediaBrowserPlacesMenu"));
    REQUIRE(placesMenu);
    spin(100);
    REQUIRE(QMetaObject::invokeMethod(placesButton, "clicked"));
    REQUIRE(waitFor([&] { return placesMenu->property("opened").toBool(); }, 5000));
    QStringList names;
    for (const QVariant& v : placesMenu->property("options").toList())
        names << v.toMap().value(QStringLiteral("name")).toString();
    INFO(names.join(QStringLiteral(" | ")).toStdString());
#ifndef Q_OS_MACOS
    CHECK(names.contains(QStringLiteral("OBS recordings")));
#endif
    CHECK(names.contains(QStringLiteral("Recent: Footage")));
    spin(50);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("media-browser-places.png"))));
    REQUIRE(QMetaObject::invokeMethod(placesMenu, "close"));
    toggleMaximizeMediaBrowser(shell);

    // A folder that does not open is not remembered.
    REQUIRE(QMetaObject::invokeMethod(panel, "openFolder", Q_RETURN_ARG(bool, ok),
                                      Q_ARG(QVariant, dir.filePath(QStringLiteral("missing")))));
    CHECK_FALSE(ok);
    CHECK(same(shell.preferences->property("mediaBrowserLastFolder").toString(), recordings));

    // The folder shown last gone: the first place (Videos), and no message about it.
    REQUIRE(QMetaObject::invokeMethod(panel, "openFolder", Q_RETURN_ARG(bool, ok), Q_ARG(QVariant, clips)));
    REQUIRE(QDir(clips).removeRecursively());
    {
        QStringList said; // what the Library said meanwhile (each message is a toast in the window)
        const auto listen = QObject::connect(library, &LibraryBridge::messageChanged, library, [&] {
            if (!library->message().isEmpty())
                said << library->message();
        });
        const auto again = makeBrowser(shell);
        QObject::disconnect(listen);
        const QVariantList places = library->browserPlaces();
        REQUIRE_FALSE(places.isEmpty());
        CHECK(same(again->property("folder").toString(), places.front().toMap().value(QStringLiteral("path")).toString()));
        INFO(said.join(QStringLiteral(" | ")).toStdString());
        CHECK(said.isEmpty());
    }

    shell.preferences->setProperty("mediaBrowserLastFolder", QString());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}
