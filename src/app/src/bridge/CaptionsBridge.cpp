#include "CaptionsBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "GlobalBridge.h"
#include "ProjectController.h"
#include "SpellingBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/CaptionBuilder.h>
#include <wabcut/ai/translate/Languages.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/CaptionText.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Spelling.h>
#include <wabcut/model/Timecode.h>

#include <QColor>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace wabcut::app {

using command::Json;

namespace {

#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

/// The UTF-16 index of byte `byte` of UTF-8 `text`.
int utf16At(const std::string& text, std::size_t byte) {
    return static_cast<int>(QString::fromUtf8(text.data(), static_cast<qsizetype>(std::min(byte, text.size()))).size());
}

QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

QString hexOf(const CaptionRgb& rgb) {
    return QColor::fromRgbF(static_cast<float>(rgb[0]), static_cast<float>(rgb[1]), static_cast<float>(rgb[2]))
        .name(QColor::HexRgb)
        .toUpper();
}

std::optional<CaptionRgb> rgbOf(const QVariant& value) {
    const QColor c(value.toString());
    if (!c.isValid())
        return std::nullopt;
    return CaptionRgb{c.redF(), c.greenF(), c.blueF()};
}

QString speakerColor(std::uint32_t rgb) {
    return QColor::fromRgb(QRgb(rgb)).name(QColor::HexRgb).toUpper();
}

/// A number from `value` within [lo, hi], or nullopt when it is not one.
std::optional<double> numberIn(const QVariant& value, double lo, double hi) {
    bool ok = false;
    const double v = value.toDouble(&ok);
    if (!ok || !std::isfinite(v))
        return std::nullopt;
    return std::clamp(v, lo, hi);
}

} // namespace

QVariantMap captionStyleMap(const CaptionStyle& s, Ticks at) {
    return {{QStringLiteral("fontFamily"), fromStd(s.fontFamily)},
            {QStringLiteral("fontWeight"), s.fontWeight},
            {QStringLiteral("fontSize"), s.fontSize},
            {QStringLiteral("fontSizePx"), s.fontSize * 1080.0},
            {QStringLiteral("fill"), hexOf(s.fill)},
            {QStringLiteral("outlineWidth"), s.outlineWidth},
            {QStringLiteral("outline"), hexOf(s.outline)},
            {QStringLiteral("shadowOpacity"), s.shadowOpacity},
            {QStringLiteral("shadowColor"), hexOf(s.shadowColor)},
            {QStringLiteral("shadowDistance"), s.shadowDistance},
            {QStringLiteral("shadowAngle"), s.shadowAngle},
            {QStringLiteral("shadowSoftness"), s.shadowSoftness},
            {QStringLiteral("box"), s.box},
            {QStringLiteral("boxColor"), hexOf(s.boxColor)},
            {QStringLiteral("boxOpacity"), s.boxOpacity},
            {QStringLiteral("boxPadding"), s.boxPadding},
            {QStringLiteral("boxRadius"), s.boxRadius},
            {QStringLiteral("placement"), fromStd(std::string(toString(s.placement)))},
            {QStringLiteral("safeArea"), s.safeArea},
            {QStringLiteral("positionX"), s.positionX.valueAt(at)},
            {QStringLiteral("positionY"), s.positionY.valueAt(at)},
            {QStringLiteral("positionAnimated"), s.positionX.isCurve() || s.positionY.isCurve()},
            {QStringLiteral("alignment"), fromStd(std::string(toString(s.alignment)))},
            {QStringLiteral("maxWidth"), s.maxWidth},
            {QStringLiteral("lineSpacing"), s.lineSpacing},
            {QStringLiteral("highlight"), hexOf(s.highlight)},
            {QStringLiteral("animation"), fromStd(std::string(toString(s.animation)))},
            {QStringLiteral("showSpeakers"), s.showSpeakers},
            {QStringLiteral("allowOverlap"), s.allowOverlap},
            // C.13: the brand kit's colour names (and font role) the style uses instead of its own ("" = none).
            {QStringLiteral("brandFill"), fromStd(s.brand.fill)},
            {QStringLiteral("brandOutline"), fromStd(s.brand.outline)},
            {QStringLiteral("brandShadow"), fromStd(s.brand.shadow)},
            {QStringLiteral("brandBox"), fromStd(s.brand.box)},
            {QStringLiteral("brandHighlight"), fromStd(s.brand.highlight)},
            {QStringLiteral("brandFont"), fromStd(s.brand.font)}};
}

