#pragma once

// Spell checking in the app (0.2 graphics, model/Spelling.h; QML singleton `Spelling` and the SpellHighlighter element).
//
//   * sharedSpelling() is the app's PersonalSpelling over the system checker (editor/SystemSpellChecker.h: Windows'
//     spell checking API): the user's dictionary lives in the app's data folder (spelling-dictionary.txt; the
//     WABCUT_SPELLING_DICTIONARY file when set), ignored words until the app quits. One for the process: every text
//     box, the Check Spelling dialog and the commands' SpellingServices use it.
//   * SpellHighlighter underlines the misspelled words of a TextEdit / TextArea (its textDocument) in the theme's
//     danger colour; it checks again when the text, the language or the dictionary changes.
//   * Spelling.issueAt() gives the right-click menu (SpellingMenu.qml) the word under the pointer and its suggestions;
//     addWord() is Add to Dictionary, ignoreWord() is Ignore All.
// Positions in QML are UTF-16 indices of the QString text.

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace wabcut {
class PersonalSpelling;
class SpellChecker;
} // namespace wabcut

namespace wabcut::app {

/// The app's spell checker with the user's dictionary (see above).
std::shared_ptr<PersonalSpelling> sharedSpelling();

/// `text` with [start, end) (UTF-16) replaced by `with`; an empty `with` (Delete, a repeated word) also takes one of
/// the spaces around it.
QString correctedText(const QString& text, int start, int end, const QString& with);

/// Tells every SpellHighlighter and Spelling singleton that the dictionary changed.
class SpellingNotifier : public QObject {
    Q_OBJECT
public:
    static SpellingNotifier& instance();
signals:
    void dictionaryChanged();
};

class SpellingBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Spelling)
    QML_SINGLETON

    /// The system can check the user's language (false: nothing is underlined).
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)
    /// The languages the system can check (BCP 47 tags).
    Q_PROPERTY(QStringList languages READ languages CONSTANT FINAL)
    /// The words added to the dictionary.
    Q_PROPERTY(QStringList words READ words NOTIFY changed FINAL)

public:
    explicit SpellingBridge(QObject* parent = nullptr);

    bool available() const;
    QStringList languages() const;
    QStringList words() const;

    /// The misspelled words of `text`: [{ start, end (UTF-16), word, action }].
    Q_INVOKABLE QVariantList check(const QString& text, const QString& language = {}) const;
    /// The misspelled word at UTF-16 `position` of `text` with its suggestions: { start, end, word, action,
    /// suggestions }; {} when the word there is right.
    Q_INVOKABLE QVariantMap issueAt(const QString& text, int position, const QString& language = {}) const;
    Q_INVOKABLE QStringList suggest(const QString& word, const QString& language = {}) const;
    /// `text` with [start, end) replaced by `with` (a suggestion chosen; Delete passes "").
    Q_INVOKABLE QString corrected(const QString& text, int start, int end, const QString& with) const;
    /// Add to Dictionary. False when the word cannot be added (spaces, too long) or the file could not be written.
    Q_INVOKABLE bool addWord(const QString& word);
    Q_INVOKABLE bool removeWord(const QString& word);
    /// Ignore All (until the app quits).
    Q_INVOKABLE void ignoreWord(const QString& word);

signals:
    void changed();
};

/// Underlines the misspelled words of a TextEdit's document (`document: edit.textDocument`).
class SpellHighlighter : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QQuickTextDocument* document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    /// BCP 47 / ISO 639-1; "" = the user's language.
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged FINAL)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    /// Misspelled words underlined now.
    Q_PROPERTY(int issueCount READ issueCount NOTIFY issuesChanged FINAL)

public:
    explicit SpellHighlighter(QObject* parent = nullptr);
    ~SpellHighlighter() override;

    QQuickTextDocument* document() const { return m_document; }
    void setDocument(QQuickTextDocument* document);
    QString language() const { return m_language; }
    void setLanguage(const QString& language);
    bool active() const { return m_active; }
    void setActive(bool active);
    QColor color() const { return m_color; }
    void setColor(const QColor& color);
    int issueCount() const;

    /// Checks the whole document again.
    Q_INVOKABLE void recheck();

signals:
    void documentChanged();
    void languageChanged();
    void activeChanged();
    void colorChanged();
    void issuesChanged();

private:
    class Highlighter;
    friend class Highlighter;
    void attach();

    QPointer<QQuickTextDocument> m_document;
    Highlighter* m_highlighter = nullptr;
    QString m_language;
    bool m_active = true;
    QColor m_color{0xE5, 0x48, 0x4D};
};

} // namespace wabcut::app
