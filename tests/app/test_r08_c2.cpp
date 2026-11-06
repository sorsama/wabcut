// 0.7, stream C, batch C2 (app level):
//   * #2   the "Normalize to -14 LUFS" chip of the YouTube, Shorts and Twitch tiles (remembered, off by default) and the
//          Streaming standard of Loudness Normalization, with screenshots (dark and light);
//   * #70  the default export folder (Settings > General > Export): last used / next to the project / next to the source
//          media / a chosen folder, where a new export's file goes, with screenshots (dark and light);
//   * #169 the master of a used subclip is in use (Remove Unused keeps it) and deleting it says what the subclips lose;
//   * #171 progress never rewrites the export queue folder;
//   * #173 Collect Files works its size out on a worker thread and the newest request wins.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "CollectFilesBridge.h"
#include "ExportController.h"
#include "KeymapDispatcher.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QQuickItem>
#include <QQuickWindow>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;
namespace fs = std::filesystem;

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "v07-c2" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
}

bool samePath(const QString& a, const QString& b) {
    return !a.isEmpty() &&
           QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}

QQuickItem* findWith(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findWith(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* byName(QQuickItem* root, const QString& name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

/// A QML `var` property read from C++ as plain Qt values (a JS object arrives as a QJSValue or already as a map).
QVariant plain(const QVariant& value) {
    return value.userType() == qMetaTypeId<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
}

/// The part of the window under `item`, saved as a PNG in the UI output.
void saveItemShot(Shell& shell, const QQuickItem* item, const QString& name) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                            .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

/// A sequence of `seconds` of nothing (exports render silence and the synthetic picture).
ProjectState emptyProject(double seconds) {
    ProjectState state;
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Clip";
    seq.width = 640;
    seq.height = 360;
    seq.frameRate = kRate30;
    seq.duration = Ticks::fromSeconds(static_cast<std::int64_t>(seconds));
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

QJsonArray keptJobs(const fs::path& folder) {
    QFile file(q(folder / "queue.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("jobs")).toArray();
}

bool finished(const ExportController& c, const QString& jobId, int timeoutMs = 60'000) {
    return waitFor([&] { return c.job(jobId).value(QStringLiteral("finished")).toBool(); }, timeoutMs);
}

/// The project of a picture "V1 |Aaaa|" whose one clip plays a subclip of media C, whose master has Master Clip Effects;
/// media D has a subclip too, but nothing plays them.
ProjectState subclipState(const Timeline& tl, MediaId& master, MediaId& subclip, MediaId& unusedMaster, MediaId& unusedSubclip) {
    master = tl.media('C');
    unusedMaster = tl.media('D');
    ProjectState state = tl.state;
    const TimeRange range = TimeRange::fromStartEnd(Ticks{}, Ticks{kTicksPerSecond / 10});
    auto made = edit::makeSubclip(state, master, range, "Answer");
    REQUIRE(made);
    subclip = made->id;
    state = made->state;
    auto other = edit::makeSubclip(state, unusedMaster, range, "Spare");
    REQUIRE(other);
    unusedSubclip = other->id;
    state = other->state;
    for (MediaPtr& m : state.media) {
        if (m->id == master) {
            auto item = std::make_shared<MediaItem>(*m);
            EffectInstance blur;
            blur.id = state.ids.allocate<EffectInstanceId>();
            blur.effect = "wabcut.blur.gaussian";
            item->masterEffects.push_back(blur);
            m = item;
        }
    }
    // The clip on the timeline plays the subclip.
    for (SequencePtr& seq : state.sequences) {
        auto copy = std::make_shared<Sequence>(*seq);
        for (TrackPtr& track : copy->videoTracks) {
            std::vector<Clip> clips(track->clips.begin(), track->clips.end());
            for (Clip& c : clips)
                c.media = subclip;
            auto next = std::make_shared<Track>(*track);
            next->clips = ClipList(std::move(clips));
            track = std::move(next);
        }
        seq = std::move(copy);
    }
    return state;
}

struct SubclipRig {
    Timeline tl{"V1 |Aaaa|"};
    ProjectController controller;
    LibraryBridge library;
    MediaId subclip;
    MediaId master;
    MediaId unusedMaster;
    MediaId unusedSubclip;

    SubclipRig() {
        controller.resetProject(subclipState(tl, master, subclip, unusedMaster, unusedSubclip), tl.seq);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setAnalysisProbe({});
        library.setController(&controller);
    }
};

/// The popup (menu, popover) holding `item`: its item under the overlay.
QQuickItem* popupOf(QQuickItem* item) {
    QQuickItem* at = item;
    while (at && at->parentItem() && !QByteArray(at->parentItem()->metaObject()->className()).contains("Overlay"))
        at = at->parentItem();
    return at;
}

} // namespace

// ---- #169 -----------------------------------------------------------------------------------------------------------

TEST_CASE("Library: the master of a used subclip is in use; Remove Unused keeps it and its effects (169)",
          "[app][library][subclip]") {
    SubclipRig r;
    const auto& unused = r.library.unused();
    CHECK_FALSE(unused.contains(r.subclip.value)); // a clip plays it
    CHECK_FALSE(unused.contains(r.master.value));  // its subclip does: the subclip plays the master's effects
    CHECK(unused.contains(r.unusedSubclip.value)); // nothing plays this subclip, so its master is unused as well
    CHECK(unused.contains(r.unusedMaster.value));
    CHECK(unused.contains(r.tl.media('B').value));

    const int removed = r.library.removeUnused();
    CHECK(removed > 0);
    const ProjectSnapshot after = r.controller.snapshot();
    REQUIRE(after->findMedia(r.master));
    CHECK(after->findMedia(r.master)->masterEffects.size() == 1);
    CHECK(after->findMedia(r.subclip));
    CHECK(after->findMedia(r.unusedMaster) == nullptr);
    CHECK(after->findMedia(r.unusedSubclip) == nullptr);
    // The subclip's clip still plays the master's effects.
    EffectStack played = masterEffectsOf(*after, r.subclip);
    CHECK(played.size() == 1);
}

TEST_CASE("Library: deleting a master says which subclips on the timeline lose its effects (169)", "[app][library][subclip]") {
    SubclipRig r;
    const QVariantMap impact = r.library.deleteImpact({mediaKey(r.master)});
    CHECK(impact.value(QStringLiteral("clips")).toInt() == 0); // no clip plays the master itself
    CHECK(impact.value(QStringLiteral("masterEffectSubclips")).toInt() == 1);
    CHECK(impact.value(QStringLiteral("masterEffectClips")).toInt() == 1);
    CHECK(impact.value(QStringLiteral("masterEffectName")).toString() == QString::fromStdString(r.tl.state.findMedia(r.master)->name));
    // A master without effects, or with no used subclip, loses nothing.
    const QVariantMap spare = r.library.deleteImpact({mediaKey(r.unusedMaster)});
    CHECK(spare.value(QStringLiteral("masterEffectSubclips")).toInt() == 0);
    const QVariantMap together = r.library.deleteImpact({mediaKey(r.master), mediaKey(r.subclip)});
    CHECK(together.value(QStringLiteral("masterEffectSubclips")).toInt() == 0); // the subclip goes too: nothing plays without them

    // The panel asks first, although no clip plays the master itself; deleting it by hand still works after the question.
    int asked = 0;
    QObject::connect(&r.library, &LibraryBridge::deleteConfirmRequested, [&] { ++asked; });
    CHECK(r.library.requestDelete({mediaKey(r.master)}, false));
    REQUIRE(asked == 1);
    CHECK(r.controller.snapshot()->findMedia(r.master) != nullptr); // nothing was deleted yet
    // Without effects it deletes at once, as before.
    CHECK(r.library.requestDelete({mediaKey(r.unusedMaster)}, false));
    CHECK(asked == 1);
    CHECK(r.controller.snapshot()->findMedia(r.unusedMaster) == nullptr);
}

TEST_CASE("Library: the delete confirmation names the master effects the subclips lose; screenshots (169)",
          "[app][library][subclip][qml][screenshot]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
        REQUIRE(project);
        REQUIRE(controller);
        REQUIRE(library);
        REQUIRE(keys);
        project->newProject();
        Timeline tl("V1 |Aaaa|");
        MediaId master, subclip, unusedMaster, unusedSubclip;
        controller->resetProject(subclipState(tl, master, subclip, unusedMaster, unusedSubclip), tl.seq);
        spin(50);
        REQUIRE_FALSE(project->demo());
        library->setPersistent(false);
        REQUIRE_FALSE(shell.grab().isNull());
        QQuickItem* root = shell.window->contentItem();
        QQuickItem* panel = shell.item("libraryPanel");
        REQUIRE(panel);

        library->select(mediaKey(master), LibraryBridge::Replace);
        REQUIRE(QMetaObject::invokeMethod(panel, "focusContent"));
        spin(50);
        REQUIRE(keys->trigger(QStringLiteral("project.delete")));
        QObject* popover = panel->findChild<QObject*>(QStringLiteral("libraryDeletePopover"));
        REQUIRE(popover);
        REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }, 3000));
        QQuickItem* warning = byName(root, QStringLiteral("libraryDeleteMasterEffectsText"));
        REQUIRE(warning);
        CHECK(warning->isVisible());
        const QString words = warning->property("text").toString();
        CHECK(words.contains(QString::fromStdString(tl.state.findMedia(master)->name)));
        CHECK(words.contains(QStringLiteral("Master Clip Effects")));
        CHECK(words.contains(QStringLiteral("1 clip")));
        // The plain "no sequence uses it" text is still there: the master itself is unused.
        CHECK(byName(root, QStringLiteral("libraryDeleteText"))->property("text").toString().contains(QStringLiteral("No sequence uses it")));
        spin(150);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        QQuickItem* card = popupOf(warning);
        const double dpr = image.devicePixelRatio();
        const QRectF area = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()))
                                .adjusted(-8, -8, 8, 8)
                                .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                    .save(QDir(outputDir()).filePath(dark ? QStringLiteral("library-delete-master-effects-dark.png")
                                                          : QStringLiteral("library-delete-master-effects-light.png"))));
        QMetaObject::invokeMethod(byName(root, QStringLiteral("libraryDeleteCancel")), "clicked");
        REQUIRE(waitFor([&] { return !popover->property("opened").toBool(); }, 3000));
        CHECK(controller->snapshot()->findMedia(master) != nullptr); // asked, not deleted
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

