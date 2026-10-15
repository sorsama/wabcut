#include "BrandBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "ProjectController.h"

#include <wabcut/command/Command.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/ProjectState.h>

#include <QColor>
#include <QFile>
#include <QFontDatabase>
#include <QQmlEngine>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

using command::Json;

namespace {

constexpr std::string_view kKitFileFormat = "wabcut-brand-kit";
constexpr int kKitFileVersion = 1;
constexpr qint64 kMaxKitFileBytes = 1024 * 1024;

std::optional<MediaId> parseMediaKey(const QString& text) { return detail::parseId<MediaId>(QLatin1Char('m'), text); }

int channel(double v) { return std::clamp(static_cast<int>(std::lround(v * 255.0)), 0, 255); }

QString fromStd(const std::string& s) { return QString::fromStdString(s); }

/// For QML: a colour value (QML reads an 8-digit hex string as #AARRGGBB, the kit's hex is #RRGGBBAA).
QColor qcolor(const std::array<double, 4>& rgba) { return QColor::fromRgbF(rgba[0], rgba[1], rgba[2], rgba[3]); }

/// Whether the media item can be a logo: it has a picture (a still or a video) and is a file, not a generator.
bool pictureMedia(const MediaItem& item) { return !item.isGenerator() && !item.info.video.empty(); }

/// A unique name from `base` among `taken` ("Colour", "Colour 2", ...).
template <class Taken>
std::string uniqueName(const std::string& base, const Taken& taken) {
    if (std::find(taken.begin(), taken.end(), base) == taken.end())
        return base;
    for (int i = 2;; ++i) {
        const std::string name = base + " " + std::to_string(i);
        if (std::find(taken.begin(), taken.end(), name) == taken.end())
            return name;
    }
}

} // namespace

QString brandHex(const std::array<double, 4>& rgba) {
    QString hex = QStringLiteral("#%1%2%3")
                      .arg(channel(rgba[0]), 2, 16, QLatin1Char('0'))
                      .arg(channel(rgba[1]), 2, 16, QLatin1Char('0'))
                      .arg(channel(rgba[2]), 2, 16, QLatin1Char('0'));
    if (channel(rgba[3]) != 255)
        hex += QStringLiteral("%1").arg(channel(rgba[3]), 2, 16, QLatin1Char('0'));
    return hex.toUpper();
}

std::optional<std::array<double, 4>> brandRgba(const QString& text) {
    QString hex = text.trimmed();
    if (hex.startsWith(QLatin1Char('#')))
        hex.remove(0, 1);
    if (hex.size() != 6 && hex.size() != 8)
        return std::nullopt;
    bool ok = false;
    const uint value = hex.toUInt(&ok, 16);
    if (!ok)
        return std::nullopt;
    const bool alpha = hex.size() == 8;
    const auto byte = [&](int shift) { return static_cast<double>((value >> shift) & 0xFFu) / 255.0; };
    return alpha ? std::array<double, 4>{byte(24), byte(16), byte(8), byte(0)}
                 : std::array<double, 4>{byte(16), byte(8), byte(0), 1.0};
}

QVariantMap brandKitMap(const BrandKit& kit, const ProjectState& state) {
    QVariantList colors;
    for (const BrandColor& c : kit.colors)
        colors.append(QVariantMap{{QStringLiteral("name"), fromStd(c.name)},
                                  {QStringLiteral("hex"), brandHex(c.rgba)},
                                  {QStringLiteral("color"), qcolor(c.rgba)},
                                  {QStringLiteral("rgba"), QVariantList{c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]}}});
    QVariantList fonts;
    for (const BrandFont& f : kit.fonts)
        fonts.append(QVariantMap{{QStringLiteral("role"), fromStd(f.role)},
                                 {QStringLiteral("family"), fromStd(f.family)},
                                 {QStringLiteral("style"), fromStd(f.style)},
                                 {QStringLiteral("weight"), f.weight}});
    QVariantList logos;
    for (const BrandLogo& l : kit.logos) {
        const MediaItem* media = state.findMedia(l.media);
        logos.append(QVariantMap{{QStringLiteral("name"), fromStd(l.name)},
                                 {QStringLiteral("media"), mediaKey(l.media)},
                                 {QStringLiteral("mediaName"), media ? fromStd(media->name) : QString()}});
    }
    return {{QStringLiteral("name"), fromStd(kit.name)},
            {QStringLiteral("colors"), colors},
            {QStringLiteral("fonts"), fonts},
            {QStringLiteral("logos"), logos}};
}

