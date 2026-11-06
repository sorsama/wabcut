// 0.7 OpenFX video plug-ins in the UI, offscreen, with the test plug-ins built in tests/ofxhost (WabcutTestOfx.ofx and the one
// that crashes) and the real wabcut-ofxhost helper:
//   * a plug-in's parameters are Effect Controls rows (sliders, a menu, a switch, colour and position channels, a text row),
//     each change one undo step with a clear name, keyframable where the plug-in allows;
//   * putting a plug-in on the selected clips is one undo step; the Effects panel lists the usable plug-ins;
//   * a plug-in the computer does not have is shown as missing (kept, bypassed); a crashed one is badged and comes back when
//     the effect is turned off and on.

#include "BridgeIds.h"
#include "EffectControls.h"
#include "EffectsTreeModel.h"
#include "OfxUi.h"
#include "PluginBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#if defined(WABCUT_WITH_OPENFX) && defined(WABCUT_TEST_OFX_DIR)

#include <wabcut/model/EditOps.h>
#include <wabcut/ofxhost/OfxCatalog.h>
#include <wabcut/ofxhost/OfxEffects.h>
#include <wabcut/ofxhost/OfxPluginHost.h>
#include <wabcut/playback/video/ExternalEffects.h>

#include <QDir>
#include <QImage>
#include <QQuickItem>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr const char* kPicture = "V2 |Bbbbbbbbbbbbbbbbbbbb|\n"
                                 "V1 |Aaaaaaaaaaaaaaaaaaaa|\n"
                                 "A1 |Aaaaaaaaaaaaaaaaaaaa|";

const QString kGain = QStringLiteral("plugin.ofx.org.wabcut.test.gain");
const QString kCrash = QStringLiteral("plugin.ofx.org.wabcut.test.crash");

std::filesystem::path ofxOutput(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "app-ofx";
    std::filesystem::create_directories(dir);
    return dir / name;
}

/// The test plug-ins as a catalog (scanned), a host and the effects, installed as the model's descriptors, the playback
/// layer's processor and the UI's source for the test's lifetime.
struct OfxRig {
    std::shared_ptr<ofxhost::OfxCatalog> catalog;
    std::shared_ptr<ofxhost::OfxPluginHost> host;
    std::shared_ptr<ofxhost::OfxEffects> effects;

    OfxRig() {
        static std::atomic<int> serial{0};
        ofxhost::OfxCatalogOptions options;
        options.helper = std::filesystem::path(WABCUT_OFXHOST_PATH);
        options.folders = {std::filesystem::path(WABCUT_TEST_OFX_DIR) / "main", std::filesystem::path(WABCUT_TEST_OFX_DIR) / "crash"};
        options.cacheFile = ofxOutput("catalog-" + std::to_string(serial.fetch_add(1)) + ".json");
        std::filesystem::remove(options.cacheFile);
        catalog = std::make_shared<ofxhost::OfxCatalog>(options);
        REQUIRE(catalog->scan().plugins == 8);
        ofxhost::OfxHostOptions hostOptions;
        hostOptions.helper = std::filesystem::path(WABCUT_OFXHOST_PATH);
        host = ofxhost::OfxPluginHost::create(hostOptions);
        effects = ofxhost::OfxEffects::create(host, catalog);
        setSecondaryEffectDescriptorProvider(catalog->descriptors());
        playback::setExternalEffectProcessor(effects);
        OfxUi::instance().setSources(catalog, effects);
    }
    ~OfxRig() {
        OfxUi::instance().setSources(nullptr, nullptr);
        playback::setExternalEffectProcessor(nullptr);
        setSecondaryEffectDescriptorProvider(nullptr);
    }
};

struct Fixture {
    Timeline tl{kPicture};
    ProjectController controller;
    ClipId v1;
    ClipId v2;
    ClipId a1;

