// Every user-visible string in the QML shell goes through qsTr() (docs/architecture.md, "app").
// A source scan: user-facing properties (text, label, title, tooltip, detail, placeholderText and the Accessible
// name/description) must not be assigned a bare, non-empty string literal or a concatenation that starts with one.

#include "EnglishPlurals.h"

#include <wabcut/model/Plural.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QList>
#include <QPair>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include <memory>

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

// ---- Real plurals (card 033) ----------------------------------------------------------------------------------------

TEST_CASE("EnglishPluralTranslator turns the (s) marker into the right form for the count", "[app][i18n][plurals]") {
    using wabcut::app::EnglishPluralTranslator;
    // Installed by the test application, as main() installs it (ShellFixture).
    const auto tr = [](const char* text, int n) { return QCoreApplication::translate("test", text, "", n); };
    CHECK(tr("%n clip(s) deleted", 0) == QStringLiteral("0 clips deleted"));
    CHECK(tr("%n clip(s) deleted", 1) == QStringLiteral("1 clip deleted"));
    CHECK(tr("%n clip(s) deleted", 2) == QStringLiteral("2 clips deleted"));
    CHECK(tr("%n clip(s) deleted", 1000) == QStringLiteral("1000 clips deleted"));
    CHECK(tr("%n match(es)", 1) == QStringLiteral("1 match"));
    CHECK(tr("%n match(es)", 3) == QStringLiteral("3 matches"));
    // "(ies)" is a noun that ends in -y: one proxy, two proxies.
    CHECK(tr("%n prox(ies)", 0) == QStringLiteral("0 proxies"));
    CHECK(tr("%n prox(ies)", 1) == QStringLiteral("1 proxy"));
    CHECK(tr("%n prox(ies)", 2) == QStringLiteral("2 proxies"));
    CHECK(tr("Remaking %n prox(ies).", 1) == QStringLiteral("Remaking 1 proxy."));
    // Several markers in one string follow the one count; text without a marker is left alone.
    CHECK(tr("%n file(s) and %n bin(s)", 1) == QStringLiteral("1 file and 1 bin"));
    CHECK(tr("%n file(s) and %n bin(s)", 4) == QStringLiteral("4 files and 4 bins"));
    CHECK(tr("Nothing to count here", 5) == QStringLiteral("Nothing to count here"));
    CHECK(tr("%n frames", 1) == QStringLiteral("1 frames")); // not a marker: the caller writes (s)
    // No count passed (-1): the text is not ours to change (a unit label such as "(s)" for seconds stays).
    CHECK(QCoreApplication::translate("test", "Source (s) · timeline (s)") == QStringLiteral("Source (s) · timeline (s)"));
    CHECK(QCoreApplication::translate("test", "%n clip(s)", "", -1) == QStringLiteral("%n clip(s)"));

    // The resolution itself, and the translator's contract with Qt.
    CHECK(EnglishPluralTranslator::resolve(QStringLiteral("%n item(s)"), 1) == QStringLiteral("%n item"));
    CHECK(EnglishPluralTranslator::resolve(QStringLiteral("%n item(s)"), 0) == QStringLiteral("%n items"));
    CHECK(EnglishPluralTranslator::resolve(QStringLiteral("%n prox(ies)"), 1) == QStringLiteral("%n proxy"));
    CHECK(EnglishPluralTranslator::resolve(QStringLiteral("%n prox(ies)"), 5) == QStringLiteral("%n proxies"));
    EnglishPluralTranslator translator;
    CHECK_FALSE(translator.isEmpty());
    CHECK(translator.translate("c", "%n clip(s)", "", -1).isNull());
    CHECK(translator.translate("c", "plain", "", 2).isNull());
    CHECK(translator.translate("c", nullptr, "", 2).isNull());
}

