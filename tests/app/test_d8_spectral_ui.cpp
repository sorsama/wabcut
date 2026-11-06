// 0.7 (stream D, batch D8) in the app, card 047 (spectral repair and spectrogram editing): the Spectral tab of Audio Tools. It
// follows the selected audio clip, draws its spectrogram off the GUI thread from a synthetic sound (a voice-like tone with
// harmonics, a hum, a squeak and a bed of noise), maps pixels to time and frequency, turns the tools' selections into the
// clip's edits (one undo step each) and draws them over the picture; screenshots of the tab in the dark and the light theme.
// Screenshots go to <build>/test-output/ui.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "ProjectController.h"
#include "SpectralRepairBridge.h"
#include "SpectralSources.h"

#include <wabcut/audio/dsp/Spectrogram.h>
#include <wabcut/playback/audio/SyntheticAudioFeed.h>
#include <wabcut/model/SpectralRepair.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QImage>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QVariantMap>

#include <cmath>
#include <numbers>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr int kSoundRate = 48000;

/// A voice-like tone (200 Hz with harmonics, pulsing), a 100 Hz hum, a 3.2 kHz squeak from 2.0 to 2.5 s and a bed of noise.
class SynthSound final : public audio::dsp::SampleSource {
public:
    explicit SynthSound(std::int64_t length) : m_length(length) {}
    std::int64_t length() const override { return m_length; }
    void read(std::int64_t start, float* out, std::int64_t count) override {
        for (std::int64_t i = 0; i < count; ++i) {
            const std::int64_t n = start + i;
            if (n < 0 || n >= m_length) {
                out[i] = 0.0f;
                continue;
            }
            const double t = static_cast<double>(n) / kSoundRate;
            double v = 0.03 * std::sin(2.0 * std::numbers::pi * 100.0 * t);
            const double pulse = 0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * 2.5 * t);
            for (int h = 1; h <= 12; ++h) {
                v += pulse * 0.08 / h * std::sin(2.0 * std::numbers::pi * 200.0 * h * t);
            }
            if (t >= 2.0 && t < 2.5) {
                v += 0.15 * std::sin(2.0 * std::numbers::pi * 3200.0 * t);
            }
            std::uint32_t s = static_cast<std::uint32_t>(n) * 2654435761U;
            s ^= s >> 15;
            s *= 2246822519U;
            s ^= s >> 13;
            v += 0.01 * (static_cast<double>(s) / 4294967295.0 * 2.0 - 1.0);
            out[i] = static_cast<float>(v);
        }
    }

private:
    std::int64_t m_length;
};

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
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

QVariantList boxPoints(double t0, double hz0, double t1, double hz1) {
    return {QVariantMap{{"time", t0}, {"hz", hz0}}, QVariantMap{{"time", t1}, {"hz", hz1}}};
}

} // namespace