// ---- #171 -----------------------------------------------------------------------------------------------------------

TEST_CASE("The export queue folder is written for changes of the queue and left alone while a job reports progress (171)",
          "[app][export][queue]") {
    resetSettings();
    const fs::path dir = freshDir("queue-progress");
    const fs::path folder = dir / "Export Queue";
    ProjectController project;
    project.resetProject(emptyProject(1.0));
    ExportController c;
    c.setProject(&project);
    c.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    c.setQueueFolder(folder);
    c.restoreQueue();
    const QStringList one = c.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "one.wav")}}, true);
    const QStringList two = c.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "two.wav")}}, true);
    REQUIRE(one.size() == 1);
    REQUIRE(two.size() == 1);
    REQUIRE(waitFor([&] { return keptJobs(folder).size() == 2; }));
    spin(50);

    // A leftover snapshot nothing names: swept by the next write, not by progress.
    const fs::path stray = folder / "q-00000000000000000000000000000000.wabcut";
    { std::ofstream(stray) << "old"; }
    const int runs = c.persistRuns();
    ExportJobModel* jobs = c.jobs();
    for (int i = 0; i < 25; ++i)
        emit jobs->dataChanged(jobs->index(0), jobs->index(0)); // what ExportJobModel::update() emits for progress
    spin(100);
    CHECK(c.persistRuns() == runs);
    CHECK(fs::exists(stray));

    // A change of the queue (a job leaves it) writes the folder once and sweeps.
    c.cancel(one.front());
    REQUIRE(waitFor([&] { return keptJobs(folder).size() == 1; }));
    CHECK(c.persistRuns() > runs);
    CHECK_FALSE(fs::exists(stray));
    // The other one runs and finishes: its states are kept, progress between them is not.
    c.startQueue();
    REQUIRE(finished(c, two.front()));
    REQUIRE(waitFor([&] { return keptJobs(folder).isEmpty(); }));
}

