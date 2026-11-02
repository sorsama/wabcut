// 0.6 Effect Controls: the Text sections of graphic clips and titles (roadmap #75: Source Text with hold keys, font and
// style, size, alignment, tracking, kerning, leading, baseline shift, capitals, faux styles, fill, strokes, background,
// shadow and the layer's transform, each edit one undo step, numbers keyframable in graphic time), keyframes copied and
// pasted at the playhead (#2), and several selected clips edited at once (#15). Offscreen; screenshots of the Text
// section in the dark and light themes land in <build>/test-output/ui/.

#include "A11yAudit.h"
#include "BridgeIds.h"
#include "EffectControls.h"
#include "GraphicsBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/TitleOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QQuickItem>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |....................................................................................................|
    V1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    A1     |Cccccccccccccccccccccccccccccccccccccccccccccccccc..................................................|
)";

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

GraphicLayer textLayer(std::uint32_t id, std::string name, std::string text, double size, double x, double y) {
    GraphicLayer l;
    l.id = id;
    l.name = std::move(name);
    l.kind = LayerKind::Text;
    TextStyle style;
    style.fontSize = size;
    l.text.runs = {TextRun{std::move(text), style}};
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

/// A bar (1) behind a name (2) and a title (3).
Graphic lowerThird() {
    Graphic g;
    g.name = "Lower third";
    GraphicLayer bar;
    bar.id = 1;
    bar.name = "Bar";
    bar.kind = LayerKind::Shape;
    bar.shape.width = ParamValue(700.0);
    bar.shape.height = ParamValue(160.0);
    bar.transform.x = ParamValue(470.0);
    bar.transform.y = ParamValue(900.0);
    g.layers = {bar, textLayer(2, "Name", "Somchai Jaidee", 64, 120, 832), textLayer(3, "Title", "Producer", 40, 120, 912)};
    return g;
}

QStringList rowKeys(const EffectControlsModel& m) {
    QStringList keys;
    for (int i = 0; i < m.rowCount(); ++i)
        keys << m.keyAt(i);
    return keys;
}

/// A graphic clip on V2 (0 to 4 s, or a responsive one) selected, the playhead at 1 s.
struct Rig {
    Timeline tl{kPicture};
    ProjectController controller;
    ClipId clip;

    explicit Rig(Graphic g = lowerThird(), Ticks length = seconds(4)) {
        auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, length, std::move(g));
        REQUIRE(added);
        clip = added->placed.at(0);
        controller.resetProject(added->state, tl.seq);
        controller.selectClip(clipKey(clip));
        controller.setPlayhead(seconds(1));
    }
    EffectControlsModel& fx() { return *controller.effectControls(); }
    std::size_t undo() { return controller.session().undoCount(); }
    const GraphicLayer& layer(std::uint32_t id) {
        const auto loc = controller.currentSequence()->findClip(clip);
        REQUIRE(loc);
        const GraphicLayer* l = loc->clip().graphic()->findLayer(id);
        REQUIRE(l);
        return *l;
    }
    QVariant role(const QString& row, const char* name) { return fx().row(row).value(QString::fromLatin1(name)); }
    /// Runs `edit`, then checks it was one undo step that `check` sees, and that undo takes it back.
    void oneStep(const std::function<void()>& edit, const std::function<bool()>& check) {
        const std::size_t before = undo();
        edit();
        fx().endEdit();
        CHECK(undo() == before + 1);
        CHECK(check());
        REQUIRE(controller.undo());
        CHECK_FALSE(check());
    }
};

} // namespace

