// M5o: the brand kit editor (C.13 brand.*: Brand = BrandBridge, BrandKitDialog), brand colours and font roles in the
// caption style (CaptionColorRow, CaptionsBridge's brand* keys), and Lottie import (GraphicsImport = GraphicsImportBridge:
// lottie.import as a graphic clip, its report in ImportReportDialog). Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BrandBridge.h"
#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "CommandJob.h"
#include "GraphicsImportBridge.h"
#include "ProjectController.h"

#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/GraphicResolve.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    V2     |..................................................|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

QString fixture(const char* relative) {
    return QDir(QStringLiteral(WABCUT_INTERCHANGE_FIXTURES)).filePath(QString::fromLatin1(relative));
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

std::unique_ptr<QObject> makeQml(Shell& shell, const QByteArray& qml) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/m5o-brand/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    return object;
}

std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml, const QRectF& rect) {
    std::unique_ptr<QObject> object = makeQml(shell, qml);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setPosition(rect.topLeft());
    item->setSize(rect.size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

QQuickItem* popupItem(QObject* popup) {
    auto* content = popup->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    return content->parentItem() ? content->parentItem() : content;
}

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name, double margin = 16) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                         .adjusted(-margin, -margin, margin, margin)
                         .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

QVariantMap kitMap(const QString& name, const QString& primary, const QString& accent, const QString& heading) {
    return {{QStringLiteral("name"), name},
            {QStringLiteral("colors"),
             QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Primary")}, {QStringLiteral("hex"), primary}},
                          QVariantMap{{QStringLiteral("name"), QStringLiteral("Accent")}, {QStringLiteral("hex"), accent}}}},
            {QStringLiteral("fonts"),
             QVariantList{QVariantMap{{QStringLiteral("role"), QStringLiteral("heading")},
                                      {QStringLiteral("family"), heading},
                                      {QStringLiteral("weight"), 700}},
                          QVariantMap{{QStringLiteral("role"), QStringLiteral("body")},
                                      {QStringLiteral("family"), QStringLiteral("Arial")},
                                      {QStringLiteral("weight"), 400}}}},
            {QStringLiteral("logos"), QVariantList{}}};
}

struct Rig {
    Timeline tl{kPicture};
    ProjectController controller;
    BrandBridge brand;

    Rig() {
        controller.resetProject(tl.state, tl.seq);
        brand.setController(&controller);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    ProjectSnapshot snap() const { return controller.snapshot(); }
};

std::size_t clipCount(const ProjectState& state, SequenceId seq) {
    std::size_t n = 0;
    const Sequence* s = state.findSequence(seq);
    for (const TrackPtr& t : s->videoTracks)
        n += t->clips.size();
    for (const TrackPtr& t : s->audioTracks)
        n += t->clips.size();
    return n;
}

/// The fill of the first shape a graphic clip draws at its start.
Rgba shapeFill(const ProjectState& state, SequenceId seq, ClipId clip) {
    const auto loc = state.findSequence(seq)->findClip(clip);
    REQUIRE(loc);
    const DrawList list = resolveGraphicClip(state, loc->clip(), loc->clip().start());
    for (const DrawItem& item : list.items) {
        if (item.kind == DrawKind::Shape)
            return item.fill.color;
    }
    FAIL("the graphic draws no shape");
    return {};
}

} // namespace

