// M5m (A.6): the audio plugins in the UI, offscreen, with the test plugins built in tests/plughost (WabcutTestGain.clap /
// .vst3, and two CLAP files that crash or hang while scanned) and the real wabcut-plughost helper. No plugin editor
// window is ever opened: "Edit plugin..." runs through injected hooks.
//   * the browser: the scan, the blocklist, groups and search, the folders;
//   * applying a plugin to a clip (a video clip's linked audio), a track and the master, one undo step each; a drop on
//     the timeline through the view's handler;
//   * badges: a plugin this computer does not have (missing), one that crashed;
//   * Edit plugin: the opener is asked, the editor's state changes are saved (merged into one undo step), a manual save;
//   * the shell: the Effects panel's plugin section, Effect Controls' plugin header, the mixer's plugin slots, the
//     settings dialog. Screenshots: plugin-browser.png, plugin-effect-controls.png, audio-plugins-settings.png.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "EffectControls.h"
#include "EffectPresetsBridge.h"
#include "EffectsTreeModel.h"
#include "PluginBridge.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <catch2/catch_test_macros.hpp>

#if defined(WABCUT_WITH_AUDIO_PLUGINS) && defined(WABCUT_TEST_PLUGIN_DIR)

#include <wabcut/audio/effects/EffectChain.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/plughost/Ipc.h>
#include <wabcut/plughost/PluginCatalog.h>
#include <wabcut/plughost/PluginEffects.h>
#include <wabcut/plughost/PluginHost.h>

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

const QString kClap = QStringLiteral("plugin.clap.org.wabcut.test.gain");

std::filesystem::path pluginDir() { return std::filesystem::path(WABCUT_TEST_PLUGIN_DIR); }

std::filesystem::path outputFile(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "app-plugins";
    std::filesystem::create_directories(dir);
    return dir / name;
}

/// The test plugin folders as a catalog (not scanned yet: the bridge scans), a host and the plugin effects installed
/// into the engine and the model for the test's lifetime. `blocked` adds the folder of the CLAP files that crash or
/// hang while scanned.
struct TestPlugins {
    std::shared_ptr<plughost::PluginCatalog> catalog;
    std::shared_ptr<plughost::PluginHost> host;
    std::shared_ptr<plughost::PluginEffects> effects;

    explicit TestPlugins(bool blocked = false, bool scan = true) {
        static std::atomic<int> serial{0};
        plughost::CatalogOptions options;
        options.helper = std::filesystem::path(WABCUT_PLUGHOST_PATH);
        options.folders = {pluginDir() / "clap", pluginDir() / "vst3"};
        if (blocked)
            options.folders.push_back(pluginDir() / "blocked");
        options.cacheFile = outputFile("catalog-" + std::to_string(QCoreApplication::applicationPid()) + "-" +
                                       std::to_string(serial.fetch_add(1)) + ".json");
        std::filesystem::remove(options.cacheFile);
        options.scanTimeout = std::chrono::milliseconds(3000); // the hanging plugin is blocked after 3 s, not 30
        catalog = std::make_shared<plughost::PluginCatalog>(options);
        if (scan)
            REQUIRE(catalog->scan().plugins == 2);
        plughost::HostOptions hostOptions;
        hostOptions.helper = options.helper;
        hostOptions.logFile = outputFile("helpers.log");
        host = plughost::PluginHost::create(hostOptions);
        effects = plughost::PluginEffects::create(host, catalog);
        plughost::installPluginEffects(effects);
    }
    ~TestPlugins() {
        audio::setExternalEffectFactory(nullptr);
        setEffectDescriptorProvider(nullptr);
    }
};

/// Fakes for "Edit plugin...": nothing opens; the "editor" changes its state when the test says so.
struct FakeEditor {
    int opened = 0;
    std::uint64_t changes = 0;
    int captures = 0;
    std::string state = "first";

    void attach(PluginBridge& bridge) {
        bridge.setEditorHooks(
            [this](const EffectInstance&) -> std::expected<void, Error> {
                ++opened;
                return {};
            },
            [this](const EffectInstance&) -> std::expected<std::string, Error> {
                ++captures;
                return plughost::base64Encode(state.data(), state.size());
            },
            [this](const EffectInstance&) -> std::optional<std::uint64_t> { return changes; });
    }
};

