// 0.7 (stream D, batch D7) in the app:
//   card 044  Dynamics Processing in Effect Controls: the graph row above the bands in use, the edits the graph makes (one
//             undo step per drag), the static transfer curve, the presets (one undo step, Input / Output / Mix kept) and the
//             live meters; the graph and its fields in the panel, dark and light (screenshots);
//   card 048  a 7.1 master in the Audio Track Mixer: the format switch with its note, the seven-speaker panner, the
//             eight-channel master meter, the 7.1 submix, track format and export choices; screenshots.
// Screenshots go to <build>/test-output/ui.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioChannelsBridge.h"
#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "EffectControls.h"
#include "MeterBar.h"
#include "ProjectController.h"

#include <wabcut/audio/dsp/DynamicsProcessing.h>
#include <wabcut/model/AudioChannelOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QImage>
#include <QQuickItem>
#include <QVariantMap>

#include <cmath>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr const char* kPicture = "V1 |Aaaaaaaaaaaaaaaaaaaa|\n"
                                 "A1 |Aaaaaaaaaaaaaaaaaaaa|\n"
                                 "A2 |....................|";

struct Fixture {
    Timeline tl{kPicture};
    ProjectController controller;
    ClipId audio;

    Fixture() {
        controller.resetProject(tl.state, tl.seq);
        audio = clipAt(*controller.snapshot(), tl.seq, "A1@0");
        controller.selectClipById(audio, ProjectController::Replace);
        controller.setPlayhead(Ticks{});
    }
    EffectControlsModel& fx() { return *controller.effectControls(); }
    std::size_t undoCount() { return controller.session().undoCount(); }
    /// Adds Dynamics Processing to the clip; its section key ("e12").
    QString addDynamics() {
        REQUIRE(fx().addEffect(QStringLiteral("wabcut.audio.dynamicsProcessing")).isEmpty());
        for (int i = 0; i < fx().rowCount(); ++i) {
            const QString key = fx().keyAt(i);
            if (key.endsWith(QLatin1String(":graph")))
                return key.left(key.size() - 6);
        }
        FAIL("no graph row");
        return {};
    }
    QVariantMap graph(const QString& section) { return fx().row(section + QStringLiteral(":graph")).value(QStringLiteral("graph")).toMap(); }
    double bandValue(const QString& section, int band, const char* name) {
        return graph(section).value(QStringLiteral("band")).toList().at(band).toMap().value(QString::fromLatin1(name)).toDouble();
    }
    const Clip& clip() const { return controller.currentSequence()->findClip(audio)->clip(); }
    const EffectInstance& instance() const { return clip().effects().back(); }
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

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void saveCrop(const Shell& shell, const QRectF& area, const QString& name) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(name)));
}

} // namespace

// ---- Card 044 -----------------------------------------------------------------------------------------------------------------

