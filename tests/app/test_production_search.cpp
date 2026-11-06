// 0.7 (card 094): the Production panel's search across all projects of a production, in the shell: typing in the field,
// the results with the project each is in, the type and "search in" choices, Import and Browse on a result, the empty and
// reading states, a narrow window, and screenshots (dark and light).

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "ProductionBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QQuickItem>
#include <QUrl>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "production-search-ui" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QUrl url(const fs::path& p) { return QUrl::fromLocalFile(QString::fromStdU16String(p.u16string())); }

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

/// One sequence and one clip whose file is `file`, named and commented as given.
ProjectState project(const fs::path& file, const std::string& sequence, const std::string& clip, const std::string& comment) {
    Timeline t("    V1 tgt |Aaaa|\n");
    ProjectState s = t.state;
    for (MediaPtr& m : s.media) {
        if (m->id != t.media('A'))
            continue; // the fixture's other items stay as they are
        MediaItem item = *m;
        item.name = clip;
        item.comment = comment;
        item.locator.path = pathToUtf8(file);
        item.info.path = item.locator.path;
        m = std::make_shared<const MediaItem>(std::move(item));
    }
    Sequence seq = *s.findSequence(t.seq);
    seq.name = sequence;
    return s.withSequence(std::make_shared<const Sequence>(std::move(seq)));
}

void saveShot(Shell& shell, const QQuickItem* item, const QString& name) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                            .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

} // namespace