// ---- #173 -----------------------------------------------------------------------------------------------------------

TEST_CASE("Collect Files works its size out on a worker thread and the newest request wins (173)", "[app][collect]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("collect-estimate");
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* collect = shell.engine.singletonInstance<CollectFilesBridge*>("Wabcut", "Collect");
    REQUIRE(file);
    REQUIRE(collect);

    // A project with one 50 MB media item no sequence uses (the estimate only stats it).
    const fs::path big = dir / "big.bin";
    {
        std::ofstream out(big, std::ios::binary);
        const std::string block(1024 * 1024, 'x');
        for (int i = 0; i < 50; ++i)
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    ProjectState state = emptyProject(1.0);
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = "big.bin";
    item.locator.path = pathToUtf8(big);
    item.info.path = item.locator.path;
    state.media.push_back(std::make_shared<MediaItem>(item));
    file->document().session().reset(state);

    const QUrl folder = QUrl::fromLocalFile(q(dir / "capsules"));
    QDir().mkpath(folder.toLocalFile());
    const QString withMedia = QStringLiteral("About ") + QLocale().formattedDataSize(1024 * 1024 + 50 * 1024 * 1024, 1, QLocale::DataSizeSIFormat);
    const QString without = QStringLiteral("About ") + QLocale().formattedDataSize(1024 * 1024, 1, QLocale::DataSizeSIFormat);

    // The synchronous form still answers (scripts, tests).
    CHECK(collect->estimate(folder, {{QStringLiteral("unused"), true}}).startsWith(withMedia));
    CHECK(collect->estimate(folder, {{QStringLiteral("unused"), false}}).startsWith(without));

    // Asking returns at once; the answer arrives later.
    int changed = 0;
    QObject::connect(collect, &CollectFilesBridge::estimateChanged, [&] { ++changed; });
    collect->requestEstimate(folder, {{QStringLiteral("unused"), true}});
    CHECK(collect->estimating());
    REQUIRE(waitFor([&] { return !collect->estimating(); }, 20'000));
    CHECK(collect->estimateText().startsWith(withMedia));
    CHECK(changed >= 2); // started, answered

    // A second request replaces a first one that is still on its way: the final text is the newest request's.
    for (int round = 0; round < 5; ++round) {
        collect->requestEstimate(folder, {{QStringLiteral("unused"), false}});
        collect->requestEstimate(folder, {{QStringLiteral("unused"), true}});
        collect->requestEstimate(folder, {{QStringLiteral("unused"), false}});
        REQUIRE(waitFor([&] { return !collect->estimating(); }, 20'000));
        spin(50); // a dropped older answer must not arrive late and overwrite it
        CHECK(collect->estimateText().startsWith(without));
    }
    // "Include transcripts and analysis results" does not write files to work out the estimate.
    collect->requestEstimate(folder, {{QStringLiteral("unused"), true}, {QStringLiteral("analysis"), true}});
    REQUIRE(waitFor([&] { return !collect->estimating(); }, 20'000));
    CHECK(collect->estimateText().startsWith(withMedia));
}

TEST_CASE("Collect Files writes and sizes analysis sidecars only for the media the capsule holds (173)", "[app][collect]") {
    // "Include transcripts and analysis results" used to write the transcripts of every media item of the project; a
    // capsule of one sequence must carry the ones of its own footage only.
    const fs::path dir = freshDir("collect-analysis-only");
    ProjectState state;
    ai::AnalysisCache cache(dir / "cache");
    std::vector<MediaId> ids;
    for (const char* name : {"one", "two", "three"}) {
        const fs::path file = dir / (std::string(name) + ".bin");
        {
            std::ofstream out(file, std::ios::binary);
            out << "media file " << name;
        }
        MediaItem item;
        item.id = state.ids.allocate<MediaId>();
        item.name = name;
        item.locator.path = pathToUtf8(file);
        item.info.path = item.locator.path;
        state.media.push_back(std::make_shared<MediaItem>(item));
        ids.push_back(item.id);
        const auto fingerprint = media::sourceFingerprint(item.locator.path);
        REQUIRE(fingerprint.has_value());
        ai::AnalysisKey key;
        key.sourceFingerprint = *fingerprint;
        key.streamIndex = 0;
        key.analyzerId = "transcript";
        key.modelVersion = "test";
        key.paramsHash = "0";
        REQUIRE(cache.put(key, std::string(R"({"text":"hello from )") + name + R"("})").has_value());
    }
    const auto always = [] { return true; };
    const std::vector<MediaId> held{ids[0], ids[2]}; // sorted by id, as editor::capsuleMedia returns them

    const auto sidecars = collectAnalysisSidecars(state, held, dir / "sidecars", dir / "cache");
    REQUIRE(sidecars.size() == 2);
    std::vector<std::string> names;
    for (const editor::CapsuleExtraFile& extra : sidecars)
        names.push_back(pathToUtf8(extra.source.filename()));
    CHECK(names == std::vector<std::string>{"one.analysis.json", "three.analysis.json"});
    CHECK_FALSE(fs::exists(dir / "sidecars" / "two.analysis.json"));
    CHECK(collectAnalysisSidecars(state, {}, dir / "none", dir / "cache").empty()); // a capsule that holds no media

    const std::int64_t some = collectAnalysisSidecarBytes(state, held, always, dir / "cache");
    const std::int64_t everything = collectAnalysisSidecarBytes(state, ids, always, dir / "cache");
    CHECK(some > 0);
    CHECK(everything > some);
    CHECK(collectAnalysisSidecarBytes(state, {}, always, dir / "cache") == 0);
}

// ---- #70 ------------------------------------------------------------------------------------------------------------

TEST_CASE("The default export folder follows its setting: last used, the project, the source media or a chosen folder (070)",
          "[app][export][folder]") {
    resetSettings();
    const fs::path dir = freshDir("export-folder");
    const fs::path footage = dir / "Footage";
    const fs::path chosen = dir / "Deliveries";
    const fs::path lastUsed = dir / "Last";
    for (const fs::path& p : {footage, chosen, lastUsed})
        fs::create_directories(p);

    // One clip whose media sits in Footage.
    Timeline tl("V1 |Aaaa|");
    ProjectState state = tl.state;
    for (MediaPtr& m : state.media) {
        if (m->id == tl.media('A')) {
            auto item = std::make_shared<MediaItem>(*m);
            item->locator.path = pathToUtf8(footage / "A.mov");
            m = item;
        }
    }
    ProjectController project;
    project.resetProject(state, tl.seq);
    ExportController c;
    c.setProject(&project);
    c.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    c.setProjectFile(dir / "Interview.wabcut");
    const QString name = QString::fromStdString(tl.sequence().name);

    // Out of the box: how it always was - the last folder, else next to the project.
    CHECK(c.defaultFolderMode() == QStringLiteral("last"));
    CHECK(samePath(c.defaultFolderFor({}), q(dir)));
    c.setLastFolder(q(lastUsed));
    CHECK(samePath(c.defaultFolderFor({}), q(lastUsed)));
    CHECK(samePath(QFileInfo(c.defaultOutputPath({}, QStringLiteral("match-source"))).absolutePath(), q(lastUsed)));

    // Next to the project: the last folder is ignored.
    c.setDefaultFolderMode(QStringLiteral("project"));
    CHECK(c.defaultFolderMode() == QStringLiteral("project"));
    CHECK(samePath(c.defaultFolderFor({}), q(dir)));
    CHECK(samePath(QFileInfo(c.defaultOutputPath({}, QStringLiteral("match-source"))).absolutePath(), q(dir)));

    // Next to the source media: the first clip's folder.
    c.setDefaultFolderMode(QStringLiteral("media"));
    CHECK(samePath(c.defaultFolderFor({}), q(footage)));
    CHECK(samePath(QFileInfo(c.defaultOutputPath({}, QStringLiteral("match-source"))).absolutePath(), q(footage)));
    // The media's folder is gone: next to the project.
    fs::remove_all(footage);
    CHECK(samePath(c.defaultFolderFor({}), q(dir)));
    fs::create_directories(footage);

    // A chosen folder: nothing chosen yet falls back to the project; a path or a URL; a folder that is gone is not used.
    c.setDefaultFolderMode(QStringLiteral("fixed"));
    CHECK(c.defaultFolder().isEmpty());
    CHECK(c.defaultFolderUrl().isEmpty());
    CHECK(samePath(c.defaultFolderFor({}), q(dir)));
    c.setDefaultFolder(q(chosen));
    CHECK(samePath(c.defaultFolder(), q(chosen)));
    CHECK(samePath(c.defaultFolderUrl().toLocalFile(), q(chosen)));
    CHECK(samePath(c.defaultFolderFor({}), q(chosen)));
    c.setDefaultFolder(QUrl::fromLocalFile(q(chosen)).toString());
    CHECK(samePath(c.defaultFolder(), q(chosen)));
    CHECK(samePath(c.defaultOutputPath({}, QStringLiteral("match-source")), q(chosen / (name.toStdU16String() + u".mp4"))));
    // The name is not taken twice: a file already there is not the next export's name.
    { std::ofstream(chosen / (name.toStdU16String() + u".mp4")) << "x"; }
    CHECK(samePath(c.defaultOutputPath({}, QStringLiteral("match-source")), q(chosen / (name.toStdU16String() + u" (2).mp4"))));
    // An export to another place (it sets the last folder) does not move a chosen folder; Last used would follow it.
    c.setLastFolder(q(lastUsed));
    CHECK(samePath(c.lastFolder(), q(lastUsed)));
    CHECK(samePath(c.defaultFolderFor({}), q(chosen)));
    fs::remove_all(chosen);
    CHECK(samePath(c.defaultFolderFor({}), q(dir))); // the drive is not there: next to the project

    // Kept for the next start; junk reads as the default; a project that was never saved goes to Movies or home.
    {
        ExportController next;
        CHECK(next.defaultFolderMode() == QStringLiteral("fixed"));
        CHECK(samePath(next.defaultFolder(), q(chosen)));
        next.setDefaultFolderMode(QStringLiteral("nonsense"));
        CHECK(next.defaultFolderMode() == QStringLiteral("last"));
        CHECK_FALSE(next.defaultFolderFor({}).isEmpty());
    }
}

// ---- #2 -------------------------------------------------------------------------------------------------------------

TEST_CASE("A sequence override of the Streaming standard reaches the export settings (002)", "[app][export][loudness]") {
    const auto settings = ExportController::settingsFor(
        QStringLiteral("youtube-1080p"),
        {{QStringLiteral("effects"),
          QVariantMap{{QStringLiteral("loudness"),
                       QVariantMap{{QStringLiteral("applied"), true}, {QStringLiteral("standard"), QStringLiteral("streaming")}}}}}});
    REQUIRE(settings);
    CHECK(settings->effects.loudness.applied);
    CHECK(settings->effects.loudness.standard == ex::LoudnessStandard::Streaming);
    CHECK(settings->effects.loudness.effectiveTarget() == -14.0);
    CHECK(settings->effects.loudness.effectiveTruePeak() == -1.0);
}

TEST_CASE("Export panel: the YouTube, Shorts and Twitch tiles offer Normalize to -14 LUFS; screenshots", "[app][export][qml][screenshot]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(2560, 1200); // wide: the details (and the chip) are a column of their own, in view
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(controller);
    REQUIRE(exporter);
    Timeline tl("V1 |....|");
    Sequence s = tl.sequence();
    s.frameRate = Rate{60, 1};
    s.duration = Ticks::fromSeconds(45);
    controller->resetProject(tl.state.withSequence(std::make_shared<const Sequence>(std::move(s))), tl.seq);
    shell.window->setProperty("workspace", 2);
    spin(200);

    QQuickItem* content = shell.window->contentItem();
    QQuickItem* panel = findWith(content, [](QQuickItem* i) { return i->property("gamingPreset").isValid(); });
    REQUIRE(panel);
    const auto chip = [&] { return byName(content, QStringLiteral("exportStreamingLoudnessChip")); };
    // The Loudness Normalization override of the panel: { applied, standard } while it is on, empty while off.
    const auto loudness = [&]() -> QVariantMap {
        const QVariantMap overrides = plain(panel->property("overrides")).toMap();
        return plain(plain(overrides.value(QStringLiteral("effects"))).toMap().value(QStringLiteral("loudness"))).toMap();
    };

    // A built-in preset, Discord and GIF have no chip; the YouTube, Shorts and Twitch tiles do.
    REQUIRE(chip() != nullptr);
    CHECK_FALSE(chip()->isVisible());
    QQuickItem* discord = byName(content, QStringLiteral("gamingPreset_discord-10mb"));
    REQUIRE(discord);
    REQUIRE(QMetaObject::invokeMethod(discord, "clicked"));
    CHECK_FALSE(chip()->isVisible());
    QQuickItem* youtube = byName(content, QStringLiteral("gamingPreset_youtube-1080p60"));
    REQUIRE(youtube);
    REQUIRE(QMetaObject::invokeMethod(youtube, "clicked"));
    REQUIRE(chip()->isVisible());
    CHECK(chip()->property("text").toString() == QStringLiteral("Normalize to -14 LUFS"));
    CHECK(loudness().isEmpty()); // off by default
    CHECK_FALSE(exporter->streamingLoudness());
    CHECK(chip()->property("variant").toString() == QStringLiteral("secondary"));
    spin(300);
    (void)shell.grab(); // a hidden window lays out when it renders
    saveItemShot(shell, byName(content, QStringLiteral("exportChosenCard")), QStringLiteral("export-streaming-loudness-off-dark.png"));

    // Clicking it turns Loudness Normalization on with the Streaming standard, and remembers the choice.
    REQUIRE(QMetaObject::invokeMethod(chip(), "clicked"));
    CHECK(loudness().value(QStringLiteral("applied")).toBool());
    CHECK(loudness().value(QStringLiteral("standard")).toString() == QStringLiteral("streaming"));
    CHECK(exporter->streamingLoudness());
    CHECK(chip()->property("variant").toString() == QStringLiteral("primary"));
    const QVariantMap summary = panel->property("summary").toMap();
    CHECK(summary.value(QStringLiteral("ok")).toBool());
    spin(300);
    (void)shell.grab(); // a hidden window lays out when it renders
    QQuickItem* card = byName(content, QStringLiteral("exportChosenCard"));
    REQUIRE(card);
    CHECK(card->width() > 200);
    CHECK(chip()->mapRectToItem(card, QRectF(0, 0, chip()->width(), chip()->height())).bottom() <= card->height()); // inside it
    saveItemShot(shell, card, QStringLiteral("export-streaming-loudness-dark.png"));

    // The next tile starts with it on (Twitch); Discord does not offer it.
    QQuickItem* twitch = byName(content, QStringLiteral("gamingPreset_twitch-1080p60"));
    REQUIRE(twitch);
    REQUIRE(QMetaObject::invokeMethod(twitch, "clicked"));
    REQUIRE(chip()->isVisible());
    CHECK(loudness().value(QStringLiteral("standard")).toString() == QStringLiteral("streaming"));
    REQUIRE(QMetaObject::invokeMethod(discord, "clicked"));
    CHECK_FALSE(chip()->isVisible());
    CHECK(loudness().isEmpty());
    REQUIRE(QMetaObject::invokeMethod(youtube, "clicked"));
    REQUIRE(chip()->isVisible());
    CHECK(loudness().value(QStringLiteral("applied")).toBool());

    // The keyboard reaches it (a real, named, focusable button).
    CHECK(chip()->property("focusPolicy").toInt() != Qt::NoFocus);
    CHECK(auditAccessibility(byName(content, QStringLiteral("exportChosenCard"))).isEmpty());

    // Off again: the choice is forgotten with the override.
    REQUIRE(QMetaObject::invokeMethod(chip(), "clicked"));
    CHECK(loudness().isEmpty());
    CHECK_FALSE(exporter->streamingLoudness());
    REQUIRE(QMetaObject::invokeMethod(twitch, "clicked"));
    CHECK(loudness().isEmpty());

    // Loudness Normalization on by hand with another standard: no chip (it would replace the user's choice).
    exporter->setStreamingLoudness(false);
    QVariantMap effects{{QStringLiteral("loudness"), QVariantMap{{QStringLiteral("applied"), true}, {QStringLiteral("standard"), QStringLiteral("ebuR128")}}}};
    panel->setProperty("overrides", QVariantMap{{QStringLiteral("effects"), effects}});
    spin(100);
    CHECK_FALSE(chip()->isVisible());

    // The light theme.
    panel->setProperty("overrides", QVariantMap{});
    REQUIRE(QMetaObject::invokeMethod(youtube, "clicked"));
    REQUIRE(QMetaObject::invokeMethod(chip(), "clicked"));
    shell.theme->setProperty("dark", false);
    spin(700);
    saveItemShot(shell, byName(content, QStringLiteral("exportChosenCard")), QStringLiteral("export-streaming-loudness-light.png"));
    shell.theme->setProperty("dark", true);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Settings > General > Export: choose where new exports go; screenshots", "[app][export][qml][screenshot][settings]") {
    resetSettings();
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(exporter);

    QObject* loader = shell.object("settingsWindowLoader");
    REQUIRE(loader);
    loader->setProperty("active", true);
    QObject* window = loader->property("item").value<QObject*>();
    REQUIRE(window);
    REQUIRE(QMetaObject::invokeMethod(window, "openAt", Q_ARG(QString, QStringLiteral("general"))));
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    bool revealed = false;
    REQUIRE(QMetaObject::invokeMethod(window, "revealRow", Q_RETURN_ARG(bool, revealed), Q_ARG(QString, QStringLiteral("general")),
                                      Q_ARG(QString, QStringLiteral("exportFolder"))));
    CHECK(revealed);
    QQuickItem* root = window->property("contentItem").value<QQuickItem*>();
    QQuickItem* row = nullptr;
    REQUIRE(waitFor([&] { return (row = byName(root, QStringLiteral("exportFolderRow"))) != nullptr; }));
    QQuickItem* tabs = byName(root, QStringLiteral("exportFolderModeTabs"));
    REQUIRE(tabs);
    CHECK(tabs->property("currentIndex").toInt() == 0);
    QQuickItem* browse = byName(root, QStringLiteral("exportFolderBrowse"));
    REQUIRE(browse);
    CHECK_FALSE(browse->isVisible()); // only the "A folder" mode has one

    // The tabs set the mode (and the row follows a change made elsewhere).
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 1)));
    CHECK(exporter->defaultFolderMode() == QStringLiteral("project"));
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 2)));
    CHECK(exporter->defaultFolderMode() == QStringLiteral("media"));
    exporter->setDefaultFolderMode(QStringLiteral("project"));
    CHECK(tabs->property("currentIndex").toInt() == 1);
    exporter->setDefaultFolder(QDir::tempPath());
    exporter->setDefaultFolderMode(QStringLiteral("fixed"));
    CHECK(tabs->property("currentIndex").toInt() == 3);
    CHECK(waitFor([&] { return browse->isVisible(); }, 3000));
    spin(300);
    CHECK(auditAccessibility(row).isEmpty());
    saveItemShot(shell, row, QStringLiteral("settings-export-folder-dark.png"));
    shell.theme->setProperty("dark", false);
    spin(700);
    saveItemShot(shell, row, QStringLiteral("settings-export-folder-light.png"));
    shell.theme->setProperty("dark", true);

    // The smallest window: nothing of the row runs off its edge.
    shell.resize(1100, 700);
    spin(400);
    (void)shell.grab(); // a hidden window lays out when it renders
    REQUIRE(waitFor([&] { return (row = byName(root, QStringLiteral("exportFolderRow"))) != nullptr; }));
    const QRectF rowRect(0, 0, row->width(), row->height());
    for (QQuickItem* control : {tabs, browse}) {
        const QRectF at = control->mapRectToItem(row, QRectF(0, 0, control->width(), control->height()));
        INFO(control->objectName().toStdString() << " " << at.left() << ".." << at.right() << " of " << row->width());
        CHECK(rowRect.contains(at));
    }
    saveItemShot(shell, row, QStringLiteral("settings-export-folder-narrow.png"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