bool setCaptionStyleField(CaptionStyle& s, const QString& key, const QVariant& value) {
    const auto set = [&](double& field, double lo, double hi) {
        const auto v = numberIn(value, lo, hi);
        if (v)
            field = *v;
        return v.has_value();
    };
    const auto color = [&](CaptionRgb& field) {
        const auto c = rgbOf(value);
        if (c)
            field = *c;
        return c.has_value();
    };
    if (key == QLatin1String("fontFamily")) {
        s.fontFamily = value.toString().trimmed().toStdString();
        s.brand.font.clear();
        return true;
    }
    // C.13 brand references: the kit's colour (or font role) of that name; "" removes the reference.
    const auto brandRef = [&](std::string& field) {
        const QString name = value.toString().trimmed();
        if (name.toUtf8().size() > 256 || name.contains(QLatin1Char('\n')))
            return false;
        field = name.toStdString();
        return true;
    };
    if (key == QLatin1String("brandFill"))
        return brandRef(s.brand.fill);
    if (key == QLatin1String("brandOutline"))
        return brandRef(s.brand.outline);
    if (key == QLatin1String("brandShadow"))
        return brandRef(s.brand.shadow);
    if (key == QLatin1String("brandBox"))
        return brandRef(s.brand.box);
    if (key == QLatin1String("brandHighlight"))
        return brandRef(s.brand.highlight);
    if (key == QLatin1String("brandFont"))
        return brandRef(s.brand.font);
    if (key == QLatin1String("fontWeight")) {
        const auto v = numberIn(value, 100, 900);
        if (v)
            s.fontWeight = static_cast<std::int32_t>(std::lround(*v / 100.0) * 100);
        return v.has_value();
    }
    if (key == QLatin1String("fontSize"))
        return set(s.fontSize, 0.005, 0.5);
    if (key == QLatin1String("fontSizePx")) {
        double px = s.fontSize * 1080.0;
        const bool ok = set(px, 0.005 * 1080.0, 0.5 * 1080.0);
        if (ok)
            s.fontSize = px / 1080.0;
        return ok;
    }
    if (key == QLatin1String("fill")) {
        s.brand.fill.clear();
        return color(s.fill);
    }
    if (key == QLatin1String("outlineWidth"))
        return set(s.outlineWidth, 0.0, 0.5);
    if (key == QLatin1String("outline")) {
        s.brand.outline.clear();
        return color(s.outline);
    }
    if (key == QLatin1String("shadowOpacity"))
        return set(s.shadowOpacity, 0.0, 1.0);
    if (key == QLatin1String("shadowColor")) {
        s.brand.shadow.clear();
        return color(s.shadowColor);
    }
    if (key == QLatin1String("shadowDistance"))
        return set(s.shadowDistance, 0.0, 2.0);
    if (key == QLatin1String("shadowAngle"))
        return set(s.shadowAngle, -360.0, 360.0);
    if (key == QLatin1String("shadowSoftness"))
        return set(s.shadowSoftness, 0.0, 2.0);
    if (key == QLatin1String("box")) {
        s.box = value.toBool();
        return true;
    }
    if (key == QLatin1String("boxColor")) {
        s.brand.box.clear();
        return color(s.boxColor);
    }
    if (key == QLatin1String("boxOpacity"))
        return set(s.boxOpacity, 0.0, 1.0);
    if (key == QLatin1String("boxPadding"))
        return set(s.boxPadding, 0.0, 2.0);
    if (key == QLatin1String("boxRadius"))
        return set(s.boxRadius, 0.0, 2.0);
    if (key == QLatin1String("placement")) {
        const auto p = captionPlacementFromString(value.toString().toStdString());
        if (p)
            s.placement = *p;
        return p.has_value();
    }
    if (key == QLatin1String("safeArea"))
        return set(s.safeArea, 0.0, 0.4);
    if (key == QLatin1String("positionX") || key == QLatin1String("positionY")) {
        const auto v = numberIn(value, 0.0, 1.0);
        if (v)
            (key == QLatin1String("positionX") ? s.positionX : s.positionY) = ParamValue(*v);
        return v.has_value();
    }
    if (key == QLatin1String("alignment")) {
        const auto a = captionAlignFromString(value.toString().toStdString());
        if (a)
            s.alignment = *a;
        return a.has_value();
    }
    if (key == QLatin1String("maxWidth"))
        return set(s.maxWidth, 0.1, 1.0);
    if (key == QLatin1String("lineSpacing"))
        return set(s.lineSpacing, 0.5, 3.0);
    if (key == QLatin1String("highlight")) {
        s.brand.highlight.clear();
        return color(s.highlight);
    }
    if (key == QLatin1String("animation")) {
        const auto a = captionAnimationFromString(value.toString().toStdString());
        if (a)
            s.animation = *a;
        return a.has_value();
    }
    if (key == QLatin1String("showSpeakers")) {
        s.showSpeakers = value.toBool();
        return true;
    }
    if (key == QLatin1String("allowOverlap")) {
        s.allowOverlap = value.toBool();
        return true;
    }
    return false;
}

CaptionsBridge::CaptionsBridge(QObject* parent) : QObject(parent) {
    // p02 audio: Hide profanity and its word lists are the user's (as Premiere keeps them in its settings).
    const QSettings settings;
    m_hideProfanity = settings.value(QStringLiteral("captions/hideProfanity"), false).toBool();
    m_profanityWords = settings.value(QStringLiteral("captions/profanityWords")).toString();
    m_allowedWords = settings.value(QStringLiteral("captions/allowedWords")).toString();
}

namespace {

/// Comma-separated words as a JSON array (trimmed, empty ones dropped).
Json wordList(const QString& words) {
    Json out = Json::array();
    for (const QString& w : words.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString t = w.trimmed();
        if (!t.isEmpty())
            out.push_back(t.toStdString());
    }
    return out;
}

} // namespace

void CaptionsBridge::setHideProfanity(bool on) {
    if (m_hideProfanity == on)
        return;
    m_hideProfanity = on;
    QSettings().setValue(QStringLiteral("captions/hideProfanity"), on);
    emit profanityChanged();
}

void CaptionsBridge::setProfanityWords(const QString& words) {
    if (m_profanityWords == words)
        return;
    m_profanityWords = words;
    QSettings().setValue(QStringLiteral("captions/profanityWords"), words);
    emit profanityChanged();
}

void CaptionsBridge::setAllowedWords(const QString& words) {
    if (m_allowedWords == words)
        return;
    m_allowedWords = words;
    QSettings().setValue(QStringLiteral("captions/allowedWords"), words);
    emit profanityChanged();
}