/// V1 / A1 linked (media A), A2 with music (M), 4 s each at 25 fps.
constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A2     |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
)";

struct Project {
    Timeline tl{kPicture};
    ProjectController controller;
    PluginBridge plugins;

    explicit Project(const TestPlugins& test) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        plugins.setController(&controller);
        plugins.setSynchronous(true);
        plugins.setPlugins(test.catalog, test.effects);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    const Clip& clip(std::string_view ref) const { return controller.currentSequence()->findClip(tl.clip(ref))->clip(); }
    const Track& track(const char* name) const { return *controller.currentSequence()->findTrack(tl.track(name)); }
    /// The newest effect of A1's clip.
    const EffectInstance& a1Plugin() const { return clip("A1@0").effects().back(); }
};

/// Runs `stack` over a second of a stereo tone (the plugins play; a crashing one dies).
void play(audio::EffectChain& chain, const EffectStack& stack) {
    // An offline render: the plugins load before they play (review A5-P2-5; in the player they load on a worker and
    // play dry meanwhile), so the stack really runs however fast this loop is.
    const audio::ScopedOfflineRender offline;
    chain.bind(stack);
    std::vector<float> buffer(48000 * 2);
    for (int k = 0; k < 48000; ++k) {
        buffer[static_cast<std::size_t>(k) * 2] = static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 440.0 * k / 48000.0));
        buffer[static_cast<std::size_t>(k) * 2 + 1] = buffer[static_cast<std::size_t>(k) * 2];
    }
    for (int done = 0; done < 48000;) {
        const int n = std::min(480, 48000 - done);
        chain.process(buffer.data() + static_cast<std::size_t>(done) * 2, n, done, audio::CurveClock{});
        done += n;
    }
}

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

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

} // namespace