TEST_CASE("Effect Controls: Dynamics Processing has a graph row above the rows of the bands in use", "[app][effectcontrols][dynamics]") {
    Fixture f;
    const QString section = f.addDynamics();
    const QVariantMap row = f.fx().row(section + QStringLiteral(":graph"));
    CHECK(row.value("kind").toString() == QStringLiteral("dynamics"));
    CHECK(row.value("section").toString() == section);
    // The graph row follows the section's header, before the first parameter.
    const int header = f.fx().rowOf(QStringLiteral("h:") + section);
    REQUIRE(header >= 0);
    CHECK(f.fx().rowOf(section + QStringLiteral(":graph")) == header + 1);
    CHECK(f.fx().rowOf(section + QStringLiteral(":inputGain")) == header + 2);

    const QVariantMap g = f.graph(section);
    CHECK(g.value("bands").toInt() == 4);
    CHECK(g.value("crossovers").toList().size() == 3);
    CHECK(g.value("crossovers").toList().at(0).toDouble() == Approx(120.0));
    CHECK(g.value("crossovers").toList().at(2).toDouble() == Approx(8000.0));
    REQUIRE(g.value("band").toList().size() == 4);
    CHECK(f.bandValue(section, 0, "mode") == 0.0);
    CHECK(f.bandValue(section, 0, "threshold") == Approx(-24.0));
    CHECK(f.bandValue(section, 0, "ratio") == Approx(2.0));
    CHECK(f.bandValue(section, 3, "release") == Approx(100.0));
    CHECK(g.value("limiter").toDouble() == 0.0);
    CHECK(g.value("mix").toDouble() == Approx(100.0));
    CHECK(g.value("instance").toULongLong() == f.instance().id.value);
    // Every band's parameters have a row (a slider, a menu or a switch) with the section.
    for (const char* key : {"bandCount", "crossover3", "band4Ratio", "band2Mode", "band3Bypass", "band1Solo", "limiterEnable", "autoGate"})
        CHECK_FALSE(f.fx().row(section + QStringLiteral(":") + QString::fromLatin1(key)).isEmpty());
    CHECK(f.fx().row(section + QStringLiteral(":band2Mode")).value("kind").toString() == QStringLiteral("choice"));
    CHECK(f.fx().row(section + QStringLiteral(":limiterEnable")).value("kind").toString() == QStringLiteral("toggle"));

    // Fewer bands: the rows of the bands that are not used (and of the crossovers beyond them) are gone.
    f.fx().setValue(section + QStringLiteral(":bandCount"), 2.0);
    f.fx().endEdit();
    CHECK(f.graph(section).value("bands").toInt() == 2);
    CHECK(f.fx().row(section + QStringLiteral(":band3Ratio")).isEmpty());
    CHECK(f.fx().row(section + QStringLiteral(":band4Threshold")).isEmpty());
    CHECK(f.fx().row(section + QStringLiteral(":crossover2")).isEmpty());
    CHECK_FALSE(f.fx().row(section + QStringLiteral(":crossover1")).isEmpty());
    CHECK_FALSE(f.fx().row(section + QStringLiteral(":band2Ratio")).isEmpty());
    f.fx().setValue(section + QStringLiteral(":bandCount"), 1.0);
    f.fx().endEdit();
    CHECK(f.fx().row(section + QStringLiteral(":crossover1")).isEmpty());
    f.fx().setValue(section + QStringLiteral(":bandCount"), 4.0);
    f.fx().endEdit();
    CHECK_FALSE(f.fx().row(section + QStringLiteral(":band4Ratio")).isEmpty());

    // Other audio effects have no graph row, and a collapsed section hides it with its rows.
    REQUIRE(f.fx().addEffect(QStringLiteral("wabcut.audio.multibandCompressor")).isEmpty());
    int graphRows = 0;
    for (int i = 0; i < f.fx().rowCount(); ++i)
        graphRows += f.fx().keyAt(i).endsWith(QLatin1String(":graph")) ? 1 : 0;
    CHECK(graphRows == 1);
    f.fx().setCollapsed(section, true);
    CHECK(f.fx().row(section + QStringLiteral(":graph")).isEmpty());
}

TEST_CASE("Dynamics Processing: the graph's edits are single undo steps and keyframable", "[app][effectcontrols][dynamics]") {
    Fixture f;
    const QString section = f.addDynamics();
    // A drag of a threshold handle: many setValue calls, one undo step.
    const std::size_t before = f.undoCount();
    for (const double v : {-26.0, -28.5, -31.0})
        f.fx().setValue(section + QStringLiteral(":band1Threshold"), v);
    f.fx().endEdit();
    CHECK(f.undoCount() == before + 1);
    CHECK(f.bandValue(section, 0, "threshold") == Approx(-31.0));
    CHECK(effectParameterAt(f.instance(), "band1Threshold", Ticks{}) == Approx(-31.0));
    REQUIRE(f.controller.undo());
    CHECK(f.bandValue(section, 0, "threshold") == Approx(-24.0));
    REQUIRE(f.controller.redo());
    CHECK(f.bandValue(section, 0, "threshold") == Approx(-31.0));

    // The mode, Solo and Bypass the graph sets are the effect's own parameters.
    f.fx().setValue(section + QStringLiteral(":band2Mode"), 2.0); // Gate
    f.fx().endEdit();
    CHECK(f.bandValue(section, 1, "mode") == 2.0);
    f.fx().setValue(section + QStringLiteral(":band3Solo"), 1.0);
    f.fx().endEdit();
    CHECK(f.bandValue(section, 2, "solo") == 1.0);
    f.fx().setValue(section + QStringLiteral(":band4Bypass"), 1.0);
    f.fx().endEdit();
    CHECK(f.bandValue(section, 3, "bypass") == 1.0);
    f.fx().setValue(section + QStringLiteral(":crossover1"), 250.0);
    f.fx().endEdit();
    CHECK(f.graph(section).value("crossovers").toList().at(0).toDouble() == Approx(250.0));

    // Every control animates: the stopwatch on a threshold keys it, and the graph then shows the value at the playhead.
    f.fx().setAnimated(section + QStringLiteral(":band1Threshold"), true);
    f.fx().setValue(section + QStringLiteral(":band1Threshold"), -40.0);
    f.fx().endEdit();
    CHECK(f.instance().find("band1Threshold")->isCurve());
    CHECK(f.bandValue(section, 0, "threshold") == Approx(-40.0));
    // The choice parameters (mode, band count, the limiter and gate switches) are not animatable.
    CHECK_FALSE(f.fx().row(section + QStringLiteral(":band1Mode")).value("animatable").toBool());
    CHECK_FALSE(f.fx().row(section + QStringLiteral(":bandCount")).value("animatable").toBool());
    CHECK(f.fx().row(section + QStringLiteral(":band1Bypass")).value("animatable").toBool());
}