    Fixture() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        v1 = clipAt(*controller.snapshot(), tl.seq, "V1@0");
        v2 = clipAt(*controller.snapshot(), tl.seq, "V2@0");
        a1 = clipAt(*controller.snapshot(), tl.seq, "A1@0");
        controller.selectClipById(v1, ProjectController::Replace);
        controller.setPlayhead(Ticks{});
    }
    EffectControlsModel& fx() { return *controller.effectControls(); }
    const Clip& clip(ClipId id) const { return controller.currentSequence()->findClip(id)->clip(); }
    std::size_t undoCount() { return controller.session().undoCount(); }
    /// Puts the plug-in on V1; its effect.
    EffectInstance add(const QString& plugin) {
        const QVariantMap done = OfxUi::instance().applyToSelectedClips(&controller, plugin);
        REQUIRE(done.value("ok").toBool());
        return clip(v1).effects().back();
    }
    /// Shows the rows of V1 again (the model rebuilds when the selection changes).
    void reselect() {
        controller.selectClipById(v2, ProjectController::Replace);
        controller.selectClipById(v1, ProjectController::Replace);
    }
};

double gainOf(const EffectInstance& e, const char* key) { return effectParameterAt(e, key, Ticks{}); }

/// An 8-bit YUV 4:2:0 frame of one grey, what a decoder hands the playback layer.
media::FrameHandle greyFrame() {
    constexpr int w = 32;
    constexpr int h = 24;
    auto storage = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w * h + 2 * (w / 2) * (h / 2)), 128);
    media::FrameHandle f;
    f.duration = Ticks{1000};
    f.width = w;
    f.height = h;
    f.format = media::PixelFormat::YUV420P;
    f.color.matrix = ColorMatrix::BT709;
    f.color.range = ColorRange::Limited;
    f.color.primaries = ColorPrimaries::BT709;
    f.color.transfer = TransferFunction::BT709;
    f.source.fingerprint = 5;
    media::CpuPlanes cpu;
    cpu.planes[0] = media::Plane{storage->data(), w, w, h};
    cpu.planes[1] = media::Plane{storage->data() + w * h, w / 2, w / 2, h / 2};
    cpu.planes[2] = media::Plane{storage->data() + w * h + (w / 2) * (h / 2), w / 2, w / 2, h / 2};
    cpu.owner = storage;
    f.payload = std::move(cpu);
    return f;
}

} // namespace

