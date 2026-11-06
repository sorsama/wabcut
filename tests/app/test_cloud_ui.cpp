// Cloud projects, review links and stock media (0.7, card 035, docs/cloud.md) at app level, over fake servers (no network):
//   * the Cloud bridge: storage locations (secrets only in the key store), a cloud project (upload, sync, fast-forward, conflicts,
//     the editing lock, the undo guard, offline mode), review links (review copy, publish to S3 and to a folder, comments back as
//     review notes in one undo step, revoke);
//   * the Stock bridge: key, search, thumbnails, download and import in one undo step with the licence recorded, offline mode;
//   * the Cloud dialog and the Stock panel (screenshots, dark and light).

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "AiController.h"
#include "CloudBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "StockBridge.h"

#include "../cloud/FakeServers.h"

#include <wabcut/ai/Secrets.h>
#include <wabcut/cloud/CloudLock.h>
#include <wabcut/cloud/CloudProject.h>
#include <wabcut/cloud/ReviewLink.h>
#include <wabcut/cloud/Stock.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ReviewOps.h>
#include <wabcut/model/ReviewNotes.h>

#include <catch2/catch_test_macros.hpp>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>

#include <fstream>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;
namespace cloudtest = wabcut::cloud::test;

namespace {

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

QQuickItem* popupOf(QQuickItem* item) {
    QQuickItem* at = item;
    while (at && at->parentItem() && !QByteArray(at->parentItem()->metaObject()->className()).contains("Overlay"))
        at = at->parentItem();
    return at;
}

bool saveCard(const Shell& shell, QQuickItem* inside, const QString& file) {
    const QImage image = shell.grab();
    if (image.isNull() || !inside)
        return false;
    QQuickItem* card = popupOf(inside);
    const double dpr = image.devicePixelRatio();
    const QRectF area = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()))
                            .adjusted(-8, -8, 8, 8)
                            .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    return image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
        .save(QDir(outputDir()).filePath(file));
}

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

fs::path toPath(const QString& text) { return fs::path(text.toStdWString()); }
QString qs_(const std::string& text) { return QString::fromStdString(text); }
std::string s(const QString& text) { return text.toStdString(); }

fs::path testMedia(const char* name) {
    const fs::path file = fs::path(WABCUT_TEST_MEDIA_DIR) / name;
    INFO("the test media file " << file.string() << " is missing: run the media fixtures first");
    REQUIRE(fs::is_regular_file(file));
    return file;
}

/// The shell over a saved project that has one clip, with the Cloud and Stock bridges on fake servers.
struct Rig {
    QTemporaryDir dir;
    QTemporaryDir cache;
    Shell shell;
    ProjectLifecycle* project = nullptr;
    ProjectController* controller = nullptr;
    AiController* ai = nullptr;
    CloudBridge* cloud = nullptr;
    StockBridge* stock = nullptr;
    ai::MemorySecretStore secrets;
    cloudtest::FakeS3 s3{"reviews", "AKIATESTKEY0001", "secret-secret-secret-1234"};
    cloudtest::FakeDav dav{"/dav/wabcut", "ann", "correct horse battery"};

    /// Routes to the fakes by host.
    struct Router final : ai::IHttpTransport {
        Rig* rig;
        std::function<std::expected<ai::HttpResponse, ai::AiError>(const ai::HttpRequest&, const ai::HttpStreamHandlers&, std::stop_token)> other;
        explicit Router(Rig* r) : rig(r) {}
        std::expected<ai::HttpResponse, ai::AiError> send(const ai::HttpRequest& request, const ai::HttpStreamHandlers& handlers,
                                                          std::stop_token stop) override {
            if (request.url.find("s3.example.com") != std::string::npos)
                return rig->s3.send(request, handlers, stop);
            if (request.url.find("dav.example.com") != std::string::npos)
                return rig->dav.send(request, handlers, stop);
            if (other)
                return other(request, handlers, stop);
            return std::unexpected(ai::AiError(ai::AiErrorCode::Network, "no such host in this test"));
        }
    } router{this};

    explicit Rig(bool withClip = true) {
        qputenv("WABCUT_AI_KEYSTORE", "memory");
        REQUIRE(dir.isValid());
        REQUIRE(cache.isValid());
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
        cloud = shell.engine.singletonInstance<CloudBridge*>("Wabcut", "Cloud");
        stock = shell.engine.singletonInstance<StockBridge*>("Wabcut", "Stock");
        REQUIRE(project);
        REQUIRE(controller);
        REQUIRE(ai);
        REQUIRE(cloud);
        REQUIRE(stock);
        project->newProject();
        REQUIRE_FALSE(project->demo());
        cloud->setSynchronous(true);
        cloud->setTransportForTesting(&router);
        cloud->setSecretStoreForTesting(&secrets);
        cloud->setCacheFolderForTesting(toPath(cache.path()));
        cloud->setHeartbeatEnabled(false);
        stock->setSynchronous(true);
        stock->setTransportForTesting(&router);
        stock->setSecretStoreForTesting(&secrets);
        stock->setDownloadFolderForTesting(toPath(dir.path()) / "Stock");
        if (withClip) {
            const QVariantMap imported = project->importFiles({QUrl::fromLocalFile(QString::fromStdWString(testMedia("shadowplay_two_track.mp4").wstring()))});
            REQUIRE(imported.value(QStringLiteral("added")).toInt() == 1);
            editor::MediaDrop drop;
            drop.media = controller->snapshot()->media.front()->id;
            drop.sequence = controller->currentSequenceId();
            drop.at = Ticks{};
            drop.mode = editor::DropMode::Overwrite;
            REQUIRE(editor::dropMedia(controller->session(), drop).has_value());
        }
        REQUIRE(project->saveAs(QUrl::fromLocalFile(dir.filePath(QStringLiteral("Film.wabcut")))));
        spin(50);
    }
    std::size_t undoCount() { return controller->session().undoCount(); }
    QString addFolderLocation(const QString& name, const QString& purpose) {
        const QString folder = dir.filePath(name);
        QDir().mkpath(folder);
        const QVariantMap saved = cloud->saveLocation({{QStringLiteral("name"), name}, {QStringLiteral("kind"), QStringLiteral("folder")},
                                                       {QStringLiteral("purpose"), purpose}, {QStringLiteral("folder"), folder}});
        INFO(saved.value(QStringLiteral("error")).toString().toStdString());
        REQUIRE(saved.value(QStringLiteral("ok")).toBool());
        return saved.value(QStringLiteral("id")).toString();
    }
    QString addS3Location(const QString& name, const QString& purpose) {
        const QVariantMap saved = cloud->saveLocation({{QStringLiteral("name"), name}, {QStringLiteral("kind"), QStringLiteral("s3")},
                                                       {QStringLiteral("purpose"), purpose}, {QStringLiteral("endpoint"), QStringLiteral("https://s3.example.com")},
                                                       {QStringLiteral("region"), QStringLiteral("us-east-1")}, {QStringLiteral("bucket"), QStringLiteral("reviews")},
                                                       {QStringLiteral("prefix"), QStringLiteral("team")}, {QStringLiteral("secretA"), QStringLiteral("AKIATESTKEY0001")},
                                                       {QStringLiteral("secretB"), QStringLiteral("secret-secret-secret-1234")}});
        INFO(saved.value(QStringLiteral("error")).toString().toStdString());
        REQUIRE(saved.value(QStringLiteral("ok")).toBool());
        return saved.value(QStringLiteral("id")).toString();
    }
};