TEST_CASE("Plugin browser: the scan lists the test plugins, blocks the ones that crash or hang, groups and searches",
          "[app][plugins]") {
    TestPlugins test(true, false);
    ProjectController controller;
    PluginBridge plugins;
    plugins.setController(&controller);
    plugins.setSynchronous(true);
    CHECK_FALSE(plugins.available());
    CHECK_FALSE(plugins.rescan().isEmpty()); // nothing to scan with
    plugins.setPlugins(test.catalog, test.effects);
    REQUIRE(plugins.available());
    CHECK(plugins.pluginCount() == 0); // not scanned yet

    REQUIRE(plugins.rescan().isEmpty());
    CHECK_FALSE(plugins.scanning());
    REQUIRE(plugins.pluginCount() == 2);
    CHECK(plugins.message().contains(QStringLiteral("2 audio plugin")));
    CHECK(plugins.message().contains(QStringLiteral("blocked")));
    // The CLAP file that crashes while scanned and the one that hangs are on the blocklist, with their reasons.
    const QVariantList blocked = plugins.blocked();
    REQUIRE(blocked.size() == 2);
    QStringList names;
    for (const QVariant& b : blocked) {
        names << b.toMap().value(QStringLiteral("name")).toString();
        CHECK_FALSE(b.toMap().value(QStringLiteral("reason")).toString().isEmpty());
    }
    CHECK(names.contains(QStringLiteral("WabcutTestCrashOnScan.clap")));
    CHECK(names.contains(QStringLiteral("WabcutTestHangOnScan.clap")));

    // The list: both formats, by name; the CLAP plugin by its effect id.
    const QVariantList list = plugins.plugins();
    REQUIRE(list.size() == 2);
    QStringList formats;
    QVariantMap clap;
    for (const QVariant& v : list) {
        formats << v.toMap().value(QStringLiteral("format")).toString();
        if (v.toMap().value(QStringLiteral("id")).toString() == kClap)
            clap = v.toMap();
    }
    CHECK(formats.contains(QStringLiteral("CLAP")));
    CHECK(formats.contains(QStringLiteral("VST3")));
    REQUIRE_FALSE(clap.isEmpty());
    CHECK(clap.value(QStringLiteral("name")).toString() == QStringLiteral("Wabcut Test Gain"));
    CHECK(clap.value(QStringLiteral("vendor")).toString() == QStringLiteral("Wabcut"));

    // Groups: by vendor (both are Wabcut's), then by category.
    QVariantList groups = plugins.groups();
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Wabcut"));
    CHECK(groups[0].toMap().value(QStringLiteral("count")).toInt() == 2);
    plugins.setGroupBy(QStringLiteral("category"));
    CHECK(plugins.groupBy() == QStringLiteral("category"));
    groups = plugins.groups();
    REQUIRE_FALSE(groups.isEmpty());
    int counted = 0;
    for (const QVariant& g : groups)
        counted += g.toMap().value(QStringLiteral("count")).toInt();
    CHECK(counted == 2);
    plugins.setGroupBy(QStringLiteral("nonsense"));
    CHECK(plugins.groupBy() == QStringLiteral("vendor"));

    // Search: name, vendor, category; the mixer's list is never filtered.
    plugins.setQuery(QStringLiteral("vst3"));
    REQUIRE(plugins.plugins().size() == 1);
    CHECK(plugins.plugins()[0].toMap().value(QStringLiteral("format")).toString() == QStringLiteral("VST3"));
    plugins.setQuery(QStringLiteral("no such plugin"));
    CHECK(plugins.plugins().isEmpty());
    CHECK(plugins.groups().isEmpty());
    CHECK(plugins.allPlugins().size() == 2);
    plugins.setQuery({});
    CHECK(plugins.plugins().size() == 2);

    // A second scan only checks the files (unchanged); "Try again" scans a blocked file again: it crashes again.
    REQUIRE(plugins.rescan().isEmpty());
    CHECK(plugins.pluginCount() == 2);
    QString crashFile;
    for (const QVariant& b : plugins.blocked()) {
        if (b.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("WabcutTestCrashOnScan.clap"))
            crashFile = b.toMap().value(QStringLiteral("file")).toString();
    }
    REQUIRE_FALSE(crashFile.isEmpty());
    REQUIRE(plugins.unblock(crashFile).isEmpty());
    CHECK(plugins.blocked().size() == 2);
    CHECK(plugins.pluginCount() == 2);

    // Folders: the scanned ones; a user folder can be added (and scanned) and removed.
    const QVariantList folders = plugins.folders();
    CHECK(folders.size() >= 3);
    QTemporaryDir extra;
    REQUIRE(extra.isValid());
    REQUIRE(plugins.addFolder(QUrl::fromLocalFile(extra.path())).isEmpty());
    bool listed = false;
    for (const QVariant& f : plugins.folders()) {
        if (f.toMap().value(QStringLiteral("user")).toBool() &&
            QDir::cleanPath(f.toMap().value(QStringLiteral("path")).toString()) == QDir::cleanPath(QDir::toNativeSeparators(extra.path())))
            listed = true;
    }
    CHECK(listed);
    CHECK(plugins.removeFolder(QDir::toNativeSeparators(extra.path())).isEmpty());
    for (const QVariant& f : plugins.folders())
        CHECK_FALSE(f.toMap().value(QStringLiteral("user")).toBool());
    CHECK_FALSE(plugins.addFolder(QUrl::fromLocalFile(QStringLiteral("Z:/no/such/folder"))).isEmpty());
}

