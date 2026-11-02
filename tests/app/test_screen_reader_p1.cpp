// P1 round, stream 3E, #61 screen-reader pass (offscreen). NVDA reads Qt's UI Automation provider, which reports what
// QAccessible reports for each item; this audit walks that tree (A11yAudit.h) for the places a screen-reader user goes
// through first - Home, every built-in workspace of the editor (its panels), each menu of the menu bar, the Export
// workspace and every Settings page - and fails on any control without a name or with a role that does not fit. The
// keyboard part checks that Tab gets into the menu bar, the panels and the Settings pages. What still needs a person
// with NVDA is in docs/accessibility.md ("Manual NVDA checklist"). The full list of findings is written to
// test-output/ui/a11y-audit-p1.txt.

#include "A11yAudit.h"
#include "ShellFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <QDirIterator>
#include <QFile>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QRegularExpression>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const char* const kMenuNames[] = {"fileMenu",     "editMenu", "clipMenu",   "sequenceMenuBar", "markersMenu",
                                  "graphicsMenu", "viewMenu", "windowMenu", "helpMenu"};
const char* const kSettingsPages[] = {"general", "appearance", "interface", "timeline", "performance",
                                      "audio",   "ai",         "keyboard",  "extensions"};
const char* const kWorkspaces[] = {"Edit", "Audio", "Export", "Color", "Captions"};

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

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

QQuickItem* popupItem(QObject* popup) {
    auto* content = popup ? popup->property("contentItem").value<QQuickItem*>() : nullptr;
    return content ? content->parentItem() : nullptr;
}

/// What A11yAudit.h leaves out but UI Automation lists (the live UIA walk found these): visible scroll bars without a
/// name (NVDA reads "scroll bar" and nothing else) and dialogs whose own element has no name.
QStringList uiaExtras(QQuickItem* root) {
    QStringList issues;
    QList<QQuickItem*> stack{root};
    while (!stack.isEmpty()) {
        QQuickItem* item = stack.takeLast();
        if (!item->isVisible() || item->width() <= 0 || item->height() <= 0)
            continue;
        if (item->inherits("QQuickScrollBar")) {
            QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(item);
            if (iface && iface->text(QAccessible::Name).trimmed().isEmpty())
                issues << a11yPath(root, item) + QStringLiteral(": scroll bar without a name");
        }
        if (item->inherits("QQuickPopupItem")) {
            QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(item);
            if (iface && iface->role() == QAccessible::Dialog && iface->text(QAccessible::Name).trimmed().isEmpty())
                issues << a11yPath(root, item) + QStringLiteral(": dialog without a name");
        }
        for (QQuickItem* child : item->childItems())
            stack << child;
    }
    return issues;
}

/// Every issue under `item`, each prefixed with where it was found.
void audit(QStringList& issues, const QString& where, QQuickItem* item) {
    if (!item) {
        issues << where + QStringLiteral(": not found");
        return;
    }
    for (const QString& issue : auditAccessibility(item) + uiaExtras(item))
        issues << where + QStringLiteral(": ") + issue;
}

/// The QML controls that need a name of their own, and the lines that give one (the static half of the audit: it
/// also covers dialogs the walk above does not open).
struct NameRule {
    const char* type;
    QStringList sources;
};
const NameRule kNameRules[] = {
    {"PillTabs", {QStringLiteral("Accessible.name")}},
    {"Toggle", {QStringLiteral("Accessible.name"), QStringLiteral("text:"), QStringLiteral("tooltip:")}},
    {"IconButton", {QStringLiteral("Accessible.name"), QStringLiteral("tooltip:"), QStringLiteral("text:")}},
    {"ValueSlider", {QStringLiteral("Accessible.name"), QStringLiteral("label:")}},
    {"NumberField", {QStringLiteral("Accessible.name"), QStringLiteral("label:")}},
    {"FieldInput", {QStringLiteral("Accessible.name"), QStringLiteral("placeholderText:"), QStringLiteral("label:")}},
};

/// "dialogs/X.qml:12 PillTabs" for each such control in the app's QML whose own block gives no name.
QStringList unnamedInSources() {
    QStringList found;
    QStringList types;
    for (const NameRule& rule : kNameRules)
        types << QString::fromLatin1(rule.type);
    const QRegularExpression opening(QStringLiteral("^\\s*(%1)\\s*\\{\\s*$").arg(types.join(QLatin1Char('|'))));
    const QDir base(QStringLiteral(WABCUT_APP_QML_DIR));
    QDirIterator it(base.path(), {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const QRegularExpressionMatch m = opening.match(lines[i]);
            if (!m.hasMatch())
                continue;
            const QString type = m.captured(1);
            QStringList sources;
            for (const NameRule& rule : kNameRules) {
                if (type == QLatin1String(rule.type))
                    sources = rule.sources;
            }
            int depth = 0;
            bool named = false;
            for (qsizetype j = i; j < lines.size() && j < i + 200; ++j) {
                const int before = depth;
                depth += static_cast<int>(lines[j].count(QLatin1Char('{')) - lines[j].count(QLatin1Char('}')));
                const QString own = lines[j].trimmed();
                if (before == 1 && std::ranges::any_of(sources, [&](const QString& s) { return own.startsWith(s); }))
                    named = true;
                if (depth <= 0 && j > i)
                    break;
            }
            if (!named)
                found << QStringLiteral("%1:%2 %3").arg(base.relativeFilePath(path)).arg(i + 1).arg(type);
        }
    }
    return found;
}