TEST_CASE("OpenFX in Effect Controls: a plug-in's parameters are rows, each change one undo step", "[app][ofx][effectcontrols]") {
    OfxRig ofx;
    Fixture f;
    const std::size_t before = f.undoCount();
    const QVariantMap done = OfxUi::instance().applyToSelectedClips(&f.controller, kGain);
    REQUIRE(done.value("ok").toBool());
    CHECK(done.value("clips").toInt() == 1);
    CHECK(f.undoCount() == before + 1); // one undo step for the whole add
    CHECK(f.controller.undoText() == QStringLiteral("Undo Add Plug-in"));
    REQUIRE(f.clip(f.v1).effects().size() == 1);
    const EffectInstance added = f.clip(f.v1).effects().front();
    CHECK(added.effect == kGain.toStdString());
    const QString section = effectKey(added.id);
    const auto key = [&](const char* param) { return section + QLatin1Char(':') + QLatin1String(param); };

    // The section: a header with the plug-in's name, known, and its parameters in the plug-in's page order.
    const QVariantMap header = f.fx().row(QStringLiteral("h:") + section);
    REQUIRE_FALSE(header.isEmpty());
    CHECK(header.value("title").toString() == QStringLiteral("Wabcut Test Gain"));
    CHECK(header.value("known").toBool());
    CHECK(header.value("sectionKind").toString() == QStringLiteral("effect"));
    CHECK(header.value("removable").toBool());

    const QVariantMap gain = f.fx().row(key("gain"));
    CHECK(gain.value("kind").toString() == QStringLiteral("param"));
    CHECK(gain.value("title").toString() == QStringLiteral("Gain"));
    CHECK(gain.value("value").toDouble() == Approx(1.0));
    CHECK(gain.value("from").toDouble() == Approx(0.0));
    CHECK(gain.value("to").toDouble() == Approx(4.0));
    CHECK(gain.value("animatable").toBool());
    const QVariantMap mode = f.fx().row(key("mode"));
    CHECK(mode.value("kind").toString() == QStringLiteral("choice"));
    CHECK(mode.value("options").toList().size() == 3);
    CHECK(mode.value("options").toList()[1].toMap().value("label").toString() == QStringLiteral("Add"));
    CHECK_FALSE(mode.value("animatable").toBool());
    CHECK(f.fx().row(key("skip")).value("kind").toString() == QStringLiteral("toggle"));
    CHECK(f.fx().row(key("offset")).value("to").toDouble() == Approx(100.0));
    CHECK(f.fx().row(key("tint.r")).value("value").toDouble() == Approx(100.0)); // a colour channel, in percent
    CHECK(f.fx().row(key("tint.b")).value("title").toString() == QStringLiteral("Tint B"));
    CHECK(f.fx().row(key("center.x")).value("value").toDouble() == Approx(50.0)); // a position, as a fraction of the picture
    const QVariantMap label = f.fx().row(section + QStringLiteral(":data:label"));
    CHECK(label.value("kind").toString() == QStringLiteral("text"));
    CHECK(label.value("title").toString() == QStringLiteral("Label"));
    CHECK(label.value("text").toString() == QStringLiteral("hello"));
    CHECK(f.fx().row(key("hidden")).isEmpty()); // hidden by the plug-in
    CHECK(f.fx().row(key("button")).isEmpty()); // a button is not a parameter

    // A slider drag is one undo step named after the plug-in and the parameter.
    std::size_t steps = f.undoCount();
    f.fx().setValue(key("gain"), 2.5);
    f.fx().endEdit();
    CHECK(gainOf(f.clip(f.v1).effects().front(), "gain") == Approx(2.5));
    CHECK(f.undoCount() == steps + 1);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Wabcut Test Gain: Gain"));
    // A menu, a switch, a colour channel and a whole-number parameter.
    f.fx().setValue(key("mode"), 1.0);
    f.fx().endEdit();
    f.fx().setValue(key("skip"), 1.0);
    f.fx().endEdit();
    f.fx().setValue(key("tint.g"), 50.0);
    f.fx().endEdit();
    f.fx().setValue(key("offset"), 40.0);
    f.fx().endEdit();
    const EffectInstance now = f.clip(f.v1).effects().front();
    CHECK(gainOf(now, "mode") == Approx(1.0));
    CHECK(gainOf(now, "skip") == Approx(1.0));
    CHECK(gainOf(now, "tint.g") == Approx(0.5));
    CHECK(gainOf(now, "offset") == Approx(40.0));
    CHECK(f.undoCount() == steps + 5);
    // The text setting is an edit too, with its own name.
    f.fx().setEffectText(section + QStringLiteral(":data:label"), QStringLiteral("a longer label"));
    const EffectData* text = f.clip(f.v1).effects().front().findData("label");
    REQUIRE(text != nullptr);
    CHECK(*text->text() == "a longer label");
    CHECK(f.undoCount() == steps + 6);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Wabcut Test Gain: Label"));
    CHECK(f.fx().row(section + QStringLiteral(":data:label")).value("text").toString() == QStringLiteral("a longer label"));

    // A parameter the plug-in allows to animate gets keys.
    f.fx().toggleKeyAtPlayhead(key("gain"));
    CHECK(f.fx().row(key("gain")).value("animated").toBool());
    CHECK(f.clip(f.v1).effects().front().find("gain")->isCurve());
    // Not one it does not.
    steps = f.undoCount();
    f.fx().toggleKeyAtPlayhead(key("mode"));
    CHECK(f.undoCount() == steps);

    // Undo walks back through the edits; the rows follow.
    REQUIRE(f.controller.undo()); // the key
    REQUIRE(f.controller.undo()); // the text
    CHECK(f.fx().row(section + QStringLiteral(":data:label")).value("text").toString() == QStringLiteral("hello"));
    REQUIRE(f.controller.undo()); // offset
    CHECK(f.fx().row(key("offset")).value("value").toDouble() == Approx(0.0));
    REQUIRE(f.controller.redo());
    CHECK(f.fx().row(key("offset")).value("value").toDouble() == Approx(40.0));

    // Resetting a parameter puts its default back; removing the section takes the effect off the clip (one step each).
    f.fx().resetValue(key("gain"));
    CHECK(gainOf(f.clip(f.v1).effects().front(), "gain") == Approx(1.0));
    steps = f.undoCount();
    f.fx().removeSection(section);
    CHECK(f.clip(f.v1).effects().empty());
    CHECK(f.undoCount() == steps + 1);
}

