// Design language (docs/design-language.md; v3 "Echo" since 2026-10-15): contrast of every accent and the text tokens
// in both themes, the clip label colours under black names, the bundled Thai+Latin type (Thai vowels and tone marks
// stack), the slice geometry SliceShape still draws, and reduce motion switching off the playhead glow, the cut spark
// and the workspace transition. Offscreen; no window is shown.

#include "ShellFixture.h"

#include "BundledFonts.h"
#include "ProjectController.h"
#include "SliceShape.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <QDir>
#include <QFont>
#include <QGlyphRun>
#include <QImage>
#include <QPainter>
#include <QQuickItem>
#include <QRawFont>
#include <QTextLayout>

#include <algorithm>
#include <utility>
#include <cmath>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

double luminance(const QColor& c) {
    const auto linear = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
}

/// WCAG contrast ratio of two opaque colours.
double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

TEST_CASE("Every accent and text token meets WCAG AA in both themes", "[app][design][contrast]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true); // the palette switches at once

    const QVariantMap accents = shell.theme->property("accents").toMap();
    const QStringList order = shell.theme->property("accentOrder").toStringList();
    REQUIRE(order == QStringList({QStringLiteral("ultraviolet"), QStringLiteral("infrared"), QStringLiteral("phosphor"),
                                  QStringLiteral("sodium"), QStringLiteral("ice")}));

    QStringList table;
    for (const bool dark : {true, false}) {
        shell.theme->setProperty("dark", dark);
        const QColor panel = shell.themeColor("panel");
        const QColor raised = shell.themeColor("raised");
        // Text on the surfaces it sits on.
        for (const char* token : {"textPrimary", "textSecondary", "textTertiary"}) {
            const QColor text = shell.themeColor(token);
            INFO((dark ? "dark " : "light ") << token);
            CHECK(contrast(text, panel) >= 4.5);
            CHECK(contrast(text, raised) >= 4.5);
            table << QStringLiteral("%1 %2: %3 on panel, %4 on raised")
                         .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"), QLatin1String(token))
                         .arg(contrast(text, panel), 0, 'f', 2)
                         .arg(contrast(text, raised), 0, 'f', 2);
        }
        for (const QString& name : order) {
            const QVariantMap a = accents.value(name).toMap();
            const QColor fill(a.value(QStringLiteral("fill")).toString());
            const QColor onFill(a.value(QStringLiteral("onFill")).toString());
            const QColor stroke(a.value(dark ? QStringLiteral("strokeDark") : QStringLiteral("strokeLight")).toString());
            INFO((dark ? "dark " : "light ") << name.toStdString());
            // Text and icons on the accent fill: 4.5:1.
            CHECK(contrast(onFill, fill) >= 4.5);
            // Strokes (focus ring, playhead, slider fill) on the surfaces: 3:1.
            CHECK(contrast(stroke, panel) >= 3.0);
            CHECK(contrast(stroke, raised) >= 3.0);
            table << QStringLiteral("%1 %2: onFill %3, stroke %4 on panel, %5 on raised")
                         .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"), name)
                         .arg(contrast(onFill, fill), 0, 'f', 2)
                         .arg(contrast(stroke, panel), 0, 'f', 2)
                         .arg(contrast(stroke, raised), 0, 'f', 2);
        }
    }
    writeOutputFile(QStringLiteral("contrast.txt"), table.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    // The live accent tokens follow the choice.
    shell.theme->setProperty("accentName", QStringLiteral("sodium"));
    CHECK(sameColor(shell.themeColor("accentOnFill"), QColor(0x00, 0x00, 0x00), 0));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Every clip label colour carries black names (solid clips, design language v3)", "[app][design][contrast]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    const QColor ink = shell.themeColor("clipInk");
    const QVariantList labels = shell.theme->property("defaultClipColors").toList();
    REQUIRE(labels.size() == 16);
    for (const QVariant& label : labels) {
        const QColor fill(label.toString());
        INFO(label.toString().toStdString() << ": " << contrast(ink, fill));
        CHECK(contrast(ink, fill) >= 4.5);
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("The bundled fonts load, and Thai vowels and tone marks stack in the UI face", "[app][design][thai]") {
    BundledFonts::registerAll();
    INFO("loaded: " << join(BundledFonts::loadedFamilies()));
    REQUIRE(BundledFonts::bundled());
    // Design language v3: Manrope for the interface and display, JetBrains Mono for numbers.
    CHECK(BundledFonts::sans() == QStringLiteral("Manrope"));
    CHECK(BundledFonts::mono() == QStringLiteral("JetBrains Mono"));
    CHECK(BundledFonts::display() == QStringLiteral("Manrope"));

    // "ที่นี่ น้ำ ผู้ใหญ่": tone marks over upper vowels (mai ek over sara ii), and over-and-under marks.
    const QString thai = QString::fromUtf16(u"ที่นี่ น้ำ "
                                            u"ผู้ใหญ่");
    for (const QString& family : {BundledFonts::sans(), BundledFonts::mono(), BundledFonts::display()}) {
        INFO(family.toStdString());
        QFont font(family);
        font.setPixelSize(26);
        QTextLayout layout(thai, font);
        layout.beginLayout();
        layout.createLine();
        layout.endLayout();
        QStringList families;
        bool tofu = false;
        bool stacked = false;
        for (const QGlyphRun& run : layout.glyphRuns()) {
            families << run.rawFont().familyName();
            const QList<quint32> glyphs = run.glyphIndexes();
            const QList<QPointF> positions = run.positions();
            for (const quint32 g : glyphs)
                tofu = tofu || g == 0;
            // ที่: the third glyph (mai ek) sits above the second (sara ii).
            if (glyphs.size() >= 3) {
                const QRectF vowel = run.rawFont().boundingRect(glyphs[1]).translated(positions[1]);
                const QRectF tone = run.rawFont().boundingRect(glyphs[2]).translated(positions[2]);
                stacked = stacked || tone.center().y() < vowel.center().y() - 1;
            }
        }
        INFO("glyph run fonts: " << join(families));
        CHECK_FALSE(tofu);
        CHECK(stacked);
        // The Latin faces hand Thai to IBM Plex Sans Thai.
        CHECK(families.contains(QStringLiteral("IBM Plex Sans Thai")));
    }

    // A specimen for the eye.
    QImage specimen(720, 200, QImage::Format_ARGB32_Premultiplied);
    specimen.fill(QColor(0x14, 0x14, 0x15));
    QPainter p(&specimen);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setPen(QColor(0xF2, 0xF2, 0xF3));
    int y = 48;
    const std::pair<QString, QFont::Weight> faces[] = {{BundledFonts::display(), QFont::ExtraBold},
                                                       {BundledFonts::sans(), QFont::Medium},
                                                       {BundledFonts::mono(), QFont::Normal}};
    for (const auto& [family, weight] : faces) {
        QFont font(family);
        font.setPixelSize(24);
        font.setWeight(weight);

        p.setFont(font);
        p.drawText(16, y, family + QStringLiteral(" · ") + thai + QStringLiteral(" · 00:01:02:03"));
        y += 56;
    }
    p.end();
    specimen.save(QDir(outputDir()).filePath(QStringLiteral("thai-type.png")));
}

TEST_CASE("The slice is a convex outline cut at 12 degrees", "[app][design][slice]") {
    // A 32 px control: the cut starts 6 px in and shears (almost) the whole right edge.
    const QList<QPointF> control = SliceShape::outline(SliceShape::Kind::Slice, QSizeF(100, 32), 6, 6, 12, false);
    REQUIRE(control.size() >= 6);
    const auto has = [](const QList<QPointF>& pts, QPointF p) {
        return std::ranges::any_of(pts, [&](const QPointF& q) { return std::hypot(q.x() - p.x(), q.y() - p.y()) < 0.05; });
    };
    CHECK(has(control, QPointF(94, 0)));
    const double drop = 6 / std::tan(12 * 3.14159265358979 / 180);
    CHECK(has(control, QPointF(100, drop)));
    // Convex and clockwise (y down): every turn has the same sign.
    for (qsizetype i = 0; i < control.size(); ++i) {
        const QPointF a = control[i];
        const QPointF b = control[(i + 1) % control.size()];
        const QPointF c = control[(i + 2) % control.size()];
        const double cross = (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x());
        CHECK(cross >= -1e-6);
    }
    // Mirrored: the cut moves to the top-left corner.
    const QList<QPointF> mirrored = SliceShape::outline(SliceShape::Kind::Slice, QSizeF(100, 32), 6, 6, 12, true);
    CHECK(has(mirrored, QPointF(6, 0)));
    CHECK(has(mirrored, QPointF(0, drop)));
    // A card: only the corner goes.
    const QList<QPointF> card = SliceShape::outline(SliceShape::Kind::Slice, QSizeF(200, 120), 8, 10, 12, false);
    CHECK(has(card, QPointF(190, 0)));
    CHECK(has(card, QPointF(200, 10 / std::tan(12 * 3.14159265358979 / 180))));
    // The slice mark: a parallelogram leaning 12°.
    const QList<QPointF> mark = SliceShape::outline(SliceShape::Kind::Lean, QSizeF(5.55, 12), 0, 0, 12, false);
    REQUIRE(mark.size() == 4);
    CHECK(std::abs(mark[0].x() - 12 * std::tan(12 * 3.14159265358979 / 180)) < 0.01);
}

TEST_CASE("Reduce motion turns off the beam glow, the cut spark and the slice wipe", "[app][design][motion][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    QObject* slice = shell.object("sliceTransition");
    QObject* view = shell.object("timelineView");
    REQUIRE(slice != nullptr);
    REQUIRE(view != nullptr);

    // With motion: a workspace switch sweeps the 12° band; the playhead glows while playing; cuts spark.
    shell.window->setProperty("workspace", 1);
    CHECK(slice->property("running").toBool());
    CHECK(slice->property("progress").toDouble() < 1.0);
    CHECK(view->property("style").toMap().value(QStringLiteral("playheadGlow")).toDouble() > 0);
    CHECK(view->property("style").toMap().value(QStringLiteral("flashDuration")).toInt() > 0);
    REQUIRE(waitFor([&] { return !slice->property("running").toBool(); }));

    // Reduce motion: a short cross-fade instead of the sweep, no glow, no spark.
    shell.theme->setProperty("reduceMotion", true);
    shell.window->setProperty("workspace", 0);
    CHECK(slice->property("running").toBool());
    CHECK(slice->property("progress").toDouble() == 1.0); // the band does not move
    CHECK(slice->property("veil").toDouble() > 0);
    CHECK(view->property("style").toMap().value(QStringLiteral("playheadGlow")).toDouble() == 0);
    CHECK(view->property("style").toMap().value(QStringLiteral("flashDuration")).toInt() == 0);
    REQUIRE(waitFor([&] { return !slice->property("running").toBool(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshot: the v2 timeline close-up (selected, disabled and offline clips; the playhead beam)",
          "[app][design][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", false); // the playhead glow is off under reduce motion
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    QQuickItem* view = shell.item("timelineView");
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(controller != nullptr);
    REQUIRE(view != nullptr);
    REQUIRE(timeline != nullptr);

    // One clip disabled, the media of another offline (relinking pending), one selected; playback running.
    ProjectState state = *controller->snapshot();
    const SequenceId seq = controller->currentSequenceId();
    const Sequence& sequence = *state.findSequence(seq);
    REQUIRE(sequence.audioTracks.size() >= 3);
    const ClipId music = sequence.audioTracks[2]->clips.front().id;
    const MediaId offlineMedia = sequence.videoTracks[2]->clips.front().media; // V3: the lower third's media
    for (MediaPtr& m : state.media) {
        if (m->id == offlineMedia) {
            MediaItem copy = *m;
            copy.locator.offline = true;
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
    edit::ClipAttributeChange disable;
    disable.enabled = false;
    auto disabled = edit::setClipAttributes(state, seq, std::vector<ClipId>{music}, disable);
    REQUIRE(disabled);
    controller->resetProject(disabled->state, seq);
    const Clip& brollClip = sequence.videoTracks[1]->clips.front();
    const ClipId broll = brollClip.id;
    controller->setSelection({broll}, broll);
    controller->setPlayhead(brollClip.start() + Ticks::fromSeconds(8)); // the beam crosses V2, A1 and A3
    view->setProperty("playing", true);
    REQUIRE_FALSE(shell.grab().isNull());

    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = timeline->mapRectToScene(QRectF(0, 0, timeline->width(), timeline->height()));
    image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
        .save(QDir(outputDir()).filePath(QStringLiteral("timeline-v2-closeup.png")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
