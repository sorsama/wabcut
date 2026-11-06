// 0.7, batch A5 (settings, backups and the taskbar):
//   * 028 the settings bundle - export, import and reset: no secrets, a round trip, untrusted input, staging;
//   * 029 where autosaves and .bak copies go (BackupSettings, ProjectFile.backupLocation / backupCount / autosaveCopies);
//   * 030 the Windows taskbar jump list's entries and its keeper (JumpList).
// Screenshots under test-output/ui (settings-a5-*.png).

#include "ShellFixture.h"

#include "AppSettingsBridge.h"
#include "BackupSettings.h"
#include "BridgeIds.h"
#include "EditingBridge.h"
#include "JumpList.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SettingsBundle.h"
#include "TimelineFixture.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExpression>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "a5" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
}

QUrl url(const fs::path& p) {
    return QUrl::fromLocalFile(q(p));
}

ProjectState withSequences(int n) {
    ProjectState state;
    for (int i = 0; i < n; ++i) {
        auto created = edit::addSequence(state, edit::SequenceSettings{});
        REQUIRE(created);
        state = std::move(created->state);
    }
    return state;
}

void addSequence(ProjectController& controller) {
    REQUIRE(controller.session().apply("add sequence", [](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto created = edit::addSequence(s, edit::SequenceSettings{});
        if (!created)
            return std::unexpected(created.error());
        return std::move(created->state);
    }));
}

void writeText(const QString& path, const QByteArray& content) {
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(content) == content.size());
}

QByteArray readText(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QStringList filesUnder(const QString& root) {
    QStringList out;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        out << QDir(root).relativeFilePath(it.next());
    out.sort();
    return out;
}

/// A bundle as JSON text with the given settings and files (area, name, content).
struct FileSpec {
    QString area;
    QString name;
    QByteArray content;
};

QByteArray bundleText(const QJsonObject& settings, const std::vector<FileSpec>& files, int version = 1,
                      const QString& format = QStringLiteral("wabcut-settings")) {
    QJsonArray fileArray;
    for (const FileSpec& f : files) {
        fileArray.append(QJsonObject{{QStringLiteral("area"), f.area},
                                     {QStringLiteral("name"), f.name},
                                     {QStringLiteral("data"), QString::fromLatin1(f.content.toBase64())}});
    }
    QJsonObject o{{QStringLiteral("format"), format},
                  {QStringLiteral("version"), version},
                  {QStringLiteral("created"), QStringLiteral("2026-11-05T10:00:00Z")},
                  {QStringLiteral("app"), QStringLiteral("0.7.0")},
                  {QStringLiteral("settings"), settings},
                  {QStringLiteral("files"), fileArray}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

/// The two folders a bundle's files live in, below a fresh temporary folder.
struct Folders {
    fs::path root;
    SettingsBundleLocations locations;
    explicit Folders(const char* name) : root(workDir(name)) {
        locations.configDir = q(root / "config");
        locations.dataDir = q(root / "data");
        QDir().mkpath(locations.configDir);
        QDir().mkpath(locations.dataDir);
    }
    QString config(const QString& rel) const { return QDir(locations.configDir).filePath(rel); }
    QString data(const QString& rel) const { return QDir(locations.dataDir).filePath(rel); }
};

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

/// An object by name below `root`: its QObject children and the items of an item.
QObject* findObject(QObject* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QObject* child : root->children()) {
        if (QObject* found = findObject(child, name))
            return found;
    }
    if (const auto* item = qobject_cast<QQuickItem*>(root)) {
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = findObject(child, name))
                return found;
        }
    }
    return nullptr;
}

QObject* openSettingsAt(Shell& shell, const QString& page, const QString& row) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, page), Q_ARG(QString, row)));
    QObject* loader = shell.object("settingsWindowLoader");
    REQUIRE(loader);
    QObject* window = loader->property("item").value<QObject*>();
    REQUIRE(window);
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    spin(150);
    return window;
}

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name, int margin = 16) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                         .adjusted(-margin, -margin, margin, margin)
                         .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

// =====================================================================================================================
// 029: where autosaves and backups go
// =====================================================================================================================

TEST_CASE("Backup settings: a new install starts with the Auto-Save folder, an older one stays beside the project (card 029)",
          "[app][settings][backup][a5]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    {
        QSettings fresh(dir.filePath(QStringLiteral("fresh.ini")), QSettings::IniFormat);
        backup::chooseDefaultForNewInstall(fresh);
        CHECK(fresh.value(QStringLiteral("project/backupLocation")).toString() == QStringLiteral("folder"));
    }
    {
        QSettings older(dir.filePath(QStringLiteral("older.ini")), QSettings::IniFormat);
        older.setValue(QStringLiteral("window/nativeFrame"), true); // anything from an earlier version
        backup::chooseDefaultForNewInstall(older);
        CHECK(older.value(QStringLiteral("project/backupLocation")).toString() == QStringLiteral("beside"));
    }
    {
        QSettings chosen(dir.filePath(QStringLiteral("chosen.ini")), QSettings::IniFormat);
        chosen.setValue(QStringLiteral("project/backupLocation"), QStringLiteral("chosen"));
        backup::chooseDefaultForNewInstall(chosen); // the user's own choice is never replaced
        CHECK(chosen.value(QStringLiteral("project/backupLocation")).toString() == QStringLiteral("chosen"));
    }

    QSettings s(dir.filePath(QStringLiteral("read.ini")), QSettings::IniFormat);
    CHECK(backup::locationFromSettings(s).mode == io::BackupLocation::Mode::Beside); // nothing set
    CHECK(backup::backupCountFromSettings(s) == 5);
    CHECK(backup::autosaveCopiesFromSettings(s) == 3);
    s.setValue(QStringLiteral("project/backupLocation"), QStringLiteral("folder"));
    CHECK(backup::locationFromSettings(s).mode == io::BackupLocation::Mode::Folder);
    // "chosen" needs a folder that is a full path; without one it is beside the project.
    s.setValue(QStringLiteral("project/backupLocation"), QStringLiteral("chosen"));
    CHECK(backup::locationFromSettings(s).mode == io::BackupLocation::Mode::Beside);
    s.setValue(QStringLiteral("project/backupFolder"), QStringLiteral("relative/folder"));
    CHECK(backup::locationFromSettings(s).mode == io::BackupLocation::Mode::Beside);
    s.setValue(QStringLiteral("project/backupFolder"), QDir::cleanPath(dir.filePath(QStringLiteral("Backups"))));
    CHECK(backup::locationFromSettings(s).mode == io::BackupLocation::Mode::Chosen);
    // Another place chosen later: the folder is still known (copies written there are still found).
    s.setValue(QStringLiteral("project/backupLocation"), QStringLiteral("folder"));
    CHECK(backup::locationFromSettings(s).mode == io::BackupLocation::Mode::Folder);
    CHECK_FALSE(backup::locationFromSettings(s).root.empty());
    s.setValue(QStringLiteral("project/backupLocation"), QStringLiteral("chosen"));
    // Counts are bounded, junk reads as the default.
    s.setValue(QStringLiteral("project/backupCount"), 99);
    s.setValue(QStringLiteral("project/autosaveCopies"), 0);
    CHECK(backup::backupCountFromSettings(s) == 10);
    CHECK(backup::autosaveCopiesFromSettings(s) == 1);
    s.setValue(QStringLiteral("project/backupCount"), QStringLiteral("many"));
    CHECK(backup::backupCountFromSettings(s) == 5);
    s.setValue(QStringLiteral("project/backupCount"), -4);
    CHECK(backup::backupCountFromSettings(s) == 0);
    CHECK(backup::normalizedLocation(QStringLiteral("elsewhere")) == QStringLiteral("beside"));
}