TEST_CASE("OpenFX in the Effects panel: usable plug-ins are listed, put on the selected video clips in one step", "[app][ofx][effectspanel]") {
    OfxRig ofx;
    Fixture f;
    // The list: the plug-ins that can run, by name; not the ones that need a GPU or two inputs.
    QStringList names;
    for (const QVariant& v : OfxUi::instance().plugins())
        names << v.toMap().value("name").toString();
    CHECK(names.contains(QStringLiteral("Wabcut Test Gain")));
    CHECK(names.contains(QStringLiteral("Wabcut Test Invert 8-bit")));
    CHECK_FALSE(names.contains(QStringLiteral("Wabcut Test GPU")));
    CHECK_FALSE(names.contains(QStringLiteral("Wabcut Test Pair")));
    QStringList why;
    for (const QVariant& v : OfxUi::instance().unusable())
        why << v.toMap().value("name").toString() + QLatin1Char(':') + v.toMap().value("problem").toString();
    CHECK(why.join(QLatin1Char('|')).contains(QStringLiteral("Wabcut Test GPU:needs GPU rendering")));
    const QVariantMap first = OfxUi::instance().plugins().first().toMap();
    CHECK(first.value("format").toString() == QStringLiteral("OpenFX"));
    CHECK(first.value("id").toString().startsWith(QStringLiteral("plugin.ofx.")));

    // The folder in the tree, and applying through it.
    EffectsTreeModel tree;
    tree.setPersistent(false);
    tree.setShowPlugins(true);
    tree.setSources(&f.controller, nullptr, nullptr);
    tree.setQuery(QStringLiteral("Test Gain")); // a search opens the folders that match
    bool listed = false;
    bool gpu = false;
    for (int i = 0; i < tree.rowCount(); ++i) {
        const QVariantMap row = tree.get(i);
        listed = listed || row.value("name").toString() == QStringLiteral("Wabcut Test Gain");
        gpu = gpu || row.value("name").toString() == QStringLiteral("Wabcut Test GPU");
    }
    CHECK(listed);
    CHECK_FALSE(gpu);

    // Two video clips selected: one instance each, one undo step.
    f.controller.setSelection({f.v1, f.v2});
    const std::size_t before = f.undoCount();
    const QVariantMap result = tree.applyToSelection(kGain);
    CHECK(result.value("ok").toBool());
    CHECK(result.value("clips").toInt() == 2);
    CHECK(f.undoCount() == before + 1);
    CHECK(f.clip(f.v1).effects().size() == 1);
    CHECK(f.clip(f.v2).effects().size() == 1);
    CHECK(f.clip(f.v1).effects().front().findData("plugin.name") != nullptr); // the name it was added with
    REQUIRE(f.controller.undo());
    CHECK(f.clip(f.v1).effects().empty());
    CHECK(f.clip(f.v2).effects().empty());

    // A video effect does not go on an audio clip; with nothing selected it says so.
    f.controller.setSelection({f.a1});
    const QVariantMap onAudio = tree.applyToSelection(kGain);
    CHECK_FALSE(onAudio.value("ok").toBool());
    CHECK(onAudio.value("error").toString().contains(QStringLiteral("video clip")));
    f.controller.setSelection({});
    CHECK_FALSE(tree.applyToSelection(kGain).value("ok").toBool());
    // Not a plug-in this computer has, or one it cannot use.
    f.controller.setSelection({f.v1});
    CHECK_FALSE(tree.applyToSelection(QStringLiteral("plugin.ofx.not.here")).value("ok").toBool());
    CHECK_FALSE(tree.applyToSelection(QStringLiteral("plugin.ofx.org.wabcut.test.gpu")).value("ok").toBool());
    CHECK(f.clip(f.v1).effects().empty());

    // The plugin bridge's drop handlers route an OpenFX id to the same code (the timeline's drop on a clip).
    PluginBridge bridge;
    bridge.setController(&f.controller);
    const std::size_t beforeDrop = f.undoCount();
    CHECK(bridge.apply(kGain, clipKey(f.v1)).isEmpty());
    CHECK(f.undoCount() == beforeDrop + 1);
    CHECK(f.clip(f.v1).effects().size() == 1);
    CHECK_FALSE(bridge.apply(kGain, QStringLiteral("master")).isEmpty()); // a video effect has no master bus
}