bool looksLikeLottie(const QByteArray& bytes) {
    // An object whose first keys are the animation's: Bodymovin writes "v", "fr", "ip", "op", "w", "h" before the
    // (possibly huge) assets and layers. Whitespace and a UTF-8 BOM may lead.
    QByteArray head = bytes.left(64 * 1024);
    if (head.startsWith("\xEF\xBB\xBF"))
        head.remove(0, 3);
    head = head.trimmed();
    if (!head.startsWith('{'))
        return false;
    const auto hasKey = [&](const char* key) {
        const QByteArray quoted = QByteArray("\"") + key + "\"";
        for (qsizetype at = head.indexOf(quoted); at >= 0; at = head.indexOf(quoted, at + 1)) {
            qsizetype i = at + quoted.size();
            while (i < head.size() && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n'))
                ++i;
            if (i < head.size() && head[i] == ':')
                return true;
        }
        return false;
    };
    for (const char* key : {"v", "fr", "ip", "op", "w", "h"}) {
        if (!hasKey(key))
            return false;
    }
    return true;
}

BrandBridge::BrandBridge(QObject* parent) : QObject(parent) {}

BrandBridge* BrandBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new BrandBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void BrandBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &BrandBridge::onRevision);
        connect(controller, &ProjectController::sequenceChanged, this, &BrandBridge::onRevision);
    }
    m_seen = current();
    m_seenMedia = controller && controller->snapshot() ? controller->snapshot()->media.size() : 0;
    emit kitChanged();
    emit mediaChanged();
}

std::optional<BrandKit> BrandBridge::current() const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    return snap ? snap->brandKit : std::nullopt;
}

void BrandBridge::onRevision() {
    // Only when the kit or the media list changed: the kit is read by every colour picker (caption style, graphics).
    const std::optional<BrandKit> now = current();
    if (now != m_seen) {
        m_seen = now;
        emit kitChanged();
    }
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const std::size_t media = snap ? snap->media.size() : 0;
    if (media != m_seenMedia) {
        m_seenMedia = media;
        emit mediaChanged();
        emit kitChanged(); // a logo's media name
    }
}

bool BrandBridge::hasKit() const { return current().has_value(); }

QVariantMap BrandBridge::kit() const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    return snap && snap->brandKit ? brandKitMap(*snap->brandKit, *snap) : QVariantMap{};
}

QVariantList BrandBridge::colors() const {
    QVariantList out;
    if (const auto kit = current()) {
        for (const BrandColor& c : kit->colors)
            out.append(QVariantMap{{QStringLiteral("name"), fromStd(c.name)},
                                   {QStringLiteral("hex"), brandHex(c.rgba)},
                                   {QStringLiteral("color"), qcolor(c.rgba)}});
    }
    return out;
}

QVariantList BrandBridge::fontRoles() const {
    QVariantList out;
    if (const auto kit = current()) {
        for (const BrandFont& f : kit->fonts) {
            const QString role = fromStd(f.role);
            //: A brand font role and its family, e.g. "heading · Kanit"
            out.append(QVariantMap{{QStringLiteral("role"), role},
                                   {QStringLiteral("family"), fromStd(f.family)},
                                   {QStringLiteral("weight"), f.weight},
                                   {QStringLiteral("label"), tr("%1 · %2").arg(role, fromStd(f.family))}});
        }
    }
    return out;
}

QStringList BrandBridge::fontFamilies() const {
    if (m_families.isEmpty())
        m_families = QFontDatabase::families();
    return m_families;
}

QVariantList BrandBridge::logoCandidates() const {
    QVariantList out;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return out;
    for (const MediaPtr& m : snap->media) {
        if (pictureMedia(*m))
            out.append(QVariantMap{{QStringLiteral("key"), mediaKey(m->id)}, {QStringLiteral("name"), fromStd(m->name)}});
    }
    return out;
}

QString BrandBridge::colorHex(const QString& name) const {
    const auto kit = current();
    const BrandColor* c = kit ? kit->color(name.toStdString()) : nullptr;
    return c ? brandHex(c->rgba) : QString();
}