TEST_CASE("Dynamics Processing: the static curve the graph draws is the DSP's", "[app][effectcontrols][dynamics]") {
    Fixture f;
    // Compress 4:1 over -20 dB with a hard knee: -80 .. 0 dB in, 81 values, one per dB.
    QVariantList curve = f.fx().dynamicsCurve(0, -20.0, 4.0, 40.0, 0.0, 0.0, -80.0, 0.0, 81);
    REQUIRE(curve.size() == 81);
    CHECK(curve.at(0).toDouble() == Approx(-80.0));
    CHECK(curve.at(60).toDouble() == Approx(-20.0)); // the threshold
    CHECK(curve.at(80).toDouble() == Approx(-15.0)); // 0 dB in
    for (int i = 0; i < 81; ++i) {
        const double in = -80.0 + i;
        audio::dsp::DynamicsBand band;
        band.thresholdDb = -20.0;
        band.ratio = 4.0;
        band.kneeDb = 0.0;
        CHECK(curve.at(i).toDouble() == Approx(audio::dsp::dynamicsCurveDb(in, band)).margin(1e-9));
    }
    // Make-up lifts it; a limiter stops at the threshold; a gate drops by its range; an expander falls twice as fast.
    CHECK(f.fx().dynamicsCurve(0, -20.0, 4.0, 40.0, 0.0, 3.0, -80.0, 0.0, 81).at(0).toDouble() == Approx(-77.0));
    CHECK(f.fx().dynamicsCurve(3, -12.0, 1.0, 40.0, 0.0, 0.0, -80.0, 0.0, 81).at(80).toDouble() == Approx(-12.0));
    CHECK(f.fx().dynamicsCurve(2, -40.0, 2.0, 30.0, 0.0, 0.0, -80.0, 0.0, 81).at(10).toDouble() == Approx(-100.0)); // -70 - 30
    CHECK(f.fx().dynamicsCurve(1, -30.0, 2.0, 40.0, 0.0, 0.0, -80.0, 0.0, 81).at(30).toDouble() == Approx(-70.0)); // -50 in = -70
    CHECK(f.fx().dynamicsCurve(0, -20.0, 4.0, 40.0, 0.0, 0.0, -80.0, 0.0, 1).size() == 2); // at least two points
}

