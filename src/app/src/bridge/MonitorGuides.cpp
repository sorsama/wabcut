#include "MonitorGuides.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>
#include <set>

namespace wabcut::app::guides {

namespace {

constexpr int kFileVersion = 1;
constexpr qsizetype kMaxFileBytes = 4 * 1024 * 1024;

QString tr(const char* text) { return QCoreApplication::translate("wabcut::app::MonitorGuides", text); }

} // namespace

double Guide::fraction(double width, double height) const {
    if (percent)
        return value / 100.0;
    const double extent = vertical ? width : height;
    return extent > 0.0 ? value / extent : 0.0;
}

double Guide::valueAt(double f, double width, double height) const {
    if (percent)
        return f * 100.0;
    return f * (vertical ? width : height);
}

QColor defaultColor() { return QColor(0x40, 0xC8, 0xF0); }

QJsonArray toJson(const std::vector<Guide>& guides) {
    QJsonArray out;
    for (const Guide& g : guides) {
        out.append(QJsonObject{{QStringLiteral("orientation"), g.vertical ? QStringLiteral("vertical") : QStringLiteral("horizontal")},
                               {QStringLiteral("position"), g.value},
                               {QStringLiteral("units"), g.percent ? QStringLiteral("percent") : QStringLiteral("pixels")},
                               {QStringLiteral("color"), g.color.name(QColor::HexRgb)}});
    }
    return out;
}

std::expected<std::vector<Guide>, QString> fromJson(const QJsonArray& array) {
    if (array.size() > kMaxGuides)
        return std::unexpected(tr("More than %1 guides").arg(kMaxGuides));
    std::vector<Guide> out;
    out.reserve(static_cast<std::size_t>(array.size()));
    for (const QJsonValue& v : array) {
        if (!v.isObject())
            return std::unexpected(tr("A guide is not an object"));
        const QJsonObject o = v.toObject();
        Guide g;
        const QString orientation = o.value(QStringLiteral("orientation")).toString();
        if (orientation != QLatin1String("horizontal") && orientation != QLatin1String("vertical"))
            return std::unexpected(tr("A guide is neither horizontal nor vertical"));
        g.vertical = orientation == QLatin1String("vertical");
        const QString units = o.value(QStringLiteral("units")).toString(QStringLiteral("pixels"));
        if (units != QLatin1String("pixels") && units != QLatin1String("percent"))
            return std::unexpected(tr("A guide's units are neither pixels nor percent"));
        g.percent = units == QLatin1String("percent");
        const QJsonValue position = o.value(QStringLiteral("position"));
        if (!position.isDouble() || !std::isfinite(position.toDouble()) ||
            std::abs(position.toDouble()) > (g.percent ? kMaxPercent : kMaxPixels))
            return std::unexpected(tr("A guide's position is out of range"));
        g.value = position.toDouble();
        const QString color = o.value(QStringLiteral("color")).toString();
        g.color = color.isEmpty() ? defaultColor() : QColor::fromString(color);
        if (!g.color.isValid() || color.size() > 32)
            return std::unexpected(tr("A guide's colour is not a colour"));
        g.color.setAlpha(255);
        out.push_back(g);
    }
    return out;
}

QJsonArray templatesToJson(const std::vector<Template>& templates) {
    QJsonArray out;
    for (const Template& t : templates)
        out.append(QJsonObject{{QStringLiteral("name"), t.name}, {QStringLiteral("guides"), toJson(t.guides)}});
    return out;
}

std::expected<std::vector<Template>, QString> templatesFromJson(const QJsonArray& array) {
    if (array.size() > kMaxTemplates)
        return std::unexpected(tr("More than %1 guide templates").arg(kMaxTemplates));
    std::vector<Template> out;
    std::set<QString> names;
    for (const QJsonValue& v : array) {
        if (!v.isObject())
            return std::unexpected(tr("A guide template is not an object"));
        const QJsonObject o = v.toObject();
        Template t;
        t.name = o.value(QStringLiteral("name")).toString().trimmed();
        if (t.name.isEmpty() || t.name.size() > kMaxNameLength)
            return std::unexpected(tr("A guide template has no name, or one longer than %1 characters").arg(kMaxNameLength));
        if (!names.insert(t.name.toCaseFolded()).second)
            return std::unexpected(tr("Two guide templates are called %1").arg(t.name));
        auto guides = fromJson(o.value(QStringLiteral("guides")).toArray());
        if (!guides)
            return std::unexpected(guides.error());
        t.guides = std::move(*guides);
        out.push_back(std::move(t));
    }
    return out;
}

QByteArray exportFile(const std::vector<Template>& templates) {
    const QJsonObject root{{QStringLiteral("format"), QStringLiteral("wabcut-guides")},
                           {QStringLiteral("version"), kFileVersion},
                           {QStringLiteral("templates"), templatesToJson(templates)}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::expected<std::vector<Template>, QString> importFile(const QByteArray& data) {
    if (data.size() > kMaxFileBytes)
        return std::unexpected(tr("The file is too large for a guides file"));
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::unexpected(tr("This is not a guides file"));
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("wabcut-guides"))
        return std::unexpected(tr("This is not a guides file"));
    if (root.value(QStringLiteral("version")).toInt(0) > kFileVersion || root.value(QStringLiteral("version")).toInt(0) < 1)
        return std::unexpected(tr("The guides file comes from a newer version of Wabcut"));
    return templatesFromJson(root.value(QStringLiteral("templates")).toArray());
}

} // namespace wabcut::app::guides