Marker marker(const char* name, int frame) {
    Marker m;
    m.start = framesToTicks(frame, Rate{25, 1});
    m.name = name;
    return m;
}

/// The cloud copy as another computer sees it.
struct OtherComputer {
    cloud::FolderStore store;
    cloud::CloudProjects projects;
    cloud::CloudLink link;
    ProjectState state;
    fs::path cache;
    fs::path folder;
    OtherComputer(const fs::path& root, const fs::path& cacheFolder, const fs::path& projectFolder)
        : store(root), projects(store, cloud::Identity{"bob", "bobs-pc", "bbbbbbbbbbbbbbbb"}, cloud::systemClock(), cloud::systemRandom(), "0.7.0"),
          cache(cacheFolder), folder(projectFolder) {}
    cloud::SyncRequest request() const {
        cloud::SyncRequest r;
        r.local = state;
        r.link = link;
        r.cacheFolder = cache;
        r.projectFolder = folder;
        return r;
    }
};

} // namespace

TEST_CASE("Cloud storage: locations are saved their secrets only in the key store and tested", "[app][cloud]") {
    resetSettings();
    Rig r(false);

    // a synced folder
    const QString folderId = r.addFolderLocation(QStringLiteral("Dropbox projects"), QStringLiteral("projects"));
    // a folder that does not exist, a name used twice
    CHECK_FALSE(r.cloud->saveLocation({{QStringLiteral("name"), QStringLiteral("Nowhere")}, {QStringLiteral("kind"), QStringLiteral("folder")},
                                       {QStringLiteral("purpose"), QStringLiteral("projects")}, {QStringLiteral("folder"), QStringLiteral("Z:/does/not/exist")}})
                     .value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(r.cloud->saveLocation({{QStringLiteral("name"), QStringLiteral("dropbox projects")}, {QStringLiteral("kind"), QStringLiteral("folder")},
                                       {QStringLiteral("purpose"), QStringLiteral("projects")}, {QStringLiteral("folder"), r.dir.path()}})
                     .value(QStringLiteral("ok")).toBool());

    // WebDAV and S3: the secrets go to the key store and nowhere else
    const QVariantMap dav = r.cloud->saveLocation({{QStringLiteral("name"), QStringLiteral("Nextcloud")}, {QStringLiteral("kind"), QStringLiteral("webdav")},
                                                   {QStringLiteral("purpose"), QStringLiteral("projects")},
                                                   {QStringLiteral("url"), QStringLiteral("https://dav.example.com/dav/wabcut")},
                                                   {QStringLiteral("user"), QStringLiteral("ann")}, {QStringLiteral("secretA"), QStringLiteral("correct horse battery")}});
    REQUIRE(dav.value(QStringLiteral("ok")).toBool());
    const QString s3Id = r.addS3Location(QStringLiteral("Bucket"), QStringLiteral("reviews"));
    const QString settingsText = QSettings().value(QStringLiteral("cloud/locations")).toString();
    CHECK_FALSE(settingsText.contains(QStringLiteral("correct horse")));
    CHECK_FALSE(settingsText.contains(QStringLiteral("secret-secret")));
    CHECK_FALSE(settingsText.contains(QStringLiteral("AKIATESTKEY0001")));
    auto stored = r.secrets.read("cloud:" + s3Id.toStdString());
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    CHECK(std::string((*stored)->reveal()).find("secret-secret-secret-1234") != std::string::npos);
    for (const QVariant& row : r.cloud->locations()) {
        const QVariantMap m = row.toMap();
        CHECK(m.value(QStringLiteral("hasSecret")).toBool());
        for (const QString& key : m.keys())
            CHECK_FALSE(m.value(key).toString().contains(QStringLiteral("secret-secret")));
    }

    // an unencrypted address is refused unless it is this computer or the user insists
    const QVariantMap insecure = r.cloud->saveLocation({{QStringLiteral("name"), QStringLiteral("NAS")}, {QStringLiteral("kind"), QStringLiteral("webdav")},
                                                        {QStringLiteral("purpose"), QStringLiteral("projects")}, {QStringLiteral("url"), QStringLiteral("http://192.168.1.5/dav")},
                                                        {QStringLiteral("user"), QStringLiteral("ann")}, {QStringLiteral("secretA"), QStringLiteral("a password")}});
    CHECK_FALSE(insecure.value(QStringLiteral("ok")).toBool());
    CHECK(insecure.value(QStringLiteral("error")).toString().contains(QStringLiteral("not encrypted")));

    // testing: the folder works, a bucket for review links needs no conditional writes, WebDAV with the right password works
    r.cloud->testLocation(folderId);
    r.cloud->testLocation(s3Id);
    r.cloud->testLocation(dav.value(QStringLiteral("id")).toString());
    for (const QVariant& row : r.cloud->locations()) {
        const QVariantMap m = row.toMap();
        INFO(m.value(QStringLiteral("name")).toString().toStdString() << ": " << m.value(QStringLiteral("checkText")).toString().toStdString());
        CHECK(m.value(QStringLiteral("check")).toString() == QStringLiteral("ok"));
    }
    // a wrong password is a problem with words, and the password is not in them
    const QVariantMap wrong = r.cloud->saveLocation({{QStringLiteral("id"), dav.value(QStringLiteral("id"))}, {QStringLiteral("name"), QStringLiteral("Nextcloud")},
                                                     {QStringLiteral("kind"), QStringLiteral("webdav")}, {QStringLiteral("purpose"), QStringLiteral("projects")},
                                                     {QStringLiteral("url"), QStringLiteral("https://dav.example.com/dav/wabcut")}, {QStringLiteral("user"), QStringLiteral("ann")},
                                                     {QStringLiteral("secretA"), QStringLiteral("wrong password")}});
    REQUIRE(wrong.value(QStringLiteral("ok")).toBool());
    r.cloud->testLocation(dav.value(QStringLiteral("id")).toString());
    for (const QVariant& row : r.cloud->locations()) {
        const QVariantMap m = row.toMap();
        if (m.value(QStringLiteral("name")).toString() == QStringLiteral("Nextcloud")) {
            CHECK(m.value(QStringLiteral("check")).toString() == QStringLiteral("problem"));
            CHECK_FALSE(m.value(QStringLiteral("checkText")).toString().contains(QStringLiteral("wrong password")));
        }
    }
    // removing a location takes its secret out of the key store
    REQUIRE(r.cloud->removeLocation(s3Id));
    auto gone = r.secrets.read("cloud:" + s3Id.toStdString());
    REQUIRE(gone);
    CHECK_FALSE(gone->has_value());
    CHECK(r.cloud->locations().size() == 2);
}

TEST_CASE("Cloud project: upload sync with another computer conflicts the undo guard the lock and offline mode", "[app][cloud]") {
    resetSettings();
    Rig r;
    const QString id = r.addFolderLocation(QStringLiteral("Dropbox projects"), QStringLiteral("projects"));
    const fs::path root = toPath(r.dir.filePath(QStringLiteral("Dropbox projects")));

    // what goes up is told first
    const QVariantMap preview = r.cloud->uploadPreview(id);
    REQUIRE(preview.value(QStringLiteral("ok")).toBool());
    CHECK(preview.value(QStringLiteral("bytes")).toULongLong() > 500);
    CHECK_FALSE(preview.value(QStringLiteral("sizeText")).toString().isEmpty());
    CHECK(preview.value(QStringLiteral("notes")).toStringList().size() >= 2);
    CHECK(r.cloud->projectState().value(QStringLiteral("linked")).toBool() == false);

    // offline mode stops it, even into a folder (a sync service uploads it)
    r.ai->setOfflineMode(true);
    CHECK_FALSE(r.cloud->blockedReason().isEmpty());
    CHECK_FALSE(r.cloud->uploadPreview(id).value(QStringLiteral("ok")).toBool());
    r.cloud->createCloudProject(id, QStringLiteral("Film"));
    CHECK(r.cloud->messageIsError());
    CHECK(fs::is_empty(root));
    r.ai->setOfflineMode(false);

    // the upload
    r.cloud->createCloudProject(id, QStringLiteral("Film"));
    INFO(r.cloud->message().toStdString());
    REQUIRE(r.cloud->projectState().value(QStringLiteral("linked")).toBool());
    CHECK(r.cloud->projectState().value(QStringLiteral("revision")).toInt() == 1);
    CHECK(r.cloud->projectState().value(QStringLiteral("lock")).toString() == QStringLiteral("mine"));
    CHECK(fs::exists(cloud::cloudLinkPath(r.controller->projectFile())));
    CHECK(r.cloud->projectState().value(QStringLiteral("name")).toString() == QStringLiteral("Film"));

    // nothing changed: up to date
    r.cloud->syncNow();
    CHECK(r.cloud->message().contains(QStringLiteral("Up to date")));

    // an edit here is sent
    const std::size_t undoBefore = r.undoCount();
    REQUIRE(r.controller->session().apply("Add marker", [&](const ProjectState& s) { return edit::addMarker(s, r.controller->currentSequenceId(), marker("Mine", 5)); }));
    r.cloud->syncNow();
    CHECK(r.cloud->projectState().value(QStringLiteral("revision")).toInt() == 2);
    CHECK(r.undoCount() == undoBefore + 1); // sending adds no undo step

    // another computer changes something else and sends it; here it comes in as ONE undo step
    OtherComputer bob(root, toPath(r.cache.filePath(QStringLiteral("bob"))), toPath(r.dir.filePath(QStringLiteral("bob-project"))));
    auto opened = bob.projects.open(cloud::readCloudLink(r.controller->projectFile())->projectId, s(id), bob.folder, bob.cache);
    REQUIRE(opened);
    bob.state = opened->first;
    bob.link = opened->second;
    ReviewNote note;
    note.sequence = bob.state.sequences.front()->id;
    note.start = framesToTicks(3, Rate{25, 1});
    note.text = "Bob's note";
    note.author = "Bob";
    bob.state = edit::addReviewNote(bob.state, note)->state;
    auto bobPush = bob.projects.sync(bob.request());
    REQUIRE(bobPush);
    CHECK(bobPush->kind == cloud::SyncKind::Pushed);
    bob.link = bobPush->link;

    const std::size_t beforePull = r.undoCount();
    r.cloud->syncNow();
    INFO(r.cloud->message().toStdString());
    CHECK(r.controller->snapshot()->reviewNotes.size() == 1);
    CHECK(r.undoCount() == beforePull + 1);
    CHECK(r.controller->session().undoLabel().value_or("") == "Sync Cloud Project");
    CHECK(r.cloud->projectState().value(QStringLiteral("revision")).toInt() == 3);

    // undoing the sync step must not be able to send the cloud's changes away
    REQUIRE(r.controller->session().undo().has_value());
    CHECK(r.controller->snapshot()->reviewNotes.size() == 0);
    CHECK(r.cloud->projectState().value(QStringLiteral("revertGuard")).toBool());
    r.cloud->syncNow();
    CHECK(r.cloud->messageIsError());
    CHECK(r.cloud->message().contains(QStringLiteral("undid")));
    CHECK(cloud::readCloudLink(r.controller->projectFile())->baseRevision == 3); // nothing was sent
    REQUIRE(r.controller->session().redo().has_value());
    CHECK_FALSE(r.cloud->projectState().value(QStringLiteral("revertGuard")).toBool());

    // both change the same thing: asked, nothing is overwritten
    bob.state = edit::addMarker(bob.state, bob.state.sequences.front()->id, marker("Bob's marker", 8))->state;
    {
        auto sent = bob.projects.sync(bob.request());
        REQUIRE(sent);
        CHECK(sent->kind == cloud::SyncKind::Pushed);
        bob.link = sent->link;
    }
    REQUIRE(r.controller->session().apply("Add marker", [&](const ProjectState& s) { return edit::addMarker(s, r.controller->currentSequenceId(), marker("Mine 2", 9)); }));
    const std::size_t beforeConflict = r.undoCount();
    r.cloud->syncNow();
    REQUIRE(r.cloud->conflicts().size() >= 1);
    CHECK(r.undoCount() == beforeConflict); // nothing changed yet
    r.cloud->resolveConflicts(QStringLiteral("theirs"));
    CHECK(r.cloud->conflicts().isEmpty());
    bool hasBobs = false;
    for (const Marker& m : r.controller->currentSequence()->markers)
        hasBobs = hasBobs || m.name == "Bob's marker";
    CHECK(hasBobs);
}

TEST_CASE("Cloud project: another editor's lock makes the project read-only here", "[app][cloud]") {
    resetSettings();
    Rig r;
    const QString id = r.addFolderLocation(QStringLiteral("Dropbox projects"), QStringLiteral("projects"));
    const fs::path root = toPath(r.dir.filePath(QStringLiteral("Dropbox projects")));
    r.cloud->createCloudProject(id, QStringLiteral("Film"));
    REQUIRE(r.cloud->projectState().value(QStringLiteral("linked")).toBool());
    const std::string projectId = cloud::readCloudLink(r.controller->projectFile())->projectId;

    // we give up our lock (as closing would); bob takes it
    r.cloud->unlinkProject();
    CHECK_FALSE(r.cloud->projectState().value(QStringLiteral("linked")).toBool());
    cloud::FolderStore store(root);
    cloud::CloudLock bobsLock(store, projectId, cloud::Identity{"bob", "bobs-pc", "bbbbbbbbbbbbbbbb"}, "0.7.0");
    REQUIRE(bobsLock.acquire());
    // relink this project to the cloud copy
    cloud::CloudLink link;
    link.location = id.toStdString();
    link.projectId = projectId;
    link.name = "Film";
    link.baseRevision = 1;
    {
        cloud::CloudProjects projects(store, cloud::Identity{"ann", "pc", "aaaaaaaaaaaaaaaa"});
        auto head = projects.head(projectId);
        REQUIRE(head);
        link.baseSha = head->sha256;
        auto normalized = io::toJson(cloud::normalizeForSync(*r.controller->snapshot()));
        REQUIRE(normalized);
        link.baseNormSha = cloud::sha256Hex(*normalized);
    }
    REQUIRE(cloud::writeCloudLink(r.controller->projectFile(), link));
    r.cloud->refreshCloud(); // reads the link file again and asks for the lock
    CHECK(r.cloud->projectState().value(QStringLiteral("lock")).toString() == QStringLiteral("other"));
    CHECK(r.cloud->projectState().value(QStringLiteral("readOnly")).toBool());
    CHECK(r.project->readOnly());
    CHECK(r.cloud->projectState().value(QStringLiteral("lockText")).toString().contains(QStringLiteral("bob")));
    // an edit here is not sent
    REQUIRE(r.controller->session().apply("Add marker", [&](const ProjectState& s) { return edit::addMarker(s, r.controller->currentSequenceId(), marker("Mine", 5)); }));
    r.cloud->syncNow();
    CHECK(r.cloud->messageIsError());
    CHECK(r.cloud->message().contains(QStringLiteral("bob")));
    CHECK(cloud::readCloudLink(r.controller->projectFile())->baseRevision == 1);
}

TEST_CASE("Review links: the review copy publishing comments back as review notes in one step revoking", "[app][cloud][review]") {
    resetSettings();
    Rig r;
    const QString s3Id = r.addS3Location(QStringLiteral("Review bucket"), QStringLiteral("reviews"));
    r.cloud->setRenderForTesting([](const fs::path& output) -> std::expected<std::int64_t, Error> {
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
        std::ofstream out(output, std::ios::binary);
        out << std::string("\0\0\0\x18" "ftypmp42", 12) << std::string(40000, 'v');
        return std::int64_t{40012};
    });

    const QVariantMap estimate = r.cloud->estimateReviewCopy({{QStringLiteral("resolution"), 720}});
    REQUIRE(estimate.value(QStringLiteral("ok")).toBool());
    CHECK(estimate.value(QStringLiteral("seconds")).toDouble() > 0.0);
    CHECK(estimate.value(QStringLiteral("bytes")).toULongLong() > 0);
    CHECK(estimate.value(QStringLiteral("resolution")).toString().contains(QStringLiteral("720")));

    // publishing before there is a copy is refused; nothing was uploaded by making the copy
    r.cloud->publishReviewLink({{QStringLiteral("locationId"), s3Id}});
    CHECK(r.cloud->messageIsError());
    r.cloud->prepareReviewCopy({{QStringLiteral("resolution"), 720}});
    REQUIRE(r.cloud->reviewCopy().value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    CHECK(r.cloud->reviewCopy().value(QStringLiteral("bytes")).toULongLong() == 40012);
    CHECK(r.s3.requests.empty());

    // offline mode stops publishing
    r.ai->setOfflineMode(true);
    r.cloud->publishReviewLink({{QStringLiteral("locationId"), s3Id}});
    CHECK(r.cloud->messageIsError());
    CHECK(r.s3.requests.empty());
    r.ai->setOfflineMode(false);

    r.cloud->publishReviewLink({{QStringLiteral("locationId"), s3Id}, {QStringLiteral("title"), QStringLiteral("Client cut")}, {QStringLiteral("expiryDays"), 3}});
    INFO(r.cloud->message().toStdString());
    REQUIRE(r.cloud->links().size() == 1);
    const QVariantMap link = r.cloud->links().front().toMap();
    CHECK(link.value(QStringLiteral("canCopyLink")).toBool());
    CHECK(link.value(QStringLiteral("url")).toString().contains(QStringLiteral("X-Amz-Signature=")));
    CHECK(r.cloud->reviewCopy().value(QStringLiteral("state")).toString() == QStringLiteral("none"));
    const auto records = cloud::readReviewLinks(r.controller->projectFile());
    REQUIRE(records.size() == 1);
    CHECK(r.s3.everythingSent().find("X-Amz-Signature") == std::string::npos);

    // a reviewer's comments file lands in the link's comments folder (a shared folder reviewers write into)
    cloud::S3Config config;
    config.endpoint = "https://s3.example.com";
    config.bucket = "reviews";
    config.prefix = "team";
    cloud::S3Store bucket(r.router, config, ai::Secret("AKIATESTKEY0001"), ai::Secret("secret-secret-secret-1234"));
    const std::string linkId = records[0].linkId;
    const std::string commentsText = nlohmann::json{{"format", "wabcut-review-comments"}, {"version", 1}, {"link", linkId}, {"reviewer", "Maria"},
                                                    {"comments", nlohmann::json::array({nlohmann::json{{"frame", 10}, {"text", "The logo is too small"},
                                                                                                       {"mark", nlohmann::json{{"x", 0.7}, {"y", 0.05}, {"width", 0.2}, {"height", 0.1}}}},
                                                                                        nlohmann::json{{"frame", 40}, {"text", "Cut this a little shorter"}}})}}.dump();
    REQUIRE(bucket.put(records[0].path() + "/comments/comments-maria-1a2b.json", commentsText, cloud::Precondition::none()));

    const std::size_t undoBefore = r.undoCount();
    r.cloud->collectComments(qs_(linkId), true);
    INFO(r.cloud->message().toStdString());
    CHECK(r.controller->snapshot()->reviewNotes.size() == 2);
    CHECK(r.controller->currentSequence()->markers.size() == 2);
    CHECK(r.undoCount() == undoBefore + 1); // notes and markers: one step
    CHECK(r.controller->session().undoLabel().value_or("") == "Import Review Comments");
    CHECK(r.controller->snapshot()->reviewNotes[0].author == "Maria (review link)");
    CHECK(r.controller->snapshot()->reviewNotes[0].start.value == records[0].startTicks + framesToTicks(10, r.controller->currentSequence()->frameRate).value);
    CHECK(r.cloud->lastImport().value(QStringLiteral("added")).toInt() == 2);
    // the same file again adds nothing and changes nothing
    const std::size_t afterFirst = r.undoCount();
    r.cloud->collectComments(qs_(linkId), true);
    CHECK(r.cloud->lastImport().value(QStringLiteral("added")).toInt() == 0);
    CHECK(r.controller->snapshot()->reviewNotes.size() == 2);
    CHECK(r.undoCount() == afterFirst);
    // a file made for another link is refused
    QFile stray(r.dir.filePath(QStringLiteral("stray.json")));
    REQUIRE(stray.open(QIODevice::WriteOnly));
    stray.write(QByteArray::fromStdString(nlohmann::json{{"format", "wabcut-review-comments"}, {"version", 1}, {"link", "000000000000000000000000"},
                                                         {"comments", nlohmann::json::array({nlohmann::json{{"frame", 1}, {"text", "x"}}})}}.dump()));
    stray.close();
    r.cloud->importCommentsFile(qs_(linkId), QUrl::fromLocalFile(stray.fileName()), false);
    CHECK(r.controller->snapshot()->reviewNotes.size() == 2);
    CHECK(r.cloud->messageIsError());

    // revoking deletes the page, the video and the comments from the storage
    r.cloud->revokeLink(qs_(linkId));
    CHECK(r.cloud->links().isEmpty());
    CHECK(cloud::readReviewLinks(r.controller->projectFile()).empty());
    auto left = bucket.listAll("");
    REQUIRE(left);
    CHECK(left->empty());
}

TEST_CASE("Review links: a folder location the copy can be discarded expired links are removed", "[app][cloud][review]") {
    resetSettings();
    Rig r;
    const QString folderId = r.addFolderLocation(QStringLiteral("Shared reviews"), QStringLiteral("reviews"));
    const QString projectsId = r.addFolderLocation(QStringLiteral("Private"), QStringLiteral("projects"));
    r.cloud->setRenderForTesting([](const fs::path& output) -> std::expected<std::int64_t, Error> {
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
        std::ofstream out(output, std::ios::binary);
        out << std::string(5000, 'v');
        return std::int64_t{5000};
    });
    std::int64_t now = 1793874937;
    r.cloud->setClockForTesting([&now] { return now; });
    r.cloud->prepareReviewCopy({});
    REQUIRE(r.cloud->reviewCopy().value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    r.cloud->discardReviewCopy();
    CHECK(r.cloud->reviewCopy().value(QStringLiteral("state")).toString() == QStringLiteral("none"));
    r.cloud->prepareReviewCopy({});

    // a projects location is not for review links
    r.cloud->publishReviewLink({{QStringLiteral("locationId"), projectsId}});
    CHECK(r.cloud->messageIsError());
    r.cloud->publishReviewLink({{QStringLiteral("locationId"), folderId}, {QStringLiteral("expiryDays"), 1}});
    REQUIRE(r.cloud->links().size() == 1);
    const QVariantMap link = r.cloud->links().front().toMap();
    CHECK_FALSE(link.value(QStringLiteral("canCopyLink")).toBool()); // a folder is shared with the sync service
    const QString folder = r.cloud->linkFolder(link.value(QStringLiteral("id")).toString());
    CHECK(QFile::exists(QDir(folder).filePath(QStringLiteral("index.html"))));
    CHECK(QFile::exists(QDir(folder).filePath(QStringLiteral("review.mp4"))));
    CHECK_FALSE(link.value(QStringLiteral("expired")).toBool());
    // the page does not name the project or this computer
    QFile page(QDir(folder).filePath(QStringLiteral("index.html")));
    REQUIRE(page.open(QIODevice::ReadOnly));
    const QString html = QString::fromUtf8(page.readAll());
    page.close(); // an open file cannot be deleted on Windows
    CHECK_FALSE(html.contains(QStringLiteral("Film.wabcut")));
    CHECK_FALSE(html.contains(r.dir.path()));

    // a day later it has expired and can be removed
    now += 2 * 86400;
    r.cloud->refreshCloud();
    REQUIRE(r.cloud->links().size() == 1);
    CHECK(r.cloud->links().front().toMap().value(QStringLiteral("expired")).toBool());
    r.cloud->removeExpiredLinks();
    INFO(r.cloud->message().toStdString());
    CHECK(r.cloud->links().isEmpty());
    CHECK_FALSE(QDir(folder).exists());
}

TEST_CASE("Review links: the review copy is really rendered by the export engine as an H.264 MP4 with sound", "[app][cloud][review][export]") {
    resetSettings();
    Rig r; // no test renderer: the real one
    r.cloud->prepareReviewCopy({{QStringLiteral("resolution"), 540}});
    INFO(r.cloud->message().toStdString());
    REQUIRE(r.cloud->reviewCopy().value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    const fs::path video = toPath(r.cloud->reviewCopy().value(QStringLiteral("path")).toString());
    REQUIRE(fs::is_regular_file(video));
    std::string bytes;
    {
        std::ifstream in(video, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    REQUIRE(bytes.size() > 5000);
    CHECK(bytes.substr(4, 4) == "ftyp");
    CHECK(bytes.find("avc1") != std::string::npos); // H.264 video
    CHECK(bytes.find("mp4a") != std::string::npos); // AAC sound
    CHECK(r.cloud->reviewCopy().value(QStringLiteral("resolution")).toString().contains(QStringLiteral("960")));
    CHECK(r.cloud->reviewCopy().value(QStringLiteral("bytes")).toULongLong() == bytes.size());
    r.cloud->discardReviewCopy();
    CHECK_FALSE(fs::exists(video)); // discarding takes the file away
}

TEST_CASE("Stock media: key search thumbnails and a download added to the project with its licence (one undo step)", "[app][cloud][stock]") {
    resetSettings();
    Rig r;
    const fs::path mp4 = testMedia("shadowplay_two_track.mp4");
    std::string mp4Bytes;
    {
        std::ifstream in(mp4, std::ios::binary);
        mp4Bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    QByteArray png;
    {
        QImage image(64, 36, QImage::Format_RGB32);
        image.fill(QColor(0x88, 0x55, 0xdd));
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
    }
    const std::string pexelsVideos = R"({"page":1,"per_page":24,"total_results":1,"videos":[{"id":856973,"width":1920,"height":1080,"url":"https://www.pexels.com/video/ocean-waves-856973/",
      "image":"https://images.pexels.com/videos/856973/free-video-856973.jpg","duration":23,"user":{"id":1,"name":"Ann Lee","url":"https://www.pexels.com/@annlee"},
      "video_files":[{"id":2,"quality":"hd","file_type":"video/mp4","width":1920,"height":1080,"fps":25.0,"link":"https://videos.pexels.com/video-files/856973/856973-hd_1920_1080_25fps.mp4"},
                     {"id":1,"quality":"sd","file_type":"video/mp4","width":640,"height":360,"fps":25.0,"link":"https://videos.pexels.com/video-files/856973/856973-sd_640_360_25fps.mp4"}]}]})";
    std::vector<std::string> seen;
    r.router.other = [&](const ai::HttpRequest& request, const ai::HttpStreamHandlers& handlers, std::stop_token) -> std::expected<ai::HttpResponse, ai::AiError> {
        seen.push_back(request.url);
        if (request.url.find("api.pexels.com") != std::string::npos) {
            const std::string key(ai::findHeader(request.headers, "authorization").value_or(""));
            if (key != "PEXELS-KEY-0123456789")
                return cloudtest::answer(request, handlers, 401, {}, "{}");
            return cloudtest::answer(request, handlers, 200, {{"X-Ratelimit-Remaining", "199"}}, pexelsVideos);
        }
        if (request.url == "https://images.pexels.com/videos/856973/free-video-856973.jpg")
            return cloudtest::answer(request, handlers, 200, {}, std::string(png.constData(), static_cast<std::size_t>(png.size())));
        if (request.url.find("856973-hd_1920_1080_25fps.mp4") != std::string::npos)
            return cloudtest::answer(request, handlers, 200, {{"Content-Length", std::to_string(mp4Bytes.size())}}, mp4Bytes);
        return cloudtest::answer(request, handlers, 404, {}, "");
    };

    // no key: asked for, nothing sent
    r.stock->search(QStringLiteral("ocean"));
    CHECK(r.stock->messageIsError());
    CHECK(seen.empty());
    // a key the service refuses is not kept; the right one is, in the key store only
    r.stock->saveKey(QStringLiteral("pexels"), QStringLiteral("WRONG-KEY-0123456789"));
    CHECK(r.stock->messageIsError());
    CHECK_FALSE(r.stock->hasKey());
    r.stock->saveKey(QStringLiteral("pexels"), QStringLiteral("PEXELS-KEY-0123456789"));
    CHECK(r.stock->hasKey());
    auto stored = r.secrets.read("stock:pexels");
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    CHECK(std::string((*stored)->reveal()) == "PEXELS-KEY-0123456789");
    for (const QString& key : QSettings().allKeys())
        CHECK_FALSE(QSettings().value(key).toString().contains(QStringLiteral("PEXELS-KEY")));

    // offline mode: no request
    seen.clear();
    r.ai->setOfflineMode(true);
    r.stock->search(QStringLiteral("ocean"));
    CHECK(r.stock->messageIsError());
    CHECK_FALSE(r.stock->blockedReason().isEmpty());
    CHECK(seen.empty());
    r.ai->setOfflineMode(false);

    r.stock->search(QStringLiteral("ocean"));
    INFO(r.stock->message().toStdString());
    REQUIRE(r.stock->results().size() == 1);
    CHECK(r.stock->quota() == 199);
    const QVariantMap card = r.stock->results().front().toMap();
    CHECK(card.value(QStringLiteral("credit")).toString() == QStringLiteral("Video by Ann Lee on Pexels"));
    CHECK(card.value(QStringLiteral("durationText")).toString() == QStringLiteral("0:23"));
    CHECK(card.value(QStringLiteral("files")).toList().size() == 2);
    r.stock->waitForThumbnails();
    CHECK(r.stock->thumbSerial() >= 1);
    // the key never travelled to a picture or a video host
    for (const std::string& url : seen) {
        CHECK(url.find("PEXELS-KEY") == std::string::npos);
    }

    // the download is added to the project as one undo step, with its licence recorded
    const std::size_t undoBefore = r.undoCount();
    const std::size_t mediaBefore = r.controller->snapshot()->media.size();
    r.stock->add(card.value(QStringLiteral("key")).toString(), card.value(QStringLiteral("files")).toList().front().toMap().value(QStringLiteral("index")).toInt());
    INFO(r.stock->message().toStdString());
    REQUIRE(r.controller->snapshot()->media.size() == mediaBefore + 1);
    CHECK(r.undoCount() == undoBefore + 1);
    CHECK(r.controller->session().undoLabel().value_or("") == "Add Stock Media");
    const MediaItem& added = *r.controller->snapshot()->media.back();
    REQUIRE(added.rights.has_value());
    CHECK(added.rights->source == "Pexels");
    CHECK(added.rights->licence == "Pexels License");
    CHECK(added.rights->creditLine == "Video by Ann Lee on Pexels");
    CHECK(added.rights->status == ClearanceStatus::Cleared);
    CHECK(added.rights->reference == "https://www.pexels.com/video/ocean-waves-856973/");
    const Bin* stockBin = nullptr;
    for (const Bin& b : r.controller->snapshot()->bins) {
        if (b.name == "Stock")
            stockBin = &b;
    }
    REQUIRE(stockBin);
    CHECK(added.bin == stockBin->id);
    CHECK(fs::exists(toPath(r.dir.path()) / "Stock" / "pexels-856973-hd.mp4"));
    // adding it again does not add it twice
    r.stock->add(card.value(QStringLiteral("key")).toString(), card.value(QStringLiteral("files")).toList().front().toMap().value(QStringLiteral("index")).toInt());
    CHECK(r.controller->snapshot()->media.size() == mediaBefore + 1);
    // undo takes it out again
    REQUIRE(r.controller->session().undo().has_value());
    CHECK(r.controller->snapshot()->media.size() == mediaBefore);
}

TEST_CASE("Cloud dialog and Stock panel: the pages the panel and screenshots (dark and light)", "[app][cloud][stock][qml][screenshot]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Rig r;
        if (!dark) {
            r.shell.setAppearance(false, "ultraviolet");
            REQUIRE(r.shell.settle(false, r.shell.themeColor("accentFill")));
        }
        REQUIRE_FALSE(r.shell.grab().isNull());
        QQuickItem* root = r.shell.window->contentItem();
        const QString projectsId = r.addFolderLocation(QStringLiteral("Dropbox projects"), QStringLiteral("projects"));
        r.addS3Location(QStringLiteral("Client reviews"), QStringLiteral("reviews"));
        r.cloud->testLocation(projectsId);

        // the menu commands open the dialog on their pages
        CHECK(evaluate(r.shell, QStringLiteral("CommandPalette.commandState(\"file.cloudProject\").enabled")).toBool());
        CHECK(evaluate(r.shell, QStringLiteral("CommandPalette.commandState(\"file.reviewLinks\").enabled")).toBool());
        evaluate(r.shell, QStringLiteral("runCommand(\"file.cloudProject\")"));
        QObject* loader = r.shell.object("cloudDialogLoader");
        REQUIRE(loader);
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = qvariant_cast<QObject*>(loader->property("item"));
            return dialog && dialog->property("opened").toBool();
        }, 5000));
        CHECK(dialog->property("page").toInt() == 1);
        spin(150);
        QQuickItem* project = byName(root, QStringLiteral("cloudProjectPage"));
        REQUIRE(project);
        QQuickItem* preview = byName(root, QStringLiteral("cloudUploadPreview"));
        REQUIRE(preview);
        QQuickItem* upload = byName(root, QStringLiteral("cloudUpload"));
        REQUIRE(upload);
        CHECK_FALSE(upload->isEnabled()); // nothing goes before the box is ticked
        REQUIRE(saveCard(r.shell, project, dark ? QStringLiteral("cloud-project-dark.png") : QStringLiteral("cloud-project-light.png")));

        // storage page
        dialog->setProperty("page", 0);
        spin(100);
        QQuickItem* storage = byName(root, QStringLiteral("cloudStoragePage"));
        REQUIRE(storage);
        CHECK(byName(root, QStringLiteral("cloudLocation_Dropbox projects")));
        REQUIRE(saveCard(r.shell, storage, dark ? QStringLiteral("cloud-storage-dark.png") : QStringLiteral("cloud-storage-light.png")));

        // review page, with a prepared copy and a published link
        r.cloud->setRenderForTesting([](const fs::path& output) -> std::expected<std::int64_t, Error> {
            std::error_code ec;
            fs::create_directories(output.parent_path(), ec);
            std::ofstream out(output, std::ios::binary);
            out << std::string(2'500'000, 'v');
            return std::int64_t{2'500'000};
        });
        dialog->setProperty("page", 2);
        r.cloud->prepareReviewCopy({});
        spin(150);
        QQuickItem* review = byName(root, QStringLiteral("cloudReviewPage"));
        REQUIRE(review);
        CHECK(byName(root, QStringLiteral("reviewCopyReady"))->isVisible());
        QQuickItem* publish = byName(root, QStringLiteral("reviewPublish"));
        REQUIRE(publish);
        CHECK_FALSE(publish->isEnabled()); // the box is not ticked yet
        REQUIRE(saveCard(r.shell, review, dark ? QStringLiteral("cloud-review-dark.png") : QStringLiteral("cloud-review-light.png")));
        if (dark) {
            // a small window: the dialog stays inside it and its body scrolls
            r.shell.resize(640, 520);
            spin(250);
            QQuickItem* card = popupOf(review);
            REQUIRE(card);
            CHECK(card->width() <= 640.5);
            CHECK(card->height() <= 520.5);
            CHECK(card->mapRectToScene(QRectF(0, 0, card->width(), card->height())).right() <= 640.5);
            REQUIRE(saveCard(r.shell, review, QStringLiteral("cloud-review-narrow-dark.png")));
            r.shell.resize(kShotWidth, kShotHeight);
            spin(150);
        }
        QMetaObject::invokeMethod(dialog, "close");
        spin(100);

        // the Stock panel
        evaluate(r.shell, QStringLiteral("openPanelFromWindowMenu(\"stock\")"));
        QQuickItem* panel = nullptr;
        REQUIRE(waitFor([&] {
            panel = byName(root, QStringLiteral("stockPanel"));
            return panel && panel->isVisible();
        }, 5000));
        spin(150);
        CHECK(byName(root, QStringLiteral("stockKeyBox"))->isVisible()); // no key yet: asked for
        CHECK(byName(root, QStringLiteral("stockSource"))->property("text").toString().contains(QStringLiteral("Pexels")));
        r.ai->setOfflineMode(true);
        spin(100);
        CHECK(byName(root, QStringLiteral("stockBlocked"))->isVisible());
        CHECK_FALSE(byName(root, QStringLiteral("stockSearch"))->isEnabled());
        REQUIRE(saveCard(r.shell, panel, dark ? QStringLiteral("stock-panel-offline-dark.png") : QStringLiteral("stock-panel-offline-light.png")));
        r.ai->setOfflineMode(false);

        // with a key and results: each card says who made the picture, where it is from and what size is added
        QByteArray png;
        {
            QImage image(64, 36, QImage::Format_RGB32);
            image.fill(QColor(0x88, 0x55, 0xdd));
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
        }
        const std::string pngBytes(png.constData(), static_cast<std::size_t>(png.size()));
        const std::string results = R"({"page":1,"per_page":24,"total_results":48,"videos":[
          {"id":856973,"width":1920,"height":1080,"url":"https://www.pexels.com/video/ocean-waves-856973/","image":"https://images.pexels.com/videos/856973/a.jpg","duration":23,
           "user":{"id":1,"name":"Ann Lee","url":"https://www.pexels.com/@annlee"},
           "video_files":[{"id":2,"quality":"hd","file_type":"video/mp4","width":1920,"height":1080,"fps":25.0,"link":"https://videos.pexels.com/video-files/856973/856973-hd_1920_1080_25fps.mp4"},
                          {"id":1,"quality":"sd","file_type":"video/mp4","width":640,"height":360,"fps":25.0,"link":"https://videos.pexels.com/video-files/856973/856973-sd_640_360_25fps.mp4"}]},
          {"id":3015510,"width":3840,"height":2160,"url":"https://www.pexels.com/video/drone-over-a-coast-3015510/","image":"https://images.pexels.com/videos/3015510/b.jpg","duration":75,
           "user":{"id":2,"name":"Jun Park","url":"https://www.pexels.com/@junpark"},
           "video_files":[{"id":4,"quality":"uhd","file_type":"video/mp4","width":3840,"height":2160,"fps":30.0,"link":"https://videos.pexels.com/video-files/3015510/3015510-uhd_3840_2160_30fps.mp4"},
                          {"id":3,"quality":"hd","file_type":"video/mp4","width":1920,"height":1080,"fps":30.0,"link":"https://videos.pexels.com/video-files/3015510/3015510-hd_1920_1080_30fps.mp4"}]}]})";
        r.router.other = [&](const ai::HttpRequest& request, const ai::HttpStreamHandlers& handlers, std::stop_token) -> std::expected<ai::HttpResponse, ai::AiError> {
            if (request.url.find("api.pexels.com") != std::string::npos)
                return cloudtest::answer(request, handlers, 200, {{"X-Ratelimit-Remaining", "199"}}, results);
            return cloudtest::answer(request, handlers, 200, {}, pngBytes);
        };
        r.stock->saveKey(QStringLiteral("pexels"), QStringLiteral("PEXELS-KEY-0123456789"));
        r.stock->search(QStringLiteral("ocean"));
        REQUIRE(r.stock->results().size() == 2);
        r.stock->waitForThumbnails();
        spin(300);
        CHECK_FALSE(byName(root, QStringLiteral("stockKeyBox"))->isVisible()); // the key is saved: the box closes
        REQUIRE(saveCard(r.shell, panel, dark ? QStringLiteral("stock-panel-results-dark.png") : QStringLiteral("stock-panel-results-light.png")));
    }
}