TEST_CASE("Plugin effects go on a clip, a track and the master, one undo step each; a timeline drop adds one",
          "[app][plugins]") {
    TestPlugins test;
    Project p(test);
    REQUIRE(p.plugins.available());
    REQUIRE(p.plugins.pluginCount() == 2); // the catalog was scanned before the bridge saw it

    // A video clip stands for its linked audio clip.
    const auto before = p.undo();
    REQUIRE(p.plugins.apply(kClap, clipKey(p.tl.clip("V1@0"))).isEmpty());
    CHECK(p.undo() == before + 1);
    CHECK(p.controller.undoText().contains(QStringLiteral("Add Plugin")));
    REQUIRE_FALSE(p.clip("A1@0").effects().empty());
    const EffectInstance& added = p.a1Plugin();
    CHECK(QString::fromStdString(added.effect) == kClap);
    // The name it was added with is kept for the "missing plugin" badge elsewhere.
    const EffectData* name = added.findData(plughost::kPluginNameKey);
    REQUIRE(name);
    REQUIRE(name->text());
    CHECK(*name->text() == "Wabcut Test Gain (Wabcut)");
    const QVariantMap info = p.plugins.effectInfo(effectKey(added.id));
    CHECK(info.value(QStringLiteral("plugin")).toBool());
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("Wabcut Test Gain"));
    CHECK(info.value(QStringLiteral("format")).toString() == QStringLiteral("CLAP"));
    CHECK(info.value(QStringLiteral("kind")).toString().isEmpty());

    // An audio track and the master.
    QString vst3;
    for (const QVariant& v : p.plugins.allPlugins()) {
        if (v.toMap().value(QStringLiteral("format")).toString() == QStringLiteral("VST3"))
            vst3 = v.toMap().value(QStringLiteral("id")).toString();
    }
    REQUIRE_FALSE(vst3.isEmpty());
    REQUIRE(p.plugins.apply(vst3, trackKey(p.tl.track("A2"))).isEmpty());
    REQUIRE(p.plugins.apply(kClap, QStringLiteral("master")).isEmpty());
    CHECK(p.undo() == before + 3);
    REQUIRE(p.track("A2").effects.size() == 1);
    CHECK(QString::fromStdString(p.track("A2").effects.front().effect) == vst3);
    REQUIRE(p.controller.currentSequence()->audioEffects.size() == 1);
    REQUIRE(p.controller.undo());
    CHECK(p.controller.currentSequence()->audioEffects.empty());

    // Refusals, with the reason.
    CHECK_FALSE(p.plugins.apply(QStringLiteral("plugin.clap.not.here"), QStringLiteral("master")).isEmpty());
    CHECK_FALSE(p.plugins.apply(kClap, trackKey(p.tl.track("V1"))).isEmpty());
    CHECK_FALSE(p.plugins.apply(kClap, QStringLiteral("junk")).isEmpty());
    CHECK(p.plugins.effectInfo(QStringLiteral("e999999")).value(QStringLiteral("plugin")).toBool() == false);

    // The selected clip.
    p.controller.setSelection({p.tl.clip("A2@0")});
    REQUIRE(p.plugins.applyToSelection(kClap).isEmpty());
    CHECK(QString::fromStdString(p.clip("A2@0").effects().back().effect) == kClap);
    p.controller.setSelection({});
    CHECK_FALSE(p.plugins.applyToSelection(kClap).isEmpty());
}

TEST_CASE("A plugin dropped on a clip in the timeline goes through the view's drop handler", "[app][plugins][timeline]") {
    TestPlugins test;
    Rig rig(kPicture, 8.0);
    const QVariantMap target = rig.view->effectDropTarget(rig.x(20), rig.y("A2"), kClap);
    CHECK(target.value(QStringLiteral("ok")).toBool());
    CHECK(target.value(QStringLiteral("kind")).toString() == QStringLiteral("clip"));
    REQUIRE(rig.view->dropEffect(rig.x(20), rig.y("A2"), kClap).isEmpty());
    const Clip& music = rig.controller.currentSequence()->findClip(rig.clip("A2@0"))->clip();
    REQUIRE_FALSE(music.effects().empty());
    CHECK(QString::fromStdString(music.effects().back().effect) == kClap);
    // A plugin is an audio effect: on a video clip it goes to the linked audio clip (V1 is linked to A1).
    CHECK(rig.view->effectDropTarget(rig.x(20), rig.y("V1"), kClap).value(QStringLiteral("ok")).toBool());
}