TEST_CASE("Brand kit: create, colours, font roles and logos, one undo step per edit; a kit file round trip",
          "[app][graphics][brand]") {
    Rig r;
    CHECK_FALSE(r.brand.hasKit());
    CHECK(r.brand.kit().isEmpty());
    // Edits need a kit.
    CHECK_FALSE(r.brand.addColor(QStringLiteral("Red"), QStringLiteral("#FF0000")).isEmpty());

    std::size_t before = r.undo();
    REQUIRE(r.brand.createKit(QStringLiteral("Studio")).isEmpty());
    CHECK(r.undo() == before + 1);
    REQUIRE(r.brand.hasKit());
    CHECK(r.brand.kit().value(QStringLiteral("name")).toString() == QStringLiteral("Studio"));
    CHECK(r.brand.colors().size() == 3);
    CHECK(r.brand.fontRoles().size() == 2);
    CHECK_FALSE(r.brand.colorHex(QStringLiteral("Primary")).isEmpty());

    // Colours: add (a unique name), recolour, rename (refused when taken), remove: one undo step each.
    before = r.undo();
    REQUIRE(r.brand.addColor(QStringLiteral("Brand Red"), QStringLiteral("#E11D48")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.brand.colorHex(QStringLiteral("Brand Red")) == QStringLiteral("#E11D48"));
    REQUIRE(r.brand.addColor(QStringLiteral("Brand Red"), QStringLiteral("#E11D48")).isEmpty());
    CHECK(r.brand.colorHex(QStringLiteral("Brand Red 2")) == QStringLiteral("#E11D48"));
    REQUIRE(r.brand.setColor(0, {}, QStringLiteral("#112233")).isEmpty());
    CHECK(r.brand.colorHex(QStringLiteral("Primary")) == QStringLiteral("#112233"));
    CHECK_FALSE(r.brand.setColor(1, QStringLiteral("Primary"), {}).isEmpty()); // taken
    CHECK_FALSE(r.brand.setColor(0, {}, QStringLiteral("not a colour")).isEmpty());
    // Translucent colours keep their alpha (#RRGGBBAA), and QML gets a colour value.
    REQUIRE(r.brand.setColor(2, {}, QStringLiteral("#FFD13F80")).isEmpty());
    CHECK(r.brand.colorHex(QStringLiteral("Accent")) == QStringLiteral("#FFD13F80"));
    const QVariantMap accent = r.brand.colors().at(2).toMap();
    CHECK(accent.value(QStringLiteral("color")).value<QColor>().alpha() == 128);
    before = r.undo();
    REQUIRE(r.brand.removeColor(4).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.brand.colorHex(QStringLiteral("Brand Red 2")).isEmpty());
    // No change: no undo step.
    before = r.undo();
    REQUIRE(r.brand.setColor(0, {}, QStringLiteral("#112233")).isEmpty());
    CHECK(r.undo() == before);

    // Font roles.
    REQUIRE(r.brand.setFont(0, {}, QStringLiteral("Georgia"), {}, 600).isEmpty());
    CHECK(r.brand.fontFamilyOf(QStringLiteral("heading")) == QStringLiteral("Georgia"));
    CHECK(r.snap()->brandKit->font("heading")->weight == 600);
    REQUIRE(r.brand.addFont(QStringLiteral("caption")).isEmpty());
    CHECK(r.brand.fontRoles().size() == 3);
    CHECK_FALSE(r.brand.setFont(2, QStringLiteral("body"), {}, {}, 0).isEmpty()); // taken
    REQUIRE(r.brand.removeFont(2).isEmpty());
    CHECK_FALSE(r.brand.fontFamilies().isEmpty());

    // Logos: pictures of the project.
    const QVariantList candidates = r.brand.logoCandidates();
    REQUIRE_FALSE(candidates.isEmpty());
    const QString media = candidates.front().toMap().value(QStringLiteral("key")).toString();
    REQUIRE(r.brand.addLogo(QStringLiteral("Wordmark"), media).isEmpty());
    const QVariantMap logo = r.brand.kit().value(QStringLiteral("logos")).toList().front().toMap();
    CHECK(logo.value(QStringLiteral("name")).toString() == QStringLiteral("Wordmark"));
    CHECK(logo.value(QStringLiteral("media")).toString() == media);
    CHECK(logo.value(QStringLiteral("mediaName")).toString() == QStringLiteral("A"));
    CHECK_FALSE(r.brand.addLogo(QStringLiteral("Bad"), QStringLiteral("nope")).isEmpty());

    // Undo takes the edits back one at a time.
    const BrandKit withLogo = *r.snap()->brandKit;
    REQUIRE(r.controller.undo());
    CHECK(r.snap()->brandKit->logos.empty());
    REQUIRE(r.controller.redo());
    CHECK(*r.snap()->brandKit == withLogo);

    // A kit file: export, remove the kit, import it again; the logo is found by its media's name.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl file = QUrl::fromLocalFile(QDir(dir.path()).filePath(QStringLiteral("studio.wabcut-brand.json")));
    REQUIRE(r.brand.exportKit(file).isEmpty());
    REQUIRE(r.brand.removeKit().isEmpty());
    CHECK_FALSE(r.brand.hasKit());
    REQUIRE(r.brand.importKit(file).isEmpty());
    REQUIRE(r.brand.hasKit());
    CHECK(*r.snap()->brandKit == withLogo);
    // Not a kit.
    QFile other(QDir(dir.path()).filePath(QStringLiteral("other.json")));
    REQUIRE(other.open(QIODevice::WriteOnly));
    other.write(R"({"format": "something-else"})");
    other.close();
    CHECK_FALSE(r.brand.importKit(QUrl::fromLocalFile(other.fileName())).isEmpty());

    // A whole kit at once, checked.
    CHECK_FALSE(r.brand.setKit(kitMap(QStringLiteral("Bad"), QStringLiteral("zzz"), QStringLiteral("#FFFFFF"),
                                      QStringLiteral("Arial")))
                    .isEmpty());
    before = r.undo();
    REQUIRE(r.brand.setKit(kitMap(QStringLiteral("Night"), QStringLiteral("#101010"), QStringLiteral("#FFD13F"),
                                  QStringLiteral("Georgia")))
                .isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.brand.kit().value(QStringLiteral("name")).toString() == QStringLiteral("Night"));
}

TEST_CASE("Brand kit swap re-themes a graphic and a caption style that use the kit's names", "[app][graphics][brand]") {
    Rig r;
    REQUIRE(r.brand.setKit(kitMap(QStringLiteral("Day"), QStringLiteral("#FF0000"), QStringLiteral("#FFFFFF"),
                                  QStringLiteral("Arial")))
                .isEmpty());

    // A lower third whose box is the kit's Primary.
    const Json graphic{{"layers", Json::array({Json{{"id", 1},
                                                    {"kind", "shape"},
                                                    {"name", "Box"},
                                                    {"shape", Json{{"type", "rectangle"},
                                                                   {"width", 600},
                                                                   {"height", 120},
                                                                   {"fill", Json{{"color", Json{{"brand", "Primary"},
                                                                                                {"rgba", Json::array({0, 0, 0, 1})}}}}}}},
                                                    {"transform", Json{{"x", 500}, {"y", 900}}}}})}};
    const command::CommandResult added =
        CommandJob::runNow(&r.controller, nullptr, "graphic.add", Json{{"at", 0}, {"track", "V2"}, {"graphic", graphic}});
    INFO(commandErrorText(added).toStdString());
    REQUIRE(added.ok());
    const ClipId clip{added.data["clip"].get<std::uint64_t>()};
    const Rgba day = shapeFill(*r.snap(), r.tl.seq, clip);
    CHECK(day[0] == 1.0);
    CHECK(day[2] == 0.0);

    // A caption track whose fill follows Primary and whose font follows the heading role.
    CaptionsBridge captions;
    captions.setController(&r.controller);
    REQUIRE(captions.addTrack(QStringLiteral("English"), {}).isEmpty());
    REQUIRE(captions.setStyle(QStringLiteral("brandFill"), QStringLiteral("Primary")).isEmpty());
    REQUIRE(captions.setStyle(QStringLiteral("brandFont"), QStringLiteral("heading")).isEmpty());
    CHECK(captions.style().value(QStringLiteral("brandFill")).toString() == QStringLiteral("Primary"));
    CHECK(captions.style().value(QStringLiteral("fill")).toString().toUpper() == QStringLiteral("#FF0000"));
    CHECK(captions.style().value(QStringLiteral("fontFamily")).toString() == QStringLiteral("Arial"));

    // Swap the kit: one undo step re-themes both.
    const std::size_t before = r.undo();
    REQUIRE(r.brand.setKit(kitMap(QStringLiteral("Night"), QStringLiteral("#0000FF"), QStringLiteral("#FFD13F"),
                                  QStringLiteral("Georgia")))
                .isEmpty());
    CHECK(r.undo() == before + 1);
    const Rgba night = shapeFill(*r.snap(), r.tl.seq, clip);
    CHECK(night[0] == 0.0);
    CHECK(night[2] == 1.0);
    CHECK(captions.style().value(QStringLiteral("fill")).toString().toUpper() == QStringLiteral("#0000FF"));
    CHECK(captions.style().value(QStringLiteral("fontFamily")).toString() == QStringLiteral("Georgia"));
    const CaptionTrack& track = r.snap()->findSequence(r.tl.seq)->captionTracks.front();
    CHECK(resolvedCaptionStyle(track.style, &*r.snap()->brandKit).fill == CaptionRgb{0.0, 0.0, 1.0});

    // A colour of its own unlinks the brand colour; a family of its own unlinks the font role.
    REQUIRE(captions.setStyle(QStringLiteral("fill"), QStringLiteral("#00FF00")).isEmpty());
    CHECK(captions.style().value(QStringLiteral("brandFill")).toString().isEmpty());
    CHECK(captions.style().value(QStringLiteral("fill")).toString().toUpper() == QStringLiteral("#00FF00"));
    REQUIRE(captions.setStyle(QStringLiteral("fontFamily"), QStringLiteral("Arial")).isEmpty());
    CHECK(captions.style().value(QStringLiteral("brandFont")).toString().isEmpty());

    // Undo the swap: Day again.
    REQUIRE(r.controller.undo());
    REQUIRE(r.controller.undo());
    REQUIRE(r.controller.undo());
    CHECK(shapeFill(*r.snap(), r.tl.seq, clip)[0] == 1.0);
}

TEST_CASE("Caption colour rows: the brand kit's colours come first, by name; picking one links the colour",
          "[app][graphics][brand][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    auto* brand = shell.engine.singletonInstance<BrandBridge*>("Wabcut", "Brand");
    REQUIRE(brand);
    REQUIRE(brand->setKit(kitMap(QStringLiteral("Day"), QStringLiteral("#FF0000"), QStringLiteral("#FFD13F"),
                                 QStringLiteral("Arial")))
                .isEmpty());

    // The host keeps what the row asked for (the panel would write it with Captions.setStyle).
    const auto host = makeItem(shell,
                               "import QtQuick\nimport Wabcut\nItem {\n    property string picked: \"\"\n"
                               "    property alias row: colorRow\n"
                               "    CaptionColorRow { id: colorRow; width: 360; key: \"fill\"; label: \"Fill\"; value: \"#FFFFFF\"\n"
                               "        onPickedBrand: name => parent.picked = name }\n}\n",
                               QRectF(0, 0, 360, 120));
    auto* row = host->property("row").value<QQuickItem*>();
    REQUIRE(row);
    QQuickItem* primary = findNamed(row, QStringLiteral("captionBrandColor_fill_Primary"));
    QQuickItem* accent = findNamed(row, QStringLiteral("captionBrandColor_fill_Accent"));
    REQUIRE(primary);
    REQUIRE(accent);
    // First in the palette.
    QQuickItem* flow = primary->parentItem();
    REQUIRE(flow);
    CHECK(flow->childItems().front() == primary);
    CHECK(primary->property("swatchColor").value<QColor>() == QColor(QStringLiteral("#FF0000")));
    CHECK(primary->property("tooltip").toString().contains(QStringLiteral("Primary")));
    CHECK_FALSE(primary->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(primary, "clicked"));
    CHECK(host->property("picked").toString() == QStringLiteral("Primary"));

    // Linked: the chip names it, its swatch is the checked one.
    QQuickItem* link = findNamed(row, QStringLiteral("captionBrandLink_fill"));
    REQUIRE(link);
    CHECK_FALSE(link->isVisible());
    row->setProperty("brandRef", QStringLiteral("Primary"));
    CHECK(link->isVisible());
    CHECK(primary->property("checked").toBool());

    // A new kit: the palette follows.
    REQUIRE(brand->setKit(kitMap(QStringLiteral("Night"), QStringLiteral("#0000FF"), QStringLiteral("#FFFFFF"),
                                 QStringLiteral("Arial")))
                .isEmpty());
    primary = findNamed(row, QStringLiteral("captionBrandColor_fill_Primary"));
    REQUIRE(primary);
    CHECK(primary->property("swatchColor").value<QColor>() == QColor(QStringLiteral("#0000FF")));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Brand kit dialog: create a kit, edit it by typing, the sample follows; screenshots of two kits",
          "[app][graphics][brand][qml][screenshot]") {
    const auto shot = [](const QVariantMap& kit, const char* file) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        auto* brand = shell.engine.singletonInstance<BrandBridge*>("Wabcut", "Brand");
        REQUIRE(brand);
        const auto host = makeItem(shell,
                                   "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: kitDialog\n"
                                   "    BrandKitDialog { id: kitDialog }\n}\n",
                                   QRectF(0, 0, kShotWidth, kShotHeight));
        auto* dialog = host->property("dialog").value<QObject*>();
        REQUIRE(dialog);
        REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
        QQuickItem* content = popupItem(dialog);

        // No kit: the name field and Create brand kit.
        QQuickItem* create = findNamed(content, QStringLiteral("brandCreateKit"));
        QQuickItem* newName = findNamed(content, QStringLiteral("brandNewKitName"));
        REQUIRE(create);
        REQUIRE(newName);
        CHECK(create->isVisible());
        newName->setProperty("text", kit.value(QStringLiteral("name")).toString());
        REQUIRE(QMetaObject::invokeMethod(create, "clicked"));
        REQUIRE(waitFor([&] { return brand->hasKit(); }, 3000));
        CHECK(brand->kit().value(QStringLiteral("name")).toString() == kit.value(QStringLiteral("name")).toString());
        // The kit's colours, typed: the field commits on editingFinished.
        REQUIRE(brand->setKit(kit).isEmpty());
        spin(100);
        QQuickItem* hex = findNamed(content, QStringLiteral("brandColorHex_0"));
        REQUIRE(hex);
        CHECK(hex->property("text").toString() == brand->colorHex(QStringLiteral("Primary")));
        hex->setProperty("text", hex->property("text").toString());
        REQUIRE(QMetaObject::invokeMethod(hex, "editingFinished"));
        CHECK(brand->colorHex(QStringLiteral("Primary")) == hex->property("text").toString()); // unchanged: no edit

        // The sample lower third uses Primary, Accent and the heading font.
        QQuickItem* name = findNamed(content, QStringLiteral("brandPreviewName"));
        REQUIRE(name);
        CHECK(name->property("color").value<QColor>() == QColor(brand->colorHex(QStringLiteral("Accent"))));
        CHECK(name->property("font").value<QFont>().family() == brand->fontFamilyOf(QStringLiteral("heading")));
        CHECK(findNamed(content, QStringLiteral("brandColorName_1")) != nullptr);
        CHECK(findNamed(content, QStringLiteral("brandFontFamily_0")) != nullptr);
        spin(300);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        saveCrop(image, content, QString::fromLatin1(file));
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    };
    shot(kitMap(QStringLiteral("Studio Day"), QStringLiteral("#6A40FA"), QStringLiteral("#FFD13F"), QStringLiteral("Arial")),
         "brand-kit-before.png");
    shot(kitMap(QStringLiteral("Studio Night"), QStringLiteral("#E11D48"), QStringLiteral("#FFFFFF"), QStringLiteral("Georgia")),
         "brand-kit-after.png");
}