TEST_CASE("OpenFX in Effect Controls: a plug-in the computer does not have is shown as missing", "[app][ofx][effectcontrols]") {
    OfxRig ofx;
    Fixture f;
    const EffectInstance added = f.add(kGain);
    const QString section = effectKey(added.id);
    f.reselect();
    CHECK(f.fx().row(QStringLiteral("h:") + section).value("known").toBool());
    CHECK_FALSE(f.fx().row(section + QStringLiteral(":gain")).isEmpty());

    // The same project on a computer without it: no descriptor, no plug-in in the catalog.
    setSecondaryEffectDescriptorProvider(nullptr);
    auto empty = std::make_shared<ofxhost::OfxCatalog>();
    auto none = ofxhost::OfxEffects::create(ofx.host, empty);
    OfxUi::instance().setSources(empty, none);
    f.reselect();
    const QVariantMap header = f.fx().row(QStringLiteral("h:") + section);
    REQUIRE_FALSE(header.isEmpty());
    CHECK(header.value("title").toString() == QStringLiteral("Wabcut Test Gain (Wabcut Tests)")); // the name it was saved with
    CHECK_FALSE(header.value("known").toBool());
    CHECK(header.value("description").toString().contains(QStringLiteral("not on this computer")));
    CHECK(f.fx().row(section + QStringLiteral(":gain")).isEmpty()); // there is nothing to show of it
    // Kept as it is: the stack still holds it, its values untouched, and it can be removed.
    REQUIRE(f.clip(f.v1).effects().size() == 1);
    CHECK(f.clip(f.v1).effects().front().effect == kGain.toStdString());
    // The badge the row shows.
    PluginBridge bridge;
    bridge.setController(&f.controller);
    const QVariantMap info = bridge.effectInfo(section);
    CHECK(info.value("plugin").toBool());
    CHECK(info.value("openfx").toBool());
    CHECK(info.value("kind").toString() == QStringLiteral("missing"));
    CHECK(info.value("text").toString() == QStringLiteral("Missing plug-in: Wabcut Test Gain (Wabcut Tests)"));
    CHECK_FALSE(info.value("hasEditor").toBool());
    const std::size_t steps = f.undoCount();
    f.fx().removeSection(section);
    CHECK(f.clip(f.v1).effects().empty());
    CHECK(f.undoCount() == steps + 1);
}

TEST_CASE("OpenFX in Effect Controls: a crashed plug-in is badged and comes back when the effect is turned off and on", "[app][ofx][crash]") {
    OfxRig ofx;
    Fixture f;
    const EffectInstance added = f.add(kCrash);
    const QString section = effectKey(added.id);
    PluginBridge bridge;
    bridge.setController(&f.controller);
    CHECK(bridge.effectInfo(section).value("kind").toString().isEmpty());

    // The plug-in crashes while its picture is made: the editor is still here, and the effect is badged.
    const media::FrameHandle frame = greyFrame();
    playback::ExternalSource source;
    source.frame = &frame;
    playback::ExternalEffect effect;
    effect.id = added.id;
    effect.effect = kCrash.toStdString();
    CHECK_FALSE(ofx.effects->process(source, {effect}));
    const QVariantMap badge = bridge.effectInfo(section);
    CHECK(badge.value("kind").toString() == QStringLiteral("crashed"));
    CHECK(badge.value("text").toString().startsWith(QStringLiteral("Crashed: the plug-in crashed")));
    CHECK(badge.value("name").toString() == QStringLiteral("Wabcut Test Crash"));
    const int revision = bridge.revision();
    CHECK(f.fx().row(QStringLiteral("h:") + section).value("known").toBool()); // still a known effect: only disabled

    // Turning it off and on tries again (a new helper): the badge goes.
    f.fx().setEffectEnabled(section, false);
    CHECK(bridge.effectInfo(section).value("kind").toString() == QStringLiteral("crashed")); // off does not clear it
    f.fx().setEffectEnabled(section, true);
    CHECK(bridge.effectInfo(section).value("kind").toString().isEmpty());
    CHECK(revision >= 0);
}

namespace {

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
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
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect()).save(QDir(outputDir()).filePath(name)));
}

} // namespace