TEST_CASE("Effects tree: the Audio Plug-ins folder with favourites and a remembered grouping", "[app][plugins][effects]") {
    TestPlugins test;
    Project p(test);
    REQUIRE(p.plugins.pluginCount() == 2);
    p.plugins.setGroupBy(QStringLiteral("vendor"));
    sharedEffectLibrary()->set({});
    EffectPresetsBridge presets;
    presets.setController(&p.controller);
    EffectsTreeModel tree;
    tree.setPersistent(false);
    tree.setSources(&p.controller, &presets, &p.plugins);

    // A top-level folder after Premiere's effect folders, its vendors as sub-folders (keys from PluginBridge::groups).
    const int folder = tree.indexOf(QStringLiteral("plugins"));
    REQUIRE(folder > 0);
    CHECK(folder == tree.indexOf(QStringLiteral("videoTransition")) + 1);
    CHECK(tree.get(folder).value(QStringLiteral("folderKind")).toString() == QStringLiteral("plugins"));
    CHECK(tree.get(folder).value(QStringLiteral("count")).toInt() == 2);
    CHECK(p.plugins.groups().value(0).toMap().value(QStringLiteral("key")).toString() == QStringLiteral("vendor:Wabcut"));
    tree.setExpanded(QStringLiteral("plugins"), true);
    CHECK(tree.indexOf(QStringLiteral("plugins/vendor:Wabcut")) == folder + 1);
    tree.setExpanded(QStringLiteral("plugins/vendor:Wabcut"), true);
    const QString rowKey = QStringLiteral("plugins/vendor:Wabcut|") + kClap;
    const int row = tree.indexOf(rowKey);
    REQUIRE(row > 0);
    const QVariantMap item = tree.get(row);
    CHECK(item.value(QStringLiteral("plugin")).toBool());
    CHECK(item.value(QStringLiteral("itemId")).toString() == kClap);
    CHECK(item.value(QStringLiteral("item")).toString() == QStringLiteral("effect:") + kClap);
    CHECK(item.value(QStringLiteral("kind")).toString() == QStringLiteral("audio"));
    CHECK(item.value(QStringLiteral("name")).toString() == QStringLiteral("Wabcut Test Gain"));
    CHECK(item.value(QStringLiteral("vendor")).toString() == QStringLiteral("Wabcut"));
    CHECK(item.value(QStringLiteral("format")).toString() == QStringLiteral("CLAP"));
    CHECK(item.value(QStringLiteral("icon")).toString() == QStringLiteral("sliders-horizontal"));
    CHECK(item.value(QStringLiteral("home")).toBool());

    // The panel's search finds it (its folders open) without touching Plugins.query; Show Audio Plug-ins off hides it.
    tree.setExpanded(QStringLiteral("plugins"), false);
    tree.setQuery(QStringLiteral("Wabcut"));
    CHECK(tree.indexOf(rowKey) > 0);
    CHECK(p.plugins.query().isEmpty());
    tree.setQuery({});
    tree.setShowPlugins(false);
    CHECK(tree.indexOf(QStringLiteral("plugins")) < 0);
    tree.setShowPlugins(true);
    tree.setFilter(QStringLiteral("plugins"));
    CHECK(tree.get(0).value(QStringLiteral("rowKey")).toString() == QStringLiteral("plugins"));
    tree.setFilter(QStringLiteral("all"));

    // Favorites and custom bins take plug-ins (the item menu); their copies say they are plug-ins.
    CHECK(presets.setFavorite(QStringLiteral("effect:") + kClap, true).isEmpty());
    const int favorite = tree.indexOf(QStringLiteral("favorites|") + kClap);
    REQUIRE(favorite > 0);
    CHECK(tree.get(favorite).value(QStringLiteral("plugin")).toBool());
    CHECK(tree.get(favorite).value(QStringLiteral("icon")).toString() == QStringLiteral("sliders-horizontal"));
    CHECK_FALSE(tree.get(favorite).value(QStringLiteral("name")).toString().isEmpty());
    tree.setExpanded(QStringLiteral("plugins"), true); // its vendor folder stayed open
    REQUIRE(tree.indexOf(rowKey) > 0);
    CHECK(tree.get(tree.indexOf(rowKey)).value(QStringLiteral("favorite")).toBool());
    CHECK(presets.newCustomBin(kClap) == QStringLiteral("Custom Bin 01"));

    // Applying from a copy goes through the plug-in bridge: every selected audio clip (a video clip stands for its
    // linked audio), one undo step, the plug-in's name kept with each.
    p.controller.setSelection({});
    CHECK_FALSE(p.plugins.applyToSelectedClips(kClap).value(QStringLiteral("ok")).toBool());
    p.controller.setSelection({p.tl.clip("V1@0"), p.tl.clip("A2@0")});
    const auto before = p.undo();
    const QVariantMap result = tree.applyToSelection(tree.get(favorite).value(QStringLiteral("itemId")).toString());
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("clips")).toInt() == 2);
    CHECK(p.undo() == before + 1);
    CHECK(QString::fromStdString(p.a1Plugin().effect) == kClap);
    CHECK(p.a1Plugin().findData(plughost::kPluginNameKey) != nullptr);
    CHECK(QString::fromStdString(p.clip("A2@0").effects().back().effect) == kClap);
    CHECK(p.clip("V1@0").effects().empty());

    // The grouping is remembered for the next start, and the tree follows it.
    p.plugins.setGroupBy(QStringLiteral("category"));
    CHECK(QSettings().value(QStringLiteral("effects/pluginGroupBy")).toString() == QStringLiteral("category"));
    PluginBridge again;
    CHECK(again.groupBy() == QStringLiteral("category"));
    CHECK(tree.indexOf(QStringLiteral("plugins/vendor:Wabcut")) < 0);
    p.plugins.setGroupBy(QStringLiteral("vendor"));
    CHECK(QSettings().value(QStringLiteral("effects/pluginGroupBy")).toString() == QStringLiteral("vendor"));
}

