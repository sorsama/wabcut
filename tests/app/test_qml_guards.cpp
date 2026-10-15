// M5j v2 polish guards over the QML sources (no engine needed):
//   * colours come from Theme: no hard-coded hex colour outside theme/Theme.qml;
//   * sentence case everywhere: no all-caps label except acronyms and file formats, and Font.AllUppercase only on
//     micro-tags (Theme.textMicro, 10 px).

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <algorithm>

namespace {

struct SourceLine {
    QString file; ///< relative to the QML directory
    int number = 0;
    QString text;
};

QList<SourceLine> qmlLines() {
    const QDir root(QStringLiteral(WABCUT_APP_QML_DIR));
    QList<SourceLine> lines;
    QDirIterator it(root.path(), {QStringLiteral("*.qml"), QStringLiteral("*.js")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QTextStream in(&file);
        int number = 0;
        while (!in.atEnd())
            lines.append({root.relativeFilePath(path), ++number, in.readLine()});
    }
    return lines;
}

QString where(const SourceLine& line) {
    return line.file + QLatin1Char(':') + QString::number(line.number) + QStringLiteral(": ") + line.text.trimmed();
}

} // namespace

TEST_CASE("QML colours come from Theme: no hard-coded hex colour outside Theme.qml", "[app][design]") {
    const QRegularExpression hex(QStringLiteral(R"(["']#[0-9A-Fa-f]{3,8}["'])"));
    const QList<SourceLine> lines = qmlLines();
    REQUIRE(lines.size() > 1000); // the scan found the sources
    QStringList offenders;
    for (const SourceLine& line : lines) {
        if (line.file == QLatin1String("theme/Theme.qml"))
            continue;
        if (hex.match(line.text).hasMatch())
            offenders << where(line);
    }
    INFO("Use a Theme token (theme/Theme.qml) instead:\n" << offenders.join(QLatin1Char('\n')).toStdString());
    CHECK(offenders.isEmpty());
}

TEST_CASE("QML labels are sentence case: all caps only for acronyms and micro-tags", "[app][design]") {
    // Acronyms, units and file formats keep their capitals.
    const QSet<QString> acronyms{QStringLiteral("LUFS"),  QStringLiteral("HEVC"), QStringLiteral("FCPXML"),
                                 QStringLiteral("OTIO"),  QStringLiteral("SDR"),  QStringLiteral("HDR"),
                                 QStringLiteral("PROXY"), QStringLiteral("JSON"), QStringLiteral("WAV"),
                                 QStringLiteral("MP4"),   QStringLiteral("MOV"),  QStringLiteral("MKV")};
    const QRegularExpression label(QStringLiteral(R"(qsTr\("([^"]+)\")"));
    const QRegularExpression capsWord(QStringLiteral(R"(\b[A-Z]{4,}\b)"));
    const QRegularExpression lower(QStringLiteral("[a-z]"));
    const QList<SourceLine> lines = qmlLines();
    QStringList offenders;
    for (int i = 0; i < lines.size(); ++i) {
        const SourceLine& line = lines[i];
        for (auto m = label.globalMatch(line.text); m.hasNext();) {
            const QString text = m.next().captured(1);
            if (lower.match(QString(text).remove(QRegularExpression(QStringLiteral("%[0-9n]")))).hasMatch())
                continue; // has lower case: sentence case or a mixed name ("ProRes")
            for (auto w = capsWord.globalMatch(text); w.hasNext();) {
                if (!acronyms.contains(w.next().captured(0))) {
                    offenders << where(line);
                    break;
                }
            }
        }
        // Font.AllUppercase: only at the micro-tag size, set within the same few lines.
        if (line.text.contains(QLatin1String("Font.AllUppercase"))) {
            bool micro = false;
            for (int j = std::max(0, i - 8); j < std::min<int>(static_cast<int>(lines.size()), i + 8); ++j)
                micro = micro || (lines[j].file == line.file && lines[j].text.contains(QLatin1String("textMicro")));
            if (!micro)
                offenders << where(line);
        }
    }
    INFO("Sentence case above Theme.textMicro (design language v2):\n"
         << offenders.join(QLatin1Char('\n')).toStdString());
    CHECK(offenders.isEmpty());
}