TEST_CASE("Backup settings reach the document: the Auto-Save folder, a chosen folder and the counts (card 029)",
          "[app][settings][backup][a5]") {
    resetSettings();
    const fs::path dir = workDir("backup-settings");
    const fs::path project = dir / "film.wabcut";
    const fs::path copies = dir / "Wabcut Auto-Save";
    REQUIRE(io::saveProject(withSequences(1), project, io::SaveOptions{0}));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    int changes = 0;
    QObject::connect(&lifecycle, &ProjectLifecycle::backupSettingsChanged, [&] { ++changes; });

    // Nothing chosen: the settings of every version before 0.7.
    CHECK(lifecycle.backupLocation() == QStringLiteral("beside"));
    CHECK(lifecycle.backupCount() == 5);
    CHECK(lifecycle.autosaveCopies() == 3);
    REQUIRE(lifecycle.open(url(project)));
    CHECK(lifecycle.backupWhere() == QStringLiteral("Next to the project"));
    addSequence(controller);
    REQUIRE(lifecycle.save());
    CHECK(fs::exists(io::backupPath(project, 1)));
    CHECK(lifecycle.hasRecoveryVersions(url(project)));

    // The Auto-Save folder: the next saves and autosaves go there; the copies from before are still listed.
    lifecycle.setBackupLocation(QStringLiteral("folder"));
    CHECK(changes > 0);
    CHECK(QSettings().value(QStringLiteral("project/backupLocation")).toString() == QStringLiteral("folder"));
    CHECK(lifecycle.backupWhere().contains(QStringLiteral("Wabcut Auto-Save")));
    addSequence(controller);
    REQUIRE(lifecycle.save());
    CHECK(fs::exists(io::backupPath(project, 1, copies)));
    addSequence(controller);
    REQUIRE(lifecycle.autosaveNow());
    CHECK(fs::exists(io::autosavePath(project, copies)));
    CHECK_FALSE(fs::exists(io::autosavePath(project)));
    CHECK(lifecycle.hasRecoveryVersions(url(project)));

    // How many backups: 1 keeps one, 0 none; the number is bounded.
    lifecycle.setBackupCount(1);
    addSequence(controller);
    REQUIRE(lifecycle.save());
    addSequence(controller);
    REQUIRE(lifecycle.save());
    CHECK(fs::exists(io::backupPath(project, 1, copies)));
    CHECK_FALSE(fs::exists(io::backupPath(project, 2, copies)));
    lifecycle.setBackupCount(99);
    CHECK(lifecycle.backupCount() == 10);
    lifecycle.setAutosaveCopies(0);
    CHECK(lifecycle.autosaveCopies() == 1);
    lifecycle.setAutosaveCopies(2);
    CHECK(QSettings().value(QStringLiteral("project/autosaveCopies")).toInt() == 2);

    // A chosen folder: one folder per project below it; choosing it switches to it.
    const fs::path root = dir / "Elsewhere";
    lifecycle.setBackupFolder(url(root));
    CHECK(lifecycle.backupLocation() == QStringLiteral("chosen"));
    CHECK(lifecycle.backupFolder().isLocalFile());
    const fs::path chosenDir = io::backupDirectory(project, io::BackupLocation{io::BackupLocation::Mode::Chosen, root});
    CHECK(QDir::fromNativeSeparators(lifecycle.backupWhere()) == QDir::fromNativeSeparators(q(chosenDir)));
    addSequence(controller);
    REQUIRE(lifecycle.save());
    CHECK(fs::exists(io::backupPath(project, 1, chosenDir)));

    // Back beside the project: the settings are remembered, the files already written are not moved or lost.
    lifecycle.setBackupLocation(QStringLiteral("beside"));
    CHECK(QSettings().value(QStringLiteral("project/backupLocation")).toString() == QStringLiteral("beside"));
    CHECK(fs::exists(io::backupPath(project, 1, chosenDir)));
    CHECK(lifecycle.hasRecoveryVersions(url(project)));
}

// =====================================================================================================================
// 028: the settings bundle
// =====================================================================================================================