QString BrandBridge::fontFamilyOf(const QString& role) const {
    const auto kit = current();
    const BrandFont* f = kit ? kit->font(role.toStdString()) : nullptr;
    return f ? fromStd(f->family) : QString();
}

void BrandBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

QString BrandBridge::apply(const std::optional<BrandKit>& next) {
    if (!m_controller)
        return tr("No project");
    if (next) {
        if (const auto problem = brandKitProblem(*next)) {
            const QString error = tr("The brand kit is not valid: %1").arg(fromStd(*problem));
            setMessage(error);
            return error;
        }
    }
    const Json kit = next ? Json::parse(io::brandKitToJson(*next)) : Json(nullptr);
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "brand.set", Json{{"kit", kit}});
    const QString error = commandErrorText(result);
    setMessage(error);
    onRevision();
    return error;
}

QString BrandBridge::edit(const std::function<QString(BrandKit&)>& change) {
    std::optional<BrandKit> kit = current();
    if (!kit) {
        const QString error = tr("This project has no brand kit yet");
        setMessage(error);
        return error;
    }
    if (const QString refused = change(*kit); !refused.isEmpty()) {
        setMessage(refused);
        return refused;
    }
    if (kit == current())
        return {}; // nothing changed: no undo step
    return apply(kit);
}

QString BrandBridge::createKit(const QString& name) {
    BrandKit kit;
    kit.name = name.trimmed().isEmpty() ? tr("Brand kit").toStdString() : name.trimmed().toStdString();
    kit.colors = {BrandColor{"Primary", {0.42, 0.25, 0.98, 1.0}}, BrandColor{"Secondary", {0.07, 0.07, 0.09, 1.0}},
                  BrandColor{"Accent", {1.0, 0.82, 0.25, 1.0}}};
    const QFont sans = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    kit.fonts = {BrandFont{"heading", sans.family().toStdString(), {}, 700},
                 BrandFont{"body", sans.family().toStdString(), {}, 400}};
    return apply(kit);
}

QString BrandBridge::removeKit() {
    if (!current())
        return {};
    return apply(std::nullopt);
}

QString BrandBridge::setKit(const QVariantMap& map) {
    BrandKit kit;
    kit.name = map.value(QStringLiteral("name")).toString().trimmed().toStdString();
    for (const QVariant& v : map.value(QStringLiteral("colors")).toList()) {
        const QVariantMap c = v.toMap();
        const auto rgba = brandRgba(c.value(QStringLiteral("hex")).toString());
        if (!rgba)
            return tr("\"%1\" is not a colour").arg(c.value(QStringLiteral("hex")).toString());
        kit.colors.push_back(BrandColor{c.value(QStringLiteral("name")).toString().trimmed().toStdString(), *rgba});
    }
    for (const QVariant& v : map.value(QStringLiteral("fonts")).toList()) {
        const QVariantMap f = v.toMap();
        kit.fonts.push_back(BrandFont{f.value(QStringLiteral("role")).toString().trimmed().toStdString(),
                                      f.value(QStringLiteral("family")).toString().trimmed().toStdString(),
                                      f.value(QStringLiteral("style")).toString().trimmed().toStdString(),
                                      f.value(QStringLiteral("weight"), 400).toInt()});
    }
    for (const QVariant& v : map.value(QStringLiteral("logos")).toList()) {
        const QVariantMap l = v.toMap();
        const auto media = parseMediaKey(l.value(QStringLiteral("media")).toString());
        if (!media)
            return tr("Choose a picture for the logo \"%1\"").arg(l.value(QStringLiteral("name")).toString());
        kit.logos.push_back(BrandLogo{l.value(QStringLiteral("name")).toString().trimmed().toStdString(), *media});
    }
    return apply(kit);
}

QString BrandBridge::setName(const QString& name) {
    return edit([&](BrandKit& kit) {
        kit.name = name.trimmed().toStdString();
        return QString();
    });
}