QString CaptionsBridge::hideProfanityNow() {
    if (!m_transcript || !shownTrack())
        return tr("Make a caption track first");
    Json params{{"captionTrack", m_track.value}, {"profanityWords", wordList(m_profanityWords)}, {"allowedWords", wordList(m_allowedWords)}};
    const command::CommandResult result = m_transcript->run("captions.hideProfanity", std::move(params));
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    setMessage(tr("Profanity hidden in %n caption(s)", nullptr, result.data.value("cues", 0)));
    return {};
}

QString CaptionsBridge::bleepProfanity(const QString& mode) {
    if (!m_transcript || !m_controller)
        return tr("No transcript");
    Json params{{"mode", mode == QLatin1String("mute") ? "mute" : "bleep"},
                {"profanityWords", wordList(m_profanityWords)},
                {"allowedWords", wordList(m_allowedWords)}};
    const command::CommandResult result = m_transcript->run("audio.bleepProfanity", std::move(params));
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    const auto words = static_cast<int>(result.data.contains("words") ? result.data["words"].size() : 0);
    setMessage(words == 0 ? tr("No profanity found in the transcript")
                          : mode == QLatin1String("mute") ? tr("%n word(s) muted", nullptr, words) : tr("%n word(s) bleeped", nullptr, words));
    return {};
}

CaptionsBridge* CaptionsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* captions = new CaptionsBridge;
    if (qmlEngine) {
        captions->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        captions->setTranscript(qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript"));
    }
    return captions;
}

void CaptionsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::revisionChanged, this, &CaptionsBridge::refresh);
        connect(m_controller, &ProjectController::sequenceChanged, this, &CaptionsBridge::refresh);
        connect(m_controller, &ProjectController::playheadChanged, this, &CaptionsBridge::updateActive);
    }
    refresh();
}

void CaptionsBridge::setTranscript(TranscriptBridge* transcript) {
    if (m_transcript == transcript)
        return;
    if (m_transcript)
        disconnect(m_transcript, nullptr, this, nullptr);
    m_transcript = transcript;
    if (m_transcript) {
        connect(m_transcript, &TranscriptBridge::transcriptChanged, this, &CaptionsBridge::canGenerateChanged);
        connect(m_transcript, &TranscriptBridge::corrected, this, [this] {
            if (shownTrack())
                setUpdateSuggested(true);
        });
    }
    emit canGenerateChanged();
}

bool CaptionsBridge::canGenerate() const {
    return m_transcript && m_transcript->wordCount() > 0;
}

const CaptionTrack* CaptionsBridge::shownTrack() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return nullptr;
    // Pointers into the sequence stay valid while the session holds its snapshot (until the next edit).
    for (const CaptionTrack& t : seq->captionTracks) {
        if (t.id == m_track)
            return &t;
    }
    return nullptr;
}

void CaptionsBridge::refresh() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (seq && !shownTrack())
        m_track = seq->captionTracks.empty() ? CaptionTrackId{} : seq->captionTracks.front().id;
    if (!shownTrack())
        setUpdateSuggested(false);
    emit tracksChanged();
    emit cuesChanged();
    emit styleChanged();
    updateActive();
}

void CaptionsBridge::setUpdateSuggested(bool suggested) {
    if (suggested == m_updateSuggested)
        return;
    m_updateSuggested = suggested;
    emit updateSuggestedChanged();
}

QVariantList CaptionsBridge::tracks() const {
    QVariantList out;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return out;
    for (const CaptionTrack& t : seq->captionTracks) {
        out.append(QVariantMap{{QStringLiteral("id"), captionTrackKey(t.id)},
                               {QStringLiteral("name"), fromStd(t.name)},
                               {QStringLiteral("language"), fromStd(t.language)},
                               {QStringLiteral("cueCount"), static_cast<int>(t.cues.size())},
                               {QStringLiteral("enabled"), t.enabled}});
    }
    return out;
}

QString CaptionsBridge::track() const {
    return captionTrackKey(m_track);
}

void CaptionsBridge::setTrack(const QString& id) {
    const CaptionTrackId next = parseCaptionTrackKey(id).value_or(CaptionTrackId{});
    if (next == m_track)
        return;
    m_track = next;
    setUpdateSuggested(false);
    emit tracksChanged();
    emit cuesChanged();
    emit styleChanged();
    updateActive();
}

QVariantMap CaptionsBridge::trackInfo() const {
    const CaptionTrack* t = shownTrack();
    if (!t)
        return {};
    return {{QStringLiteral("name"), fromStd(t->name)},
            {QStringLiteral("language"), fromStd(t->language)},
            {QStringLiteral("enabled"), t->enabled},
            {QStringLiteral("trackStyle"), fromStd(t->trackStyle)}};
}

QString CaptionsBridge::setTrackStyle(const QString& preset) {
    if (!shownTrack())
        return tr("Make a caption track first");
    const QString error = runCommand("captions.setTrackStyle", QVariantMap{{QStringLiteral("style"), preset}});
    refresh();
    return error;
}

QString CaptionsBridge::pushTrackStyle() {
    if (!shownTrack())
        return tr("Make a caption track first");
    const QString error = runCommand("captions.pushTrackStyle", {});
    refresh();
    return error;
}