TEST_CASE("Settings bundle: no secret is ever exported (card 028)", "[app][settings][bundle][a5]") {
    resetSettings();
    Folders f("bundle-secrets");
    QSettings s;
    s.setValue(QStringLiteral("timeline/linkedSelection"), true);
    s.setValue(QStringLiteral("ai/keys/openai/hasKey"), true);
    s.setValue(QStringLiteral("ai/keys/openai/last4"), QStringLiteral("1234"));
    s.setValue(QStringLiteral("ai/plaintextKeys"), true);
    s.setValue(QStringLiteral("ai/customProviders"),
               QStringLiteral(R"([{"id":"custom-x","baseUrl":"https://user:hunter2secret@models.example.com/v1"}])"));
    s.setValue(QStringLiteral("extra/apiToken"), QStringLiteral("not-shown"));
    s.setValue(QStringLiteral("extra/note"), QStringLiteral("sk-ant-abcdefghijklmnopqrstuvwxyz0123456789"));
    s.setValue(QStringLiteral("extra/bearer"), QStringLiteral("Authorization: Bearer abcdefghijklmnopqrstuvwxyz012345"));
    s.setValue(QStringLiteral("extra/url"), QStringLiteral("https://models.example.com/v1?api_key=abcdefgh12345678"));
    s.setValue(QStringLiteral("window/width"), 1500); // only fits this screen
    // The legacy plaintext key file lives in the config folder and is never read.
    writeText(f.config(QStringLiteral("ai-keys.json")), R"({"openai":"sk-live-SECRETSECRETSECRETSECRET"})");
    writeText(f.config(QStringLiteral("keymap/overrides-windows.json")), R"({"version":1})");

    SettingsBundle bundle(f.locations);
    int skipped = 0;
    const QJsonObject built = bundle.build(&skipped);
    const QByteArray text = QJsonDocument(built).toJson();
    const QJsonObject settings = built.value(QStringLiteral("settings")).toObject();
    CHECK(settings.contains(QStringLiteral("timeline/linkedSelection")));
    for (const char* secret : {"ai/keys", "ai/plaintextKeys", "extra/apiToken", "extra/note", "extra/bearer", "extra/url",
                               "ai/customProviders", "window/width"})
        CHECK_FALSE(settings.contains(QString::fromLatin1(secret)));
    for (const char* leak : {"hunter2", "sk-ant", "sk-live", "SECRETSECRET", "not-shown", "abcdefgh12345678", "Bearer"})
        CHECK_FALSE(text.contains(leak));
    CHECK(skipped >= 3); // the values that looked like secrets were counted (a key named like one is left out unseen)
    // The files: the shortcuts yes, the key file never.
    QStringList names;
    for (const QJsonValue& file : built.value(QStringLiteral("files")).toArray())
        names << file.toObject().value(QStringLiteral("name")).toString();
    CHECK(names == QStringList{QStringLiteral("overrides-windows.json")});

    // The same rules, as functions.
    CHECK(SettingsBundle::isSecretKey(QStringLiteral("ai/keys/anthropic/last4")));
    CHECK(SettingsBundle::isSecretKey(QStringLiteral("x/clientSecret")));
    CHECK(SettingsBundle::isSecretKey(QStringLiteral("x/Password")));
    CHECK(SettingsBundle::isSecretKey(QStringLiteral("x/api_key")));
    CHECK(SettingsBundle::isSecretKey(QStringLiteral("x/privateKey")));
    CHECK_FALSE(SettingsBundle::isSecretKey(QStringLiteral("ai/maxTokens")));
    CHECK_FALSE(SettingsBundle::isSecretKey(QStringLiteral("timeline/snapToKeyframes")));
    CHECK(SettingsBundle::looksLikeSecret(QStringLiteral("sk-proj-abcdefghijklmnopqrstuvwxyz")));
    CHECK(SettingsBundle::looksLikeSecret(QStringLiteral("AIzaSyA-abcdefghijklmnopqrstuvwxyz0123456")));
    CHECK(SettingsBundle::looksLikeSecret(QStringLiteral("ghp_abcdefghijklmnopqrstuvwxyz0123456789")));
    CHECK(SettingsBundle::looksLikeSecret(QStringLiteral("-----BEGIN RSA PRIVATE KEY----- MIIE")));
    CHECK_FALSE(SettingsBundle::looksLikeSecret(QStringLiteral("D:/Projects/Film.wabcut")));
    CHECK_FALSE(SettingsBundle::looksLikeSecret(QStringLiteral("https://github.com/sorsama/wabcut/releases")));
    CHECK_FALSE(SettingsBundle::looksLikeSecret(QStringLiteral("short")));
}

TEST_CASE("Settings bundle: export, change, import gives the settings and files back (card 028)", "[app][settings][bundle][a5]") {
    resetSettings();
    Folders f("bundle-roundtrip");
    SettingsBundle bundle(f.locations);
    QSettings s;
    s.setValue(QStringLiteral("timeline/linkedSelection"), false);
    s.setValue(QStringLiteral("timeline/trimLargeOffset"), 12);
    s.setValue(QStringLiteral("monitor/zoom"), 0.75);
    s.setValue(QStringLiteral("appearance/themeName"), QStringLiteral("Daylight"));
    s.setValue(QStringLiteral("highlights/games"), QStringList{QStringLiteral("a"), QStringLiteral("b")});
    s.setValue(QStringLiteral("layout/blob"), QByteArray("\x01\x02\x03\x04", 4));
    s.setValue(QStringLiteral("home/recentProjects"),
               QVariantList{QVariantMap{{QStringLiteral("path"), QStringLiteral("D:/Films/One.wabcut")},
                                        {QStringLiteral("opened"), QStringLiteral("2026-11-01T10:00:00")}}});
    s.setValue(QStringLiteral("ai/keys/openai/hasKey"), true); // the keychain's registry: not touched by an import
    s.setValue(QStringLiteral("window/width"), 1500);          // this screen's: not touched either
    writeText(f.config(QStringLiteral("keymap/overrides-windows.json")), R"({"version":1,"bindings":{"a":"b"}})");
    writeText(f.config(QStringLiteral("Export Presets/Mine.json")), R"({"name":"Mine"})");
    writeText(f.data(QStringLiteral("effects-library.json")), R"({"presets":[]})");
    writeText(f.data(QStringLiteral("templates/Lower third.wabcut-template.json")), R"({"format":"t"})");
    writeText(f.data(QStringLiteral("spelling-dictionary.txt")), "wabcut\nzyphite\n");

    const QString file = f.config(QStringLiteral("out/settings.json"));
    const QVariantMap exported = bundle.exportTo(file);
    REQUIRE(exported.value(QStringLiteral("ok")).toBool());
    CHECK(exported.value(QStringLiteral("files")).toInt() == 5);
    CHECK(exported.value(QStringLiteral("settings")).toInt() == 7); // the keychain's registry and the window's place are not in it
    // The file is complete JSON (written whole) and leaves no temporary file.
    CHECK_FALSE(QJsonDocument::fromJson(readText(file)).isNull());
    CHECK(filesUnder(QFileInfo(file).absolutePath()) == QStringList{QStringLiteral("settings.json")});

    // The preview says what it holds without changing anything.
    const QVariantMap preview = SettingsBundle::previewMap(SettingsBundle::parseFile(file));
    REQUIRE(preview.value(QStringLiteral("ok")).toBool());
    CHECK(preview.value(QStringLiteral("files")).toInt() == 5);
    CHECK(preview.value(QStringLiteral("areas")).toList().size() == 5);
    CHECK(preview.value(QStringLiteral("text")).toString().contains(QStringLiteral("setting")));
    CHECK(QSettings().value(QStringLiteral("timeline/trimLargeOffset")).toInt() == 12);

    // Everything changes meanwhile.
    s.setValue(QStringLiteral("timeline/linkedSelection"), true);
    s.setValue(QStringLiteral("timeline/trimLargeOffset"), 3);
    s.setValue(QStringLiteral("monitor/zoom"), 2.0);
    s.setValue(QStringLiteral("extra/newer"), QStringLiteral("added after the export"));
    s.remove(QStringLiteral("appearance/themeName"));
    s.setValue(QStringLiteral("ai/keys/openai/hasKey"), false);
    s.setValue(QStringLiteral("window/width"), 1111);
    writeText(f.config(QStringLiteral("keymap/overrides-windows.json")), R"({"version":1,"bindings":{"changed":"yes"}})");
    QFile::remove(f.config(QStringLiteral("Export Presets/Mine.json")));
    writeText(f.config(QStringLiteral("Export Presets/Other.json")), R"({"name":"Other"})");
    writeText(f.data(QStringLiteral("spelling-dictionary.txt")), "different\n");

    // Import: staged (with a backup of what is there), applied at the restart.
    const QVariantMap staged = bundle.stageImport(file);
    REQUIRE(staged.value(QStringLiteral("ok")).toBool());
    CHECK(bundle.hasStaged());
    const QString backupFile = staged.value(QStringLiteral("backup")).toString();
    REQUIRE(QFileInfo(backupFile).isFile());
    CHECK(QFileInfo(backupFile).absolutePath() == QDir(f.locations.backupsDir()).absolutePath());
    CHECK(QSettings().value(QStringLiteral("timeline/trimLargeOffset")).toInt() == 3); // nothing changed yet
    CHECK(bundle.applyStaged() == QStringLiteral("import"));
    CHECK_FALSE(bundle.hasStaged());
    CHECK(bundle.applyStaged().isEmpty()); // once

    QSettings after;
    CHECK(after.value(QStringLiteral("timeline/linkedSelection")).toBool() == false);
    CHECK(after.value(QStringLiteral("timeline/trimLargeOffset")).toInt() == 12);
    CHECK(after.value(QStringLiteral("monitor/zoom")).toDouble() == 0.75);
    CHECK(after.value(QStringLiteral("appearance/themeName")).toString() == QStringLiteral("Daylight"));
    CHECK(after.value(QStringLiteral("highlights/games")).toStringList() == QStringList{QStringLiteral("a"), QStringLiteral("b")});
    CHECK(after.value(QStringLiteral("layout/blob")).toByteArray() == QByteArray("\x01\x02\x03\x04", 4));
    const QVariantList recents = after.value(QStringLiteral("home/recentProjects")).toList();
    REQUIRE(recents.size() == 1);
    CHECK(recents.front().toMap().value(QStringLiteral("path")).toString() == QStringLiteral("D:/Films/One.wabcut"));
    CHECK_FALSE(after.contains(QStringLiteral("extra/newer"))); // what the file does not have goes
    // The keychain's registry and this screen's window are not part of an import.
    CHECK(after.value(QStringLiteral("ai/keys/openai/hasKey")).toBool() == false);
    CHECK(after.value(QStringLiteral("window/width")).toInt() == 1111);
    // Files: replaced where the bundle has them, removed where it has fewer.
    CHECK(readText(f.config(QStringLiteral("keymap/overrides-windows.json"))).contains("\"a\""));
    CHECK(readText(f.config(QStringLiteral("Export Presets/Mine.json"))).contains("Mine"));
    CHECK_FALSE(QFileInfo::exists(f.config(QStringLiteral("Export Presets/Other.json"))));
    CHECK(readText(f.data(QStringLiteral("spelling-dictionary.txt"))) == "wabcut\nzyphite\n");
    CHECK(QFileInfo::exists(f.data(QStringLiteral("templates/Lower third.wabcut-template.json"))));

    // The backup holds what was there before the import and can itself be imported.
    const SettingsBundle::Parsed backup = SettingsBundle::parseFile(backupFile);
    REQUIRE(backup.ok);
    CHECK(backup.bundle.value(QStringLiteral("settings")).toObject().value(QStringLiteral("extra/newer")).toString() ==
          QStringLiteral("added after the export"));
}

TEST_CASE("Settings bundle: an imported file is untrusted input (card 028)", "[app][settings][bundle][a5]") {
    resetSettings();
    Folders f("bundle-untrusted");
    QSettings().setValue(QStringLiteral("keep/me"), 1);

    // Not a settings file at all.
    CHECK_FALSE(SettingsBundle::parse("").ok);
    CHECK_FALSE(SettingsBundle::parse("not json").ok);
    CHECK_FALSE(SettingsBundle::parse("[1,2,3]").ok);
    CHECK_FALSE(SettingsBundle::parse(bundleText({}, {}, 1, QStringLiteral("something-else"))).ok);
    CHECK_FALSE(SettingsBundle::parse(bundleText({}, {}, 0)).ok);
    const auto newer = SettingsBundle::parse(bundleText({}, {}, 99));
    CHECK_FALSE(newer.ok);
    CHECK(newer.error.contains(QStringLiteral("newer")));
    CHECK_FALSE(SettingsBundle::parse(QByteArray(17 * 1024 * 1024, ' ')).ok);
    CHECK_FALSE(SettingsBundle::parseFile(f.config(QStringLiteral("missing.json"))).ok);

    // Settings: bad names, secrets, nulls, big or deep values are dropped one by one; the good ones stay.
    QJsonObject settings;
    settings.insert(QStringLiteral("good/value"), 5);
    settings.insert(QStringLiteral("good/text"), QStringLiteral("fine"));
    settings.insert(QStringLiteral("../escape"), 1);
    settings.insert(QStringLiteral("bad//empty"), 1);
    settings.insert(QStringLiteral("/leading"), 1);
    settings.insert(QStringLiteral("trailing/"), 1);
    settings.insert(QStringLiteral("with\nnewline"), 1);
    settings.insert(QStringLiteral("a/b/c/d..e"), 1);
    settings.insert(QStringLiteral("ai/keys/openai/hasKey"), true);
    settings.insert(QStringLiteral("x/apiToken"), QStringLiteral("abc"));
    settings.insert(QStringLiteral("x/leak"), QStringLiteral("sk-abcdefghijklmnopqrstuvwxyz0123"));
    settings.insert(QStringLiteral("x/null"), QJsonValue());
    settings.insert(QStringLiteral("window/width"), 5000);
    QJsonValue deep = 1;
    for (int i = 0; i < 20; ++i)
        deep = QJsonArray{deep};
    settings.insert(QStringLiteral("x/deep"), deep);
    settings.insert(QStringLiteral("x/nestedSecret"), QJsonObject{{QStringLiteral("password"), QStringLiteral("p")}});
    settings.insert(QStringLiteral("x/huge"), QString(2 * 1024 * 1024, QLatin1Char('a')));
    const SettingsBundle::Parsed parsed = SettingsBundle::parse(bundleText(settings, {}));
    REQUIRE(parsed.ok);
    const QJsonObject kept = parsed.bundle.value(QStringLiteral("settings")).toObject();
    CHECK(kept.keys() == QStringList{QStringLiteral("good/text"), QStringLiteral("good/value")});
    CHECK(parsed.settings == 2);
    CHECK(parsed.skipped >= 10);
    CHECK(parsed.skippedWhy.size() <= 5);

    // Files: only known areas, one-level names of the right kind, content that is what it says it is.
    const QByteArray json = "{\"ok\":true}";
    const std::vector<FileSpec> files{
        {QStringLiteral("keymap"), QStringLiteral("overrides-windows.json"), json},                 // good
        {QStringLiteral("keymap"), QStringLiteral("../../evil.json"), json},                        // traversal
        {QStringLiteral("keymap"), QStringLiteral("sub/dir.json"), json},                           // two levels
        {QStringLiteral("keymap"), QStringLiteral("..\\evil.json"), json},                          // Windows separator
        {QStringLiteral("keymap"), QStringLiteral("C:\\evil.json"), json},                          // drive
        {QStringLiteral("keymap"), QStringLiteral("CON.json"), json},                               // device name
        {QStringLiteral("keymap"), QStringLiteral(".hidden.json"), json},                           // leading dot
        {QStringLiteral("keymap"), QStringLiteral("notjson.txt"), json},                            // wrong kind
        {QStringLiteral("keymap"), QStringLiteral("broken.json"), "{not json"},                     // does not parse
        {QStringLiteral("keymap"), QStringLiteral("empty.json"), ""},                               // empty
        {QStringLiteral("exportPresets"), QStringLiteral("Mine.json"), json},                       // good
        {QStringLiteral("exportPresets"), QStringLiteral("mine.JSON"), json},                       // the same name again (Windows)
        {QStringLiteral("system32"), QStringLiteral("x.json"), json},                               // unknown area
        {QStringLiteral("effectLibrary"), QStringLiteral("other-name.json"), json},                 // a single-file area has one name
        {QStringLiteral("effectLibrary"), QStringLiteral("effects-library.json"), json},            // good
        {QStringLiteral("spelling"), QStringLiteral("spelling-dictionary.txt"), "words\n"},         // good
        {QStringLiteral("spelling"), QStringLiteral("spelling-dictionary.txt.exe"), "MZ"},          // wrong kind
        {QStringLiteral("graphicTemplates"), QStringLiteral("big.wabcut-template.json"),
         "{\"x\":\"" + QByteArray(3 * 1024 * 1024, 'a') + "\"}"},                                   // over 2 MB
    };
    const SettingsBundle::Parsed withFiles = SettingsBundle::parse(bundleText({}, files));
    REQUIRE(withFiles.ok);
    QStringList names;
    for (const QJsonValue& v : withFiles.bundle.value(QStringLiteral("files")).toArray())
        names << v.toObject().value(QStringLiteral("area")).toString() + "/" + v.toObject().value(QStringLiteral("name")).toString();
    names.sort();
    CHECK(names == QStringList{QStringLiteral("effectLibrary/effects-library.json"), QStringLiteral("exportPresets/Mine.json"),
                               QStringLiteral("keymap/overrides-windows.json"), QStringLiteral("spelling/spelling-dictionary.txt")});

    // Too many files or settings is refused whole.
    std::vector<FileSpec> many;
    for (int i = 0; i < SettingsBundle::kMaxFiles + 1; ++i)
        many.push_back({QStringLiteral("keymap"), QStringLiteral("k%1.json").arg(i), json});
    CHECK_FALSE(SettingsBundle::parse(bundleText({}, many)).ok);
    QJsonObject lots;
    for (int i = 0; i < SettingsBundle::kMaxSettings + 1; ++i)
        lots.insert(QStringLiteral("many/k%1").arg(i), i);
    CHECK_FALSE(SettingsBundle::parse(bundleText(lots, {})).ok);

    // Applying the cleaned bundle writes inside the two folders only, and only what was kept.
    SettingsBundle::applyBundle(withFiles.bundle, f.locations);
    QStringList written = filesUnder(f.locations.configDir) + filesUnder(f.locations.dataDir);
    written.sort();
    CHECK(written == QStringList{QStringLiteral("Export Presets/Mine.json"), QStringLiteral("effects-library.json"),
                                 QStringLiteral("keymap/overrides-windows.json"), QStringLiteral("spelling-dictionary.txt")});
    CHECK_FALSE(QFileInfo::exists(q(f.root / "evil.json")));
    CHECK_FALSE(QFileInfo::exists(q(f.root.parent_path() / "evil.json")));

    // A staged import is checked again when it is applied: a file edited in between cannot smuggle anything in.
    const QString stagedFile = f.locations.stagedFile();
    writeText(stagedFile, QJsonDocument(QJsonObject{
                              {QStringLiteral("action"), QStringLiteral("import")},
                              {QStringLiteral("bundle"),
                               QJsonDocument::fromJson(bundleText(settings, {{QStringLiteral("keymap"), QStringLiteral("..\\evil.json"), json}}))
                                   .object()}})
                              .toJson());
    SettingsBundle bundle(f.locations);
    CHECK(bundle.applyStaged() == QStringLiteral("import"));
    CHECK_FALSE(QSettings().contains(QStringLiteral("x/leak")));
    CHECK_FALSE(QSettings().contains(QStringLiteral("ai/keys/openai/hasKey")));
    CHECK_FALSE(QFileInfo::exists(q(f.root / "evil.json")));
    // Junk in the staged file does nothing (and does not run again).
    writeText(stagedFile, "{ garbage");
    CHECK(bundle.applyStaged().isEmpty());
    CHECK_FALSE(bundle.hasStaged());
}

// A settings file from someone else must not decide where this person's keys or media go.
TEST_CASE("Settings bundle: a file never sets provider addresses, watch folders, cloud locations or network paths (card 028)",
          "[app][settings][bundle][a5]") {
    resetSettings();
    Folders f("bundle-redirect");
    for (const char* key : {"ai/baseUrls/azure", "ai/baseUrls", "AI/BaseUrls/bedrock", "ai/customProviders", "ai/consent", "ai/spend",
                            "export/watchFolders", "cloud/locations", "cloud/other/x"})
        CHECK(SettingsBundle::isRedirectingKey(QString::fromLatin1(key)));
    for (const char* key : {"ai/maxTokens", "ai/userModels", "export/lastPreset", "cloudy/mood", "timeline/snap"})
        CHECK_FALSE(SettingsBundle::isRedirectingKey(QString::fromLatin1(key)));
    CHECK(SettingsBundle::holdsNetworkPath(QStringLiteral("//attacker/share")));
    CHECK(SettingsBundle::holdsNetworkPath(QStringLiteral("\\\\attacker\\share\\out")));
    CHECK(SettingsBundle::holdsNetworkPath(QStringLiteral("\\\\?\\UNC\\attacker\\share")));
    CHECK(SettingsBundle::holdsNetworkPath(QJsonArray{QStringLiteral("D:/x"), QStringLiteral("/\\attacker/share")}));
    CHECK(SettingsBundle::holdsNetworkPath(QJsonObject{{QStringLiteral("output"), QStringLiteral("//attacker/share")}}));
    CHECK_FALSE(SettingsBundle::holdsNetworkPath(QStringLiteral("D:/Projects/Film.wabcut")));
    CHECK_FALSE(SettingsBundle::holdsNetworkPath(QStringLiteral("/home/me/media")));
    CHECK_FALSE(SettingsBundle::holdsNetworkPath(QStringLiteral("https://github.com/sorsama/wabcut")));

    QJsonObject settings;
    settings.insert(QStringLiteral("timeline/snap"), true);
    settings.insert(QStringLiteral("ai/baseUrls/azure"), QStringLiteral("https://attacker.example.com/v1"));
    settings.insert(QStringLiteral("ai/customProviders"), QStringLiteral(R"([{"id":"custom-openrouter","baseUrl":"https://attacker.example.com/v1"}])"));
    settings.insert(QStringLiteral("ai/consent"), QStringLiteral("{}"));
    settings.insert(QStringLiteral("export/watchFolders"), QStringLiteral(R"([{"input":"D:/in","output":"//attacker/share"}])"));
    settings.insert(QStringLiteral("cloud/locations"), QStringLiteral("[]"));
    settings.insert(QStringLiteral("media/cacheFolder"), QStringLiteral("//attacker/share/cache"));
    const SettingsBundle::Parsed parsed = SettingsBundle::parse(bundleText(settings, {}));
    REQUIRE(parsed.ok);
    CHECK(parsed.bundle.value(QStringLiteral("settings")).toObject().keys() == QStringList{QStringLiteral("timeline/snap")});
    CHECK(parsed.settings == 1);
    CHECK(parsed.skipped == 6);

    // The cloud locations are this computer's own accounts: not exported either.
    QSettings().setValue(QStringLiteral("cloud/locations"), QStringLiteral(R"([{"url":"https://dav.example.com"}])"));
    QSettings().setValue(QStringLiteral("timeline/snap"), true);
    const SettingsBundle bundle(f.locations);
    const QJsonObject built = bundle.build().value(QStringLiteral("settings")).toObject();
    CHECK(built.contains(QStringLiteral("timeline/snap")));
    CHECK_FALSE(built.contains(QStringLiteral("cloud/locations")));
}

TEST_CASE("Settings bundle: reset backs up, keeps what is asked and leaves content and secrets alone (card 028)",
          "[app][settings][bundle][a5]") {
    resetSettings();
    Folders f("bundle-reset");
    SettingsBundle bundle(f.locations);
    QSettings s;
    s.setValue(QStringLiteral("timeline/trimLargeOffset"), 12);
    s.setValue(QStringLiteral("appearance/themeName"), QStringLiteral("Daylight"));
    s.setValue(QStringLiteral("home/recentProjects"), QVariantList{QVariantMap{{QStringLiteral("path"), QStringLiteral("D:/x.wabcut")}}});
    s.setValue(QStringLiteral("home/starred"), QStringLiteral("[\"D:/x.wabcut\"]"));
    s.setValue(QStringLiteral("aiFeatures/enabled"), true);
    s.setValue(QStringLiteral("updates/automatic"), true);
    s.setValue(QStringLiteral("ai/keys/openai/hasKey"), true);
    s.setValue(QStringLiteral("window/width"), 1500);
    writeText(f.config(QStringLiteral("keymap/overrides-windows.json")), "{}");
    writeText(f.config(QStringLiteral("keymap/overrides-macos.json")), "{}");
    writeText(f.config(QStringLiteral("Export Presets/Mine.json")), R"({"name":"Mine"})");
    writeText(f.data(QStringLiteral("spelling-dictionary.txt")), "wabcut\n");

    CHECK(bundle.nextBackupName().startsWith(QStringLiteral("settings-")));
    const QVariantMap staged = bundle.stageReset(true, QStringLiteral("before-reset.json"));
    REQUIRE(staged.value(QStringLiteral("ok")).toBool());
    CHECK(QFileInfo(staged.value(QStringLiteral("backup")).toString()).fileName() == QStringLiteral("before-reset.json"));
    const SettingsBundle::Parsed backup = SettingsBundle::parseFile(staged.value(QStringLiteral("backup")).toString());
    REQUIRE(backup.ok);
    CHECK(backup.bundle.value(QStringLiteral("settings")).toObject().contains(QStringLiteral("appearance/themeName")));
    CHECK(QSettings().value(QStringLiteral("timeline/trimLargeOffset")).toInt() == 12); // not yet
    CHECK(bundle.applyStaged() == QStringLiteral("reset"));

    QSettings after;
    CHECK_FALSE(after.contains(QStringLiteral("timeline/trimLargeOffset")));
    CHECK_FALSE(after.contains(QStringLiteral("appearance/themeName")));
    CHECK(after.contains(QStringLiteral("home/recentProjects")));
    CHECK(after.contains(QStringLiteral("home/starred")));
    CHECK(after.contains(QStringLiteral("aiFeatures/enabled")));
    CHECK(after.contains(QStringLiteral("updates/automatic")));
    CHECK(after.contains(QStringLiteral("ai/keys/openai/hasKey"))); // the keychain's registry
    CHECK(after.value(QStringLiteral("window/width")).toInt() == 1500);
    CHECK_FALSE(QFileInfo::exists(f.config(QStringLiteral("keymap/overrides-windows.json"))));
    CHECK_FALSE(QFileInfo::exists(f.config(QStringLiteral("keymap/overrides-macos.json"))));
    CHECK(QFileInfo::exists(f.config(QStringLiteral("Export Presets/Mine.json")))); // content, not settings
    CHECK(QFileInfo::exists(f.data(QStringLiteral("spelling-dictionary.txt"))));

    // Without keepRecents the recent projects and opt-ins go too; a taken backup name gets a number.
    QSettings().setValue(QStringLiteral("timeline/trimLargeOffset"), 7);
    const QVariantMap second = bundle.stageReset(false, QStringLiteral("before-reset.json"));
    REQUIRE(second.value(QStringLiteral("ok")).toBool());
    CHECK(QFileInfo(second.value(QStringLiteral("backup")).toString()).fileName() != QStringLiteral("before-reset.json"));
    CHECK(bundle.applyStaged() == QStringLiteral("reset"));
    QSettings last;
    CHECK_FALSE(last.contains(QStringLiteral("home/recentProjects")));
    CHECK_FALSE(last.contains(QStringLiteral("aiFeatures/enabled")));
    CHECK(last.contains(QStringLiteral("ai/keys/openai/hasKey")));

    // A staged change can be called off (the restart was cancelled).
    REQUIRE(bundle.stageReset(true).value(QStringLiteral("ok")).toBool());
    CHECK(bundle.hasStaged());
    bundle.discardStaged();
    CHECK_FALSE(bundle.hasStaged());
    CHECK(bundle.applyStaged().isEmpty());
    // Backups are bounded: only the newest few stay.
    for (int i = 0; i < SettingsBundle::kKeptBackups + 3; ++i)
        REQUIRE_FALSE(bundle.writeBackup(QStringLiteral("settings-2026-01-%1.json").arg(10 + i)).isEmpty());
    CHECK(QDir(f.locations.backupsDir()).entryList({QStringLiteral("settings-*.json")}, QDir::Files).size() ==
          SettingsBundle::kKeptBackups);
}

TEST_CASE("AppSettings exports, previews, imports and resets through the bridge (card 028)", "[app][settings][bundle][a5]") {
    resetSettings();
    Folders f("bundle-bridge");
    AppSettingsBridge bridge;
    bridge.setBundleLocations(f.locations);
    QSettings().setValue(QStringLiteral("timeline/trimLargeOffset"), 9);
    int staged = 0;
    QObject::connect(&bridge, &AppSettingsBridge::changeStagedChanged, [&] { ++staged; });

    const fs::path file = f.root / "mine.json";
    const QVariantMap exported = bridge.exportSettings(url(file));
    REQUIRE(exported.value(QStringLiteral("ok")).toBool());
    CHECK(fs::exists(file));
    CHECK_FALSE(bridge.exportSettings(QUrl(QStringLiteral("https://example.com/x.json"))).value(QStringLiteral("ok")).toBool());
    CHECK(bridge.exportSettings(url(f.root / "no" / "such" / "deep.json")).value(QStringLiteral("ok")).toBool()); // folders are created

    const QVariantMap preview = bridge.previewImport(url(file));
    CHECK(preview.value(QStringLiteral("ok")).toBool());
    CHECK(preview.value(QStringLiteral("settings")).toInt() >= 1);
    CHECK_FALSE(bridge.changeStaged());
    CHECK_FALSE(bridge.previewImport(url(f.root / "missing.json")).value(QStringLiteral("ok")).toBool());
    writeText(q(f.root / "bad.json"), "{\"format\":\"wabcut-project\"}");
    const QVariantMap bad = bridge.previewImport(url(f.root / "bad.json"));
    CHECK_FALSE(bad.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(bad.value(QStringLiteral("error")).toString().isEmpty());
    CHECK_FALSE(bridge.importSettings(url(f.root / "bad.json")).value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(bridge.changeStaged());

    const QVariantMap plan = bridge.planReset();
    CHECK(plan.value(QStringLiteral("backupName")).toString().endsWith(QStringLiteral(".json")));
    CHECK_FALSE(plan.value(QStringLiteral("backupFolder")).toString().isEmpty());
    const QVariantMap reset = bridge.resetAllSettings(true, plan.value(QStringLiteral("backupName")).toString());
    REQUIRE(reset.value(QStringLiteral("ok")).toBool());
    CHECK(QFileInfo(reset.value(QStringLiteral("backup")).toString()).fileName() == plan.value(QStringLiteral("backupName")).toString());
    CHECK(bridge.changeStaged());
    CHECK(staged >= 1);
    bridge.discardStagedChange();
    CHECK_FALSE(bridge.changeStaged());

    const QVariantMap imported = bridge.importSettings(url(file));
    REQUIRE(imported.value(QStringLiteral("ok")).toBool());
    CHECK(bridge.changeStaged());
    bridge.discardStagedChange();
    // The Settings search knows the new rows.
    for (const char* id : {"exportSettings", "importSettings", "resetSettings", "backupLocation", "backupCount", "autosaveCopies", "jumpList"})
        CHECK_FALSE(bridge.entry(QString::fromLatin1(id)).isEmpty());
    const QVariantList found = bridge.search(QStringLiteral("reset defaults"));
    REQUIRE_FALSE(found.isEmpty());
    CHECK(found.first().toMap().value(QStringLiteral("id")).toString() == QStringLiteral("resetSettings"));
}

// =====================================================================================================================
// 030: the taskbar jump list
// =====================================================================================================================

TEST_CASE("Jump list entries: starred first, then recent, no duplicates, no missing files, titles told apart (card 030)",
          "[app][jumplist][a5]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto make = [&](const char* folder, const char* name) {
        const QString path = QDir(dir.path()).filePath(QString::fromLatin1(folder) + QLatin1Char('/') + QString::fromLatin1(name));
        writeText(path, "{}");
        return path;
    };
    const QString alpha = make("a", "Alpha.wabcut");
    const QString beta = make("a", "Beta.wabcut");
    const QString gamma = make("b", "Gamma.wabcut");
    const QString twinA = make("a", "Same.wabcut");
    const QString twinB = make("b", "Same.wabcut");
    const QString gone = QDir(dir.path()).filePath(QStringLiteral("a/Gone.wabcut"));

    const auto entries = jumpListEntries({alpha, gone, beta, alpha, gamma}, {gamma, gone}, 10);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].title == QStringLiteral("Gamma"));
    CHECK(entries[0].starred);
    CHECK(entries[1].title == QStringLiteral("Alpha"));
    CHECK_FALSE(entries[1].starred);
    CHECK(entries[2].title == QStringLiteral("Beta"));
    CHECK(entries[0].path == QDir::toNativeSeparators(gamma));
    CHECK(entries[0].tooltip == entries[0].path);

    // The limit counts the starred ones first; zero lists nothing.
    CHECK(jumpListEntries({alpha, beta}, {gamma}, 2).size() == 2);
    CHECK(jumpListEntries({alpha, beta}, {gamma}, 2)[0].title == QStringLiteral("Gamma"));
    CHECK(jumpListEntries({alpha, beta}, {gamma}, 0).empty());
    CHECK(jumpListEntries({}, {}).empty());
    CHECK(jumpListEntries({QString(), QStringLiteral("  ")}, {}).empty());

    // The same file written another way is the same project.
    CHECK(jumpListEntries({alpha, QDir::toNativeSeparators(alpha), alpha + QStringLiteral("/../Alpha.wabcut")}, {}).size() == 1);

    // Two projects with one name get their folder in the title.
    const auto twins = jumpListEntries({twinA, twinB}, {});
    REQUIRE(twins.size() == 2);
    CHECK(twins[0].title == QStringLiteral("Same (a)"));
    CHECK(twins[1].title == QStringLiteral("Same (b)"));

    // An injected existence check (the keeper's tests).
    const auto fake = jumpListEntries({QStringLiteral("/x/One.wabcut"), QStringLiteral("/x/Two.wabcut")}, {}, 10,
                                      [](const QString& p) { return p.endsWith(QStringLiteral("Two.wabcut")); });
    REQUIRE(fake.size() == 1);
    CHECK(fake[0].title == QStringLiteral("Two"));
}

TEST_CASE("The jump list keeper rebuilds the list from the stored projects, debounced, and deletes it when switched off (card 030)",
          "[app][jumplist][a5]") {
    resetSettings();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto make = [&](const char* name) {
        const QString path = QDir(dir.path()).filePath(QString::fromLatin1(name));
        writeText(path, "{}");
        return path;
    };
    const QString one = make("One.wabcut");
    const QString two = make("Two.wabcut");
    const QString three = make("Three.wabcut");
    QSettings s;
    s.setValue(QStringLiteral("home/recentProjects"),
               QVariantList{QVariantMap{{QStringLiteral("path"), one}}, QVariantMap{{QStringLiteral("path"), two}}});
    s.setValue(QStringLiteral("home/starred"), QString::fromUtf8(QJsonDocument(QJsonArray{three}).toJson(QJsonDocument::Compact)));
    CHECK(JumpList::storedRecent() == QStringList{one, two});
    CHECK(JumpList::storedStarred() == QStringList{three});

    JumpList list;
#ifdef _WIN32
    CHECK(list.available());
#else
    CHECK_FALSE(list.available());
#endif
    CHECK(list.enabled()); // on unless switched off
    std::vector<std::vector<JumpListEntry>> writes;
    int clears = 0;
    list.setBackend({[&](const std::vector<JumpListEntry>& e) {
                         writes.push_back(e);
                         return true;
                     },
                     [&] {
                         ++clears;
                         return true;
                     }});
    REQUIRE(list.refreshNow());
    REQUIRE(writes.size() == 1);
    REQUIRE(writes[0].size() == 3);
    CHECK(writes[0][0].title == QStringLiteral("Three")); // starred first
    CHECK(writes[0][0].starred);
    CHECK(writes[0][1].title == QStringLiteral("One"));
    CHECK(list.lastEntries().size() == 3);

    // Several changes in a row make one rebuild, about a second later.
    s.setValue(QStringLiteral("home/recentProjects"), QVariantList{QVariantMap{{QStringLiteral("path"), two}}});
    list.refresh();
    list.refresh();
    list.refresh();
    spin(200);
    CHECK(writes.size() == 1); // not yet
    REQUIRE(waitFor([&] { return writes.size() == 2; }, 5000));
    spin(1300);
    CHECK(writes.size() == 2); // one, not three
    CHECK(writes[1].size() == 2);

    // Switched off: the list is deleted and stays so; switched on: it is written again.
    list.setEnabled(false);
    CHECK_FALSE(list.enabled());
    CHECK(QSettings().value(QStringLiteral("jumpList/enabled")).toBool() == false);
    CHECK(clears == 1);
    CHECK(list.lastEntries().empty());
    list.setEnabled(false); // no change, nothing runs
    CHECK(clears == 1);
    list.setEnabled(true);
    CHECK(writes.size() == 3);

    // A refused list (Windows said no) is not remembered as written.
    list.setBackend({[](const std::vector<JumpListEntry>&) { return false; }, [] { return false; }});
    CHECK_FALSE(list.refreshNow());
    CHECK(list.lastEntries().empty());
}

// =====================================================================================================================
// Settings > General: the rows and the dialog
// =====================================================================================================================

TEST_CASE("Settings > General has the backup rows, the jump list switch and Backup and reset, dark and light (cards 028 029 030)",
          "[app][settings][ui][a5][screenshot]") {
    // Two views of the page: the backup rows (Projects) and, further down, Backup and reset; each in both themes.
    for (const int variant : {0, 1, 2, 3}) {
        const bool dark = variant % 2 == 0;
        const bool resetSection = variant >= 2;
        INFO((dark ? "dark" : "light"));
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        QObject* window = openSettingsAt(shell, QStringLiteral("general"),
                                         resetSection ? QStringLiteral("settingsBackup") : QStringLiteral("backupLocation"));
        auto* content = window->property("contentItem").value<QQuickItem*>();
        REQUIRE(content);

        // 029: the location tabs, the counts and the description naming where the copies go.
        QQuickItem* tabs = findNamed(content, QStringLiteral("backupLocationTabs"));
        REQUIRE(tabs);
        CHECK(tabs->property("currentIndex").toInt() == 0); // nothing chosen: next to the project
        CHECK(findNamed(content, QStringLiteral("settingsBackupCount")));
        CHECK(findNamed(content, QStringLiteral("settingsAutosaveCopies")));
        QQuickItem* browse = findNamed(content, QStringLiteral("backupFolderBrowse"));
        REQUIRE(browse);
        CHECK_FALSE(browse->isVisible()); // only for "A folder"
        // 030: the switch is on by default and only works where there is a jump list.
        QQuickItem* jump = findNamed(content, QStringLiteral("jumpListToggle"));
        REQUIRE(jump);
#ifdef _WIN32
        CHECK(jump->property("checked").toBool());
        CHECK(jump->property("enabled").toBool());
#else
        CHECK_FALSE(jump->property("enabled").toBool()); // there is no jump list here
#endif
        // 028: the three rows.
        for (const char* name : {"exportSettingsButton", "importSettingsButton", "resetSettingsButton"})
            CHECK(findNamed(content, QString::fromLatin1(name)));

        if (resetSection) { // scroll to the end of the page (once it has been laid out: a hidden window lays out when it renders)
            REQUIRE_FALSE(shell.grab().isNull());
            spin(100);
            QQuickItem* section = findNamed(content, QStringLiteral("settingsBackupSection"));
            REQUIRE(section);
            for (QQuickItem* up = section->parentItem(); up; up = up->parentItem()) {
                if (up->property("contentHeight").isValid() && up->property("contentY").isValid()) {
                    up->setProperty("contentY", std::max(0.0, up->property("contentHeight").toDouble() - up->height()));
                    break;
                }
            }
            spin(200);
        }
        // The page, as the user sees it (the backup rows are scrolled into view by the search key).
        const QImage pageShot = shell.grab();
        REQUIRE_FALSE(pageShot.isNull());
        auto* popup = qobject_cast<QQuickItem*>(content->parentItem());
        REQUIRE(popup);
        const QString stem = resetSection ? QStringLiteral("settings-a5-backup-reset") : QStringLiteral("settings-a5-general");
        saveCrop(pageShot, popup, stem + (dark ? QStringLiteral(".png") : QStringLiteral("-light.png")));
        checkNoMessages();
    }
}

TEST_CASE("The reset dialog names the backup file and offers to keep recent projects (card 028)",
          "[app][settings][ui][a5][screenshot]") {
    for (const bool dark : {true, false}) {
        INFO((dark ? "dark" : "light"));
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        QObject* window = openSettingsAt(shell, QStringLiteral("general"), QStringLiteral("settingsBackup"));
        QObject* dialog = findObject(window, QStringLiteral("settingsBackupDialog"));
        REQUIRE(dialog);
        REQUIRE(QMetaObject::invokeMethod(dialog, "openReset"));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
        spin(150);
        auto* backupText = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("settingsBackupBackupText")));
        REQUIRE(backupText);
        const QVariant planValue = dialog->property("plan");
        const QVariantMap plan = planValue.metaType() == QMetaType::fromType<QJSValue>() ? planValue.value<QJSValue>().toVariant().toMap() : planValue.toMap();
        const QString planned = plan.value(QStringLiteral("backupName")).toString();
        CHECK(planned.startsWith(QStringLiteral("settings-")));
        CHECK(backupText->property("text").toString().contains(planned)); // the dialog names the file
        CHECK(findObject(dialog, QStringLiteral("settingsBackupKeepToggle")));
        auto* confirm = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("settingsBackupConfirm")));
        REQUIRE(confirm);
        CHECK(confirm->property("text").toString().contains(QStringLiteral("Reset")));
        const QImage shot = shell.grab();
        REQUIRE_FALSE(shot.isNull());
        auto* popup = dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(popup);
        saveCrop(shot, popup->parentItem(), dark ? QStringLiteral("settings-a5-reset.png") : QStringLiteral("settings-a5-reset-light.png"));
        // Cancel changes nothing.
        QMetaObject::invokeMethod(dialog, "close");
        spin(100);
        CHECK_FALSE(SettingsBundle().hasStaged());
        checkNoMessages();
    }
}

TEST_CASE("The import dialog shows what a settings file holds, or why it cannot be imported (card 028)",
          "[app][settings][ui][a5][screenshot]") {
    for (const bool dark : {true, false}) {
        INFO((dark ? "dark" : "light"));
        resetSettings();
        clearMessages();
        Folders f("bundle-dialog");
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        QObject* window = openSettingsAt(shell, QStringLiteral("general"), QStringLiteral("settingsBackup"));
        QObject* dialog = findObject(window, QStringLiteral("settingsBackupDialog"));
        REQUIRE(dialog);

        QSettings().setValue(QStringLiteral("timeline/trimLargeOffset"), 12);
        writeText(f.config(QStringLiteral("keymap/overrides-windows.json")), "{}");
        AppSettingsBridge bridge;
        bridge.setBundleLocations(f.locations);
        const fs::path file = f.root / "mine.json";
        REQUIRE(bridge.exportSettings(url(file)).value(QStringLiteral("ok")).toBool());
        const QVariantMap preview = bridge.previewImport(url(file));
        REQUIRE(preview.value(QStringLiteral("ok")).toBool());

        REQUIRE(QMetaObject::invokeMethod(dialog, "openImport", Q_ARG(QVariant, QVariant::fromValue(preview)),
                                          Q_ARG(QVariant, QVariant::fromValue(url(file)))));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
        spin(150);
        CHECK(findObject(dialog, QStringLiteral("settingsBackupContents")));
        auto* confirm = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("settingsBackupConfirm")));
        REQUIRE(confirm);
        CHECK(confirm->isVisible());
        CHECK(confirm->property("text").toString().contains(QStringLiteral("restart")));
        const QImage shot = shell.grab();
        REQUIRE_FALSE(shot.isNull());
        auto* popup = dialog->property("contentItem").value<QQuickItem*>();
        saveCrop(shot, popup->parentItem(), dark ? QStringLiteral("settings-a5-import.png") : QStringLiteral("settings-a5-import-light.png"));
        QMetaObject::invokeMethod(dialog, "close");
        spin(100);

        // A file that is not a settings file: the reason and only Close.
        const QVariantMap bad = bridge.previewImport(url(f.root / "missing.json"));
        REQUIRE_FALSE(bad.value(QStringLiteral("ok")).toBool());
        REQUIRE(QMetaObject::invokeMethod(dialog, "openImport", Q_ARG(QVariant, QVariant::fromValue(bad)),
                                          Q_ARG(QVariant, QVariant::fromValue(url(f.root / "missing.json")))));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
        spin(100);
        auto* hiddenConfirm = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("settingsBackupConfirm")));
        REQUIRE(hiddenConfirm);
        CHECK_FALSE(hiddenConfirm->isVisible());
        QMetaObject::invokeMethod(dialog, "close");
        checkNoMessages();
    }
}

