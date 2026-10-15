#include "Titles.h"

#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/TitleOps.h>

#include <QColor>
#include <QJsonDocument>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

const QStringList& styleKeys() {
    static const QStringList keys{
        QStringLiteral("text"),          QStringLiteral("font"),          QStringLiteral("size"),
        QStringLiteral("color"),         QStringLiteral("bold"),          QStringLiteral("alignment"),
        QStringLiteral("strokeEnabled"), QStringLiteral("strokeColor"),   QStringLiteral("strokeWidth"),
        QStringLiteral("shadowEnabled"), QStringLiteral("shadowColor"),   QStringLiteral("shadowDistance"),
        QStringLiteral("shadowSoftness"), QStringLiteral("preset"),
    };
    return keys;
}

struct FontChoice {
    const char* key;
    const char* label;
    const char* family;
};
constexpr FontChoice kFonts[] = {
    {"sans", QT_TRANSLATE_NOOP("wabcut::app::TitlesBridge", "Sans"), "Arial"},
    {"serif", QT_TRANSLATE_NOOP("wabcut::app::TitlesBridge", "Serif"), "Georgia"},
    {"mono", QT_TRANSLATE_NOOP("wabcut::app::TitlesBridge", "Mono"), "Courier New"},
    {"display", QT_TRANSLATE_NOOP("wabcut::app::TitlesBridge", "Display"), "Impact"},
};

const char* familyOf(const QString& key) {
    for (const FontChoice& f : kFonts) {
        if (key == QLatin1String(f.key))
            return f.family;
    }
    return kFonts[0].family;
}

QString fontKeyOf(const std::string& family) {
    for (const FontChoice& f : kFonts) {
        if (family == f.family)
            return QLatin1String(f.key);
    }
    return QStringLiteral("sans");
}

QColor colorOf(const QVariant& v) {
    const QColor c = v.value<QColor>().isValid() ? v.value<QColor>() : QColor(v.toString());
    return c.isValid() ? c : QColor(Qt::white);
}

QString hex(const QColor& c) {
    return c.name(QColor::HexRgb).toUpper();
}

QString alignmentName(double v) {
    return v < 0.5 ? QStringLiteral("left") : v > 1.5 ? QStringLiteral("right") : QStringLiteral("center");
}

double alignmentValue(const QString& name) {
    return name == QLatin1String("left") ? 0.0 : name == QLatin1String("right") ? 2.0 : 1.0;
}

constexpr double kShadowOpacity = 0.6; // a shadow switched on

/// The style effect's parameters for UI style values (only the keys present in `values`).
std::vector<std::pair<std::string, double>> styleParams(const QVariantMap& values, const QVariantMap& style) {
    std::vector<std::pair<std::string, double>> out;
    const auto rgb = [&](const char* prefix, const QVariant& v) {
        const QColor c = colorOf(v);
        out.emplace_back(std::string(prefix) + ".red", c.redF());
        out.emplace_back(std::string(prefix) + ".green", c.greenF());
        out.emplace_back(std::string(prefix) + ".blue", c.blueF());
    };
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const QString& key = it.key();
        if (key == QLatin1String("size"))
            out.emplace_back("size", it.value().toDouble());
        else if (key == QLatin1String("color"))
            rgb("fill", it.value());
        else if (key == QLatin1String("strokeColor"))
            rgb("stroke", it.value());
        else if (key == QLatin1String("strokeEnabled"))
            out.emplace_back("stroke.width", it.value().toBool() ? style.value(QStringLiteral("strokeWidth")).toDouble() : 0.0);
        else if (key == QLatin1String("strokeWidth") && style.value(QStringLiteral("strokeEnabled")).toBool())
            out.emplace_back("stroke.width", it.value().toDouble());
        else if (key == QLatin1String("shadowEnabled"))
            out.emplace_back("shadow.opacity", it.value().toBool() ? kShadowOpacity : 0.0);
        else if (key == QLatin1String("shadowDistance"))
            out.emplace_back("shadow.distance", it.value().toDouble());
        else if (key == QLatin1String("shadowSoftness"))
            out.emplace_back("shadow.softness", it.value().toDouble());
        else if (key == QLatin1String("alignment"))
            out.emplace_back("alignment", alignmentValue(it.value().toString()));
    }
    return out;
}

} // namespace

TitlesBridge::TitlesBridge(QObject* parent) : QObject(parent), m_style(defaultStyle()), m_draft(m_style) {}

TitlesBridge* TitlesBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* titles = new TitlesBridge;
    if (qmlEngine)
        titles->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return titles;
}

void TitlesBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::selectionChanged, this, &TitlesBridge::syncFromSelection);
        connect(m_controller, &ProjectController::revisionChanged, this, &TitlesBridge::syncFromSelection);
        connect(m_controller, &ProjectController::playheadChanged, this, &TitlesBridge::syncFromSelection);
    }
    syncFromSelection();
}