TEST_CASE("Dynamics Processing: presets are one undo step and keep Input, Output and Mix", "[app][effectcontrols][dynamics]") {
    Fixture f;
    const QString section = f.addDynamics();
    const QStringList names = f.fx().dynamicsPresets();
    CHECK(names.size() >= 6);
    for (const char* name : {"Voice Leveler", "De-esser", "Noise Gate", "Bass Tamer", "Podcast Glue", "Broadcast Limiter"})
        CHECK(names.contains(QString::fromLatin1(name)));
    f.fx().setValue(section + QStringLiteral(":inputGain"), 3.0);
    f.fx().setValue(section + QStringLiteral(":outputGain"), -2.0);
    f.fx().setValue(section + QStringLiteral(":mix"), 80.0);
    f.fx().endEdit();

    const std::size_t before = f.undoCount();
    REQUIRE(f.fx().applyDynamicsPreset(section, QStringLiteral("Voice Leveler")));
    CHECK(f.undoCount() == before + 1);
    CHECK(f.controller.undoText() == QStringLiteral("Undo Dynamics Processing: Voice Leveler"));
    QVariantMap g = f.graph(section);
    CHECK(g.value("bands").toInt() == 3);
    CHECK(g.value("crossovers").toList().at(0).toDouble() == Approx(150.0));
    CHECK(g.value("crossovers").toList().at(1).toDouble() == Approx(4000.0));
    CHECK(f.bandValue(section, 1, "threshold") == Approx(-26.0));
    CHECK(f.bandValue(section, 1, "ratio") == Approx(3.0));
    CHECK(f.bandValue(section, 1, "makeup") == Approx(3.0));
    CHECK(g.value("limiter").toDouble() == 1.0);
    CHECK(g.value("inputGain").toDouble() == Approx(3.0)); // what is not the bands' stays
    CHECK(g.value("outputGain").toDouble() == Approx(-2.0));
    CHECK(g.value("mix").toDouble() == Approx(80.0));
    // A gate preset sets the mode.
    REQUIRE(f.fx().applyDynamicsPreset(section, QStringLiteral("Noise Gate")));
    CHECK(f.graph(section).value("bands").toInt() == 1);
    CHECK(f.bandValue(section, 0, "mode") == 2.0);
    CHECK(f.graph(section).value("limiter").toDouble() == 0.0); // not in the preset: back to its default
    REQUIRE(f.controller.undo());
    CHECK(f.graph(section).value("bands").toInt() == 3);
    REQUIRE(f.controller.undo());
    CHECK(f.graph(section).value("bands").toInt() == 4);
    CHECK(f.bandValue(section, 1, "threshold") == Approx(-24.0));
    CHECK(f.graph(section).value("inputGain").toDouble() == Approx(3.0));
    // No such preset, no such section, not a Dynamics Processing section: nothing happens.
    const std::size_t steady = f.undoCount();
    CHECK_FALSE(f.fx().applyDynamicsPreset(section, QStringLiteral("No such preset")));
    CHECK_FALSE(f.fx().applyDynamicsPreset(QStringLiteral("e999999"), QStringLiteral("Voice Leveler")));
    REQUIRE(f.fx().addEffect(QStringLiteral("wabcut.audio.multibandCompressor")).isEmpty());
    const std::size_t afterAdd = f.undoCount();
    CHECK(afterAdd == steady + 1);
    QString other;
    for (int i = 0; i < f.fx().rowCount(); ++i) {
        const QString key = f.fx().keyAt(i);
        if (key.startsWith(QLatin1String("h:e")) && key != QStringLiteral("h:") + section)
            other = key.mid(2);
    }
    REQUIRE_FALSE(other.isEmpty());
    CHECK_FALSE(f.fx().applyDynamicsPreset(other, QStringLiteral("Voice Leveler")));
    CHECK(f.undoCount() == afterAdd);
}

TEST_CASE("Dynamics Processing: the meters read what the processors note", "[app][effectcontrols][dynamics]") {
    Fixture f;
    const QString section = f.addDynamics();
    const auto id = parseEffectKey(section);
    REQUIRE(id);
    // Nothing plays: no meters, the floor.
    QVariantMap idle = f.fx().dynamicsMeters(section);
    CHECK_FALSE(idle.value("active").toBool());
    CHECK(idle.value("inputDb").toDouble() == Approx(-90.0));
    CHECK(idle.value("reduction").toList().size() == 4);
    // A processor of the instance is playing (it holds the meters while it lives).
    const auto meters = audio::dsp::dynamicsMetersFor(id->value);
    meters->note(0.5f, 0.25f, {6.0f, 0.0f, 2.5f, 0.0f});
    QVariantMap m = f.fx().dynamicsMeters(section);
    CHECK(m.value("active").toBool());
    CHECK(m.value("inputDb").toDouble() == Approx(20.0 * std::log10(0.5)).margin(0.01));
    CHECK(m.value("outputDb").toDouble() == Approx(20.0 * std::log10(0.25)).margin(0.01));
    CHECK(m.value("reduction").toList().at(0).toDouble() == Approx(6.0));
    CHECK(m.value("reduction").toList().at(2).toDouble() == Approx(2.5));
    // Read again with nothing new: the audio stopped, the peaks are cleared.
    m = f.fx().dynamicsMeters(section);
    CHECK_FALSE(m.value("active").toBool());
    CHECK(m.value("inputDb").toDouble() == Approx(-90.0));
    CHECK(m.value("reduction").toList().at(0).toDouble() == Approx(0.0));
    meters->note(0.1f, 0.1f, {0.0f, 0.0f, 0.0f, 0.0f});
    CHECK(f.fx().dynamicsMeters(section).value("active").toBool());
    // Not a section of an effect: the floor.
    CHECK_FALSE(f.fx().dynamicsMeters(QStringLiteral("audio")).value("active").toBool());
}