TEST_CASE("Plugin badges: missing on a computer without the plugin, crashed when its helper dies", "[app][plugins]") {
    SECTION("Crashed") {
        TestPlugins test;
        Project p(test);
        REQUIRE(p.plugins.apply(kClap, clipKey(p.tl.clip("A1@0"))).isEmpty());
        const EffectInstanceId id = p.a1Plugin().id;
        // Its "Crash" parameter (p3) on: the next block kills the helper.
        REQUIRE(p.controller.applyEditFn("Crash it", [&](const ProjectState& state) {
            edit::EffectChange change;
            change.params.push_back(EffectParam{"p3", ParamValue(1.0)});
            return edit::setEffect(state, p.tl.seq, edit::EffectTarget::ofClip(p.tl.clip("A1@0")), id, change);
        }));
        const int revision = p.plugins.revision();
        audio::EffectChain chain(2, 48000);
        play(chain, EffectStack{p.a1Plugin()});
        const QVariantMap info = p.plugins.effectInfo(effectKey(id));
        CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("crashed"));
        CHECK(info.value(QStringLiteral("text")).toString().contains(QStringLiteral("rash")));
        p.plugins.pollStatus(); // the badge's signature changed: bindings re-read
        CHECK(p.plugins.revision() > revision);
    }
    SECTION("Missing") {
        // Added on a computer that has it...
        TestPlugins test;
        Project p(test);
        REQUIRE(p.plugins.apply(kClap, clipKey(p.tl.clip("A1@0"))).isEmpty());
        const QString key = effectKey(p.a1Plugin().id);
        // ...opened on one that does not (an empty catalog).
        TestPlugins elsewhere(false, false);
        p.plugins.setPlugins(elsewhere.catalog, elsewhere.effects);
        const QVariantMap info = p.plugins.effectInfo(key);
        CHECK(info.value(QStringLiteral("plugin")).toBool());
        CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("missing"));
        CHECK(info.value(QStringLiteral("text")).toString() == QStringLiteral("Missing plugin: Wabcut Test Gain (Wabcut)"));
        CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("Wabcut Test Gain (Wabcut)"));
        CHECK_FALSE(info.value(QStringLiteral("hasEditor")).toBool());
        FakeEditor editor;
        editor.attach(p.plugins);
        CHECK(p.plugins.openEditor(key).contains(QStringLiteral("Missing plugin")));
        CHECK(editor.opened == 0);
    }
}

