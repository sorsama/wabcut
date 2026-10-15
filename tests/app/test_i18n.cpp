// Every user-visible string in the QML shell goes through qsTr() (docs/architecture.md, "app").
// A source scan: user-facing properties (text, label, title, tooltip, detail, placeholderText and the Accessible
// name/description) must not be assigned a bare, non-empty string literal or a concatenation that starts with one.

#include <catch2/catch_test_macros.hpp>

#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

TEST_CASE("User-visible QML strings are translatable", "[app][i18n]") {
    // The pattern contains `)"`, so the raw literal needs its own delimiter.
    static const char* const kPattern =
        R"re(^\s*(text|label|title|tooltip|detail|placeholderText|Accessible\.name|Accessible\.description)\s*:\s*"([^"]+)")re";
    static const QRegularExpression bareLiteral(QString::fromLatin1(kPattern));

    QStringList violations;
    int files = 0;
    QDirIterator it(QStringLiteral(WABCUT_APP_QML_DIR), {QStringLiteral("*.qml")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
        ++files;
        QTextStream in(&file);
        int lineNumber = 0;
        while (!in.atEnd()) {
            const QString line = in.readLine();
            ++lineNumber;
            const QRegularExpressionMatch match = bareLiteral.match(line);
            if (match.hasMatch())
                violations << QStringLiteral("%1:%2: %3").arg(path).arg(lineNumber).arg(line.trimmed());
        }
    }
    CHECK(files >= 40);
    INFO("untranslated literals:\n" << violations.join(QLatin1Char('\n')).toStdString());
    CHECK(violations.isEmpty());
}
