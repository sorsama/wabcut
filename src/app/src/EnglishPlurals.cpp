#include "EnglishPlurals.h"

namespace wabcut::app {

QString EnglishPluralTranslator::resolve(const QString& text, int n) {
    QString out = text;
    const bool one = n == 1;
    out.replace(QStringLiteral("(ies)"), one ? QStringLiteral("y") : QStringLiteral("ies"));
    out.replace(QStringLiteral("(es)"), one ? QString() : QStringLiteral("es"));
    out.replace(QStringLiteral("(s)"), one ? QString() : QStringLiteral("s"));
    return out;
}

QString EnglishPluralTranslator::translate(const char*, const char* sourceText, const char*, int n) const {
    if (n < 0 || !sourceText) {
        return {};
    }
    const QString text = QString::fromUtf8(sourceText);
    if (!text.contains(QLatin1String("(s)")) && !text.contains(QLatin1String("(es)")) &&
        !text.contains(QLatin1String("(ies)"))) {
        return {};
    }
    return resolve(text, n);
}

} // namespace wabcut::app