QVariantList CaptionsBridge::cues() const {
    QVariantList out;
    const CaptionTrack* t = shownTrack();
    if (!t || !m_controller)
        return out;
    const ProjectSnapshot snap = m_controller->snapshot();
    const double readable = ai::translate::rulesFor(ai::translate::normalizeLanguage(t->language)).charsPerSecond;
    for (std::size_t i = 0; i < t->cues.size(); ++i) {
        const CaptionCue& c = t->cues[i];
        const double seconds = c.duration.toSeconds();
        const double cps = seconds > 0 ? static_cast<double>(captionDisplayLength(c.text)) / seconds : 0.0;
        const Speaker* speaker = c.speaker.isValid() && snap ? snap->findSpeaker(c.speaker) : nullptr;
        out.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                               {QStringLiteral("text"), fromStd(c.text)},
                               {QStringLiteral("startText"), m_controller->formatTimecode(c.start)},
                               {QStringLiteral("endText"), m_controller->formatTimecode(c.end())},
                               {QStringLiteral("durationText"), tr("%1 s").arg(seconds, 0, 'f', 1)},
                               {QStringLiteral("start"), c.start.toSeconds()},
                               {QStringLiteral("end"), c.end().toSeconds()},
                               {QStringLiteral("speaker"), speaker ? speakerKey(speaker->id) : QString()},
                               {QStringLiteral("speakerName"), speaker ? fromStd(speaker->name) : QString()},
                               {QStringLiteral("speakerColor"), speaker ? speakerColor(speaker->color) : QString()},
                               {QStringLiteral("cps"), std::round(cps * 10.0) / 10.0},
                               {QStringLiteral("fast"), cps > readable + 0.05}});
    }
    return out;
}

QVariantMap CaptionsBridge::style() const {
    const CaptionTrack* t = shownTrack();
    const CaptionStyle style = t ? t->style : CaptionStyle{};
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const BrandKit* kit = snap && snap->brandKit ? &*snap->brandKit : nullptr;
    // What is drawn: brand references resolved (the references themselves stay in the brand* keys).
    return captionStyleMap(resolvedCaptionStyle(style, kit), m_controller ? m_controller->playhead() : Ticks{});
}

QVariantList CaptionsBridge::presets() const {
    QVariantList out;
    const CaptionTrack* t = shownTrack();
    const auto row = [&](const CaptionPreset& p, bool builtin) {
        out.append(QVariantMap{{QStringLiteral("name"), fromStd(p.name)},
                               {QStringLiteral("builtin"), builtin},
                               {QStringLiteral("current"), t && t->style == p.style},
                               {QStringLiteral("fill"), hexOf(p.style.fill)},
                               {QStringLiteral("box"), p.style.box},
                               {QStringLiteral("boxColor"), hexOf(p.style.boxColor)},
                               {QStringLiteral("outlineWidth"), p.style.outlineWidth},
                               {QStringLiteral("fontWeight"), p.style.fontWeight}});
    };
    for (const CaptionPreset& p : builtinCaptionPresets())
        row(p, true);
    if (const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr) {
        for (const CaptionPreset& p : snap->captionPresets)
            row(p, false);
    }
    return out;
}

QVariantList CaptionsBridge::speakers() const {
    QVariantList out;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return out;
    for (const Speaker& s : snap->speakers) {
        out.append(QVariantMap{{QStringLiteral("key"), speakerKey(s.id)},
                               {QStringLiteral("name"), fromStd(s.name)},
                               {QStringLiteral("color"), speakerColor(s.color)}});
    }
    return out;
}

void CaptionsBridge::updateActive() {
    int active = -1;
    const CaptionTrack* t = shownTrack();
    if (t && t->enabled && m_controller) {
        const Ticks p = m_controller->playhead();
        for (std::size_t i = 0; i < t->cues.size(); ++i) {
            if (p >= t->cues[i].start && p < t->cues[i].end()) {
                active = static_cast<int>(i);
                break;
            }
        }
    }
    if (active != m_activeCue) {
        m_activeCue = active;
        emit activeChanged();
    }
}

void CaptionsBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

void CaptionsBridge::setOverlay(bool on) {
    if (on == m_overlay)
        return;
    m_overlay = on;
    emit styleChanged();
}

// ---- Edits -------------------------------------------------------------------------------------------------------

QString CaptionsBridge::runCommand(const std::string& name, const QVariantMap& params) {
    if (!m_controller)
        return tr("No project");
    Json p = Json::parse(QJsonDocument(QJsonObject::fromVariantMap(params)).toJson(QJsonDocument::Compact).toStdString());
    if (shownTrack() && !p.contains("captionTrack"))
        p["captionTrack"] = m_track.value;
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, name, std::move(p));
    setMessage(commandErrorText(result));
    return commandErrorText(result);
}

template <class Fn>
QString CaptionsBridge::editTrack(const char* label, Fn&& fn, const std::string& mergeKey) {
    const CaptionTrack* t = shownTrack();
    if (!m_controller || !t)
        return tr("Make a caption track first");
    const SequenceId seq = m_controller->currentSequenceId();
    const CaptionTrackId track = m_track;
    const bool ok = m_controller->applyEditFn(
        label, [&](const ProjectState& state) -> edit::EditOutcome { return fn(state, seq, track); }, mergeKey);
    if (!ok) {
        setMessage(m_controller->lastError());
        return m_message;
    }
    setMessage({});
    return {};
}