TEST_CASE("The Spectral tab draws the spectrogram of the selected clip and repairs what is selected, dark and light", "[app][spectral][d8]") {
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* spectral = shell.engine.singletonInstance<SpectralRepairBridge*>("Wabcut", "SpectralRepair");
    REQUIRE(controller != nullptr);
    REQUIRE(spectral != nullptr);
    const std::string clipLine(150, 'a'); // six seconds at 25 fps
    Timeline tl{"V1 tgt |A" + clipLine + "|\nA1 tgt dialogue |A" + clipLine + "|\n"};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    spectral->setSourceFactory([](const SpectralClip& clip, int) { return std::make_shared<SynthSound>(clip.lengthSamples); });
    spin(200);

    // Nothing selected: the tab says what to do.
    evaluate(shell, QStringLiteral("window.focusPanel('audio')"));
    spin(400);
    QQuickItem* panel = shell.item("audioToolsPanel");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->isVisible());
    CHECK_FALSE(spectral->available());
    panel->setProperty("view", 2);
    spin(300);
    QQuickItem* tab = findNamed(panel, QStringLiteral("spectralTab"));
    REQUIRE(tab != nullptr);
    CHECK(tab->isVisible());
    CHECK_FALSE(findNamed(panel, QStringLiteral("spectralStage"))->isVisible());
    CHECK(findNamed(panel, QStringLiteral("spectralClipName"))->property("text").toString() == spectral->reason());
    CHECK_FALSE(spectral->reason().isEmpty());

    // Select the audio clip: the spectrogram appears, analysed in the background.
    const ClipId audio = controller->currentSequence()->audioTracks[0]->clips.front().id;
    controller->setSelection({audio});
    spin(200);
    (void)shell.grab(); // a window that is never shown polishes its layouts only when a frame is drawn
    spin(200);
    REQUIRE(spectral->available());
    CHECK(spectral->sampleRate() == kSoundRate);
    CHECK(spectral->rangeEnd() > 5.0);
    CHECK(spectral->clip() == clipKey(audio));
    QQuickItem* view = findNamed(panel, QStringLiteral("spectrogramView"));
    REQUIRE(view != nullptr);
    REQUIRE(findNamed(panel, QStringLiteral("spectralStage"))->isVisible());
    {
        const bool viewReady = waitFor([&] { return view->property("ready").toBool(); });
        INFO("view " << view->width() << "x" << view->height() << " visible " << view->isVisible() << " analysing " << view->property("analysing").toBool()
                     << " progress " << view->property("progress").toDouble() << " hasImage " << view->property("hasImage").toBool());
        REQUIRE(viewReady);
    }
    CHECK(view->property("hasImage").toBool());
    CHECK(view->property("minHz").toDouble() == Approx(20.0));
    CHECK(view->property("maxHz").toDouble() == Approx(24000.0));

    // Pixels and seconds, pixels and hertz, both ways; the level under the pointer.
    const auto call = [&](const char* name, double v) {
        double out = 0.0;
        REQUIRE(QMetaObject::invokeMethod(view, name, Q_RETURN_ARG(double, out), Q_ARG(double, v)));
        return out;
    };
    const auto callAt = [&](const char* name, double x, double y) {
        double out = 0.0;
        REQUIRE(QMetaObject::invokeMethod(view, name, Q_RETURN_ARG(double, out), Q_ARG(double, x), Q_ARG(double, y)));
        return out;
    };
    CHECK(call("timeAt", call("xAt", 2.25)) == Approx(2.25).margin(1e-6));
    CHECK(call("hzAt", call("yAt", 1000.0)) == Approx(1000.0).margin(1e-3));
    CHECK(call("yAt", 200.0) > call("yAt", 2000.0)); // low is lower on the screen
    // The squeak at 3.2 kHz, 2.25 s: loud there, quiet a kHz above it and before it.
    const double loud = callAt("levelAt", call("xAt", 2.25), call("yAt", 3200.0));
    const double above = callAt("levelAt", call("xAt", 2.25), call("yAt", 7000.0));
    const double before = callAt("levelAt", call("xAt", 1.5), call("yAt", 3200.0));
    INFO("squeak " << loud << " dB, above " << above << " dB, before it " << before << " dB");
    CHECK(loud > -25.0);
    CHECK(above < loud - 25.0);
    CHECK(before < loud - 25.0);
    // The tone finder snaps to the 100 Hz hum.
    CHECK(spectral->findTone(0.5, 5.5, 70.0, 140.0) == Approx(100.0).margin(8.0));

    const QRectF area = sceneRect(panel).adjusted(-8, -8, 8, 8);
    saveCrop(shell, area, QStringLiteral("spectral-tab-dark.png"));

    // The Box tool's selection through the tab's own function: Heal is one undo step and the edit is drawn over the picture.
    const std::size_t undo = controller->session().undoCount();
    REQUIRE(tab->setProperty("tool", "rectangle"));
    REQUIRE(tab->setProperty("points", boxPoints(1.95, 2600.0, 2.55, 4200.0)));
    spin(100);
    CHECK(findNamed(panel, QStringLiteral("spectralSelection"))->property("text").toString().contains(QStringLiteral("2.55")));
    CHECK(findNamed(panel, QStringLiteral("spectralHeal"))->property("enabled").toBool());
    saveCrop(shell, area, QStringLiteral("spectral-tab-selection-dark.png"));
    REQUIRE(QMetaObject::invokeMethod(tab, "apply", Q_ARG(QVariant, QStringLiteral("heal"))));
    spin(200);
    CHECK(controller->session().undoCount() == undo + 1);
    REQUIRE(spectral->edits().size() == 1);
    CHECK(spectral->edits().front().toMap().value("kind").toString() == QStringLiteral("heal"));
    CHECK(spectral->hasRepair());
    CHECK(tab->property("points").toList().isEmpty()); // the draft is cleared
    CHECK(findNamed(panel, QStringLiteral("spectralEdit_0")) != nullptr);

    // The Result view shows the repaired sound: the squeak is gone from the spectrogram.
    spectral->setShowResult(true);
    {
        const bool viewReady = waitFor([&] { return view->property("ready").toBool(); });
        INFO("view " << view->width() << "x" << view->height() << " visible " << view->isVisible() << " analysing " << view->property("analysing").toBool()
                     << " progress " << view->property("progress").toDouble() << " hasImage " << view->property("hasImage").toBool());
        REQUIRE(viewReady);
    }
    const double repaired = callAt("levelAt", call("xAt", 2.25), call("yAt", 3200.0));
    INFO("squeak after the repair " << repaired << " dB");
    CHECK(repaired < loud - 20.0);
    saveCrop(shell, area, QStringLiteral("spectral-tab-result-dark.png"));

    // Attenuate with a lasso and Remove tone on the hum, each its own undo step; the list shows all three.
    REQUIRE(tab->setProperty("tool", "lasso"));
    REQUIRE(tab->setProperty("points", QVariantList{QVariantMap{{"time", 3.0}, {"hz", 400.0}}, QVariantMap{{"time", 3.8}, {"hz", 400.0}},
                                                    QVariantMap{{"time", 3.4}, {"hz", 2500.0}}}));
    REQUIRE(tab->setProperty("gainDb", -30.0));
    REQUIRE(QMetaObject::invokeMethod(tab, "apply", Q_ARG(QVariant, QStringLiteral("attenuate"))));
    REQUIRE(tab->setProperty("tool", "rectangle"));
    REQUIRE(tab->setProperty("points", boxPoints(0.0, 70.0, 6.0, 140.0)));
    REQUIRE(tab->setProperty("harmonics", 3));
    REQUIRE(QMetaObject::invokeMethod(tab, "apply", Q_ARG(QVariant, QStringLiteral("tone"))));
    spin(200);
    REQUIRE(spectral->edits().size() == 3);
    CHECK(spectral->edits().at(1).toMap().value("kind").toString() == QStringLiteral("attenuate"));
    const QVariantMap toneEdit = spectral->edits().at(2).toMap();
    CHECK(toneEdit.value("kind").toString() == QStringLiteral("tone"));
    CHECK(toneEdit.value("hz").toDouble() == Approx(100.0).margin(8.0));
    CHECK(toneEdit.value("harmonics").toInt() == 3);
    CHECK(controller->session().undoCount() == undo + 3);
    CHECK(findNamed(panel, QStringLiteral("spectralEdit_2")) != nullptr);

    // Undo goes back one repair at a time; the A/B switch and the listen modes are commands too.
    REQUIRE(controller->undo());
    spin(100);
    CHECK(spectral->edits().size() == 2);
    REQUIRE(controller->redo());
    spin(100);
    CHECK(spectral->edits().size() == 3);
    CHECK(spectral->setListen(2).isEmpty());
    spin(100);
    CHECK(spectral->listen() == 2);
    CHECK(spectral->setListen(0).isEmpty());
    CHECK(spectral->setRepairEnabled(false).isEmpty());
    spin(100);
    CHECK_FALSE(spectral->repairEnabled());
    CHECK(spectral->setRepairEnabled(true).isEmpty());
    spin(100);
    spectral->setShowResult(false);
    {
        const bool viewReady = waitFor([&] { return view->property("ready").toBool(); });
        INFO("view " << view->width() << "x" << view->height() << " visible " << view->isVisible() << " analysing " << view->property("analysing").toBool()
                     << " progress " << view->property("progress").toDouble() << " hasImage " << view->property("hasImage").toBool());
        REQUIRE(viewReady);
    }

    // Zoom and scroll: the view narrows around the point and stays inside the clip.
    const double span = spectral->viewSpan();
    spectral->zoomView(4.0, 2.25);
    CHECK(spectral->viewSpan() == Approx(span / 4.0));
    CHECK(spectral->viewStart() >= spectral->rangeStart());
    spectral->scrollView(-100.0);
    CHECK(spectral->viewStart() == Approx(spectral->rangeStart()));
    spectral->fitView();
    CHECK(spectral->viewSpan() == Approx(span));
    {
        const bool viewReady = waitFor([&] { return view->property("ready").toBool(); });
        INFO("view " << view->width() << "x" << view->height() << " visible " << view->isVisible() << " analysing " << view->property("analysing").toBool()
                     << " progress " << view->property("progress").toDouble() << " hasImage " << view->property("hasImage").toBool());
        REQUIRE(viewReady);
    }

    // Light theme.
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(300);
    {
        const bool viewReady = waitFor([&] { return view->property("ready").toBool(); });
        INFO("view " << view->width() << "x" << view->height() << " visible " << view->isVisible() << " analysing " << view->property("analysing").toBool()
                     << " progress " << view->property("progress").toDouble() << " hasImage " << view->property("hasImage").toBool());
        REQUIRE(viewReady);
    }
    saveCrop(shell, area, QStringLiteral("spectral-tab-light.png"));
    REQUIRE(tab->setProperty("tool", "brush"));
    REQUIRE(tab->setProperty("points", QVariantList{QVariantMap{{"time", 0.8}, {"hz", 800.0}}, QVariantMap{{"time", 1.4}, {"hz", 1600.0}}}));
    spin(100);
    saveCrop(shell, area, QStringLiteral("spectral-tab-brush-light.png"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("The Spectral tab stays usable at a small panel size and when the clip has no audio to repair", "[app][spectral][d8]") {
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(1100, 700);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* spectral = shell.engine.singletonInstance<SpectralRepairBridge*>("Wabcut", "SpectralRepair");
    REQUIRE(controller != nullptr);
    REQUIRE(spectral != nullptr);
    Timeline tl{"V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|\n"
                "A1 tgt dialogue |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|\n"};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    spectral->setSourceFactory([](const SpectralClip& clip, int) { return std::make_shared<SynthSound>(clip.lengthSamples); });
    // A video clip stands for its linked audio.
    controller->setSelection({controller->currentSequence()->videoTracks[0]->clips.front().id});
    spin(200);
    REQUIRE(spectral->available());
    evaluate(shell, QStringLiteral("window.focusPanel('audio')"));
    spin(400);
    QQuickItem* panel = shell.item("audioToolsPanel");
    REQUIRE(panel != nullptr);
    panel->setProperty("view", 2);
    spin(300);
    (void)shell.grab(); // a window that is never shown polishes its layouts only when a frame is drawn
    spin(200);
    QQuickItem* view = findNamed(panel, QStringLiteral("spectrogramView"));
    REQUIRE(view != nullptr);
    {
        const bool viewReady = waitFor([&] { return view->property("ready").toBool(); });
        INFO("view " << view->width() << "x" << view->height() << " visible " << view->isVisible() << " analysing " << view->property("analysing").toBool()
                     << " progress " << view->property("progress").toDouble() << " hasImage " << view->property("hasImage").toBool());
        REQUIRE(viewReady);
    }
    // Nothing runs off the edge of the panel: every row of the tab is as wide as the tab at most.
    QQuickItem* tab = findNamed(panel, QStringLiteral("spectralTab"));
    REQUIRE(tab != nullptr);
    const QRectF tabRect = sceneRect(tab);
    for (const char* name : {"spectralViewSettings", "spectralRepairRow", "spectralListenRow", "spectralStage"}) {
        QQuickItem* row = findNamed(tab, QString::fromLatin1(name));
        REQUIRE(row != nullptr);
        INFO(name);
        CHECK(sceneRect(row).right() <= tabRect.right() + 1.0);
        CHECK(sceneRect(row).left() >= tabRect.left() - 1.0);
    }
    saveCrop(shell, sceneRect(panel).adjusted(-8, -8, 8, 8), QStringLiteral("spectral-tab-small.png"));
    // Deselect: the tab is back to its message.
    controller->setSelection({});
    spin(200);
    CHECK_FALSE(spectral->available());
    CHECK_FALSE(findNamed(panel, QStringLiteral("spectralStage"))->isVisible());
}

// ---- The sources of the spectrogram ------------------------------------------------------------------------------------------

TEST_CASE("The spectrogram sources read a stream's channels, and repaired reads agree wherever they start", "[app][spectral][d8]") {
    // A 48 kHz stream with a 1 kHz tone on the left and half of it on the right, through the synthetic feed the player tests use.
    Timeline tl{"A1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|\n"};
    const MediaId media = tl.media('A');
    auto feed = std::make_shared<playback::SyntheticAudioFeed>();
    playback::SyntheticSignal tone;
    tone.kind = playback::SyntheticSignal::Kind::Sine;
    tone.frequency = 1000.0;
    tone.amplitude = 0.4f;
    tone.channelGains = {1.0f, 0.5f};
    feed->setSignal(media, 0, tone);
    const std::int64_t length = kSoundRate * 6;
    const auto sourceOf = [&](int channel) { return std::make_shared<FeedSource>(feed, media, 0, kSoundRate, length, 2, channel); };
    const Rate rate{kSoundRate, 1};

    // Left, right and the mix are what the feed holds; chunk boundaries (8192 frames) are invisible; outside the media is silence.
    for (const auto& [channel, scale] : {std::pair{0, 1.0f}, std::pair{1, 0.5f}, std::pair{-1, 0.75f}}) {
        INFO("channel " << channel);
        auto source = sourceOf(channel);
        std::vector<float> block(30000);
        source->read(8000, block.data(), static_cast<std::int64_t>(block.size())); // across the 8192 and 16384 boundaries
        double worst = 0.0;
        for (std::size_t i = 0; i < block.size(); ++i) {
            const double expected = scale * playback::SyntheticAudioFeed::evaluate(tone, 8000 + static_cast<std::int64_t>(i), 0, rate);
            worst = std::max(worst, std::fabs(block[i] - expected));
        }
        CHECK(worst < 1e-6);
        std::vector<float> outside(100, 1.0f);
        source->read(-50, outside.data(), 100); // 50 before the start
        CHECK(outside[10] == 0.0f);
        CHECK(outside[80] == Approx(scale * playback::SyntheticAudioFeed::evaluate(tone, 30, 0, rate)).margin(1e-6));
        source->read(length - 50, outside.data(), 100); // 50 past the end
        CHECK(outside[80] == 0.0f);
    }
    CHECK(sourceOf(0)->length() == length);

    // The repaired source: a 1 kHz tone attenuated by 24 dB over 2 to 4 s, read in two overlapping pieces that start at different
    // places. They agree to the last bit where they overlap (the STFT grid is anchored to the media's own sample numbers) and
    // the level drop is what was asked; outside the edit the sound is the original's.
    SpectralEdit edit;
    edit.kind = SpectralKind::Attenuate;
    edit.start = 2.0;
    edit.end = 4.0;
    edit.low = 800.0;
    edit.high = 1250.0;
    edit.gainDb = -24.0;
    const auto original = sourceOf(0);
    RepairedSource repaired(original, {edit}, kSoundRate, 2048);
    std::vector<float> a(kSoundRate), b(kSoundRate);
    repaired.read(kSoundRate * 2, a.data(), static_cast<std::int64_t>(a.size()));      // 2.0 to 3.0 s
    repaired.read(kSoundRate * 2 + 12345, b.data(), static_cast<std::int64_t>(b.size())); // 2.26 to 3.26 s
    for (std::size_t i = 0; i < a.size() - 12345; ++i) {
        if (a[i + 12345] != b[i]) {
            FAIL("the two reads differ at " << i << ": " << a[i + 12345] << " vs " << b[i]);
        }
    }
    double peakA = 0.0;
    for (std::size_t i = 10000; i < a.size(); ++i) {
        peakA = std::max(peakA, static_cast<double>(std::fabs(a[i])));
    }
    CHECK(peakA == Approx(0.4 * std::pow(10.0, -24.0 / 20.0)).margin(0.003));
    std::vector<float> before(5000), kept(5000);
    repaired.read(kSoundRate, before.data(), 5000);
    original->read(kSoundRate, kept.data(), 5000);
    CHECK(before == kept);

    // The cache over it shows the drop at the tone's bin: 24 dB between the original's tile and the repaired one.
    auto originalCache = std::make_shared<audio::dsp::SpectrogramCache>(original, kSoundRate, 2048, 8U << 20);
    auto repairedCache = std::make_shared<audio::dsp::SpectrogramCache>(std::make_shared<RepairedSource>(sourceOf(0), std::vector<SpectralEdit>{edit}, kSoundRate, 2048),
                                                                        kSoundRate, 2048, 8U << 20);
    const int column = static_cast<int>(3.0 * kSoundRate / originalCache->hop()); // 3 s: column 281, the 26th of the second tile
    const auto t0 = originalCache->tile(0, column / audio::dsp::kSpectrogramTileColumns);
    const auto t1 = repairedCache->tile(0, column / audio::dsp::kSpectrogramTileColumns);
    REQUIRE((t0 && t1));
    const int bin = static_cast<int>(std::lround(1000.0 / (static_cast<double>(kSoundRate) / 2048)));
    const int inTile = column % audio::dsp::kSpectrogramTileColumns;
    const double drop = audio::dsp::spectrogramDb(t0->at(inTile, bin)) - audio::dsp::spectrogramDb(t1->at(inTile, bin));
    INFO("the tone dropped " << drop << " dB");
    CHECK(drop == Approx(24.0).margin(1.0));
}