QVariantMap TitlesBridge::defaultStyle() {
    return {
        {QStringLiteral("text"), tr("Your Title")},
        {QStringLiteral("font"), QStringLiteral("sans")},
        {QStringLiteral("size"), 96.0},
        {QStringLiteral("color"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("bold"), true},
        {QStringLiteral("alignment"), QStringLiteral("center")},
        {QStringLiteral("strokeEnabled"), false},
        {QStringLiteral("strokeColor"), QStringLiteral("#101013")},
        {QStringLiteral("strokeWidth"), 4.0},
        {QStringLiteral("shadowEnabled"), true},
        {QStringLiteral("shadowColor"), QStringLiteral("#000000")},
        {QStringLiteral("shadowDistance"), 6.0},
        {QStringLiteral("shadowSoftness"), 12.0},
        {QStringLiteral("preset"), QString()},
    };
}

bool TitlesBridge::available() const {
    return command::builtinCommands().find(kCommand) != nullptr;
}

QString TitlesBridge::unavailableReason() const {
    return available() ? QString() : tr("Titles need the title.add command");
}

QVariantList TitlesBridge::fonts() const {
    QVariantList out;
    for (const FontChoice& f : kFonts)
        out.append(QVariantMap{{QStringLiteral("key"), QLatin1String(f.key)},
                               {QStringLiteral("label"), tr(f.label)},
                               {QStringLiteral("family"), QLatin1String(f.family)}});
    return out;
}

void TitlesBridge::syncFromSelection() {
    if (m_applying)
        return;
    ClipId clip;
    MediaId media;
    EffectInstanceId styleEffect;
    QString name;
    QVariantMap style = m_draft;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = seq && m_controller->primaryClip() ? seq->findClip(m_controller->primaryClip()) : std::nullopt;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* item = loc && snap ? snap->findMedia(loc->clip().media) : nullptr;
    if (item && item->isTitle()) {
        const Clip& c = loc->clip();
        clip = c.id;
        media = item->id;
        name = QString::fromStdString(c.name());
        style.insert(QStringLiteral("text"), QString::fromStdString(item->generator->text));
        style.insert(QStringLiteral("font"), fontKeyOf(item->generator->fontFamily));
        style.insert(QStringLiteral("bold"), QString::fromStdString(item->generator->fontStyle).contains(QLatin1String("Bold")));
        for (const EffectInstance& e : c.effects()) {
            if (e.effect != edit::kTitleStyleEffect)
                continue;
            styleEffect = e.id;
            const Ticks t = m_controller->curveTimeAtPlayhead(c);
            const auto value = [&](const char* key) { return effectParameterAt(e, key, t); };
            const auto rgb = [&](const char* prefix) {
                return hex(QColor::fromRgbF(static_cast<float>(value((std::string(prefix) + ".red").c_str())),
                                            static_cast<float>(value((std::string(prefix) + ".green").c_str())),
                                            static_cast<float>(value((std::string(prefix) + ".blue").c_str()))));
            };
            style.insert(QStringLiteral("size"), value("size"));
            style.insert(QStringLiteral("color"), rgb("fill"));
            style.insert(QStringLiteral("strokeColor"), rgb("stroke"));
            const double stroke = value("stroke.width");
            style.insert(QStringLiteral("strokeEnabled"), stroke > 0.0);
            if (stroke > 0.0)
                style.insert(QStringLiteral("strokeWidth"), stroke);
            style.insert(QStringLiteral("shadowEnabled"), value("shadow.opacity") > 0.0);
            style.insert(QStringLiteral("shadowDistance"), value("shadow.distance"));
            style.insert(QStringLiteral("shadowSoftness"), value("shadow.softness"));
            style.insert(QStringLiteral("alignment"), alignmentName(value("alignment")));
        }
    }
    const bool targetChangedNow = clip != m_clip || name != m_targetName;
    m_clip = clip;
    m_media = media;
    m_styleEffect = styleEffect;
    m_targetName = name;
    if (style != m_style) {
        m_style = style;
        emit styleChanged();
    }
    if (targetChangedNow)
        emit targetChanged();
}

void TitlesBridge::applyToTarget(const QVariantMap& values, const char* label, const std::string& mergeKey) {
    if (!m_controller || !m_clip)
        return;
    m_applying = true;
    const MediaId media = m_media;
    if (values.contains(QStringLiteral("text"))) {
        const std::string text = values.value(QStringLiteral("text")).toString().toStdString();
        m_controller->applyEditFn(LABEL("Edit Title Text"),
                                  [&](const ProjectState& state) { return edit::setTitleText(state, media, text); },
                                  "title-text:" + std::to_string(media.value));
    }
    if (values.contains(QStringLiteral("font")) || values.contains(QStringLiteral("bold"))) {
        const std::string family = familyOf(m_style.value(QStringLiteral("font")).toString());
        const std::string fontStyle = m_style.value(QStringLiteral("bold")).toBool() ? "Bold" : "Regular";
        m_controller->applyEditFn(LABEL("Set Title Font"), [&](const ProjectState& state) {
            return edit::setTitleFont(state, media, family, fontStyle);
        }, mergeKey);
    }
    if (m_styleEffect) {
        for (const auto& [key, value] : styleParams(values, m_style))
            m_controller->setParameterAtPlayhead(m_clip, edit::ParamRef::ofEffect(m_styleEffect, key), value, label, mergeKey);
    }
    m_applying = false;
    syncFromSelection();
}

void TitlesBridge::setStyleValue(const QString& key, const QVariant& value) {
    if (!styleKeys().contains(key) || m_style.value(key) == value)
        return;
    m_style.insert(key, value);
    if (key != QLatin1String("preset") && key != QLatin1String("text"))
        m_style.insert(QStringLiteral("preset"), QString()); // edited: no longer exactly the preset
    if (m_clip) {
        applyToTarget({{key, value}}, LABEL("Title Style"), "title-style:" + std::to_string(m_clip.value) + ":" + key.toStdString());
        // Switches, swatches and choices are one step each; drags and typing merge until endEdit().
        static const QStringList kContinuous{QStringLiteral("size"), QStringLiteral("strokeWidth"), QStringLiteral("shadowDistance"),
                                             QStringLiteral("shadowSoftness"), QStringLiteral("text")};
        if (!kContinuous.contains(key))
            endEdit();
    } else {
        m_draft = m_style;
    }
    emit styleChanged();
}

void TitlesBridge::endEdit() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

void TitlesBridge::applyPreset(const QVariantMap& preset) {
    QVariantMap values;
    for (auto it = preset.cbegin(); it != preset.cend(); ++it) {
        if (styleKeys().contains(it.key()) && it.key() != QLatin1String("text") && m_style.value(it.key()) != it.value())
            values.insert(it.key(), it.value());
    }
    if (values.isEmpty())
        return;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        m_style.insert(it.key(), it.value());
    if (m_clip) {
        static int counter = 0;
        applyToTarget(values, LABEL("Apply Title Style"), "title-preset:" + std::to_string(++counter));
        endEdit();
        m_style.insert(QStringLiteral("preset"), preset.value(QStringLiteral("preset")));
    } else {
        m_draft = m_style;
    }
    emit styleChanged();
}

void TitlesBridge::resetStyle() {
    QVariantMap values = defaultStyle();
    values.remove(QStringLiteral("text"));
    applyPreset(values);
}

QString TitlesBridge::trackForNewTitle() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || seq->videoTracks.empty())
        return QStringLiteral("V1");
    // From the topmost targeted video track up: the first unlocked track free for the title's length (a new
    // graphic should not overwrite the picture under it).
    std::size_t from = 0;
    for (std::size_t i = seq->videoTracks.size(); i-- > 0;) {
        if (seq->videoTracks[i]->targeted) {
            from = i;
            break;
        }
    }
    const Ticks at = m_controller->playhead();
    const TimeRange span{at, edit::kDefaultStillDuration};
    for (std::size_t i = from; i < seq->videoTracks.size(); ++i) {
        const Track& t = *seq->videoTracks[i];
        if (t.locked)
            continue;
        const bool free = std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) {
            return c.start() < span.end() && c.end() > span.start;
        });
        if (free)
            return QStringLiteral("V%1").arg(i + 1);
    }
    return QStringLiteral("V%1").arg(seq->videoTracks.size());
}