TEST_CASE("Effect Controls Text section: a graphic's text layers edit the layer one undo step each", "[app][effects][text]") {
    Rig r;
    EffectControlsModel& fx = r.fx();
    const QStringList keys = rowKeys(fx);
    // One section per text layer, the top layer first, above the clip's Transform; the shape has none.
    const auto title = keys.indexOf(QStringLiteral("h:g3"));
    const auto name = keys.indexOf(QStringLiteral("h:g2"));
    REQUIRE(title >= 0);
    REQUIRE(name > title);
    CHECK(keys.indexOf(QStringLiteral("h:motion")) > name);
    CHECK_FALSE(keys.contains(QStringLiteral("h:g1")));
    CHECK(r.role(QStringLiteral("h:g2"), "title").toString() == QStringLiteral("Text (Name)"));
    CHECK(r.role(QStringLiteral("h:g2"), "sectionKind").toString() == QStringLiteral("text"));
    for (const char* row : {"g2:text", "g2:font", "g2:fontStyle", "g2:size", "g2:align", "g2:tracking", "g2:kerning", "g2:leading",
                            "g2:baselineShift", "g2:caps", "g2:fauxBold", "g2:fauxItalic", "g2:underline", "g2:fill", "g2:strokes",
                            "g2:background", "g2:shadow", "g2:x", "g2:y", "g2:scale", "g2:rotation", "g2:opacity", "g2:anchorX"}) {
        INFO(row);
        CHECK(keys.contains(QString::fromLatin1(row)));
    }
    CHECK(r.role(QStringLiteral("g2:text"), "kind").toString() == QStringLiteral("textarea"));
    CHECK(r.role(QStringLiteral("g2:text"), "text").toString() == QStringLiteral("Somchai Jaidee"));
    CHECK(r.role(QStringLiteral("g2:font"), "kind").toString() == QStringLiteral("font"));
    CHECK(r.role(QStringLiteral("g2:size"), "value").toDouble() == 64.0);
    CHECK(r.role(QStringLiteral("g2:size"), "animatable").toBool());
    CHECK(r.role(QStringLiteral("g2:fill"), "kind").toString() == QStringLiteral("color"));
    CHECK(r.role(QStringLiteral("g2:fill"), "text").toString() == QStringLiteral("#FFFFFF"));
    CHECK_FALSE(keys.contains(QStringLiteral("g2:valign"))); // point text: no box to align in
    // A text section is not an effect: it is not selected for Copy / Remove.
    fx.selectSection(QStringLiteral("g2"), 0);
    CHECK(fx.selectedSections().isEmpty());

    const auto style = [&] { return r.layer(2).text.runs.at(0).style; };
    r.oneStep([&] { fx.setEffectText(QStringLiteral("g2:text"), QStringLiteral("Mali\nNoor")); },
              [&] { return r.layer(2).text.text() == "Mali\nNoor"; });
    CHECK(fx.row(QStringLiteral("g2:text")).value(QStringLiteral("text")).toString() == QStringLiteral("Somchai Jaidee"));
    r.oneStep([&] { fx.setEffectText(QStringLiteral("g2:font"), QStringLiteral("Arial")); },
              [&] { return style().fontFamily == "Arial"; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:size"), 80.0); }, [&] { return style().fontSize == 80.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:align"), 1.0); }, [&] { return r.layer(2).text.align == TextAlign::Center; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:tracking"), 120.0); }, [&] { return style().tracking == 120.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:kerning"), 0.0); }, [&] { return !style().kerning; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:leading"), 90.0); }, [&] { return style().leading == 90.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:baselineShift"), -12.0); }, [&] { return style().baselineShift == -12.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:caps"), 1.0); }, [&] { return style().caps == TextCaps::AllCaps; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:fauxBold"), 1.0); }, [&] { return style().fauxBold; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:fauxItalic"), 1.0); }, [&] { return style().fauxItalic; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:underline"), 1.0); }, [&] { return style().underline; });
    r.oneStep([&] { fx.setColor(QStringLiteral("g2:fill"), QStringLiteral("#FF0000")); },
              [&] { return style().fill.rgba == std::array<double, 4>{1.0, 0.0, 0.0, 1.0}; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:x"), 300.0); }, [&] { return r.layer(2).transform.x.constant() == 300.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:scale"), 50.0); }, [&] {
        return r.layer(2).transform.scaleX.constant() == 50.0 && r.layer(2).transform.scaleY.constant() == 50.0;
    });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:opacity"), 40.0); }, [&] { return r.layer(2).transform.opacity.constant() == 40.0; });

    // Strokes: add one (its rows appear), set its colour, width and position, remove it.
    r.oneStep([&] { fx.rowAction(QStringLiteral("g2:strokes"), QStringLiteral("addStroke")); },
              [&] { return r.layer(2).text.strokes.size() == 1; });
    fx.rowAction(QStringLiteral("g2:strokes"), QStringLiteral("addStroke"));
    REQUIRE(r.layer(2).text.strokes.size() == 1);
    for (const char* row : {"g2:stroke0", "g2:stroke0.width", "g2:stroke0.position"}) {
        INFO(row);
        CHECK(rowKeys(fx).contains(QString::fromLatin1(row)));
    }
    r.oneStep([&] { fx.setColor(QStringLiteral("g2:stroke0"), QStringLiteral("#00FF00")); },
              [&] { return r.layer(2).text.strokes[0].color.rgba == std::array<double, 4>{0.0, 1.0, 0.0, 1.0}; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:stroke0.width"), 9.0); },
              [&] { return r.layer(2).text.strokes[0].width.constant() == 9.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:stroke0.position"), 2.0); },
              [&] { return r.layer(2).text.strokes[0].position == StrokePosition::Inside; });
    r.oneStep([&] { fx.rowAction(QStringLiteral("g2:stroke0"), QStringLiteral("remove")); },
              [&] { return r.layer(2).text.strokes.empty(); });

    // Background and shadow: switched on, their rows appear and edit the layer.
    fx.setValue(QStringLiteral("g2:background"), 1.0);
    fx.setValue(QStringLiteral("g2:shadow"), 1.0);
    REQUIRE(r.layer(2).text.background.enabled);
    REQUIRE(r.layer(2).shadow.enabled);
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:background.padding"), 30.0); },
              [&] { return r.layer(2).text.background.padding == 30.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:background.opacity"), 25.0); },
              [&] { return std::abs(r.layer(2).text.background.color.rgba[3] - 0.25) < 1e-9; });
    r.oneStep([&] { fx.setColor(QStringLiteral("g2:shadow.color"), QStringLiteral("#0000FF")); },
              [&] { return r.layer(2).shadow.color.rgba[2] == 1.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:shadow.size"), 6.0); }, [&] { return r.layer(2).shadow.size == 6.0; });
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:shadow.softness"), 20.0); }, [&] { return r.layer(2).shadow.softness == 20.0; });

    // A paragraph box: the vertical alignment row.
    REQUIRE(r.controller.applyEditFn("Box", [&](const ProjectState& s) {
        GraphicLayer l = r.layer(2);
        l.text.boxWidth = 600.0;
        l.text.boxHeight = 300.0;
        return edit::updateGraphicLayer(s, r.tl.seq, r.clip, 2, l);
    }));
    REQUIRE(rowKeys(fx).contains(QStringLiteral("g2:valign")));
    r.oneStep([&] { fx.setValue(QStringLiteral("g2:valign"), 2.0); },
              [&] { return r.layer(2).text.verticalAlign == TextVerticalAlign::Bottom; });
}

// Fix round 1 (ARGON-33, not reproduced): a section closed on one graphic clip stays closed when another clip is
// selected and when the first is selected again (the state is the panel's for the session, by section).
TEST_CASE("Effect Controls Text section: a closed section stays closed across graphic clip selections", "[app][effects][text]") {
    Rig r;
    const ProjectState before = *r.controller.snapshot();
    auto second = edit::addGraphic(before, r.tl.seq, r.tl.track("V2"), seconds(5), seconds(2), lowerThird());
    REQUIRE(second);
    const ClipId other = second->placed.at(0);
    r.controller.resetProject(second->state, r.tl.seq);
    r.controller.selectClip(clipKey(r.clip));
    r.controller.setPlayhead(seconds(1));
    r.fx().setCollapsed(QStringLiteral("g3"), true);
    CHECK(r.role(QStringLiteral("h:g3"), "collapsed").toBool());
    CHECK_FALSE(r.role(QStringLiteral("h:g2"), "collapsed").toBool());
    // The other lower third: its Title section is closed too, its Name section open.
    r.controller.selectClip(clipKey(other));
    r.controller.setPlayhead(seconds(6));
    REQUIRE(r.fx().rowOf(QStringLiteral("h:g3")) >= 0);
    CHECK(r.role(QStringLiteral("h:g3"), "collapsed").toBool());
    CHECK_FALSE(r.role(QStringLiteral("h:g2"), "collapsed").toBool());
    // Back to the first: still as it was left.
    r.controller.selectClip(clipKey(r.clip));
    r.controller.setPlayhead(seconds(1));
    CHECK(r.role(QStringLiteral("h:g3"), "collapsed").toBool());
}

TEST_CASE("Effect Controls Text section: size keys and Source Text hold keys at the playhead", "[app][effects][text][keyframes]") {
    Rig r;
    EffectControlsModel& fx = r.fx();
    const QString size = QStringLiteral("g2:size");
    // Frame-exact times (the playhead stands on frames: 25 fps).
    r.controller.setPlayhead(f(10));
    fx.setAnimated(size, true);
    r.controller.setPlayhead(f(40));
    fx.setValue(size, 100.0);
    fx.endEdit();
    REQUIRE(r.layer(2).text.sizeKeys);
    CHECK(r.layer(2).text.sizeKeys->keys.size() == 2);
    CHECK(fx.row(size).value(QStringLiteral("keys")).toList().size() == 2);
    r.controller.setPlayhead(seconds(1.0));
    CHECK(fx.row(size).value(QStringLiteral("value")).toDouble() == Approx(82.0).margin(0.01)); // halfway 64 -> 100
    fx.goToKey(size, -1);
    CHECK(r.controller.playhead() == r.controller.currentSequence()->findClip(r.clip)->clip().start() + f(10));

    // Source Text: the stopwatch keys the text; a change at another time adds a hold key there.
    const QString text = QStringLiteral("g2:text");
    r.controller.setPlayhead(f(10));
    fx.setAnimated(text, true);
    CHECK(fx.row(text).value(QStringLiteral("animated")).toBool());
    r.controller.setPlayhead(seconds(2));
    const std::size_t before = r.undo();
    fx.setEffectText(text, QStringLiteral("Guest"));
    CHECK(r.undo() == before + 1);
    const QVariantList keys = fx.row(text).value(QStringLiteral("keys")).toList();
    REQUIRE(keys.size() == 2);
    CHECK(keys[1].toMap().value(QStringLiteral("shape")).toString() == QStringLiteral("hold"));
    CHECK(fx.row(text).value(QStringLiteral("text")).toString() == QStringLiteral("Guest"));
    r.controller.setPlayhead(seconds(1));
    CHECK(fx.row(text).value(QStringLiteral("text")).toString() == QStringLiteral("Somchai Jaidee"));
    // The style rows follow the version shown: faux bold at 1 s does not change the key at 2 s.
    fx.setValue(QStringLiteral("g2:fauxBold"), 1.0);
    const TextContent& t = r.layer(2).text;
    CHECK(sourceRunsAt(t, seconds(1)).at(0).style.fauxBold);
    CHECK_FALSE(sourceRunsAt(t, seconds(2)).at(0).style.fauxBold);
    // Selected Source Text keys delete like others: no key left, the text at the playhead stays.
    fx.selectKeysInRange({text}, 0.0, 1.0, 0);
    r.controller.setPlayhead(seconds(2.5));
    fx.deleteSelectedKeys();
    CHECK(r.layer(2).text.sourceKey.isConstant());
    CHECK(r.layer(2).text.text() == "Guest");
}

TEST_CASE("Essential Graphics edits the same text layer: faux styles and capitals and strokes and keyed size", "[app][graphics][text]") {
    Rig r;
    QTemporaryDir library;
    REQUIRE(library.isValid());
    GraphicsBridge graphics;
    graphics.setUserLibrary(std::filesystem::path(library.path().toStdU16String()));
    graphics.setController(&r.controller);
    graphics.setSelectedLayer(2);
    REQUIRE(graphics.layer().value(QStringLiteral("id")).toInt() == 2);
    const auto style = [&] { return r.layer(2).text.runs.at(0).style; };
    r.oneStep([&] { CHECK(graphics.setRun(2, 0, QStringLiteral("fauxItalic"), true).isEmpty()); graphics.endEdit(); },
              [&] { return style().fauxItalic; });
    r.oneStep([&] { CHECK(graphics.setRun(2, 0, QStringLiteral("caps"), QStringLiteral("smallCaps")).isEmpty()); graphics.endEdit(); },
              [&] { return style().caps == TextCaps::SmallCaps; });
    r.oneStep([&] { CHECK(graphics.setTextStroke(2, -1, QStringLiteral("add")).isEmpty()); },
              [&] { return r.layer(2).text.strokes.size() == 1; });
    REQUIRE(graphics.setTextStroke(2, -1, QStringLiteral("add")).isEmpty());
    const QVariantList strokes = graphics.layer().value(QStringLiteral("text")).toMap().value(QStringLiteral("strokes")).toList();
    REQUIRE(strokes.size() == 1);
    CHECK(strokes[0].toMap().value(QStringLiteral("position")).toString() == QStringLiteral("outside"));
    r.oneStep([&] { CHECK(graphics.setTextStroke(2, 0, QStringLiteral("position"), QStringLiteral("center")).isEmpty()); },
              [&] { return r.layer(2).text.strokes[0].position == StrokePosition::Center; });
    r.oneStep([&] { CHECK(graphics.setColor(2, QStringLiteral("textStroke:0"), QStringLiteral("#FF00FF")).isEmpty()); graphics.endEdit(); },
              [&] { return r.layer(2).text.strokes[0].color.rgba[0] == 1.0; });
    r.oneStep([&] { CHECK(graphics.setValue(2, QStringLiteral("text.stroke0.width"), 11.0).isEmpty()); graphics.endEdit(); },
              [&] { return r.layer(2).text.strokes[0].width.constant() == 11.0; });
    r.oneStep([&] { CHECK(graphics.setTextStroke(2, 0, QStringLiteral("remove")).isEmpty()); },
              [&] { return r.layer(2).text.strokes.empty(); });
    r.oneStep([&] { CHECK(graphics.updateLayer(2, {{QStringLiteral("shadow"), QVariantMap{{QStringLiteral("size"), 4.0}}}}).isEmpty()); },
              [&] { return r.layer(2).shadow.size == 4.0; });

    // A size keyed in Effect Controls: the panel shows it at the playhead and its size field sets the key there.
    EffectControlsModel& fx = r.fx();
    fx.setAnimated(QStringLiteral("g2:size"), true);
    REQUIRE(r.layer(2).text.sizeKeys);
    CHECK(graphics.layer().value(QStringLiteral("text")).toMap().value(QStringLiteral("sizeKeyed")).toBool());
    r.controller.setPlayhead(seconds(2));
    CHECK(graphics.setRun(2, 0, QStringLiteral("fontSize"), 90.0).isEmpty());
    graphics.endEdit();
    REQUIRE(r.layer(2).text.sizeKeys->keys.size() == 2);
    CHECK(fx.row(QStringLiteral("g2:size")).value(QStringLiteral("value")).toDouble() == 90.0);
    const QVariantList runs = graphics.layer().value(QStringLiteral("text")).toMap().value(QStringLiteral("runs")).toList();
    CHECK(runs.at(0).toMap().value(QStringLiteral("fontSize")).toDouble() == 90.0);
    // With Source Text keys its text box edits the text shown at the playhead.
    fx.setAnimated(QStringLiteral("g2:text"), true);
    r.controller.setPlayhead(seconds(3));
    CHECK(graphics.setText(2, QStringLiteral("Later")).isEmpty());
    graphics.endEdit();
    CHECK(sourceRunsAt(r.layer(2).text, seconds(3)).at(0).text == "Later");
    CHECK(sourceRunsAt(r.layer(2).text, seconds(2)).at(0).text == "Somchai Jaidee");
}

TEST_CASE("Effect Controls Text section: keys of a graphic with responsive time sit at their timeline place", "[app][effects][text][keyframes]") {
    Graphic g = lowerThird();
    g.responsive = ResponsiveTime{seconds(4), seconds(1), seconds(1)};
    Rig r(g, seconds(6)); // designed 4 s long, played 6 s: the outro is the clip's last second
    EffectControlsModel& fx = r.fx();
    const QString opacity = QStringLiteral("g2:opacity");
    r.controller.setPlayhead(f(140)); // design time 3.6 s
    fx.setAnimated(opacity, true);
    const ParamValue v = r.layer(2).transform.opacity;
    REQUIRE(v.curve());
    CHECK(v.curve()->keys.at(0).time == f(90));
    const QVariantList keys = fx.row(opacity).value(QStringLiteral("keys")).toList();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].toMap().value(QStringLiteral("x")).toDouble() == Approx(5.6 / 6.0).margin(0.002));
    // A new value at 0.4 s (the intro) keys design time 0.4 s.
    r.controller.setPlayhead(f(10));
    fx.setValue(opacity, 20.0);
    fx.endEdit();
    REQUIRE(r.layer(2).transform.opacity.curve()->keys.size() == 2);
    CHECK(r.layer(2).transform.opacity.curve()->keys.at(0).time == f(10));
}

TEST_CASE("Effect Controls Text section: a title shows its text and font and colours as rows", "[app][effects][text][titles]") {
    Timeline tl{kPicture};
    auto added = edit::addTitle(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(3), "Hello");
    REQUIRE(added);
    const ClipId clip = added->placed.at(0);
    ProjectController controller;
    controller.resetProject(added->state, tl.seq);
    controller.selectClip(clipKey(clip));
    EffectControlsModel& fx = *controller.effectControls();
    const Clip& c = controller.currentSequence()->findClip(clip)->clip();
    const QString section = effectKey(c.effects().front().id);
    CHECK(fx.row(QStringLiteral("h:") + section).value(QStringLiteral("title")).toString().startsWith(QStringLiteral("Text (")));
    CHECK_FALSE(fx.row(QStringLiteral("h:") + section).value(QStringLiteral("removable")).toBool());
    const QString text = section + QStringLiteral(":titleText");
    CHECK(fx.row(text).value(QStringLiteral("kind")).toString() == QStringLiteral("textarea"));
    CHECK(fx.row(text).value(QStringLiteral("text")).toString() == QStringLiteral("Hello"));
    CHECK(fx.row(section + QStringLiteral(":titleFont")).value(QStringLiteral("kind")).toString() == QStringLiteral("font"));
    CHECK(fx.row(section + QStringLiteral(":alignment")).value(QStringLiteral("kind")).toString() == QStringLiteral("choice"));
    CHECK(fx.rowOf(section + QStringLiteral(":fill.red")) < 0); // one colour row instead
    const QString fill = section + QStringLiteral(":color:fill");
    REQUIRE(fx.rowOf(fill) >= 0);

    const MediaId media = c.media;
    const std::size_t before = controller.session().undoCount();
    fx.setEffectText(text, QStringLiteral("Hello\nworld"));
    CHECK(controller.snapshot()->findMedia(media)->generator->text == "Hello\nworld");
    fx.setColor(fill, QStringLiteral("#00FF00"));
    fx.endEdit();
    const auto red = [&] {
        return effectParameterAt(controller.currentSequence()->findClip(clip)->clip().effects().front(), "fill.red", Ticks{});
    };
    CHECK(red() == 0.0);
    CHECK(fx.row(fill).value(QStringLiteral("text")).toString() == QStringLiteral("#00FF00"));
    fx.setEffectText(section + QStringLiteral(":titleFont"), QStringLiteral("Georgia"));
    CHECK(controller.snapshot()->findMedia(media)->generator->fontFamily == "Georgia");
    CHECK(controller.session().undoCount() == before + 3);
    REQUIRE(controller.undo());
    REQUIRE(controller.undo());
    CHECK(red() == 1.0);
    REQUIRE(controller.undo());
    CHECK(controller.snapshot()->findMedia(media)->generator->text == "Hello");
}

TEST_CASE("Keyframe clipboard: selected keys copy and paste at the playhead onto another clip", "[app][keyframes][clipboard]") {
    Timeline tl{kPicture};
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    EffectControlsModel& fx = *controller.effectControls();
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@50");
    const auto curveOf = [&](ClipId id) { return controller.currentSequence()->findClip(id)->clip().properties()[ParamId::Scale]; };
    controller.selectClip(clipKey(a));
    const QString scale = QStringLiteral("p:scale");
    controller.setPlayhead(f(5));
    fx.setAnimated(scale, true);
    controller.setPlayhead(f(15));
    fx.setValue(scale, 150.0);
    fx.endEdit();
    REQUIRE(curveOf(a).curve());
    REQUIRE(curveOf(a).curve()->keys.size() == 2);

    // Ctrl+C with keys selected copies them (not the clip).
    fx.selectKeysInRange({scale}, 0.0, 1.0, 0);
    REQUIRE(fx.selectedKeyCount() == 2);
    CHECK_FALSE(fx.canPasteKeys());
    CHECK(fx.runCommand(QStringLiteral("edit.copy")));
    CHECK(fx.canPasteKeys());

    // Ctrl+V on clip B at 2 frames into it: the keys keep their spacing, one undo step.
    controller.selectClip(clipKey(b));
    const Ticks bStart = controller.currentSequence()->findClip(b)->clip().start();
    controller.setPlayhead(bStart + f(2));
    const std::size_t before = controller.session().undoCount();
    CHECK(fx.runCommand(QStringLiteral("edit.paste")));
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(controller.undoText() == QStringLiteral("Undo Paste Keyframes"));
    const ParamValue pasted = curveOf(b);
    REQUIRE(pasted.curve());
    REQUIRE(pasted.curve()->keys.size() == 2);
    const Clip& bClip = controller.currentSequence()->findClip(b)->clip();
    CHECK(pasted.curve()->keys[0].time == bClip.curveTimeAt(bStart + f(2)));
    CHECK(pasted.curve()->keys[1].time == bClip.curveTimeAt(bStart + f(12)));
    CHECK(pasted.curve()->keys[1].value == Approx(1.5));
    // Pasting again at the same place replaces the keys there (no duplicates).
    fx.pasteKeys();
    CHECK(curveOf(b).curve()->keys.size() == 2);
    REQUIRE(controller.undo());
    REQUIRE(controller.undo());
    CHECK(curveOf(b).isConstant());

    // Cut: the keys go (one undo step "Cut Keyframes") and wait to be pasted.
    controller.selectClip(clipKey(a));
    fx.selectKeysInRange({scale}, 0.0, 1.0, 0);
    CHECK(fx.runCommand(QStringLiteral("edit.cut")));
    CHECK(curveOf(a).isConstant());
    CHECK(controller.undoText() == QStringLiteral("Undo Cut Keyframes"));
    CHECK(fx.canPasteKeys());
    // A clip copied after them is the newest copy: Ctrl+V pastes clips again.
    controller.copySelection();
    CHECK_FALSE(fx.canPasteKeys());
    CHECK_FALSE(fx.runCommand(QStringLiteral("edit.paste")));

    // An effect's keys go to the same effect on the other clip; a clip without it gets a notice.
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    const QString radius = effectKey(controller.currentSequence()->findClip(a)->clip().effects().back().id) + QStringLiteral(":radius");
    controller.setPlayhead(f(5));
    fx.setAnimated(radius, true);
    fx.selectKeysInRange({radius}, 0.0, 1.0, 0);
    REQUIRE(fx.copySelectedKeys() == 1);
    controller.selectClip(clipKey(b));
    controller.setPlayhead(bStart + f(1));
    CHECK_FALSE(fx.pasteKeys().isEmpty()); // B has no Gaussian Blur
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    CHECK(fx.pasteKeys().isEmpty());
    CHECK(controller.currentSequence()->findClip(b)->clip().effects().back().find("radius")->isCurve());
}

TEST_CASE("Effect Controls with several clips: shared rows set every clip of their kind in one undo step", "[app][effects][multi]") {
    Timeline tl{kPicture};
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    EffectControlsModel& fx = *controller.effectControls();
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@50");
    const ClipId c = tl.clip("A1@0");
    const auto scaleOf = [&](ClipId id) { return controller.currentSequence()->findClip(id)->clip().properties()[ParamId::Scale]; };
    // B starts at 150 %; A is animated (a key at 0).
    controller.selectClip(clipKey(b));
    fx.setValue(QStringLiteral("p:scale"), 150.0);
    fx.endEdit();
    controller.selectClip(clipKey(a));
    controller.setPlayhead(f(0));
    fx.setAnimated(QStringLiteral("p:scale"), true);

    controller.selectClip(clipKey(b), ProjectController::Add);
    controller.selectClip(clipKey(c), ProjectController::Add);
    CHECK(fx.mode() == QStringLiteral("multi"));
    CHECK(fx.multiCount() == 3);
    CHECK(fx.title() == QStringLiteral("3 clips"));
    const QStringList keys = rowKeys(fx);
    CHECK(keys.contains(QStringLiteral("m:scale")));
    CHECK(keys.contains(QStringLiteral("m:opacity")));
    CHECK(keys.contains(QStringLiteral("m:volume")));
    CHECK_FALSE(keys.contains(QStringLiteral("p:scale")));
    CHECK(fx.row(QStringLiteral("m:scale")).value(QStringLiteral("mixed")).toBool()); // 100 % and 150 %
    CHECK_FALSE(fx.row(QStringLiteral("m:opacity")).value(QStringLiteral("mixed")).toBool());
    CHECK_FALSE(fx.row(QStringLiteral("m:scale")).value(QStringLiteral("animatable")).toBool()); // no stopwatch

    // A drag sets every video clip (the animated one gets a key at the playhead): one undo step.
    controller.setPlayhead(f(10));
    const std::size_t before = controller.session().undoCount();
    fx.setValue(QStringLiteral("m:scale"), 60.0);
    fx.setValue(QStringLiteral("m:scale"), 50.0);
    fx.endEdit();
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(scaleOf(b).constant() == Approx(0.5));
    REQUIRE(scaleOf(a).curve());
    CHECK(scaleOf(a).curve()->keys.size() == 2);
    CHECK(fx.notice() == QStringLiteral("Scale set on 2 clips (1 audio skipped)"));
    CHECK_FALSE(fx.row(QStringLiteral("m:scale")).value(QStringLiteral("mixed")).toBool());
    REQUIRE(controller.undo());
    CHECK(scaleOf(b).constant() == Approx(1.5));
    CHECK(scaleOf(a).curve()->keys.size() == 1);

    // One clip again: the usual rows.
    controller.selectClip(clipKey(a));
    CHECK(fx.mode() == QStringLiteral("clip"));
    CHECK(fx.multiCount() == 0);
}

// ---- Screenshots -----------------------------------------------------------------------------------------------------

namespace {

QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, match))
            return found;
    }
    return nullptr;
}