QString CaptionsBridge::generate(int maxCharsPerLine, int maxLines) {
    if (!m_transcript || !m_controller)
        return tr("No transcript");
    Json params{{"maxCharsPerLine", std::clamp(maxCharsPerLine, 8, 200)}, {"maxLines", std::clamp(maxLines, 1, 4)}};
    if (m_track && shownTrack())
        params["replace"] = m_track.value;
    if (m_hideProfanity) { // p02 audio
        params["hideProfanity"] = true;
        params["profanityWords"] = wordList(m_profanityWords);
        params["allowedWords"] = wordList(m_allowedWords);
    }
    const command::CommandResult result = m_transcript->run("captions.generateFromTranscript", std::move(params));
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    if (!result.createdIds.empty())
        m_track = CaptionTrackId{result.createdIds.front()};
    else if (result.data.contains("captionTrack"))
        m_track = CaptionTrackId{result.data.value("captionTrack", std::uint64_t{0})};
    setMessage({});
    setUpdateSuggested(false);
    refresh();
    return {};
}

QString CaptionsBridge::updateFromTranscript() {
    if (!m_transcript || !shownTrack())
        return tr("Make a caption track first");
    Json params{{"captionTrack", m_track.value}};
    if (m_hideProfanity) { // p02 audio
        params["hideProfanity"] = true;
        params["profanityWords"] = wordList(m_profanityWords);
        params["allowedWords"] = wordList(m_allowedWords);
    }
    const command::CommandResult result = m_transcript->run("captions.updateFromTranscript", std::move(params));
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    setUpdateSuggested(false);
    setMessage(tr("Captions updated from the transcript"));
    return {};
}

QString CaptionsBridge::addTrack(const QString& name, const QString& preset) {
    if (!m_controller)
        return tr("No project");
    Json params{{"name", name.trimmed().isEmpty() ? std::string("Captions") : name.trimmed().toStdString()}};
    if (!preset.isEmpty())
        params["preset"] = preset.toStdString();
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "captions.addTrack", std::move(params));
    if (!result.ok()) {
        setMessage(commandErrorText(result));
        return m_message;
    }
    if (!result.createdIds.empty())
        m_track = CaptionTrackId{result.createdIds.front()};
    else if (result.data.contains("captionTrack"))
        m_track = CaptionTrackId{result.data.value("captionTrack", std::uint64_t{0})};
    setMessage({});
    refresh();
    return {};
}

QString CaptionsBridge::setTrackName(const QString& name) {
    const std::string value = name.trimmed().toStdString();
    if (value.empty())
        return tr("A caption track needs a name");
    return editTrack(LABEL("Rename Caption Track"), [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
        edit::CaptionTrackChange change;
        change.name = value;
        return edit::setCaptionTrackInfo(s, seq, track, change);
    });
}

QString CaptionsBridge::setTrackLanguage(const QString& language) {
    const std::string value = ai::translate::normalizeLanguage(language.trimmed().toStdString());
    return editTrack(LABEL("Caption Language"), [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
        edit::CaptionTrackChange change;
        change.language = value;
        return edit::setCaptionTrackInfo(s, seq, track, change);
    });
}

QString CaptionsBridge::setTrackEnabled(bool enabled) {
    return editTrack(enabled ? LABEL("Show Captions") : LABEL("Hide Captions"),
                     [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
                         edit::CaptionTrackChange change;
                         change.enabled = enabled;
                         return edit::setCaptionTrackInfo(s, seq, track, change);
                     });
}

QString CaptionsBridge::removeTrack() {
    if (!m_controller || !shownTrack())
        return {};
    const QString error = editTrack(LABEL("Delete Caption Track"), [](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
        return edit::removeCaptionTrack(s, seq, track);
    });
    if (!error.isEmpty())
        return error;
    m_track = {};
    refresh();
    return {};
}

QString CaptionsBridge::setStyle(const QString& key, const QVariant& value) {
    const CaptionTrack* t = shownTrack();
    if (!t)
        return tr("Make a caption track first");
    CaptionStyle next = t->style;
    if (!setCaptionStyleField(next, key, value)) {
        setMessage(tr("No such caption style setting: %1").arg(key));
        return m_message;
    }
    if (next == t->style)
        return {};
    return editTrack(
        LABEL("Caption Style"),
        [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) { return edit::setCaptionStyle(s, seq, track, next); },
        "caption-style:" + std::to_string(m_track.value) + ":" + key.toStdString());
}

void CaptionsBridge::endStyleEdit() {
    endEdit();
}

QString CaptionsBridge::applyPreset(const QString& name) {
    if (!shownTrack())
        return tr("Make a caption track first");
    endEdit();
    return runCommand("captions.applyPreset", {{QStringLiteral("preset"), name}});
}

QString CaptionsBridge::savePreset(const QString& name) {
    if (!shownTrack())
        return tr("Make a caption track first");
    if (name.trimmed().isEmpty())
        return tr("Name the preset");
    endEdit();
    const QString error = runCommand("captions.savePreset", {{QStringLiteral("name"), name.trimmed()}});
    if (error.isEmpty())
        setMessage(tr("Saved “%1” in the project").arg(name.trimmed()));
    emit styleChanged();
    return error;
}

QString CaptionsBridge::removePreset(const QString& name) {
    if (!m_controller)
        return tr("No project");
    const command::CommandResult result =
        CommandJob::runNow(m_controller, nullptr, "captions.removePreset", Json{{"name", name.toStdString()}});
    setMessage(commandErrorText(result));
    emit styleChanged();
    return commandErrorText(result);
}

QString CaptionsBridge::setCueText(int index, const QString& text) {
    const CaptionTrack* t = shownTrack();
    if (!t || index < 0 || index >= static_cast<int>(t->cues.size()))
        return tr("No such caption");
    const std::string value = text.toStdString();
    if (t->cues[static_cast<std::size_t>(index)].text == value)
        return {};
    return editTrack(
        LABEL("Edit Caption"),
        [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
            edit::CaptionCueChange change;
            change.text = value;
            return edit::updateCaptionCue(s, seq, track, static_cast<std::size_t>(index), change);
        },
        "caption-text:" + std::to_string(m_track.value) + ":" + std::to_string(index));
}