TEST_CASE("Effect Controls: the Dynamics Processing graph in the panel; screenshots", "[app][effectcontrols][dynamics][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, 2000); // tall: room for the whole graph in the Effect Controls list
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    const ClipId audio = clipAt(*controller->snapshot(), tl.seq, "A1@0");
    controller->selectClipById(audio, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    EffectControlsModel& fx = *controller->effectControls();
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.audio.dynamicsProcessing")).isEmpty());
    REQUIRE_FALSE(shell.grab().isNull());
    spin(200);
    QString section;
    for (int i = 0; i < fx.rowCount(); ++i) {
        if (fx.keyAt(i).endsWith(QLatin1String(":graph")))
            section = fx.keyAt(i).left(fx.keyAt(i).size() - 6);
    }
    REQUIRE_FALSE(section.isEmpty());
    // A voice-leveller setting so the picture has something in it, and a little level on the meters.
    REQUIRE(fx.applyDynamicsPreset(section, QStringLiteral("Voice Leveler")));
    fx.setValue(section + QStringLiteral(":band2Solo"), 1.0);
    fx.endEdit();
    const auto id = parseEffectKey(section);
    REQUIRE(id);
    const auto meters = audio::dsp::dynamicsMetersFor(id->value);
    for (int i = 0; i < 6; ++i) {
        meters->note(0.5f, 0.3f, {1.5f, 7.0f, 3.5f, 0.0f});
        spin(40);
    }

    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    const int index = fx.rowOf(section + QStringLiteral(":graph"));
    REQUIRE(index >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index - 1), Q_ARG(int, 0)));
    spin(300);
    QQuickItem* graph = findNamed(shell.window->contentItem(), QStringLiteral("dynamicsGraph_") + section);
    REQUIRE(graph != nullptr);
    CHECK(graph->isVisible());
    CHECK(graph->width() > 200);
    for (const char* name : {"dynamicsFrequencyView", "dynamicsTransferView", "dynamicsBand_1", "dynamicsBand_3", "dynamicsSolo_2",
                             "dynamicsBypass_1", "dynamicsPresets", "dynamicsMode", "dynamicsDivider_1", "dynamicsThreshold_2",
                             "dynamicsCurveThreshold", "dynamicsCurveRatio", "dynamicsField_threshold", "dynamicsField_makeup",
                             "dynamicsLimiter", "dynamicsAutoGate", "dynamicsBandCount"}) {
        INFO(name);
        QQuickItem* item = findNamed(graph, QString::fromLatin1(name));
        REQUIRE(item != nullptr);
    }
    CHECK(findNamed(graph, QStringLiteral("dynamicsBand_4")) == nullptr); // three bands in the preset
    QQuickItem* solo = findNamed(graph, QStringLiteral("dynamicsSolo_2"));
    CHECK(solo->property("checked").toBool());
    // Dragging a divider through the graph's own signal edits the crossover (one undo step).
    const std::size_t before = controller->session().undoCount();
    QQuickItem* divider = findNamed(graph, QStringLiteral("dynamicsDivider_1"));
    REQUIRE(divider != nullptr);
    REQUIRE(QMetaObject::invokeMethod(divider, "dragged", Q_ARG(double, divider->x() + divider->width() / 2 + 30.0), Q_ARG(double, 10.0)));
    REQUIRE(QMetaObject::invokeMethod(divider, "dragged", Q_ARG(double, divider->x() + divider->width() / 2 + 40.0), Q_ARG(double, 10.0)));
    REQUIRE(QMetaObject::invokeMethod(divider, "finished"));
    spin(100);
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(effectParameterAt(controller->currentSequence()->findClip(audio)->clip().effects().back(), "crossover1", Ticks{}) > 150.0);
    REQUIRE(controller->undo());
    spin(100);

    const QRectF area = sceneRect(graph).adjusted(-8, -8, 8, 8);
    {
        // The graph's row must be as tall as the graph (the first picture of the card showed the pictures hidden behind the
        // next section): the whole graph, the frequency view and the transfer curve inside it.
        // The list's delegate (the item the ListView stacks): the graph's ancestor just under the list's content item.
        auto* listItem = qobject_cast<QQuickItem*>(list);
        REQUIRE(listItem != nullptr);
        QQuickItem* content = listItem->property("contentItem").value<QQuickItem*>();
        REQUIRE(content != nullptr);
        QQuickItem* row = graph;
        while (row->parentItem() != nullptr && row->parentItem() != content)
            row = row->parentItem();
        REQUIRE(row->parentItem() == content);
        QQuickItem* freq = findNamed(graph, QStringLiteral("dynamicsFrequencyView"));
        QQuickItem* curve = findNamed(graph, QStringLiteral("dynamicsTransferView"));
        REQUIRE(freq != nullptr);
        REQUIRE(curve != nullptr);
        INFO("graph " << graph->width() << " x " << graph->height() << " implicit " << graph->implicitHeight() << "; row " << row->width()
                      << " x " << row->height() << "; freq " << freq->height() << " at " << sceneRect(freq).bottom() << "; curve "
                      << curve->height() << " at " << sceneRect(curve).bottom() << "; row bottom " << sceneRect(row).bottom());
        CHECK(graph->height() >= graph->implicitHeight() - 1.0);
        CHECK(row->height() >= graph->height());
        CHECK(sceneRect(freq).bottom() <= sceneRect(row).bottom() + 0.5);
        CHECK(sceneRect(curve).bottom() <= sceneRect(row).bottom() + 0.5);
        CHECK(freq->height() >= 100);
        // The window is tall enough that the screenshot shows the whole graph.
        CHECK(sceneRect(graph).bottom() <= sceneRect(listItem).bottom() + 0.5);
        // Side by side from about 440 px: the two pictures share a row in the default panel width.
        CHECK(std::abs(sceneRect(freq).top() - sceneRect(curve).top()) < 1.0);
        // The playhead line stops above the graph and starts again below it (it would sweep across the picture).
        QQuickItem* playhead = findNamed(shell.window->contentItem(), QStringLiteral("lanePlayhead"));
        REQUIRE(playhead != nullptr);
        CHECK(playhead->isVisible());
        int pieces = 0;
        for (QQuickItem* piece : playhead->childItems()) {
            if (piece->width() <= 0 || piece->height() <= 0)
                continue; // the repeater itself
            ++pieces;
            const QRectF r = sceneRect(piece);
            CHECK((r.bottom() <= sceneRect(graph).top() + 0.5 || r.top() >= sceneRect(graph).bottom() - 0.5));
        }
        CHECK(pieces >= 1);
    }
    saveCrop(shell, area, QStringLiteral("dynamics-graph.png"));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(300);
    saveCrop(shell, sceneRect(graph).adjusted(-8, -8, 8, 8), QStringLiteral("dynamics-graph-light.png"));
    CHECK(takeMessages().isEmpty());
}