// =====================================================================================================================
// 046: the sequence work area in the app
// =====================================================================================================================

TEST_CASE("Alt+[ and Alt+] set the work area at the playhead, one undo step each; Sequence Settings shows and clears it (card 046)",
          "[app][workarea][a5]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    const auto workArea = [&] { return controller.currentSequence()->workArea; };
    CHECK_FALSE(workArea().has_value());

    // Out first: from 0 to the playhead.
    controller.setPlayhead(f(5));
    const auto steps = controller.session().undoCount();
    REQUIRE(controller.runCommand(QStringLiteral("timeline.setWorkAreaOut")));
    REQUIRE(workArea().has_value());
    CHECK(workArea()->start == Ticks{});
    CHECK(workArea()->end() == f(5));
    CHECK(controller.session().undoCount() == steps + 1);
    CHECK(controller.undoText().contains(QStringLiteral("Work Area")));
    // In: the playhead; the Out stays.
    controller.setPlayhead(f(1));
    REQUIRE(controller.runCommand(QStringLiteral("timeline.setWorkAreaIn")));
    CHECK(workArea()->start == f(1));
    CHECK(workArea()->end() == f(5));
    CHECK(controller.session().undoCount() == steps + 2);
    // An In at or after the Out is refused (no step, a reason).
    controller.setPlayhead(f(6));
    REQUIRE(controller.runCommand(QStringLiteral("timeline.setWorkAreaIn")));
    CHECK(controller.session().undoCount() == steps + 2);
    CHECK_FALSE(controller.lastError().isEmpty());
    CHECK(workArea()->start == f(1));
    // Undo walks back, one end at a time.
    REQUIRE(controller.undo());
    CHECK(workArea()->start == Ticks{});
    REQUIRE(controller.undo());
    CHECK_FALSE(workArea().has_value());

    // Sequence Settings reads the work area as timecodes, and the command clears it.
    EditingBridge editing;
    editing.setController(&controller);
    controller.setPlayhead(f(10));
    REQUIRE(controller.runCommand(QStringLiteral("timeline.setWorkAreaOut")));
    const QVariantMap settings = editing.sequenceSettings(sequenceKey(tl.seq));
    CHECK(settings.value(QStringLiteral("workAreaIn")).toString() == QStringLiteral("00:00:00:00"));
    CHECK_FALSE(settings.value(QStringLiteral("workAreaOut")).toString().isEmpty());
    CHECK(editing.applySequenceSettings(sequenceKey(tl.seq), QVariantMap{{QStringLiteral("clearWorkArea"), true}}).isEmpty());
    CHECK_FALSE(workArea().has_value());
    CHECK(editing.sequenceSettings(sequenceKey(tl.seq)).value(QStringLiteral("workAreaIn")).toString().isEmpty());
}