QString BrandBridge::addColor(const QString& name, const QString& hex) {
    return edit([&](BrandKit& kit) {
        const auto rgba = brandRgba(hex.isEmpty() ? QStringLiteral("#FFFFFF") : hex);
        if (!rgba)
            return tr("\"%1\" is not a colour").arg(hex);
        std::vector<std::string> taken;
        for (const BrandColor& c : kit.colors)
            taken.push_back(c.name);
        const std::string base = name.trimmed().isEmpty() ? tr("Colour").toStdString() : name.trimmed().toStdString();
        kit.colors.push_back(BrandColor{uniqueName(base, taken), *rgba});
        return QString();
    });
}

QString BrandBridge::setColor(int index, const QString& name, const QString& hex) {
    return edit([&](BrandKit& kit) {
        if (index < 0 || index >= static_cast<int>(kit.colors.size()))
            return tr("No such colour");
        BrandColor& c = kit.colors[static_cast<std::size_t>(index)];
        if (!hex.trimmed().isEmpty()) {
            const auto rgba = brandRgba(hex);
            if (!rgba)
                return tr("\"%1\" is not a colour").arg(hex);
            c.rgba = *rgba;
        }
        if (!name.trimmed().isEmpty()) {
            const std::string next = name.trimmed().toStdString();
            for (std::size_t i = 0; i < kit.colors.size(); ++i) {
                if (static_cast<int>(i) != index && kit.colors[i].name == next)
                    return tr("The kit already has a colour named \"%1\"").arg(name.trimmed());
            }
            c.name = next;
        }
        return QString();
    });
}

QString BrandBridge::removeColor(int index) {
    return edit([&](BrandKit& kit) {
        if (index < 0 || index >= static_cast<int>(kit.colors.size()))
            return tr("No such colour");
        kit.colors.erase(kit.colors.begin() + index);
        return QString();
    });
}

QString BrandBridge::addFont(const QString& role) {
    return edit([&](BrandKit& kit) {
        std::vector<std::string> taken;
        for (const BrandFont& f : kit.fonts)
            taken.push_back(f.role);
        const std::string base = role.trimmed().isEmpty() ? std::string("role") : role.trimmed().toStdString();
        const QFont sans = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        kit.fonts.push_back(BrandFont{uniqueName(base, taken), sans.family().toStdString(), {}, 400});
        return QString();
    });
}

QString BrandBridge::setFont(int index, const QString& role, const QString& family, const QString& style, int weight) {
    return edit([&](BrandKit& kit) {
        if (index < 0 || index >= static_cast<int>(kit.fonts.size()))
            return tr("No such font role");
        BrandFont& f = kit.fonts[static_cast<std::size_t>(index)];
        if (!role.trimmed().isEmpty()) {
            const std::string next = role.trimmed().toStdString();
            for (std::size_t i = 0; i < kit.fonts.size(); ++i) {
                if (static_cast<int>(i) != index && kit.fonts[i].role == next)
                    return tr("The kit already has a \"%1\" font").arg(role.trimmed());
            }
            f.role = next;
        }
        if (!family.trimmed().isEmpty())
            f.family = family.trimmed().toStdString();
        f.style = style.trimmed().toStdString();
        if (weight > 0)
            f.weight = std::clamp(static_cast<int>(std::lround(weight / 100.0) * 100), 100, 900);
        return QString();
    });
}

QString BrandBridge::removeFont(int index) {
    return edit([&](BrandKit& kit) {
        if (index < 0 || index >= static_cast<int>(kit.fonts.size()))
            return tr("No such font role");
        kit.fonts.erase(kit.fonts.begin() + index);
        return QString();
    });
}

QString BrandBridge::addLogo(const QString& name, const QString& mediaKeyText) {
    return edit([&](BrandKit& kit) {
        const auto media = parseMediaKey(mediaKeyText);
        if (!media)
            return tr("Choose a picture for the logo");
        std::vector<std::string> taken;
        for (const BrandLogo& l : kit.logos)
            taken.push_back(l.name);
        const std::string base = name.trimmed().isEmpty() ? tr("Logo").toStdString() : name.trimmed().toStdString();
        kit.logos.push_back(BrandLogo{uniqueName(base, taken), *media});
        return QString();
    });
}