void textSectionShot(bool dark) {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, 1280);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl{kPicture};
    Graphic g = lowerThird();
    g.layers[1].text.strokes = {TextStroke{}};
    g.layers[1].text.background.enabled = true;
    g.layers[1].shadow.enabled = true;
    auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(4), g);
    REQUIRE(added);
    controller->resetProject(added->state, tl.seq);
    controller->selectClip(clipKey(added->placed.at(0)));
    controller->setPlayhead(seconds(1));
    EffectControlsModel& fx = *controller->effectControls();
    fx.setCollapsed(QStringLiteral("g3"), true); // the Name section open, the Title closed
    fx.setAnimated(QStringLiteral("g2:size"), true);
    if (!dark) {
        shell.setAppearance(false, "ultraviolet");
        REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    }
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    spin(300);
    // The Name section from its header (Source Text, font, size, alignment...).
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:g2"))), Q_ARG(int, 0)));
    spin(150);
    // The Source Text box and a colour row are on screen and named.
    QQuickItem* root = shell.window->contentItem();
    CHECK(findItem(root, [](QQuickItem* i) { return i->objectName() == QLatin1String("sourceTextField") && i->isVisible(); }));
    CHECK(findItem(root, [](QQuickItem* i) { return i->objectName() == QLatin1String("colorSwatch") && i->isVisible(); }));
    QQuickItem* panel = shell.item("effectControls");
    REQUIRE(panel != nullptr);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    REQUIRE(image.copy(QRectF(rect.topLeft() * dpr, rect.size() * dpr).toRect())
                .save(outputDir() + (dark ? QStringLiteral("/effect-controls-text-dark.png") : QStringLiteral("/effect-controls-text-light.png"))));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("Effect Controls Text section screenshot in the dark theme", "[app][screenshots][text]") {
    textSectionShot(true);
}