TEST_CASE("Cloud dialog and Stock panel: every control has an accessible name and the keyboard reaches the main buttons", "[app][cloud][stock][qml][a11y]") {
    resetSettings();
    clearMessages();
    A11yScope a11y; // before the shell, so the controls register their names
    Rig r;
    REQUIRE_FALSE(r.shell.grab().isNull());
    QQuickItem* root = r.shell.window->contentItem();
    const QString projectsId = r.addFolderLocation(QStringLiteral("Dropbox projects"), QStringLiteral("projects"));
    r.addFolderLocation(QStringLiteral("Client reviews"), QStringLiteral("reviews"));
    r.cloud->setRenderForTesting([](const fs::path& output) -> std::expected<std::int64_t, Error> {
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
        std::ofstream out(output, std::ios::binary);
        out << std::string(3000, 'v');
        return std::int64_t{3000};
    });
    r.cloud->createCloudProject(projectsId, QStringLiteral("Film"));
    REQUIRE(r.cloud->projectState().value(QStringLiteral("linked")).toBool());
    r.cloud->prepareReviewCopy({});
    evaluate(r.shell, QStringLiteral("runCommand(\"file.cloudProject\")"));
    QObject* loader = r.shell.object("cloudDialogLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = qvariant_cast<QObject*>(loader->property("item"));
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    spin(200);
    QQuickItem* card = popupOf(byName(root, QStringLiteral("cloudTabs")));
    REQUIRE(card);
    for (const int page : {1, 2, 0}) {
        dialog->setProperty("page", page);
        spin(150);
        if (page == 0)
            QMetaObject::invokeMethod(byName(root, QStringLiteral("cloudLocationAdd")), "clicked"); // opens the form
        spin(100);
        const QStringList issues = auditAccessibility(card);
        INFO("page " << page << ": " << join(issues));
        CHECK(issues.isEmpty());
    }
    CHECK(tabReaches(card, QStringLiteral("cloudTabs")));
    CHECK(tabReaches(card, QStringLiteral("cloudLocationSave")));
    dialog->setProperty("page", 1);
    spin(150);
    CHECK(tabReaches(card, QStringLiteral("cloudSync")));
    dialog->setProperty("page", 2);
    spin(150);
    CHECK(tabReaches(card, QStringLiteral("reviewMake")) == false); // a prepared copy hides it
    CHECK_FALSE(byName(root, QStringLiteral("reviewPublish"))->isEnabled()); // nothing goes before the box is ticked
    REQUIRE(byName(root, QStringLiteral("cloudReviewPage")));
    byName(root, QStringLiteral("cloudReviewPage"))->setProperty("confirmed", true);
    spin(100);
    CHECK(tabReaches(card, QStringLiteral("reviewPublish")));
    QMetaObject::invokeMethod(dialog, "close");
    spin(100);

    evaluate(r.shell, QStringLiteral("openPanelFromWindowMenu(\"stock\")"));
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = byName(root, QStringLiteral("stockPanel"));
        return panel && panel->isVisible();
    }, 5000));
    spin(200);
    // Qt gives a field in password mode no accessible name through the system interface (the key must stay masked), so the key
    // field is described instead: the audit leaves it out and this checks the description names the service.
    const QStringList issues = auditAccessibility(panel, {QStringLiteral("stockKeyField")});
    INFO(join(issues));
    CHECK(issues.isEmpty());
    QQuickItem* keyField = byName(root, QStringLiteral("stockKeyField"));
    REQUIRE(keyField);
    CHECK(keyField->property("echoMode").toInt() == 2); // TextInput.Password
    QAccessibleInterface* keyIface = QAccessible::queryAccessibleInterface(keyField);
    REQUIRE(keyIface);
    CHECK(keyIface->text(QAccessible::Description).contains(QStringLiteral("Pexels")));
    CHECK(tabReaches(panel, QStringLiteral("stockQuery")));
    CHECK(tabReaches(panel, QStringLiteral("stockKeyField")));
}
