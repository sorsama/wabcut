#include "SpellingBridge.h"

#include <wabcut/editor/SystemSpellChecker.h>
#include <wabcut/model/Spelling.h>

#include <QDir>
#include <QStandardPaths>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>

#include <mutex>

namespace wabcut::app {

namespace {

/// A QString as UTF-8 and the UTF-16 index of every byte offset (plus the end).
struct Utf8Text {
    std::string bytes;
    std::vector<int> utf16;
};

Utf8Text utf8Of(const QString& text) {
    Utf8Text out;
    out.bytes.reserve(static_cast<std::size_t>(text.size()));
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t c = text.at(i).unicode();
        const qsizetype at = i;
        if (QChar::isHighSurrogate(c) && i + 1 < text.size() && QChar::isLowSurrogate(text.at(i + 1).unicode())) {
            c = QChar::surrogateToUcs4(static_cast<char16_t>(c), text.at(i + 1).unicode());
            ++i;
        } else if (QChar::isSurrogate(c)) {
            c = 0xFFFD;
        }
        const std::size_t before = out.bytes.size();
        if (c < 0x80) {
            out.bytes.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.bytes.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.bytes.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.bytes.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.bytes.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.bytes.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.bytes.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.bytes.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.bytes.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.bytes.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        for (std::size_t k = before; k < out.bytes.size(); ++k) {
            out.utf16.push_back(static_cast<int>(at));
        }
    }
    out.utf16.push_back(static_cast<int>(text.size()));
    return out;
}

QString qs(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QStringList qlist(const std::vector<std::string>& v) {
    QStringList out;
    for (const std::string& s : v) {
        out << qs(s);
    }
    return out;
}

QVariantMap issueMap(const SpellIssue& issue, const Utf8Text& text) {
    const auto at = [&](std::size_t byte) { return text.utf16[std::min(byte, text.utf16.size() - 1)]; };
    return QVariantMap{{QStringLiteral("start"), at(issue.start)},
                       {QStringLiteral("end"), at(issue.end)},
                       {QStringLiteral("word"), qs(issue.word)},
                       {QStringLiteral("action"), QString::fromLatin1(toString(issue.action).data(),
                                                                      static_cast<qsizetype>(toString(issue.action).size()))},
                       {QStringLiteral("suggestions"), qlist(issue.suggestions)}};
}

} // namespace

std::shared_ptr<PersonalSpelling> sharedSpelling() {
    static std::once_flag once;
    static std::shared_ptr<PersonalSpelling> spelling;
    std::call_once(once, [] {
        const QString env = qEnvironmentVariable("WABCUT_SPELLING_DICTIONARY");
        const QString file = !env.isEmpty() ? env
                                            : QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                                  .filePath(QStringLiteral("spelling-dictionary.txt"));
        spelling = std::make_shared<PersonalSpelling>(editor::systemSpellChecker(), std::filesystem::path(file.toStdU16String()));
    });
    return spelling;
}

SpellingNotifier& SpellingNotifier::instance() {
    static SpellingNotifier notifier;
    return notifier;
}

// ---- Spelling ---------------------------------------------------------------------------------------------------------

SpellingBridge::SpellingBridge(QObject* parent) : QObject(parent) {
    connect(&SpellingNotifier::instance(), &SpellingNotifier::dictionaryChanged, this, &SpellingBridge::changed);
}

bool SpellingBridge::available() const { return sharedSpelling()->supports({}); }

QStringList SpellingBridge::languages() const { return qlist(editor::systemSpellingLanguages()); }

QStringList SpellingBridge::words() const { return qlist(sharedSpelling()->words()); }

QVariantList SpellingBridge::check(const QString& text, const QString& language) const {
    QVariantList out;
    const Utf8Text t = utf8Of(text);
    for (const SpellIssue& issue : sharedSpelling()->check(t.bytes, language.toStdString())) {
        out.push_back(issueMap(issue, t));
    }
    return out;
}

QVariantMap SpellingBridge::issueAt(const QString& text, int position, const QString& language) const {
    const Utf8Text t = utf8Of(text);
    const std::string lang = language.toStdString();
    const auto spelling = sharedSpelling();
    for (SpellIssue& issue : spelling->check(t.bytes, lang)) {
        const int start = t.utf16[issue.start];
        const int end = t.utf16[std::min(issue.end, t.utf16.size() - 1)];
        if (position >= start && position <= end) {
            if (issue.suggestions.empty() && issue.action != SpellAction::Delete) {
                issue.suggestions = spelling->suggest(issue.word, lang);
            }
            return issueMap(issue, t);
        }
    }
    return {};
}

QStringList SpellingBridge::suggest(const QString& word, const QString& language) const {
    return qlist(sharedSpelling()->suggest(word.toStdString(), language.toStdString()));
}

QString SpellingBridge::corrected(const QString& text, int start, int end, const QString& with) const {
    return correctedText(text, start, end, with);
}

QString correctedText(const QString& text, int start, int end, const QString& with) {
    start = std::clamp(start, 0, static_cast<int>(text.size()));
    end = std::clamp(end, start, static_cast<int>(text.size()));
    QString out = text.left(start) + with + text.mid(end);
    if (with.isEmpty()) {
        // Delete (a repeated word): one of the spaces around it goes too.
        if (start > 0 && start <= out.size() && out.at(start - 1).isSpace() &&
            (start == out.size() || out.at(start).isSpace())) {
            out.remove(start - 1, 1);
        }
    }
    return out;
}

bool SpellingBridge::addWord(const QString& word) {
    const bool ok = sharedSpelling()->addWord(word.toStdString());
    emit SpellingNotifier::instance().dictionaryChanged();
    return ok;
}

bool SpellingBridge::removeWord(const QString& word) {
    const bool ok = sharedSpelling()->removeWord(word.toStdString());
    if (ok) {
        emit SpellingNotifier::instance().dictionaryChanged();
    }
    return ok;
}

void SpellingBridge::ignoreWord(const QString& word) {
    sharedSpelling()->ignoreWord(word.toStdString());
    emit SpellingNotifier::instance().dictionaryChanged();
}

// ---- SpellHighlighter -------------------------------------------------------------------------------------------------

class SpellHighlighter::Highlighter final : public QSyntaxHighlighter {
public:
    Highlighter(SpellHighlighter* owner, QTextDocument* document) : QSyntaxHighlighter(document), m_owner(owner) {}