QVariantMap TitlesBridge::commandParams() const {
    const QVariantMap& s = m_clip ? m_draft : m_style;
    QVariantMap style;
    for (const auto& [key, value] : styleParams(s, s))
        style.insert(QString::fromStdString(key), value);
    QVariantMap params{
        {QStringLiteral("text"), s.value(QStringLiteral("text")).toString()},
        {QStringLiteral("fontFamily"), QString::fromLatin1(familyOf(s.value(QStringLiteral("font")).toString()))},
        {QStringLiteral("fontStyle"), s.value(QStringLiteral("bold")).toBool() ? QStringLiteral("Bold") : QStringLiteral("Regular")},
        {QStringLiteral("style"), style},
    };
    if (m_controller) {
        params.insert(QStringLiteral("at"), m_controller->playheadText());
        params.insert(QStringLiteral("track"), trackForNewTitle());
    }
    return params;
}

QString TitlesBridge::createTitle() {
    if (!available())
        return unavailableReason();
    if (!m_controller)
        return tr("No project");
    command::CommandRequest request;
    request.command = kCommand;
    const QByteArray json = QJsonDocument::fromVariant(commandParams()).toJson(QJsonDocument::Compact);
    request.params = command::Json::parse(json.constData(), json.constData() + json.size());
    request.params["sequence"] = m_controller->currentSequenceId().value;
    command::CommandEngine engine(m_controller->session());
    const command::CommandResult result = engine.execute(request);
    if (!result.ok())
        return QString::fromStdString(result.error->message);
    // Select the new clip: Effect Controls and this panel then edit it.
    if (const SequencePtr seq = m_controller->currentSequence()) {
        for (std::uint64_t id : result.createdIds) {
            if (seq->findClip(ClipId{id})) {
                m_controller->selectClipById(ClipId{id}, ProjectController::Replace);
                break;
            }
        }
    }
    return {};
}

} // namespace wabcut::app