/// Tab from `root` reaches an item under it whose objectName starts with `prefix`.
bool tabReachesPrefix(QQuickItem* root, const QString& prefix) {
    for (const QQuickItem* item : tabChain(root)) {
        for (const QQuickItem* p = item; p && p != root; p = p->parentItem()) {
            if (p->objectName().startsWith(prefix))
                return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("3E Screen reader: Home, the editor panels, the menu bar, Export and Settings name every control",
          "[app][a11y][qml][3e]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    spin(200);
    QStringList issues;
    QStringList notes;

    // The editor: each built-in workspace (0 Edit, 1 Audio, 2 Export, 3 Color, 4 Captions) with its panels.
    for (int w = 0; w < 5; ++w) {
        shell.window->setProperty("workspace", w);
        spin(300);
        audit(issues, QStringLiteral("Workspace %1").arg(QLatin1String(kWorkspaces[w])), shell.window->contentItem());
        notes << QStringLiteral("Tab order, %1 workspace: %2")
                     .arg(QLatin1String(kWorkspaces[w]), describeTabChain(shell.window->contentItem()));
    }
    shell.window->setProperty("workspace", 0);
    spin(300);
    // The keyboard gets into the menu bar and the timeline from the window.
    CHECK(tabReachesPrefix(shell.window->contentItem(), QStringLiteral("appMenuBar")));
    CHECK(tabReachesPrefix(shell.window->contentItem(), QStringLiteral("timeline")));

    // Each menu of the menu bar, open.
    QObject* bar = shell.object("appMenuBar");
    REQUIRE(bar);
    for (int i = 0; i < 9; ++i) {
        REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, i), Q_ARG(bool, false)));
        QObject* menu = shell.object(kMenuNames[i]);
        REQUIRE(menu);
        REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }));
        spin(50);
        audit(issues, QStringLiteral("Menu %1").arg(QLatin1String(kMenuNames[i])), popupItem(menu));
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
        REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }));
    }

    // Every Settings page.
    for (const char* key : kSettingsPages) {
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, QString::fromLatin1(key)),
                                          Q_ARG(QString, QString())));
        QObject* loader = shell.object("settingsWindowLoader");
        REQUIRE(loader);
        QObject* settings = loader->property("item").value<QObject*>();
        REQUIRE(settings);
        REQUIRE(waitFor([&] { return settings->property("opened").toBool(); }));
        spin(150);
        QQuickItem* box = popupItem(settings);
        audit(issues, QStringLiteral("Settings %1").arg(QLatin1String(key)), box);
        if (QAccessibleInterface* iface = box ? QAccessible::queryAccessibleInterface(box) : nullptr)
            CHECK(iface->text(QAccessible::Name) == QStringLiteral("Settings"));
        if (QString::fromLatin1(key) == QLatin1String("appearance"))
            CHECK(tabReaches(box, QStringLiteral("uiScaleTabs")));
        REQUIRE(QMetaObject::invokeMethod(settings, "close"));
        REQUIRE(waitFor([&] { return !settings->property("visible").toBool(); }));
    }

    // Home (New project, recent projects, the side bar).
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    QQuickItem* home = shell.item("homeScreen");
    REQUIRE(home);
    REQUIRE(waitFor([&] { return home->isVisible() && evaluate(shell, QStringLiteral("home.opened")).toBool(); }));
    spin(300);
    audit(issues, QStringLiteral("Home"), home);
    notes << QStringLiteral("Tab order, Home: %1").arg(describeTabChain(home));

    writeOutputFile(QStringLiteral("a11y-audit-p1.txt"),
                    QStringLiteral("Issues (%1):\n%2\n\n%3\n").arg(issues.size()).arg(issues.join(QLatin1Char('\n')),
                                                                         notes.join(QLatin1String("\n\n"))));
    INFO(join(issues));
    CHECK(issues.isEmpty());
    // The QML sources: every tab strip, toggle, icon button, slider and field has a name of its own.
    const QStringList unnamed = unnamedInSources();
    INFO(join(unnamed));
    CHECK(unnamed.isEmpty());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
