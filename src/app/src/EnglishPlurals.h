#pragma once

// EnglishPluralTranslator: real English plurals for the "%n thing(s)" strings (0.7). Wabcut's user text is English and
// carries no translation file, so Qt's numerus handling ("%n clip(s)", with the count passed as qsTr()'s third argument
// or tr()'s) used to print the marker literally ("2 item(s)"). This translator, installed once at startup, turns the
// marker into the right form for the count: "(s)" and "(es)" are dropped for 1 and become "s" and "es" for any other
// number (0 included); "(ies)" stands for a noun that ends in -y: "y" for 1, "ies" for any other number. Qt then puts
// the count in for %n, as it does after every translator.
//
//   qsTr("%n clip(s) deleted", "", n)      ->  "1 clip deleted", "0 clips deleted", "5 clips deleted"
//   qsTr("%n match(es)", "", n)            ->  "1 match", "2 matches"
//   qsTr("%n prox(ies)", "", n)            ->  "1 proxy", "2 proxies"
//
// Strings without the marker, and calls without a count, fall through to Qt unchanged. A future translation file works
// beside it: install it after this one (it is asked first) and it replaces the strings it knows.

#include <QString>
#include <QTranslator>

namespace wabcut::app {

class EnglishPluralTranslator : public QTranslator {
    Q_OBJECT

public:
    using QTranslator::QTranslator;

    /// Always false: this translator has no catalog, but it answers (QCoreApplication skips translators that say empty).
    bool isEmpty() const override { return false; }
    /// `sourceText` with its plural markers resolved for `n`; a null string (not ours) when `n` is negative (no count
    /// was passed) or the text has no marker.
    QString translate(const char* context, const char* sourceText, const char* disambiguation, int n) const override;

    /// The resolution itself, for callers that format by hand and for tests: "(s)", "(es)" and "(ies)" in `text` for `n`.
    static QString resolve(const QString& text, int n);
};

} // namespace wabcut::app