TEST_CASE("Lottie sniffing: Bodymovin JSON is claimed, other JSON and files are not", "[app][graphics][lottie]") {
    QFile lower(fixture("lottie/lower_third.json"));
    REQUIRE(lower.open(QIODevice::ReadOnly));
    const QByteArray bytes = lower.readAll();
    CHECK(looksLikeLottie(bytes));
    CHECK(looksLikeLottie(QByteArray("\xEF\xBB\xBF  \n") + bytes));
    CHECK_FALSE(looksLikeLottie(R"({"name": "x", "layers": []})"));
    CHECK_FALSE(looksLikeLottie(R"({"format": "wabcut-project", "version": 13})"));
    CHECK_FALSE(looksLikeLottie(R"([{"v": "5.7.4"}])"));
    CHECK_FALSE(looksLikeLottie("not json"));
    // Keys must be keys, not text inside strings.
    CHECK_FALSE(looksLikeLottie(R"({"note": "\"v\" \"fr\" \"ip\" \"op\" \"w\" \"h\""})"));

    GraphicsImportBridge import;
    CHECK(import.isLottie(QUrl::fromLocalFile(lower.fileName())));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto write = [&](const char* name, const QByteArray& data) {
        QFile f(QDir(dir.path()).filePath(QString::fromLatin1(name)));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(data);
        return QUrl::fromLocalFile(f.fileName());
    };
    CHECK_FALSE(import.isLottie(write("settings.json", R"({"theme": "dark"})")));
    CHECK_FALSE(import.isLottie(write("lower_third.txt", bytes)));
    CHECK(import.isLottie(write("lower_third.json", bytes)));
    CHECK(import.isLottie(write("confetti.lottie", "PK\x03\x04")));
    CHECK_FALSE(import.nameFilters().isEmpty());
}