TEST_CASE("Edit plugin: the injected opener is asked; the editor's state changes are saved as one undo step",
          "[app][plugins]") {
    TestPlugins test;
    Project p(test);
    FakeEditor editor;
    editor.attach(p.plugins);
    REQUIRE(p.plugins.apply(kClap, clipKey(p.tl.clip("A1@0"))).isEmpty());
    const QString key = effectKey(p.a1Plugin().id);
    const auto before = p.undo();

    REQUIRE(p.plugins.openEditor(key).isEmpty());
    CHECK(editor.opened == 1);
    CHECK(p.plugins.editing() == QStringList{key});
    // Nothing changed in the editor: nothing is saved.
    p.plugins.pollEditors();
    CHECK(editor.captures == 0);
    CHECK(p.undo() == before);

    // The editor changes the state twice: both are saved, merged into one undo step.
    editor.changes = 1;
    editor.state = "after one turn of a knob";
    p.plugins.pollEditors();
    CHECK(p.undo() == before + 1);
    CHECK(plughost::pluginState(p.a1Plugin()) == plughost::base64Encode(editor.state.data(), editor.state.size()));
    CHECK(p.controller.undoText().contains(QStringLiteral("Plugin Settings")));
    editor.changes = 2;
    editor.state = "after two turns";
    p.plugins.pollEditors();
    CHECK(p.undo() == before + 1);
    CHECK(plughost::pluginState(p.a1Plugin()) == plughost::base64Encode(editor.state.data(), editor.state.size()));
    CHECK(editor.captures == 2);

    // "Save plugin state" by hand: its own step; the same state again is no step.
    editor.state = "saved by hand";
    REQUIRE(p.plugins.captureState(key).isEmpty());
    CHECK(p.undo() == before + 2);
    REQUIRE(p.plugins.captureState(key).isEmpty());
    CHECK(p.undo() == before + 2);
    // Undo brings the editor's previous state back into the project.
    REQUIRE(p.controller.undo());
    CHECK(plughost::pluginState(p.a1Plugin()) == plughost::base64Encode(std::string("after two turns").data(), 15));

    // Only plugin effects have an editor; a removed effect stops being polled.
    CHECK_FALSE(p.plugins.openEditor(QStringLiteral("e999999")).isEmpty());
    CHECK_FALSE(p.plugins.message().isEmpty());
    REQUIRE(p.controller.applyEditFn("Remove", [&](const ProjectState& state) {
        return edit::removeEffect(state, p.tl.seq, p.tl.clip("A1@0"), p.a1Plugin().id);
    }));
    CHECK(p.plugins.editing().isEmpty());
    CHECK(editor.opened == 1);
}

