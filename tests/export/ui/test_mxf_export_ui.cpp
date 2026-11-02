// 0.6 (#73): MXF delivery and embedded captions in the Export workspace - ExportController.describe's summary (the
// operational pattern, the audio layout, where the captions go, why a format refuses them), the finished job's report
// line, and the settings drawer's MXF and Embedded captions rows on the real QML shell, with screenshots in the dark and
// the light theme (<build>/test-output/ui/export-mxf-{dark,light}.png and export-mxf-captions-{dark,light}.png).

#include "ExportController.h"
#include "ProjectController.h"
#include "ShellFixture.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/RenderedVideoSource.h>

#include <QImage>
#include <QQuickItem>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string_view>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;

/// A 1280x720 50 fps sequence "Promo" of 2 s starting at 01:00:00:00 with a caption track "English".
ProjectState captionedProject() {
    ProjectState state;
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Promo";
    seq.width = 1280;
    seq.height = 720;
    seq.frameRate = kRate50;
    seq.duration = Ticks::fromSeconds(2);
    seq.startTimecode = Ticks::fromSeconds(3600);
    CaptionTrack track;
    track.id = state.ids.allocate<CaptionTrackId>();
    track.name = "English";
    track.language = "en";
    track.cues = {{Ticks::fromSeconds(0) + Ticks{kTicksPerSecond / 5}, Ticks{kTicksPerSecond / 2}, "Hello"},
                  {Ticks::fromSeconds(1), Ticks{kTicksPerSecond / 2}, "World"}};
    seq.captionTracks = {track};
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

QVariantMap mxfOverrides() {
    return {{QStringLiteral("width"), 1280},
            {QStringLiteral("height"), 720},
            {QStringLiteral("audioChannels"), 8},
            {QStringLiteral("reelName"), QStringLiteral("A001")},
            {QStringLiteral("captions"), QStringLiteral("embed")}};
}

std::filesystem::path freshDir(const char* name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "export_ui" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

/// Lays out `root` and everything under it now, parents first (a window that is never shown polishes only when a frame
/// is rendered, so layouts keep stale positions until the grab; as tests/app/test_home_ui.cpp does).
void polishAll(QQuickItem* root) {
    if (!root) {
        return;
    }
    root->ensurePolished();
    for (QQuickItem* child : root->childItems()) {
        polishAll(child);
    }
}

/// The ExportPanel around the settings drawer (the item with the panel's presetId).
QQuickItem* exportPanelOf(QQuickItem* drawer) {
    for (QQuickItem* item = drawer; item; item = item->parentItem()) {
        if (item->property("presetId").isValid() && item->property("overrides").isValid()) {
            return item;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Export MXF delivery: the summary names the pattern the audio layout and the captions and the job reports them",
          "[export][app][mxf]") {
    clearMessages();
    const auto dir = freshDir("mxf");
    ProjectController project;
    project.resetProject(captionedProject());
    ExportController controller;
    controller.setProject(&project);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());

    QVariantMap d = controller.describe({}, QStringLiteral("mxf-op1a-xdcam-hd422"), mxfOverrides());
    INFO(d.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("format")).toString().startsWith(QStringLiteral("MXF OP1a · XDCAM HD422")));
    CHECK(d.value(QStringLiteral("mxf")).toBool());
    CHECK(d.value(QStringLiteral("operationalPattern")).toString() == QStringLiteral("OP1a"));
    CHECK(d.value(QStringLiteral("reelName")).toString() == QStringLiteral("A001"));
    CHECK(d.value(QStringLiteral("audio")).toString() ==
          QStringLiteral("PCM 24-bit · 48 kHz · 8 mono tracks · 1-2 the stereo mix, 3-8 silent"));
    CHECK(d.value(QStringLiteral("embeddedCaptions")).toString() ==
          QStringLiteral("CEA-608 CC1 + CEA-708 service 1 in an SMPTE 436M track"));
    CHECK(d.value(QStringLiteral("captionTrackAvailable")).toBool());
    CHECK(d.value(QStringLiteral("captionInVideoAvailable")).toBool()); // MPEG-2 user data
    CHECK(d.value(QStringLiteral("extension")).toString() == QStringLiteral(".mxf"));

    // OP-Atom carries no captions: refused in plain words, the drawer shows them.
    QVariantMap opAtom = mxfOverrides();
    opAtom[QStringLiteral("container")] = QStringLiteral("mxf-opatom");
    d = controller.describe({}, QStringLiteral("mxf-op1a-xdcam-hd422"), opAtom);
    CHECK_FALSE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("error")).toString().contains(QStringLiteral("OP-Atom")));

    // The finished job says what it delivered.
    QVariantMap overrides = mxfOverrides();
    overrides[QStringLiteral("output")] = QString::fromStdU16String((dir / "Promo.mxf").u16string());
    const QString job = controller.queueExport({}, QStringLiteral("mxf-op1a-xdcam-hd422"), overrides);
    REQUIRE_FALSE(job.isEmpty());
    REQUIRE(waitFor([&] { return controller.jobs()->find(job) && controller.jobs()->find(job)->finished; }, 120'000));
    const ExportJobModel::Row* row = controller.jobs()->find(job);
    INFO(row->detail.toStdString());
    CHECK(row->state == QStringLiteral("done"));
    CHECK(row->detail.contains(QStringLiteral("MXF OP1a · TC 01:00:00:00")));
    CHECK(row->detail.contains(QStringLiteral("8 mono tracks")));
    CHECK(row->detail.contains(QStringLiteral("UMID 0x060A2B34")));
    CHECK(row->detail.contains(QStringLiteral("CEA-608 CC1 + CEA-708 service 1 in an SMPTE 436M track (2 captions)")));
    CHECK(std::filesystem::exists(dir / "Promo.mxf"));
    CHECK(takeMessages().isEmpty());
}

TEST_CASE("Export settings screenshots: MXF delivery with Embedded captions in the dark and light themes",
          "[export][app][screenshot][mxf]") {
    qputenv("QML_IMPORT_PATH", QByteArrayLiteral(WABCUT_QT_QML_DIR));
    resetSettings();
    clearMessages();
    {
        Shell warmUp; // initializes the renderer once, like the app's screenshot suite
        REQUIRE(warmUp.window != nullptr);
        REQUIRE_FALSE(warmUp.grab().isNull());
    }
    // Two shots a theme (one grab per window): the drawer's top (format, MXF, codec) and its audio, reel and Embedded
    // captions rows.
    struct Shot {
        bool dark;
        const char* anchor; ///< the row scrolled to the top of the panel
        const char* file;
    };
    const Shot shots[] = {{true, "exportSettingsDrawer", "/export-mxf-dark.png"},
                          {true, "exportMxfPcmTabs", "/export-mxf-captions-dark.png"},
                          {false, "exportSettingsDrawer", "/export-mxf-light.png"},
                          {false, "exportMxfPcmTabs", "/export-mxf-captions-light.png"}};
    for (const Shot& shot : shots) {
        const bool dark = shot.dark;
        INFO(shot.file);
        Shell shell;
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        shell.theme->setProperty("reduceMotion", true);
        auto* project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(project != nullptr);
        project->resetProject(captionedProject());
        shell.window->setProperty("workspace", 2);
        auto* drawer = qobject_cast<QQuickItem*>(shell.object("exportSettingsDrawer"));
        REQUIRE(drawer != nullptr);
        QQuickItem* panel = exportPanelOf(drawer);
        REQUIRE(panel != nullptr);
        panel->setProperty("presetId", QStringLiteral("mxf-op1a-xdcam-hd422"));
        panel->setProperty("overrides", mxfOverrides());
        drawer->setProperty("open", true);
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        // The drawer sits under the preset tiles: scroll the panel to the shot's row (laid out first).
        for (int pass = 0; pass < 3; ++pass) {
            polishAll(shell.window->contentItem());
        }
        QQuickItem* anchor = std::string_view(shot.anchor) == "exportSettingsDrawer"
                                 ? drawer
                                 : drawer->findChild<QQuickItem*>(QString::fromLatin1(shot.anchor));
        REQUIRE(anchor != nullptr);
        QQuickItem* flickable = drawer->parentItem();
        while (flickable && !flickable->property("contentY").isValid()) {
            flickable = flickable->parentItem();
        }
        REQUIRE(flickable != nullptr);
        auto* content = flickable->property("contentItem").value<QQuickItem*>();
        REQUIRE(content != nullptr);
        const double top = anchor->mapToItem(content, QPointF(0, 0)).y();
        const double most = std::max(0.0, flickable->property("contentHeight").toDouble() - flickable->height());
        flickable->setProperty("contentY", std::clamp(top - 8.0, 0.0, most));
        spin(300);
        // The row is now at the top of the panel's view (so the shot shows what it says).
        const double shown = anchor->mapToItem(flickable, QPointF(0, 0)).y();
        INFO("top " << top << ", most " << most << ", contentY " << flickable->property("contentY").toDouble()
                    << ", view height " << flickable->height() << ", " << flickable->metaObject()->className());
        CHECK(shown >= 0.0);
        CHECK(shown < 40.0);

        for (const char* name : {"exportMxfPatternTabs", "exportMxfPcmTabs", "exportMxfLayoutTabs", "exportMxfReel",
                                 "exportCaptionStandardTabs", "exportCc608Tabs", "exportCc708Service",
                                 "exportCaptionCarriageTabs", "exportCaptionDisplayTabs", "exportEmbeddedCaptionsPlan"}) {
            INFO(name);
            auto* item = drawer->findChild<QQuickItem*>(QString::fromLatin1(name));
            REQUIRE(item != nullptr);
            CHECK(item->isVisible());
        }
        auto* plan = drawer->findChild<QQuickItem*>(QStringLiteral("exportEmbeddedCaptionsPlan"));
        CHECK(plan->property("text").toString() == QStringLiteral("CEA-608 CC1 + CEA-708 service 1 in an SMPTE 436M track"));
        auto* reel = drawer->findChild<QQuickItem*>(QStringLiteral("exportMxfReel"));
        CHECK(reel->property("text").toString() == QStringLiteral("A001"));
        // The codec row offers MXF's codecs; XDCAM HD422 is chosen.
        auto* codecs = drawer->findChild<QQuickItem*>(QStringLiteral("exportCodecTabs"));
        REQUIRE(codecs != nullptr);
        CHECK(codecs->property("currentIndex").toInt() == 1);

        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const QString path = outputDir() + QLatin1String(shot.file);
        REQUIRE(image.save(path));
        UNSCOPED_INFO("wrote " << path.toStdString());
    }
    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}