// ---- Card 048 -----------------------------------------------------------------------------------------------------------------

TEST_CASE("Mixer: a 7.1 master has the seven-speaker panner, an eight-channel meter and says what the upmix leaves silent; screenshot",
          "[app][channels][surround71][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* channels = shell.engine.singletonInstance<AudioChannelsBridge*>("Wabcut", "AudioChannels");
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(controller);
    REQUIRE(channels);
    REQUIRE(mixer);
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    mixer->setMeterSource([](int index) -> std::optional<AudioMixerBridge::MeterReading> {
        if (index == -1)
            return AudioMixerBridge::MeterReading{8, {0.7f, 0.66f, 0.5f, 0.2f, 0.25f, 0.22f, 0.35f, 0.3f}, {0.3f, 0.29f, 0.22f, 0.08f, 0.1f, 0.09f, 0.15f, 0.12f}, false};
        if (index == 0)
            return AudioMixerBridge::MeterReading{2, {0.52f, 0.48f}, {0.22f, 0.2f}, false};
        return std::nullopt;
    });
    shell.window->setProperty("workspace", 1); // Audio
    spin(500);

    // 5.1 first, then 7.1: the note says where the surrounds went.
    REQUIRE(channels->setMasterFormat(QStringLiteral("5.1")).isEmpty());
    REQUIRE(channels->setMasterFormat(QStringLiteral("7.1")).isEmpty());
    CHECK(channels->masterFormat() == QStringLiteral("7.1"));
    CHECK(controller->currentSequence()->masterFormat == ChannelFormat::Surround71);
    CHECK(channels->masterNote().contains(QStringLiteral("rear pair")));
    CHECK(channels->masterNote().contains(QStringLiteral("silent")));
    REQUIRE(channels->setMasterFormat(QStringLiteral("5.1")).isEmpty());
    CHECK(channels->masterNote().contains(QStringLiteral("folded")));
    REQUIRE(channels->setMasterFormat(QStringLiteral("7.1")).isEmpty());
    spin(300);

    QQuickItem* strip = findNamed(shell.window->contentItem(), QStringLiteral("mixerStrip_A1"));
    REQUIRE(strip);
    QQuickItem* panner = findNamed(strip, QStringLiteral("surroundPanner"));
    REQUIRE(panner);
    REQUIRE(waitFor([&] { return panner->isVisible(); }, 2000));
    CHECK(panner->property("surround71").toBool());
    QQuickItem* tabs = findNamed(shell.window->contentItem(), QStringLiteral("masterFormatTabs"));
    REQUIRE(tabs);
    CHECK(tabs->property("currentIndex").toInt() == 2);
    QQuickItem* note = findNamed(shell.window->contentItem(), QStringLiteral("masterFormatNote"));
    REQUIRE(note);
    CHECK(note->isVisible());
    // The panner's puck at the back right: the stereo track's sound goes to Rs / Rrs (the model keeps x and y).
    REQUIRE(channels->setSurroundPan(trackKey(tl.track("A1")), 0.9, -0.9, -144.0).isEmpty());
    channels->endSurroundPan();
    spin(200);
    // The master meter: eight channels, named in a 7.1 file's order.
    auto* masterMeter = qobject_cast<MeterBar*>(findNamed(shell.window->contentItem(), QStringLiteral("masterMeter")));
    REQUIRE(masterMeter);
    REQUIRE(waitFor([&] { return masterMeter->channels() == 8; }, 5000));
    // The other places that offer a format: new submixes and the track formats.
    bool offersSubmix = false;
    for (const QVariant& choice : mixer->submixFormats())
        offersSubmix = offersSubmix || choice.toMap().value(QStringLiteral("id")).toString() == QStringLiteral("new:7.1");
    CHECK(offersSubmix);
    REQUIRE(channels->setTrackFormat(trackKey(tl.track("A2")), QStringLiteral("7.1")).isEmpty());
    CHECK(channels->trackFormats().value(trackKey(tl.track("A2"))).toString() == QStringLiteral("7.1"));
    REQUIRE(mixer->addSubmix(QStringLiteral("7.1")).isEmpty());
    CHECK(controller->currentSequence()->submixTracks.back()->format == ChannelFormat::Surround71);
    CHECK(channelCount(controller->currentSequence()->submixTracks.back()->format) == 8);

    QQuickItem* mixerPanel = findNamed(shell.window->contentItem(), QStringLiteral("audioMixerPanel"));
    REQUIRE(mixerPanel);
    saveCrop(shell, sceneRect(mixerPanel), QStringLiteral("surround71-mixer.png"));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(300);
    saveCrop(shell, sceneRect(mixerPanel), QStringLiteral("surround71-mixer-light.png"));
    CHECK(takeMessages().isEmpty());
}