TEST_CASE("qsTr in QML gets real plurals from the same translator (card 033)", "[app][i18n][plurals][qml]") {
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"qml(
        import QtQml
        QtObject {
            property var rows: [qsTr("%n item(s)", "", 0), qsTr("%n item(s)", "", 1), qsTr("%n item(s)", "", 2),
                                qsTr("Delete %n Keyframe(s)", "", 1), qsTr("Delete %n Keyframe(s)", "", 3),
                                qsTr("%1 of %n suggestion(s)", "", 1).arg(0), qsTr("%1 of %n suggestion(s)", "", 5).arg(2),
                                qsTr("%n field(s)", "", 1), qsTr("%n field(s)", "", 2),
                                qsTr("%n prox(ies)", "", 1), qsTr("%n prox(ies)", "", 2)]
        }
    )qml", QUrl());
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    const QStringList rows = object->property("rows").toStringList();
    CHECK(rows == QStringList{QStringLiteral("0 items"), QStringLiteral("1 item"), QStringLiteral("2 items"),
                              QStringLiteral("Delete 1 Keyframe"), QStringLiteral("Delete 3 Keyframes"),
                              QStringLiteral("0 of 1 suggestion"), QStringLiteral("2 of 5 suggestions"),
                              QStringLiteral("1 field"), QStringLiteral("2 fields"), QStringLiteral("1 proxy"),
                              QStringLiteral("2 proxies")});
}

namespace {

/// One call of a translation function in a source file: `qsTr(text, disambiguation, n)`, `tr(...)`, `translate(context,
/// text, disambiguation, n)` or the app's local `tx(...)`.
struct TranslationCall {
    QString function;
    QStringList arguments; // the top-level arguments, as written
    int line = 0;
    QString lineText;
};

/// The top-level arguments of the call whose '(' is at `open` (strings, brackets and // comments skipped); empty when the
/// call does not close.
QStringList splitArguments(const QString& text, qsizetype open) {
    QStringList args;
    int depth = 0;
    qsizetype begin = open + 1;
    for (qsizetype i = open; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            for (++i; i < text.size() && text.at(i) != c; ++i) {
                if (text.at(i) == QLatin1Char('\\'))
                    ++i;
            }
        } else if (c == QLatin1Char('/') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('/')) {
            while (i < text.size() && text.at(i) != QLatin1Char('\n'))
                ++i;
        } else if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) {
            ++depth;
        } else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
            if (--depth == 0) {
                args << text.mid(begin, i - begin);
                return args;
            }
        } else if (c == QLatin1Char(',') && depth == 1) {
            args << text.mid(begin, i - begin);
            begin = i + 1;
        }
    }
    return {};
}

QList<TranslationCall> translationCalls(const QString& text) {
    static const QRegularExpression head(QStringLiteral(R"re((?<![A-Za-z0-9_])(qsTr|tr|translate|tx)\s*\()re"));
    QList<TranslationCall> calls;
    QRegularExpressionMatchIterator it = head.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const qsizetype lineStart = text.lastIndexOf(QLatin1Char('\n'), m.capturedStart()) + 1;
        qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), m.capturedStart());
        if (lineEnd < 0)
            lineEnd = text.size();
        const QString lineText = text.mid(lineStart, lineEnd - lineStart);
        const QString trimmed = lineText.trimmed();
        if (trimmed.startsWith(QLatin1String("//")) || trimmed.startsWith(QLatin1Char('*')))
            continue; // a comment, a translator comment (//: ... %n ...) included
        TranslationCall call;
        call.function = m.captured(1);
        call.arguments = splitArguments(text, m.capturedEnd() - 1);
        call.line = static_cast<int>(text.left(m.capturedStart()).count(QLatin1Char('\n'))) + 1;
        call.lineText = trimmed;
        if (!call.arguments.isEmpty())
            calls << call;
    }
    return calls;
}

/// The words after "%n" that read wrong at one: a noun written in the plural ("%n proxies"), or a verb that agrees with it
/// ("%n were already there"). Empty when the text is fine.
QString fixedPluralWord(const QString& text) {
    // What follows "%n" up to the next punctuation mark (or the end of the text); its first three words are looked at.
    static const QRegularExpression afterCount(QStringLiteral(R"re(%n\s+([^.,;:!?()%]*))re"));
    static const QRegularExpression wordPattern(QStringLiteral(R"re([A-Za-z\x{2019}'-]+)re"));
    static const QRegularExpression pluralNoun(QStringLiteral(R"re(^[a-z]{3,}(?:ies|ves|[a-rt-z]s|ss?es|ches|shes|xes|[a-z]ys)$)re"));
    // Words that end in "s" and are not plural nouns.
    static const QStringList notNouns{QStringLiteral("was"),    QStringLiteral("has"),  QStringLiteral("does"),
                                      QStringLiteral("is"),     QStringLiteral("its"),  QStringLiteral("this"),
                                      QStringLiteral("less"),   QStringLiteral("across"), QStringLiteral("always"),
                                      QStringLiteral("plus"),   QStringLiteral("yes"),  QStringLiteral("his"),
                                      QStringLiteral("thus"),   QStringLiteral("as"),   QStringLiteral("us"),
                                      QStringLiteral("status"), QStringLiteral("focus"), QStringLiteral("bonus")};
    static const QStringList agreeingVerbs{QStringLiteral("were"), QStringLiteral("are"), QStringLiteral("have"),
                                           QStringLiteral("do"), QStringLiteral("need")};
    QRegularExpressionMatchIterator it = afterCount.globalMatch(text);
    while (it.hasNext()) {
        const QString following = it.next().captured(1);
        QRegularExpressionMatchIterator words = wordPattern.globalMatch(following);
        for (int seen = 0; seen < 3 && words.hasNext(); ++seen) {
            const QString word = words.next().captured(0);
            const QString lower = word.toLower();
            if (agreeingVerbs.contains(lower) || (!notNouns.contains(lower) && pluralNoun.match(lower).hasMatch()))
                return word;
        }
    }
    return {};
}