TEST_CASE("Sequence Settings shows the work area and Clear removes it, dark and light (card 046)",
          "[app][workarea][qml][screenshot][a5]") {
    for (const bool dark : {true, false}) {
        INFO((dark ? "dark" : "light"));
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        Timeline tl(R"(
            V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
            A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        )");
        controller->resetProject(tl.state, tl.seq);
        controller->setPlayhead(f(10));
        REQUIRE(controller->runCommand(QStringLiteral("timeline.setWorkAreaOut")));
        REQUIRE(controller->currentSequence()->workArea.has_value());
        auto* editing = shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
        REQUIRE(editing);
        REQUIRE(editing->runCommand(QStringLiteral("sequence.settings"))); // Sequence > Sequence Settings
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = shell.window->findChild<QObject*>(QStringLiteral("sequenceSettingsDialog"));
            return dialog && dialog->property("opened").toBool();
        }, 5000));
        spin(150);
        auto* text = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("sequenceSettingsWorkArea")));
        REQUIRE(text);
        CHECK(text->property("text").toString().contains(QStringLiteral("00:00:00:00")));
        auto* clear = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("sequenceSettingsClearWorkArea")));
        REQUIRE(clear);
        CHECK(clear->isVisible());
        const QImage shot = shell.grab();
        REQUIRE_FALSE(shot.isNull());
        auto* popup = dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(popup);
        saveCrop(shot, popup->parentItem(), dark ? QStringLiteral("sequence-settings-a5-workarea.png") : QStringLiteral("sequence-settings-a5-workarea-light.png"));
        // Clear, then OK: the work area is gone in one undo step.
        REQUIRE(QMetaObject::invokeMethod(clear, "clicked"));
        spin(50);
        CHECK(text->property("text").toString().contains(QStringLiteral("Not set")));
        const auto steps = controller->session().undoCount();
        auto* ok = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("sequenceSettingsOk")));
        REQUIRE(ok);
        REQUIRE(QMetaObject::invokeMethod(ok, "clicked"));
        spin(100);
        CHECK_FALSE(controller->currentSequence()->workArea.has_value());
        CHECK(controller->session().undoCount() == steps + 1);
        checkNoMessages();
    }
}