QString CaptionsBridge::setCueTiming(int index, const QString& start, const QString& end) {
    const CaptionTrack* t = shownTrack();
    if (!t || !m_controller || index < 0 || index >= static_cast<int>(t->cues.size()))
        return tr("No such caption");
    const auto s = m_controller->parseTimecodeText(start);
    const auto e = m_controller->parseTimecodeText(end);
    if (!s || !e) {
        setMessage(tr("Type times such as 00:00:01:12"));
        return m_message;
    }
    const auto i = static_cast<std::size_t>(index);
    // Kept between the neighbours unless the style lets cues overlap.
    const bool free = t->style.allowOverlap;
    const Ticks lo = !free && i > 0 ? t->cues[i - 1].end() : Ticks{};
    const Ticks hi = !free && i + 1 < t->cues.size() ? t->cues[i + 1].start : Ticks::max();
    const Ticks newStart = std::clamp(*s, lo, hi);
    const Ticks newEnd = std::clamp(*e, newStart, hi);
    if (newEnd <= newStart) {
        setMessage(tr("A caption needs a length"));
        return m_message;
    }
    if (newStart == t->cues[i].start && newEnd == t->cues[i].end())
        return {};
    const QString error = editTrack(LABEL("Caption Timing"), [&](const ProjectState& st, SequenceId seq, CaptionTrackId track) {
        edit::CaptionCueChange change;
        change.start = newStart;
        change.end = newEnd;
        return edit::updateCaptionCue(st, seq, track, i, change);
    });
    endEdit();
    return error;
}

QString CaptionsBridge::nudgeCue(int index, const QString& edge, int frames) {
    const CaptionTrack* t = shownTrack();
    const SequencePtr seqPtr = m_controller ? m_controller->currentSequence() : nullptr;
    if (!t || !seqPtr || index < 0 || index >= static_cast<int>(t->cues.size()) || frames == 0)
        return tr("No such caption");
    const auto i = static_cast<std::size_t>(index);
    const Ticks step = framesToTicks(std::abs(frames), seqPtr->frameRate, Rounding::Floor).value_or(Ticks{});
    const Ticks delta{frames < 0 ? -step.value : step.value};
    const CaptionCue& cue = t->cues[i];
    Ticks start = cue.start;
    Ticks end = cue.end();
    if (edge == QLatin1String("start") || edge == QLatin1String("both"))
        start = start + delta;
    if (edge == QLatin1String("end") || edge == QLatin1String("both"))
        end = end + delta;
    const bool free = t->style.allowOverlap;
    const Ticks lo = !free && i > 0 ? t->cues[i - 1].end() : Ticks{};
    const Ticks hi = !free && i + 1 < t->cues.size() ? t->cues[i + 1].start : Ticks::max();
    if (start < lo || end > hi || end <= start) {
        setMessage(start < lo || end > hi ? tr("The caption would run into its neighbour") : tr("A caption needs a length"));
        return m_message;
    }
    return editTrack(
        LABEL("Nudge Caption"),
        [&](const ProjectState& st, SequenceId seq, CaptionTrackId track) {
            edit::CaptionCueChange change;
            change.start = start;
            change.end = end;
            return edit::updateCaptionCue(st, seq, track, i, change);
        },
        "caption-nudge:" + std::to_string(m_track.value) + ":" + std::to_string(index));
}

QString CaptionsBridge::splitCueAtPlayhead(int index) {
    const CaptionTrack* t = shownTrack();
    if (!t || !m_controller || index < 0 || index >= static_cast<int>(t->cues.size()))
        return tr("No such caption");
    const Ticks at = m_controller->playhead();
    const CaptionCue& cue = t->cues[static_cast<std::size_t>(index)];
    if (at <= cue.start || at >= cue.end()) {
        setMessage(tr("Put the playhead inside the caption to split it there"));
        return m_message;
    }
    endEdit();
    const QString error = editTrack(LABEL("Split Caption"), [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
        return edit::splitCaptionCue(s, seq, track, static_cast<std::size_t>(index), at, std::nullopt, *qtLineBreaker());
    });
    endEdit();
    return error;
}

QString CaptionsBridge::mergeWithNext(int index) {
    const CaptionTrack* t = shownTrack();
    if (!t || index < 0 || index + 1 >= static_cast<int>(t->cues.size()))
        return tr("There is no next caption to join");
    endEdit();
    const QString error = editTrack(LABEL("Merge Captions"), [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
        return edit::mergeCaptionCues(s, seq, track, static_cast<std::size_t>(index), 2, *qtLineBreaker());
    });
    endEdit();
    return error;
}

QString CaptionsBridge::addCueAtPlayhead(const QString& text) {
    const CaptionTrack* t = shownTrack();
    if (!t || !m_controller)
        return tr("Make a caption track first");
    const Ticks at = m_controller->playhead();
    Ticks end = at + Ticks::fromSeconds(2);
    for (const CaptionCue& c : t->cues) {
        if (!t->style.allowOverlap && c.start <= at && at < c.end()) {
            setMessage(tr("A caption is already there"));
            return m_message;
        }
        if (!t->style.allowOverlap && c.start > at)
            end = std::min(end, c.start);
    }
    CaptionCue cue;
    cue.start = at;
    cue.duration = end - at;
    cue.text = text.trimmed().isEmpty() ? tr("New caption").toStdString() : text.toStdString();
    endEdit();
    const QString error = editTrack(LABEL("Add Caption"), [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
        return edit::addCaptionCue(s, seq, track, cue);
    });
    endEdit();
    return error;
}