QString BrandBridge::setLogo(int index, const QString& name, const QString& mediaKeyText) {
    return edit([&](BrandKit& kit) {
        if (index < 0 || index >= static_cast<int>(kit.logos.size()))
            return tr("No such logo");
        BrandLogo& l = kit.logos[static_cast<std::size_t>(index)];
        if (!mediaKeyText.isEmpty()) {
            const auto media = parseMediaKey(mediaKeyText);
            if (!media)
                return tr("Choose a picture for the logo");
            l.media = *media;
        }
        if (!name.trimmed().isEmpty()) {
            const std::string next = name.trimmed().toStdString();
            for (std::size_t i = 0; i < kit.logos.size(); ++i) {
                if (static_cast<int>(i) != index && kit.logos[i].name == next)
                    return tr("The kit already has a logo named \"%1\"").arg(name.trimmed());
            }
            l.name = next;
        }
        return QString();
    });
}

QString BrandBridge::removeLogo(int index) {
    return edit([&](BrandKit& kit) {
        if (index < 0 || index >= static_cast<int>(kit.logos.size()))
            return tr("No such logo");
        kit.logos.erase(kit.logos.begin() + index);
        return QString();
    });
}

QString BrandBridge::exportKit(const QUrl& file) {
    const auto kit = current();
    if (!kit) {
        const QString error = tr("This project has no brand kit yet");
        setMessage(error);
        return error;
    }
    // Logos are media of this project: the file names them so another project can find its own media of that name.
    Json kitJson = Json::parse(io::brandKitToJson(*kit));
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (kitJson.contains("logos") && kitJson["logos"].is_array() && snap) {
        for (std::size_t i = 0; i < kit->logos.size() && i < kitJson["logos"].size(); ++i) {
            if (const MediaItem* m = snap->findMedia(kit->logos[i].media))
                kitJson["logos"][i]["media"] = m->name;
        }
    }
    const Json doc{{"format", std::string(kKitFileFormat)}, {"version", kKitFileVersion}, {"kit", kitJson}};
    QSaveFile out(file.toLocalFile());
    const QByteArray bytes = QByteArray::fromStdString(doc.dump(2));
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        const QString error = tr("Could not write %1").arg(file.toLocalFile());
        setMessage(error);
        return error;
    }
    setMessage({});
    return {};
}

QString BrandBridge::importKit(const QUrl& file) {
    QFile in(file.toLocalFile());
    if (!in.open(QIODevice::ReadOnly) || in.size() > kMaxKitFileBytes) {
        const QString error = tr("Could not read %1").arg(file.toLocalFile());
        setMessage(error);
        return error;
    }
    const Json doc = Json::parse(in.read(kMaxKitFileBytes).toStdString(), nullptr, false);
    if (!doc.is_object() || doc.value("format", std::string()) != kKitFileFormat || !doc.contains("kit")) {
        const QString error = tr("%1 is not a Wabcut brand kit").arg(file.fileName());
        setMessage(error);
        return error;
    }
    if (doc.value("version", 0) > kKitFileVersion) {
        const QString error = tr("%1 was saved by a newer Wabcut").arg(file.fileName());
        setMessage(error);
        return error;
    }
    // Logos name their media: keep those this project has (by name), leave the others out.
    Json kitJson = doc["kit"];
    int dropped = 0;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (kitJson.contains("logos") && kitJson["logos"].is_array()) {
        Json kept = Json::array();
        for (const Json& logo : kitJson["logos"]) {
            const MediaItem* found = nullptr;
            if (snap && logo.is_object() && logo.contains("media") && logo["media"].is_string()) {
                const std::string name = logo["media"].get<std::string>();
                for (const MediaPtr& m : snap->media) {
                    if (m->name == name && pictureMedia(*m)) {
                        found = m.get();
                        break;
                    }
                }
            }
            if (!found) {
                ++dropped;
                continue;
            }
            Json copy = logo;
            copy["media"] = found->id.value;
            kept.push_back(copy);
        }
        kitJson["logos"] = kept;
    }
    auto parsed = io::brandKitFromJson(kitJson.dump());
    if (!parsed) {
        const QString error = tr("%1 is not a valid brand kit: %2").arg(file.fileName(), fromStd(parsed.error().message));
        setMessage(error);
        return error;
    }
    const QString error = apply(*parsed);
    if (error.isEmpty() && dropped > 0)
        setMessage(tr("%n logo(s) of the kit are not in this project and were left out", "", dropped));
    return error;
}

} // namespace wabcut::app