/// The lines of `path` outside comments, with their numbers (a /* ... */ block and // lines are left out).
QList<QPair<int, QString>> codeLines(const QString& path) {
    QFile file(path);
    QList<QPair<int, QString>> lines;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return lines;
    QTextStream in(&file);
    bool inBlock = false;
    int number = 0;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        ++number;
        const QString trimmed = line.trimmed();
        if (inBlock) {
            inBlock = !trimmed.contains(QLatin1String("*/"));
            continue;
        }
        if (trimmed.startsWith(QLatin1String("/*"))) {
            inBlock = !trimmed.contains(QLatin1String("*/"));
            continue;
        }
        if (trimmed.startsWith(QLatin1String("//")) || trimmed.startsWith(QLatin1Char('*')))
            continue;
        lines.append({number, line});
    }
    return lines;
}

} // namespace

TEST_CASE("No app string with a plural marker or a count is translated without its count (card 033)",
          "[app][i18n][plurals]") {
    // qsTr("... item(s)") with no count prints the marker literally; "%n" with no count prints "%n". A source scan of every
    // call, on one line or several: the QML shell and the app's C++ (qsTr / tr / translate / tx).
    static const QRegularExpression marker(QStringLiteral(R"re([A-Za-z]\((?:s|es|ies)\)|%n)re"));
    QStringList violations;
    int calls = 0;
    const auto scan = [&](const QString& root, const QStringList& filters) {
        QDirIterator it(root, filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile file(path);
            REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
            const QString text = QString::fromUtf8(file.readAll());
            for (const TranslationCall& call : translationCalls(text)) {
                const int textArgument = call.function == QLatin1String("translate") ? 1 : 0;
                if (textArgument >= call.arguments.size() || !call.arguments.at(textArgument).contains(QLatin1Char('"')))
                    continue; // the text is a variable: its own literal is checked where it is written
                ++calls;
                if (!marker.match(call.arguments.at(textArgument)).hasMatch())
                    continue;
                // qsTr / tr: (text, disambiguation, n); translate: (context, text, disambiguation, n); tx: (text, "", n).
                const int needed = call.function == QLatin1String("tx") ? 2 : textArgument + 3;
                if (call.arguments.size() < needed)
                    violations << QStringLiteral("%1:%2: no count: %3").arg(path).arg(call.line).arg(call.lineText);
            }
        }
    };
    scan(QStringLiteral(WABCUT_APP_QML_DIR), {QStringLiteral("*.qml")});
    scan(QDir::cleanPath(QStringLiteral(WABCUT_APP_QML_DIR) + QStringLiteral("/../src")), {QStringLiteral("*.cpp")});
    CHECK(calls > 1000); // the scan finds the app's translated strings
    INFO("strings that would print their marker:\n" << violations.join(QLatin1Char('\n')).toStdString());
    CHECK(violations.isEmpty());
}

