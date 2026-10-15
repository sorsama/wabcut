// 0.1.3 icon set guards over the sources (no engine needed): the icons are Boxicons (MIT) under Wabcut's logical
// names (tools/icons/boxicons-map.json, written by tools/icons/import_boxicons.py), plus a few own drawings.
//   * every icon the QML names (iconName / icon / actionIcon literals) has a file in src/app/resources/icons;
//   * every file there is mapped or own, a 24x24 glyph in black (Icon.qml tints it), and in the module's resources.

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace {

const QDir qmlDir() {
    return QDir(QStringLiteral(WABCUT_APP_QML_DIR));
}

QString iconDir() {
    return qmlDir().absoluteFilePath(QStringLiteral("../resources/icons"));
}

QString readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QSet<QString> iconFiles() {
    QSet<QString> names;
    for (const QString& f : QDir(iconDir()).entryList({QStringLiteral("*.svg")}, QDir::Files))
        names.insert(f.chopped(4));
    return names;
}

} // namespace

TEST_CASE("Icons: every icon name the QML uses has a file", "[app][icons]") {
    // The value after iconName:/icon:/actionIcon: up to the next comma or brace, so both branches of a ternary count.
    const QRegularExpression use(QStringLiteral(R"(\b(?:iconName|icon|actionIcon)\s*:\s*([^,}\n]+))"));
    const QRegularExpression literal(QStringLiteral(R"re("([a-z0-9]+(?:-[a-z0-9]+)*)")re"));
    const QSet<QString> files = iconFiles();
    REQUIRE(files.size() > 100);
    QStringList missing;
    int uses = 0;
    QDirIterator it(qmlDir().path(), {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QStringList lines = readAll(path).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            for (auto m = use.globalMatch(lines[i]); m.hasNext();) {
                const QString expression = m.next().captured(1);
                if (expression.contains(QLatin1String("qsTr(")))
                    continue;
                for (auto l = literal.globalMatch(expression); l.hasNext();) {
                    const QRegularExpressionMatch match = l.next();
                    // A ternary's condition compares with literals ("kind === \"file\" ? ..."): not icons.
                    const QString before = expression.left(match.capturedStart()).trimmed();
                    if (before.endsWith(QLatin1Char('=')))
                        continue;
                    const QString name = match.captured(1);
                    ++uses;
                    if (!files.contains(name))
                        missing << qmlDir().relativeFilePath(path) + QLatin1Char(':') + QString::number(i + 1) + QStringLiteral(": ")
                                       + name;
                }
            }
        }
    }
    CHECK(uses > 300); // the scan found the uses
    INFO("Add the icon with: python -I tools/icons/import_boxicons.py add <bx-name> <name>\n"
         << missing.join(QLatin1Char('\n')).toStdString());
    CHECK(missing.isEmpty());
}

TEST_CASE("Icons: each file is a mapped Boxicons glyph or an own drawing, 24x24, listed in the module", "[app][icons]") {
    const QString mapPath = qmlDir().absoluteFilePath(QStringLiteral("../../../tools/icons/boxicons-map.json"));
    const QJsonObject map = QJsonDocument::fromJson(readAll(mapPath).toUtf8()).object();
    REQUIRE(map.value(QStringLiteral("license")).toString() == QLatin1String("MIT"));
    const QJsonObject mapped = map.value(QStringLiteral("icons")).toObject();
    QSet<QString> own;
    for (const auto& v : map.value(QStringLiteral("own")).toArray())
        own.insert(v.toString());
    REQUIRE(mapped.size() > 90);

    const QString cmake = readAll(qmlDir().absoluteFilePath(QStringLiteral("../CMakeLists.txt")));
    REQUIRE(!cmake.isEmpty());
    const QString license = readAll(iconDir() + QStringLiteral("/LICENSE"));
    CHECK(license.startsWith(QLatin1String("MIT License")));
    CHECK(license.contains(QLatin1String("Boxicons")));

    QStringList problems;
    for (const QString& name : iconFiles()) {
        const QString svg = readAll(iconDir() + QLatin1Char('/') + name + QStringLiteral(".svg"));
        if (mapped.contains(name)) {
            const QString glyph = mapped.value(name).toString().section(QLatin1Char('/'), -1);
            if (!svg.contains(QStringLiteral("<!-- Boxicons %1 ").arg(glyph)))
                problems << name + QStringLiteral(": not the mapped glyph ") + glyph + QStringLiteral(" (run sync)");
        } else if (!own.contains(name)) {
            problems << name + QStringLiteral(": neither mapped nor own");
        }
        if (!svg.contains(QLatin1String(R"(viewBox="0 0 24 24")")))
            problems << name + QStringLiteral(": no 24x24 viewBox");
        // Icon.qml lifts black to white and colorizes it: a coloured glyph would tint wrongly.
        static const QRegularExpression colour(QStringLiteral(R"((?:fill|stroke)="(?!none|#000|currentColor)[^"]*")"));
        if (colour.match(svg).hasMatch())
            problems << name + QStringLiteral(": a colour other than black");
        if (!cmake.contains(QStringLiteral("resources/icons/%1.svg").arg(name)))
            problems << name + QStringLiteral(": not listed in src/app/CMakeLists.txt");
    }
    INFO(problems.join(QLatin1Char('\n')).toStdString());
    CHECK(problems.isEmpty());
}