TEST_CASE("Plugins in the shell: the Effects panel's plugin section, Effect Controls' plugin header, the mixer; screenshots",
          "[app][plugins][qml][screenshot]") {
    resetSettings();
    clearMessages();
    TestPlugins test(true);
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* plugins = shell.engine.singletonInstance<PluginBridge*>("Wabcut", "Plugins");
    REQUIRE(controller);
    REQUIRE(plugins);
    FakeEditor editor;
    editor.attach(*plugins);
    plugins->setSynchronous(true);
    plugins->setPlugins(test.catalog, test.effects);
    REQUIRE(plugins->rescan().isEmpty()); // the blocked folder: the crash and the hang go on the blocklist
    REQUIRE(plugins->blocked().size() == 2);
    Timeline tl(kPicture);
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());

    // A1's clip: the CLAP plugin (crashed) and the VST3 one; A2's track: the VST3 plugin.
    QString vst3;
    for (const QVariant& v : plugins->allPlugins()) {
        if (v.toMap().value(QStringLiteral("format")).toString() == QStringLiteral("VST3"))
            vst3 = v.toMap().value(QStringLiteral("id")).toString();
    }
    REQUIRE(plugins->apply(kClap, clipKey(tl.clip("A1@0"))).isEmpty());
    const EffectInstanceId crashing = controller->currentSequence()->findClip(tl.clip("A1@0"))->clip().effects().back().id;
    REQUIRE(plugins->apply(vst3, clipKey(tl.clip("A1@0"))).isEmpty());
    REQUIRE(plugins->apply(vst3, trackKey(tl.track("A2"))).isEmpty());
    REQUIRE(controller->applyEditFn("Crash it", [&](const ProjectState& state) {
        edit::EffectChange change;
        change.params.push_back(EffectParam{"p3", ParamValue(1.0)});
        return edit::setEffect(state, tl.seq, edit::EffectTarget::ofClip(tl.clip("A1@0")), crashing, change);
    }));
    audio::EffectChain chain(2, 48000);
    EffectStack crashStack;
    for (const EffectInstance& e : controller->currentSequence()->findClip(tl.clip("A1@0"))->clip().effects()) {
        if (e.id == crashing)
            crashStack.push_back(e);
    }
    play(chain, crashStack);
    plugins->pollStatus();
    REQUIRE(plugins->effectInfo(effectKey(crashing)).value(QStringLiteral("kind")).toString() == QStringLiteral("crashed"));

    // The Effects panel, searched down to the plugins; A1's clip in Effect Controls.
    controller->setSelection({tl.clip("A1@0")});
    REQUIRE(QMetaObject::invokeMethod(w, "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("effects"))));
    QQuickItem* search = findNamed(w->contentItem(), QStringLiteral("effectsSearch"));
    REQUIRE(search);
    search->setProperty("text", QStringLiteral("Wabcut"));
    // Effect Controls from the crashed plugin's header (the list only makes the rows in view).
    spin(100);
    const int headerRow = controller->effectControls()->rowOf(QStringLiteral("h:") + effectKey(crashing));
    REQUIRE(headerRow >= 0);
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, headerRow), Q_ARG(int, 0)));
    spin(450);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    QQuickItem* row = findNamed(w->contentItem(), QStringLiteral("pluginItem_") + kClap);
    REQUIRE(row);
    CHECK(row->isVisible());
    QQuickItem* notice = findNamed(w->contentItem(), QStringLiteral("pluginBlockedNotice"));
    REQUIRE(notice);
    CHECK(notice->isVisible());
    QQuickItem* panel = shell.item("effectsPanel");
    REQUIRE(panel);
    saveCrop(image, panel, QStringLiteral("plugin-browser.png"));
    QQuickItem* fx = shell.item("effectControls");
    REQUIRE(fx);
    // Every effect header has the button; it shows on the plugin effects' headers only.
    std::vector<QQuickItem*> edits;
    const std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
        if (item->objectName() == QStringLiteral("editPluginButton"))
            edits.push_back(item);
        for (QQuickItem* child : item->childItems())
            collect(child);
    };
    collect(fx);
    REQUIRE_FALSE(edits.empty());
    CHECK(std::any_of(edits.begin(), edits.end(), [](QQuickItem* e) { return e->isVisible(); }));
    QQuickItem* badge = findNamed(fx, QStringLiteral("pluginBadge"));
    REQUIRE(badge);
    saveCrop(image, fx, QStringLiteral("plugin-effect-controls.png"));

    // The Audio workspace: the A2 strip's slot is the plugin, by its name; the "+ Effect" menu offers the plugins.
    shell.window->setProperty("workspace", 1);
    spin(500);
    QQuickItem* strip = findNamed(w->contentItem(), QStringLiteral("mixerStrip_A2"));
    REQUIRE(strip);
    QQuickItem* slot = findNamed(strip, QStringLiteral("effectSlot_0"));
    REQUIRE(slot);
    CHECK(slot->property("slotName").toString() == QStringLiteral("Wabcut Test Gain VST3"));
    QObject* addMenu = strip->findChild<QObject*>(QStringLiteral("effectAddMenu"));
    REQUIRE(addMenu);
    QStringList offered;
    for (const QVariant& o : addMenu->property("options").toList())
        offered << o.toMap().value(QStringLiteral("id")).toString();
    CHECK(offered.contains(kClap));
    CHECK(offered.contains(QStringLiteral("wabcut.audio.gain")));
    shell.window->setProperty("workspace", 0);
    spin(200);

    // The settings dialog (Main.qml's loader): the folders and the blocklist with "Try again".
    plugins->openSettings();
    QObject* loader = shell.object("audioPluginsSettingsLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    CHECK(findNamed(content->parentItem(), QStringLiteral("unblockPlugin_0")));
    CHECK(findNamed(content->parentItem(), QStringLiteral("rescanPlugins")));
    spin(150);
    REQUIRE(shell.grab().save(QDir(outputDir()).filePath(QStringLiteral("audio-plugins-settings.png"))));
    QMetaObject::invokeMethod(dialog, "close");
    spin(150);
    CHECK(editor.opened == 0); // no editor was asked for, and no window ever opened
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

#else

TEST_CASE("Plugin bridge without plugin support says so", "[app][plugins]") {
    wabcut::app::PluginBridge plugins;
    CHECK_FALSE(plugins.available());
    CHECK_FALSE(plugins.rescan().isEmpty());
    CHECK_FALSE(plugins.apply(QStringLiteral("plugin.clap.x"), QStringLiteral("master")).isEmpty());
}

#endif