TEST_CASE("Lottie import: lower_third.json becomes a graphic clip at the playhead in one undo step, with its report",
          "[app][graphics][lottie]") {
    Rig r;
    GraphicsImportBridge import;
    import.setController(&r.controller);
    import.setSynchronous(true);
    std::vector<QVariantMap> imported;
    QObject::connect(&import, &GraphicsImportBridge::imported, [&](const QVariantMap& report) { imported.push_back(report); });
    r.controller.setPlayhead(f(10));

    const std::size_t before = r.undo();
    const std::size_t clipsBefore = clipCount(*r.snap(), r.tl.seq);
    REQUIRE(import.importLottie(QUrl::fromLocalFile(fixture("lottie/lower_third.json"))).isEmpty());
    INFO(import.message().toStdString());
    REQUIRE(imported.size() == 1);
    CHECK(import.message().isEmpty());
    CHECK_FALSE(import.busy());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText().contains(QStringLiteral("Lottie")));
    CHECK(clipCount(*r.snap(), r.tl.seq) == clipsBefore + 1);

    // The report: the shape of ImportReportDialog's map.
    const QVariantMap report = imported.front();
    CHECK(report.value(QStringLiteral("file")).toString() == QStringLiteral("lower_third.json"));
    CHECK(report.value(QStringLiteral("format")).toString() == QStringLiteral("Lottie"));
    const QString summary = report.value(QStringLiteral("summary")).toString();
    CHECK(summary.startsWith(QStringLiteral("Lottie animation")));
    CHECK(summary.contains(QStringLiteral("fps")));
    CHECK(report.value(QStringLiteral("carried")).toInt() > 0);
    const int listed = report.value(QStringLiteral("approximated")).toInt() + report.value(QStringLiteral("dropped")).toInt();
    const QVariantList entries = report.value(QStringLiteral("entries")).toList();
    CHECK(entries.size() == std::min(listed, 200));
    for (const QVariant& e : entries) {
        const QVariantMap m = e.toMap();
        CHECK((m.value(QStringLiteral("outcome")).toString() == QStringLiteral("approximated")
               || m.value(QStringLiteral("outcome")).toString() == QStringLiteral("dropped")));
        CHECK_FALSE(m.value(QStringLiteral("reason")).toString().isEmpty());
    }

    // The new clip: a graphic at the playhead, selected.
    const QStringList selected = r.controller.selectedClips();
    REQUIRE(selected.size() >= 1);
    const auto id = parseClipKey(selected.front());
    REQUIRE(id);
    const auto loc = r.snap()->findSequence(r.tl.seq)->findClip(*id);
    REQUIRE(loc);
    CHECK(loc->clip().start() == f(10));
    REQUIRE(loc->clip().graphic());
    CHECK_FALSE(loc->clip().graphic()->layers.empty());

    // Undo takes it out.
    REQUIRE(r.controller.undo());
    CHECK(clipCount(*r.snap(), r.tl.seq) == clipsBefore);

    // Kept as a template too.
    REQUIRE(import.importLottie(QUrl::fromLocalFile(fixture("lottie/lower_third.json")),
                                {{QStringLiteral("saveAsTemplate"), true}})
                .isEmpty());
    CHECK(imported.back().value(QStringLiteral("summary")).toString().contains(QStringLiteral("template")));
    CHECK_FALSE(r.snap()->templates.get().empty());

    // Refusals: a dotLottie archive, a JSON file that is not an animation.
    CHECK_FALSE(import.importLottie(QUrl::fromLocalFile(QStringLiteral("C:/nowhere/confetti.lottie"))).isEmpty());
    CHECK(import.message().contains(QStringLiteral("dotLottie")));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QFile bad(QDir(dir.path()).filePath(QStringLiteral("settings.json")));
    REQUIRE(bad.open(QIODevice::WriteOnly));
    bad.write(R"({"theme": "dark"})");
    bad.close();
    const std::size_t count = imported.size();
    import.importLottie(QUrl::fromLocalFile(bad.fileName()));
    CHECK(imported.size() == count);
    CHECK_FALSE(import.message().isEmpty());
}