TEST_CASE("OpenFX in the shell: the Effects panel's folder and Effect Controls with plug-ins; screenshots", "[app][ofx][qml][screenshot]") {
    resetSettings();
    clearMessages();
    OfxRig ofx;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl(kPicture);
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());

    // V1 gets the gain plug-in and the one that crashes; the crash happens while a picture is made.
    controller->selectClipById(tl.clip("V1@0"), ProjectController::Replace);
    REQUIRE(OfxUi::instance().applyToSelectedClips(controller, kGain).value("ok").toBool());
    REQUIRE(OfxUi::instance().applyToSelectedClips(controller, kCrash).value("ok").toBool());
    const EffectStack stack = controller->currentSequence()->findClip(tl.clip("V1@0"))->clip().effects();
    REQUIRE(stack.size() == 2);
    const media::FrameHandle frame = greyFrame();
    playback::ExternalSource source;
    source.frame = &frame;
    playback::ExternalEffect crashing;
    crashing.id = stack[1].id;
    crashing.effect = kCrash.toStdString();
    CHECK_FALSE(ofx.effects->process(source, {crashing}));
    REQUIRE(OfxUi::instance().effectInfo(stack[1]).value("kind").toString() == QStringLiteral("crashed"));

    REQUIRE(QMetaObject::invokeMethod(w, "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("effects"))));
    QQuickItem* search = findNamed(w->contentItem(), QStringLiteral("effectsSearch"));
    REQUIRE(search);
    search->setProperty("text", QStringLiteral("Wabcut Test"));
    spin(150);
    const int gainHeader = controller->effectControls()->rowOf(QStringLiteral("h:") + effectKey(stack[0].id));
    const int crashHeader = controller->effectControls()->rowOf(QStringLiteral("h:") + effectKey(stack[1].id));
    REQUIRE(gainHeader >= 0);
    REQUIRE(crashHeader >= 0);
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list);

    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        const QString suffix = dark ? QStringLiteral("dark") : QStringLiteral("light");
        // The gain plug-in's section from its header down: a slider, a menu, switches, colour and position channels, a text row.
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, gainHeader), Q_ARG(int, 0)));
        spin(900);
        QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        QQuickItem* panel = shell.item("effectsPanel");
        REQUIRE(panel);
        saveCrop(image, panel, QStringLiteral("ofx-effects-panel-") + suffix + QStringLiteral(".png"));
        QQuickItem* fx = shell.item("effectControls");
        REQUIRE(fx);
        saveCrop(image, fx, QStringLiteral("ofx-effect-controls-") + suffix + QStringLiteral(".png"));
        // The crashed plug-in's header at the bottom of the list: its badge.
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, crashHeader), Q_ARG(int, 2)));
        spin(900);
        image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        saveCrop(image, fx, QStringLiteral("ofx-effect-controls-crashed-") + suffix + QStringLiteral(".png"));
        std::vector<QQuickItem*> badges;
        const std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
            if (item->objectName() == QStringLiteral("pluginBadge"))
                badges.push_back(item);
            for (QQuickItem* child : item->childItems())
                collect(child);
        };
        collect(fx);
        REQUIRE_FALSE(badges.empty());
        CHECK(std::any_of(badges.begin(), badges.end(), [](QQuickItem* b) { return b->isVisible(); })); // the crashed one's
        CHECK(std::none_of(badges.begin(), badges.end(), [](QQuickItem* b) { return b->isVisible() && !b->property("kind").toString().startsWith(QLatin1String("c")); }));
    }
    // The tree model behind the panel lists the folder and the plug-ins that can run.
    bool folder = false;
    bool gainListed = false;
    EffectsTreeModel model;
    model.setPersistent(false);
    model.setShowPlugins(true);
    model.setSources(controller, nullptr, nullptr);
    for (int i = 0; i < model.rowCount(); ++i) {
        folder = folder || model.get(i).value("title").toString() == QStringLiteral("OpenFX Plug-ins");
    }
    model.setQuery(QStringLiteral("Test Gain"));
    for (int i = 0; i < model.rowCount(); ++i) {
        gainListed = gainListed || model.get(i).value("name").toString() == QStringLiteral("Wabcut Test Gain");
    }
    CHECK(folder);
    CHECK(gainListed);
}

#else

TEST_CASE("OpenFX in Effect Controls: needs a build with OpenFX and its test plug-ins", "[app][ofx][.]") { SUCCEED(); }

#endif