QString CaptionsBridge::removeCue(int index) {
    const CaptionTrack* t = shownTrack();
    if (!t || index < 0 || index >= static_cast<int>(t->cues.size()))
        return tr("No such caption");
    endEdit();
    const QString error = editTrack(LABEL("Delete Caption"), [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
        return edit::removeCaptionCue(s, seq, track, static_cast<std::size_t>(index));
    });
    endEdit();
    return error;
}

QString CaptionsBridge::setCueSpeaker(int index, const QString& speaker) {
    const CaptionTrack* t = shownTrack();
    if (!t || index < 0 || index >= static_cast<int>(t->cues.size()))
        return tr("No such caption");
    const SpeakerId id = parseSpeakerKey(speaker).value_or(SpeakerId{});
    if (!speaker.isEmpty() && !id.isValid())
        return tr("No such speaker");
    endEdit();
    const QString error = editTrack(LABEL("Caption Speaker"), [&](const ProjectState& s, SequenceId seq, CaptionTrackId track) {
        edit::CaptionCueChange change;
        change.speaker = id;
        return edit::updateCaptionCue(s, seq, track, static_cast<std::size_t>(index), change);
    });
    endEdit();
    return error;
}

QString CaptionsBridge::addSpeaker(const QString& name) {
    if (!m_controller)
        return {};
    if (name.trimmed().isEmpty()) {
        setMessage(tr("Name the speaker"));
        return {};
    }
    const command::CommandResult result =
        CommandJob::runNow(m_controller, nullptr, "transcript.addSpeaker", Json{{"name", name.trimmed().toStdString()}});
    setMessage(commandErrorText(result));
    if (!result.ok())
        return {};
    std::uint64_t id = result.data.value("speaker", std::uint64_t{0});
    if (id == 0 && !result.createdIds.empty())
        id = result.createdIds.front();
    if (id == 0) // the newest speaker
        if (const ProjectSnapshot snap = m_controller->snapshot(); snap && !snap->speakers.empty())
            id = snap->speakers.back().id.value;
    emit tracksChanged();
    return speakerKey(SpeakerId{id});
}

QString CaptionsBridge::renameSpeaker(const QString& speaker, const QString& name, const QString& color) {
    const auto id = parseSpeakerKey(speaker);
    if (!id || !m_controller)
        return tr("No such speaker");
    Json params{{"speaker", id->value}};
    if (!name.trimmed().isEmpty())
        params["name"] = name.trimmed().toStdString();
    if (!color.isEmpty())
        params["color"] = color.toStdString();
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "transcript.renameSpeaker", std::move(params));
    setMessage(commandErrorText(result));
    emit tracksChanged();
    emit cuesChanged();
    return commandErrorText(result);
}

void CaptionsBridge::endEdit() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

void CaptionsBridge::seekToCue(int index) {
    const CaptionTrack* t = shownTrack();
    if (!t || !m_controller || index < 0 || index >= static_cast<int>(t->cues.size()))
        return;
    const CaptionCue& cue = t->cues[static_cast<std::size_t>(index)];
    const SequencePtr seq = m_controller->currentSequence();
    m_controller->setPlayhead(seq ? seekPointIn(TimeRange{cue.start, cue.duration}, seq->frameRate) : cue.start);
}

QString CaptionsBridge::exportText(const QString& format) const {
    const CaptionTrack* t = shownTrack();
    if (!t)
        return {};
    return fromStd(format == QLatin1String("vtt") ? ai::edit::toVtt(*t) : ai::edit::toSrt(*t));
}

QString CaptionsBridge::exportFile(const QUrl& file, const QString& format) {
    if (!shownTrack()) {
        setMessage(tr("Generate captions first"));
        return m_message;
    }
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    std::ofstream out(pathFromUtf8(path.toStdString()), std::ios::binary);
    if (!out) {
        setMessage(tr("Cannot write %1").arg(path));
        return m_message;
    }
    const std::string text = exportText(format).toStdString();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        setMessage(tr("Cannot write %1").arg(path));
        return m_message;
    }
    setMessage(tr("Saved %1").arg(QUrl::fromLocalFile(path).fileName()));
    return {};
}

QString CaptionsBridge::findReplace(const QString& find, const QString& replace, bool matchCase, bool wholeWord,
                                    const QString& scope) {
    if (!m_transcript)
        return tr("Not available here");
    if (find.isEmpty())
        return tr("Type the text to find");
    Json params{{"find", find.toStdString()}, {"replace", replace.toStdString()}, {"matchCase", matchCase}, {"wholeWord", wholeWord}};
    const bool transcript = scope == QLatin1String("transcript");
    if (scope == QLatin1String("track")) {
        if (!shownTrack())
            return tr("Make a caption track first");
        params["captionTrack"] = m_track.value;
    }
    const command::CommandResult result = m_transcript->run(transcript ? "transcript.findReplace" : "captions.findReplace",
                                                            std::move(params));
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    const int replaced = result.data.value("replaced", 0);
    setMessage(replaced == 0 ? tr("\"%1\" was not found").arg(find) : tr("Replaced %n match(es)", nullptr, replaced));
    refresh();
    return {};
}

QString CaptionsBridge::importFile(const QUrl& file) {
    if (!m_transcript)
        return tr("Not available here");
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const command::CommandResult result = m_transcript->run("captions.import", Json{{"path", path.toStdString()}});
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    if (!result.createdIds.empty())
        m_track = CaptionTrackId{result.createdIds.front()};
    setMessage(tr("Imported %n caption(s)", nullptr, result.data.value("cues", 0)));
    refresh();
    return {};
}