TEST_CASE("Mixer: Dynamics Processing on a track and on the master opens its graph in the effect settings; screenshot",
          "[app][mixer][dynamics][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(controller);
    REQUIRE(mixer);
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    shell.window->setProperty("workspace", 1); // Audio
    spin(500);
    const QString track = trackKey(tl.track("A1"));
    for (const QString& target : {track, QStringLiteral("master")}) {
        INFO(target.toStdString());
        REQUIRE(mixer->addEffect(target, QStringLiteral("wabcut.audio.dynamicsProcessing")).isEmpty());
    }
    spin(300);
    const EffectStack* stack = &controller->currentSequence()->findTrack(tl.track("A1"))->effects;
    REQUIRE(stack->size() == 1);
    const QString effect = effectKey(stack->front().id);

    // The same curve and meters as Effect Controls' (the mixer bridge reads the same functions).
    const QVariantList curve = mixer->dynamicsCurve(0, -20.0, 4.0, 40.0, 0.0, 0.0, -80.0, 0.0, 81);
    REQUIRE(curve.size() == 81);
    CHECK(curve.at(80).toDouble() == Approx(-15.0));
    const auto meters = audio::dsp::dynamicsMetersFor(stack->front().id.value);
    meters->note(0.5f, 0.5f, {3.0f, 0.0f, 0.0f, 0.0f});
    const QVariantMap reading = mixer->dynamicsMeters(effect);
    CHECK(reading.value("active").toBool());
    CHECK(reading.value("reduction").toList().at(0).toDouble() == Approx(3.0));

    QQuickItem* content = shell.window->contentItem();
    QQuickItem* strip = findNamed(content, QStringLiteral("mixerStrip_A1"));
    REQUIRE(strip);
    QQuickItem* slot = nullptr;
    REQUIRE(waitFor([&] { return (slot = findNamed(strip, QStringLiteral("effectSlot_0"))) != nullptr; }, 5000));
    REQUIRE(QMetaObject::invokeMethod(slot, "doubleClicked"));
    QObject* loader = shell.window->findChild<QObject*>(QStringLiteral("mixerEffectSettingsLoader"));
    if (!loader)
        loader = content->findChild<QObject*>(QStringLiteral("mixerEffectSettingsLoader"));
    REQUIRE(loader);
    QObject* popup = nullptr;
    REQUIRE(waitFor([&] {
        popup = loader->property("item").value<QObject*>();
        return popup && popup->property("opened").toBool();
    }, 5000));
    auto* body = popup->property("contentItem").value<QQuickItem*>();
    REQUIRE(body);
    QQuickItem* box = body->parentItem();
    REQUIRE(box);
    QQuickItem* graph = findNamed(box, QStringLiteral("mixerDynamicsGraph"));
    REQUIRE(graph);
    CHECK(graph->isVisible());
    CHECK(graph->property("bandCount").toInt() == 4);
    QQuickItem* presets = findNamed(graph, QStringLiteral("dynamicsPresets"));
    REQUIRE(presets);
    CHECK_FALSE(presets->isVisible()); // the mixer has no preset menu
    // The graph's edits are the popup's own: a value typed in a field, one undo step.
    const std::size_t before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(graph, "valueEdited", Q_ARG(QString, QStringLiteral("band1Threshold")), Q_ARG(double, -33.0)));
    REQUIRE(QMetaObject::invokeMethod(graph, "valueEdited", Q_ARG(QString, QStringLiteral("band1Threshold")), Q_ARG(double, -35.0)));
    REQUIRE(QMetaObject::invokeMethod(graph, "editFinished"));
    spin(100);
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(effectParameterAt(controller->currentSequence()->findTrack(tl.track("A1"))->effects.front(), "band1Threshold", Ticks{}) ==
          Approx(-35.0));
    CHECK(graph->property("fx").toMap().value("band").toList().at(0).toMap().value("threshold").toDouble() == Approx(-35.0));
    // The bands in use: three bands hide the fourth.
    REQUIRE(QMetaObject::invokeMethod(graph, "valueEdited", Q_ARG(QString, QStringLiteral("bandCount")), Q_ARG(double, 3.0)));
    REQUIRE(QMetaObject::invokeMethod(graph, "editFinished"));
    spin(100);
    CHECK(graph->property("bandCount").toInt() == 3);
    CHECK(waitFor([&] { return findNamed(graph, QStringLiteral("dynamicsBand_4")) == nullptr; }, 2000));
    spin(200);
    // The popover opens upward from the slot and stays inside the window (the graph scrolls when it has no room). A frame is
    // rendered first: the layouts polish with it.
    REQUIRE_FALSE(shell.grab().isNull());
    spin(100);
    {
        const QRectF popupRect = sceneRect(box);
        INFO("popover " << popupRect.x() << "," << popupRect.y() << " " << popupRect.width() << "x" << popupRect.height() << " in a window of "
                        << shell.window->width() << "x" << shell.window->height());
        CHECK(popupRect.top() >= -0.5);
        CHECK(popupRect.bottom() <= shell.window->height() + 0.5);
        CHECK(popupRect.right() <= shell.window->width() + 0.5);
        INFO("graph " << graph->width() << "x" << graph->height() << " implicit " << graph->implicitHeight());
        std::string tree;
        std::function<void(QQuickItem*, int)> dump = [&](QQuickItem* item, int depth) {
            if (depth > 4)
                return;
            tree += std::string(static_cast<std::size_t>(depth) * 2, ' ') + item->metaObject()->className() + " '" +
                    item->objectName().toStdString() + "' " + std::to_string(static_cast<int>(item->x())) + "," +
                    std::to_string(static_cast<int>(item->y())) + " " + std::to_string(static_cast<int>(item->width())) + "x" +
                    std::to_string(static_cast<int>(item->height())) + (item->isVisible() ? "" : " hidden") + "\n";
            for (QQuickItem* child : item->childItems())
                dump(child, depth + 1);
        };
        dump(box, 0);
        INFO(tree);
        CHECK(graph->width() > 300);
        CHECK(graph->height() >= 200);
        CHECK(sceneRect(graph).bottom() <= sceneRect(box).bottom() + 0.5);
    }
    saveCrop(shell, sceneRect(box).adjusted(-8, -8, 8, 8), QStringLiteral("dynamics-mixer-settings.png"));
    QMetaObject::invokeMethod(popup, "close");
    spin(100);
    CHECK(takeMessages().isEmpty());
}