TEST_CASE("No app string writes a counted noun in the plural without a marker (card 033)", "[app][i18n][plurals]") {
    // "%n proxies" reads "1 proxies"; "%n were already there" reads "1 were". The count goes with a marker: "%n prox(ies)".
    QStringList violations;
    const auto scan = [&](const QString& root, const QStringList& filters) {
        QDirIterator it(root, filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile file(path);
            REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
            const QString text = QString::fromUtf8(file.readAll());
            for (const TranslationCall& call : translationCalls(text)) {
                const int textArgument = call.function == QLatin1String("translate") ? 1 : 0;
                if (textArgument >= call.arguments.size())
                    continue;
                // The literal text of the argument: its string literals one after another.
                QString literal;
                static const QRegularExpression strings(QStringLiteral(R"re("((?:[^"\\\n]|\\.)*)")re"));
                QRegularExpressionMatchIterator it2 = strings.globalMatch(call.arguments.at(textArgument));
                while (it2.hasNext())
                    literal += it2.next().captured(1);
                static const QRegularExpression marker(QStringLiteral(R"re(\((?:s|es|ies)\))re"));
                if (!literal.contains(QLatin1String("%n")) || marker.match(literal).hasMatch())
                    continue;
                if (const QString word = fixedPluralWord(literal); !word.isEmpty())
                    violations << QStringLiteral("%1:%2: \"%3\" in %4").arg(path).arg(call.line).arg(word, call.lineText);
            }
        }
    };
    scan(QStringLiteral(WABCUT_APP_QML_DIR), {QStringLiteral("*.qml")});
    scan(QDir::cleanPath(QStringLiteral(WABCUT_APP_QML_DIR) + QStringLiteral("/../src")), {QStringLiteral("*.cpp")});
    INFO("counted strings that read wrong at 1:\n" << violations.join(QLatin1Char('\n')).toStdString());
    CHECK(violations.isEmpty());

    // The check itself.
    CHECK(fixedPluralWord(QStringLiteral("Remaking %n proxies.")) == QStringLiteral("proxies"));
    CHECK(fixedPluralWord(QStringLiteral("%n channels")) == QStringLiteral("channels"));
    CHECK(fixedPluralWord(QStringLiteral("%n already have proxies")) == QStringLiteral("have"));
    CHECK(fixedPluralWord(QStringLiteral("%n were already there.")) == QStringLiteral("were"));
    CHECK(fixedPluralWord(QStringLiteral("%n queued")).isEmpty());
    CHECK(fixedPluralWord(QStringLiteral("%n with a proxy already")).isEmpty());
}

TEST_CASE("No engine string prints a literal plural marker (card 033)", "[app][i18n][plurals]") {
    // The Qt-free libraries write plain text (command results, warnings, errors, parameter descriptions) that nothing
    // translates: "3 clip(s)" would reach the user as it is. They count in whole words with wabcut::counted(). The app's
    // own strings are checked above; "http(s)" is a URL scheme and Premiere's own command name "Meter Input(s) Only"
    // stays in the registry's premiereName.
    static const QRegularExpression marker(QStringLiteral(R"re([A-Za-z]\((?:s|es|ies)\))re"));
    static const QRegularExpression literalString(QStringLiteral(R"re("((?:[^"\\\n]|\\.)*)")re"));
    const QString srcRoot = QDir::cleanPath(QStringLiteral(WABCUT_APP_QML_DIR) + QStringLiteral("/../.."));
    QStringList violations;
    int files = 0;
    QDirIterator it(srcRoot, {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.contains(QLatin1String("/app/")) || path.contains(QLatin1String("/third_party/")))
            continue;
        ++files;
        for (const auto& [number, line] : codeLines(path)) {
            QRegularExpressionMatchIterator strings = literalString.globalMatch(line);
            while (strings.hasNext()) {
                const QString text = strings.next().captured(1);
                if (!marker.match(text).hasMatch() || text.contains(QLatin1String("http(s)")) ||
                    text == QLatin1String("Meter Input(s) Only"))
                    continue;
                violations << QStringLiteral("%1:%2: %3").arg(path).arg(number).arg(line.trimmed());
                break;
            }
        }
    }
    CHECK(files > 300);
    INFO("engine strings with a literal marker:\n" << violations.join(QLatin1Char('\n')).toStdString());
    CHECK(violations.isEmpty());
}

TEST_CASE("The engine libraries count in whole words (card 033)", "[app][i18n][plurals]") {
    CHECK(wabcut::counted(0, "clip") == "0 clips");
    CHECK(wabcut::counted(1, "clip") == "1 clip");
    CHECK(wabcut::counted(2, "media item") == "2 media items");
    CHECK(wabcut::counted(1, "match", "matches") == "1 match");
    CHECK(wabcut::counted(2, "match", "matches") == "2 matches");
    CHECK(wabcut::counted(12345678901LL, "frame") == "12345678901 frames");
}