QString CaptionsBridge::exportTranscript(const QUrl& file) {
    if (!m_transcript)
        return tr("Not available here");
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const command::CommandResult result =
        m_transcript->run("transcript.export", Json{{"path", path.toStdString()}, {"overwrite", true}});
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    setMessage(tr("Saved %1").arg(QUrl::fromLocalFile(path).fileName()));
    return {};
}


QVariantList CaptionsBridge::findMatches(const QString& find, bool matchCase, bool wholeWord, const QString& scope) {
    QVariantList out;
    if (!m_transcript || find.isEmpty())
        return out;
    Json params{{"find", find.toStdString()}, {"matchCase", matchCase}, {"wholeWord", wholeWord}};
    const bool transcript = scope == QLatin1String("transcript");
    if (scope == QLatin1String("track")) {
        if (!shownTrack())
            return out;
        params["captionTrack"] = m_track.value;
    }
    const command::CommandResult result = m_transcript->run(transcript ? "transcript.find" : "captions.find", std::move(params));
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return out;
    }
    for (const Json& m : result.data.value("matches", Json::array())) {
        QVariantMap item;
        item[QStringLiteral("time")] = fromStd(m.value("time", std::string()));
        const std::string text = m.value("text", std::string());
        item[QStringLiteral("text")] = fromStd(text);
        if (transcript) {
            item[QStringLiteral("words")] = QVariantList{m["words"][0].get<int>(), m["words"][1].get<int>()};
        } else {
            item[QStringLiteral("captionTrack")] = static_cast<qulonglong>(m.value("captionTrack", std::uint64_t{0}));
            item[QStringLiteral("trackKey")] = captionTrackKey(CaptionTrackId{m.value("captionTrack", std::uint64_t{0})});
            item[QStringLiteral("cue")] = m.value("cue", 0);
            item[QStringLiteral("startByte")] = m.value("start", 0);
            item[QStringLiteral("start")] = utf16At(text, m.value("start", std::size_t{0}));
            item[QStringLiteral("end")] = utf16At(text, m.value("end", std::size_t{0}));
        }
        out.push_back(item);
    }
    return out;
}

QString CaptionsBridge::replaceMatch(const QString& find, const QString& replace, bool matchCase, bool wholeWord,
                                     const QString& scope, const QVariantMap& match) {
    if (!m_transcript)
        return tr("Not available here");
    if (find.isEmpty())
        return tr("Type the text to find");
    Json params{{"find", find.toStdString()}, {"replace", replace.toStdString()}, {"matchCase", matchCase}, {"wholeWord", wholeWord}};
    const bool transcript = scope == QLatin1String("transcript");
    if (scope == QLatin1String("track")) {
        if (!shownTrack())
            return tr("Make a caption track first");
        params["captionTrack"] = m_track.value;
    }
    if (transcript) {
        const QVariantList words = match.value(QStringLiteral("words")).toList();
        if (words.size() != 2)
            return tr("Find the text first");
        params["words"] = Json::array({words[0].toInt(), words[1].toInt()});
    } else {
        if (!match.contains(QStringLiteral("cue")))
            return tr("Find the text first");
        params["at"] = Json{{"captionTrack", match.value(QStringLiteral("captionTrack")).toULongLong()},
                            {"cue", match.value(QStringLiteral("cue")).toInt()},
                            {"start", match.value(QStringLiteral("startByte")).toInt()}};
    }
    const command::CommandResult result =
        m_transcript->run(transcript ? "transcript.findReplace" : "captions.findReplace", std::move(params));
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    setMessage(tr("Replaced %n match(es)", nullptr, result.data.value("replaced", 0)));
    refresh();
    return {};
}

QVariantList CaptionsBridge::spellingIssues() {
    QVariantList out;
    const CaptionTrack* track = shownTrack();
    if (!track)
        return out;
    for (const TextSpellIssue& i : spellingOf(*track, *sharedSpelling(), {}, true, 2000)) {
        const std::string& text = track->cues[i.cue].text;
        QStringList suggestions;
        for (const std::string& s : i.issue.suggestions)
            suggestions << fromStd(s);
        out.push_back(QVariantMap{{QStringLiteral("cue"), static_cast<int>(i.cue)},
                                  {QStringLiteral("start"), utf16At(text, i.issue.start)},
                                  {QStringLiteral("end"), utf16At(text, i.issue.end)},
                                  {QStringLiteral("word"), fromStd(i.issue.word)},
                                  {QStringLiteral("action"), fromStd(std::string(toString(i.issue.action)))},
                                  {QStringLiteral("suggestions"), suggestions},
                                  {QStringLiteral("text"), fromStd(text)}});
    }
    return out;
}

QString CaptionsBridge::changeWord(int cue, int start, int end, const QString& with) {
    const CaptionTrack* t = shownTrack();
    if (!t || cue < 0 || cue >= static_cast<int>(t->cues.size()))
        return tr("No such caption");
    const QString text = fromStd(t->cues[static_cast<std::size_t>(cue)].text);
    const QString error = setCueText(cue, correctedText(text, start, end, with));
    endEdit();
    return error;
}

QString CaptionsBridge::changeAll(const QString& word, const QString& with) {
    if (!shownTrack())
        return tr("Make a caption track first");
    return findReplace(word, with, true, true, QStringLiteral("track"));
}

} // namespace wabcut::app