    int issues() const {
        int n = 0;
        for (QTextBlock b = document() ? document()->begin() : QTextBlock(); b.isValid(); b = b.next()) {
            n += b.userState() > 0 ? b.userState() : 0;
        }
        return n;
    }

protected:
    void highlightBlock(const QString& text) override {
        int count = 0;
        if (m_owner->m_active && !text.isEmpty()) {
            QTextCharFormat format;
            format.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
            format.setUnderlineColor(m_owner->m_color);
            format.setFontUnderline(true);
            const Utf8Text t = utf8Of(text);
            for (const SpellIssue& issue : sharedSpelling()->check(t.bytes, m_owner->m_language.toStdString())) {
                const int start = t.utf16[issue.start];
                const int end = t.utf16[std::min(issue.end, t.utf16.size() - 1)];
                if (end > start) {
                    setFormat(start, end - start, format);
                    ++count;
                }
            }
        }
        setCurrentBlockState(count);
    }

private:
    SpellHighlighter* m_owner;
};

SpellHighlighter::SpellHighlighter(QObject* parent) : QObject(parent) {
    connect(&SpellingNotifier::instance(), &SpellingNotifier::dictionaryChanged, this, &SpellHighlighter::recheck);
}

SpellHighlighter::~SpellHighlighter() {
    // The highlighter belongs to the document, which may outlive this item: it must not call back into it.
    delete m_highlighter;
}

void SpellHighlighter::setDocument(QQuickTextDocument* document) {
    if (m_document == document) {
        return;
    }
    m_document = document;
    attach();
    emit documentChanged();
}

void SpellHighlighter::setLanguage(const QString& language) {
    if (m_language == language) {
        return;
    }
    m_language = language;
    recheck();
    emit languageChanged();
}

void SpellHighlighter::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    recheck();
    emit activeChanged();
}

void SpellHighlighter::setColor(const QColor& color) {
    if (m_color == color) {
        return;
    }
    m_color = color;
    recheck();
    emit colorChanged();
}

int SpellHighlighter::issueCount() const { return m_highlighter ? m_highlighter->issues() : 0; }

void SpellHighlighter::recheck() {
    if (m_highlighter) {
        m_highlighter->rehighlight();
        emit issuesChanged();
    }
}

void SpellHighlighter::attach() {
    delete m_highlighter;
    m_highlighter = nullptr;
    QTextDocument* doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc) {
        emit issuesChanged();
        return;
    }
    m_highlighter = new Highlighter(this, doc); // the document owns it (QSyntaxHighlighter's parent)
    connect(doc, &QTextDocument::contentsChanged, this, &SpellHighlighter::issuesChanged);
    connect(m_highlighter, &QObject::destroyed, this, [this] { m_highlighter = nullptr; });
    m_highlighter->rehighlight();
    emit issuesChanged();
}

} // namespace wabcut::app