TEST_CASE("Production panel: a search lists the matches of every project; narrowing, actions and screenshots (094)",
          "[app][production][search][qml][screenshot]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(1600, 960);
    shell.theme->setProperty("reduceMotion", true);
    auto* production = shell.engine.singletonInstance<ProductionBridge*>("Wabcut", "Production");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(production);
    REQUIRE(controller);
    REQUIRE(file);

    const fs::path dir = scratch("panel");
    fs::create_directories(dir / "Footage");
    for (const char* name : {"river.mov", "bridge.mov", "town.mov", "weir.mov"})
        std::ofstream(dir / "Footage" / name, std::ios::binary) << std::string(256, 'x');
    REQUIRE(production->makeProduction(url(dir / "Doc"), QStringLiteral("River film")));
    REQUIRE(io::saveProject(project(dir / "Footage" / "river.mov", "Interview assembly", "River interview with Anna", "talks about the weir"),
                            dir / "Doc" / "Interviews.wabcut", io::SaveOptions{0}));
    REQUIRE(io::saveProject(project(dir / "Footage" / "bridge.mov", "B-roll selects", "Bridge at dusk", "the river behind it"),
                            dir / "Doc" / "B-roll.wabcut", io::SaveOptions{0}));
    REQUIRE(io::saveProject(project(dir / "Footage" / "town.mov", "Town cut", "Town square", "market day"),
                            dir / "Doc" / "Town.wabcut", io::SaveOptions{0}));
    production->refresh();

    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("production"))));
    QQuickItem* content = shell.window->contentItem();
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] { return (panel = findNamed(content, QStringLiteral("productionPanel"))) != nullptr; }));
    QQuickItem* search = nullptr;
    REQUIRE(waitFor([&] { return (search = findNamed(panel, QStringLiteral("productionSearch"))) != nullptr; }));
    QQuickItem* field = findNamed(search, QStringLiteral("productionSearchField"));
    REQUIRE(field);
    const auto list = [&] { return findNamed(search, QStringLiteral("productionSearchResults")); };
    const auto entries = [&] { return findNamed(panel, QStringLiteral("productionEntries")); };

    // Nothing typed: the project list shows, no results.
    REQUIRE(waitFor([&] { return entries() != nullptr && entries()->isVisible(); }));
    CHECK_FALSE(search->property("active").toBool());
    CHECK((list() == nullptr || !list()->isVisible()));

    // Typing searches after a pause; the project list gives way to the results.
    field->setProperty("text", QStringLiteral("river"));
    REQUIRE(waitFor([&] { return production->searched() && !production->searching(); }, 20'000));
    REQUIRE(waitFor([&] { return list() != nullptr && list()->isVisible(); }));
    CHECK(search->property("active").toBool());
    CHECK_FALSE(entries()->isVisible());
    CHECK(production->searchResults().size() == 2);
    QQuickItem* status = findNamed(search, QStringLiteral("productionSearchStatus"));
    REQUIRE(status);
    CHECK(status->property("text").toString().startsWith(QStringLiteral("2 result")));
    QQuickItem* first = findNamed(search, QStringLiteral("productionSearchResult_0"));
    REQUIRE(first);
    CHECK(first->property("height").toDouble() > 20);
    CHECK(first->property("height").toDouble() < 80);
    CHECK(production->searchResults()[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("River interview with Anna"));
    spin(300);
    saveShot(shell, panel, QStringLiteral("production-search-dark.png"));
    CHECK(auditAccessibility(search).isEmpty());

    // A type narrows it: the sequences of every project (no words needed).
    field->setProperty("text", QString());
    QQuickItem* types = findNamed(search, QStringLiteral("productionSearchTypes"));
    REQUIRE(types);
    types->setProperty("currentIndex", 1);
    REQUIRE(waitFor([&] { return production->searched() && !production->searching() && production->searchResults().size() == 3; }, 20'000));
    spin(100);
    CHECK(search->property("type").toString() == QStringLiteral("sequence"));
    for (const QVariant& r : production->searchResults())
        CHECK(r.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("sequence"));
    // Footage again, in the comment only: "river" is the comment of one clip, the name of the other.
    types->setProperty("currentIndex", 0);
    field->setProperty("text", QStringLiteral("river"));
    QMetaObject::invokeMethod(search, "runSearch");
    REQUIRE(waitFor([&] { return production->searched() && !production->searching() && production->searchResults().size() == 2; }, 20'000));
    search->setProperty("field", QStringLiteral("comment"));
    QMetaObject::invokeMethod(search, "runSearch");
    REQUIRE(waitFor([&] { return production->searched() && !production->searching() && production->searchResults().size() == 1; }, 20'000));
    CHECK(production->searchResults()[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Bridge at dusk"));
    search->setProperty("field", QString());

    // No match: said in words, no list.
    field->setProperty("text", QStringLiteral("zeppelin"));
    QMetaObject::invokeMethod(search, "runSearch");
    REQUIRE(waitFor([&] { return production->searched() && !production->searching() && production->searchResults().isEmpty(); }, 20'000));
    spin(100);
    CHECK(status->property("text").toString().startsWith(QStringLiteral("No results")));
    CHECK((list() == nullptr || !list()->isVisible()));

    // Import on a result: a copy of the clip comes into the project being edited (one undo step).
    field->setProperty("text", QStringLiteral("market"));
    QMetaObject::invokeMethod(search, "runSearch");
    REQUIRE(waitFor([&] { return production->searched() && !production->searching() && production->searchResults().size() == 1; }, 20'000));
    REQUIRE(waitFor([&] { return findNamed(search, QStringLiteral("productionSearchImport_0")) != nullptr; }));
    const std::size_t media = controller->snapshot()->media.size();
    const std::size_t undo = controller->session().undoCount();
    QMetaObject::invokeMethod(findNamed(search, QStringLiteral("productionSearchImport_0")), "clicked");
    CHECK(controller->session().undoCount() == undo + 1);
    CHECK(controller->snapshot()->media.size() == media + 1);

    // Browse on a result: the project opens read-only next to this one and the results make room for its items.
    QMetaObject::invokeMethod(findNamed(search, QStringLiteral("productionSearchBrowse_0")), "clicked");
    CHECK(production->browsing() == url(dir / "Doc" / "Town.wabcut"));
    REQUIRE(waitFor([&] { return findNamed(panel, QStringLiteral("productionBrowseItems")) != nullptr; }));
    CHECK_FALSE(list()->isVisible());
    CHECK(production->revealedKey() == production->searchResults()[0].toMap().value(QStringLiteral("key")).toString());
    production->stopBrowsing();

    // Esc in the field clears it and the project list is back; the results are forgotten.
    field->setProperty("text", QStringLiteral("river"));
    QMetaObject::invokeMethod(search, "runSearch");
    REQUIRE(waitFor([&] { return production->searched() && !production->searching() && production->searchResults().size() == 2; }, 20'000));
    field->setProperty("text", QString());
    types->setProperty("currentIndex", 0);
    REQUIRE(waitFor([&] { return !production->searched(); }, 20'000));
    CHECK_FALSE(search->property("active").toBool());
    REQUIRE(waitFor([&] { return entries()->isVisible(); }));

    // The light theme and a narrow window: nothing runs off an edge.
    field->setProperty("text", QStringLiteral("river"));
    QMetaObject::invokeMethod(search, "runSearch");
    REQUIRE(waitFor([&] { return production->searched() && !production->searching() && production->searchResults().size() == 2; }, 20'000));
    shell.theme->setProperty("dark", false);
    spin(300);
    saveShot(shell, panel, QStringLiteral("production-search-light.png"));
    CHECK(auditAccessibility(search).isEmpty());
    shell.resize(1100, 700);
    spin(300);
    QQuickItem* row = findNamed(search, QStringLiteral("productionSearchResult_0"));
    REQUIRE(row);
    CHECK(row->mapRectToItem(search, QRectF(0, 0, row->width(), row->height())).right() <= search->width() + 0.5);
    saveShot(shell, panel, QStringLiteral("production-search-narrow-light.png"));
    shell.theme->setProperty("dark", true);
    spin(300);
    saveShot(shell, panel, QStringLiteral("production-search-narrow-dark.png"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.filter(QStringLiteral("qml")).isEmpty());
    (void)file;
}