TEST_CASE("Effect Controls Text section screenshot in the light theme", "[app][screenshots][text]") {
    textSectionShot(false);
}

TEST_CASE("Effect Controls with several clips screenshot", "[app][screenshots][multi]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, 1280);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl{kPicture};
    controller->resetProject(tl.state, tl.seq);
    EffectControlsModel& fx = *controller->effectControls();
    controller->selectClip(clipKey(tl.clip("V1@50")));
    fx.setValue(QStringLiteral("p:scale"), 150.0); // the clips differ: Scale shows "—"
    fx.endEdit();
    controller->selectClip(clipKey(tl.clip("V1@0")));
    controller->selectClip(clipKey(tl.clip("V1@50")), ProjectController::Add);
    controller->selectClip(clipKey(tl.clip("A1@0")), ProjectController::Add);
    REQUIRE(fx.mode() == QStringLiteral("multi"));
    fx.setValue(QStringLiteral("m:opacity"), 80.0);
    fx.endEdit();
    spin(300);
    QQuickItem* panel = shell.item("effectControls");
    REQUIRE(panel != nullptr);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    // Scale differs between the clips: "—" (after the grab: the rows are laid out).
    CHECK(findItem(shell.window->contentItem(),
                   [](QQuickItem* i) { return i->objectName() == QLatin1String("mixedValue") && i->isVisible(); }));
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    REQUIRE(image.copy(QRectF(rect.topLeft() * dpr, rect.size() * dpr).toRect())
                .save(outputDir() + QStringLiteral("/effect-controls-multi.png")));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Keyframe clipboard: Source Text keys paste their own text onto another graphic clip (fix F1A)",
          "[app][keyframes][clipboard][text]") {
    // ARGON-09: a Source Text key's value is an index into its layer's text versions; pasted elsewhere (or after a cut
    // dropped those versions) it showed the target's text at that index instead of the copied text.
    Timeline tl{kPicture};
    auto one = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), lowerThird());
    REQUIRE(one);
    Graphic otherGraphic = lowerThird();
    otherGraphic.layers[1].text.runs.front().text = "Another name";
    auto two = edit::addGraphic(one->state, tl.seq, tl.track("V2"), seconds(3), seconds(2), otherGraphic);
    REQUIRE(two);
    const ClipId a = one->placed.at(0);
    const ClipId b = two->placed.at(0);
    ProjectController controller;
    controller.resetProject(two->state, tl.seq);
    EffectControlsModel& fx = *controller.effectControls();
    const QString text = QStringLiteral("g2:text");
    const auto shownAt = [&](Ticks t) {
        controller.setPlayhead(t);
        return fx.row(text).value(QStringLiteral("text")).toString();
    };

    // A: "Somchai Jaidee" from frame 10, "Guest" from 1 s (25 fps).
    controller.selectClip(clipKey(a));
    controller.setPlayhead(f(10));
    fx.setAnimated(text, true);
    controller.setPlayhead(seconds(1));
    fx.setEffectText(text, QStringLiteral("Guest"));
    fx.selectKeysInRange({text}, 0.0, 1.0, 0);
    REQUIRE(fx.selectedKeyCount() == 2);
    REQUIRE(fx.copySelectedKeys() == 2);

    // Pasted on B at its start: B shows the copied texts at the copied spacing (0.6 s apart), its own text gone.
    controller.selectClip(clipKey(b));
    controller.setPlayhead(seconds(3));
    const std::size_t before = controller.session().undoCount();
    REQUIRE(fx.pasteKeys().isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(shownAt(seconds(3.2)) == QStringLiteral("Somchai Jaidee"));
    CHECK(shownAt(seconds(3.6)) == QStringLiteral("Guest"));
    CHECK(shownAt(seconds(4.5)) == QStringLiteral("Guest"));
    // Undo: B as it was.
    REQUIRE(controller.undo());
    CHECK(shownAt(seconds(3.6)) == QStringLiteral("Another name"));

    // Cut from A (its versions go with the keys), pasted back on A later: still the right texts.
    controller.selectClip(clipKey(a));
    fx.selectKeysInRange({text}, 0.0, 1.0, 0);
    REQUIRE(fx.copySelectedKeys(true) == 2);
    controller.setPlayhead(seconds(0.2));
    REQUIRE(fx.pasteKeys().isEmpty());
    CHECK(shownAt(seconds(0.4)) == QStringLiteral("Somchai Jaidee"));
    CHECK(shownAt(seconds(0.8)) == QStringLiteral("Guest"));
}