TEST_CASE("Lottie report in the import report dialog: the summary, the counts and each feature's path; screenshot",
          "[app][graphics][lottie][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* import = shell.engine.singletonInstance<GraphicsImportBridge*>("Wabcut", "GraphicsImport");
    REQUIRE(controller);
    REQUIRE(import);
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    import->setSynchronous(true);
    QVariantMap report;
    QObject::connect(import, &GraphicsImportBridge::imported, [&](const QVariantMap& r) { report = r; });
    REQUIRE(import->importLottie(QUrl::fromLocalFile(fixture("lottie/lower_third.json"))).isEmpty());
    REQUIRE_FALSE(report.isEmpty());

    const auto host = makeItem(shell,
                               "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: reportDialog\n"
                               "    ImportReportDialog { id: reportDialog }\n}\n",
                               QRectF(0, 0, kShotWidth, kShotHeight));
    auto* dialog = host->property("dialog").value<QObject*>();
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(dialog, "show", Q_ARG(QVariant, report)));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
    QQuickItem* content = popupItem(dialog);
    CHECK(dialog->property("title").toString() == QStringLiteral("Imported lower_third.json"));
    QQuickItem* summary = findNamed(content, QStringLiteral("importReportSummary"));
    REQUIRE(summary);
    CHECK(summary->property("text").toString() == report.value(QStringLiteral("summary")).toString());
    QQuickItem* rows = findNamed(content, QStringLiteral("importReportEntries"));
    REQUIRE(rows);
    CHECK(rows->property("count").toInt() == report.value(QStringLiteral("entries")).toList().size());
    spin(300);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, content, QStringLiteral("lottie-report.png"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Lottie import: files dropped together import one after the other on a worker, each after the one before",
          "[app][graphics][lottie]") {
    Rig r;
    GraphicsImportBridge import;
    import.setController(&r.controller); // asynchronous, as in the app
    std::vector<QVariantMap> imported;
    QObject::connect(&import, &GraphicsImportBridge::imported, [&](const QVariantMap& report) { imported.push_back(report); });
    r.controller.setPlayhead(f(5));
    const QUrl lowerThird = QUrl::fromLocalFile(fixture("lottie/lower_third.json"));
    const std::size_t before = r.undo();
    REQUIRE(import.importLottie(lowerThird).isEmpty());
    REQUIRE(import.importLottie(lowerThird).isEmpty()); // queued behind the first
    REQUIRE(waitFor([&] { return imported.size() == 2 && !import.busy(); }, 30000));
    INFO(import.message().toStdString());
    CHECK(r.undo() == before + 2);
    // Two graphic clips, the second starting where the first ends.
    std::vector<const Clip*> graphics;
    const Sequence* seq = r.snap()->findSequence(r.tl.seq);
    for (const TrackPtr& t : seq->videoTracks) {
        for (const Clip& c : t->clips) {
            if (c.graphic())
                graphics.push_back(&c);
        }
    }
    REQUIRE(graphics.size() == 2);
    std::sort(graphics.begin(), graphics.end(), [](const Clip* a, const Clip* b) { return a->start() < b->start(); });
    CHECK(graphics[0]->start() == f(5));
    CHECK(graphics[1]->start() == graphics[0]->end());
}
