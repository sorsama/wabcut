// AudioMixerBridge, the 0.7 parts that grew beside it: the Audio Track Mixer menu (cards 032: Show/Hide Tracks, Meter
// Input(s) Only), the Loudness Radar (cards 104 and 141), and the Audio Tools panel's Match tab (card 045: EQ Match and
// Level Match, over audio.eqMatch / audio.levelMatch; MatchCommands.cpp).

#include "AudioMixerBridge.h"

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "VoiceOverBridge.h"

#include <wabcut/audio/edit/MatchCurve.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/Effects.h>
#include <wabcut/playback/audio/TimelineAudioPlayer.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut::app {

using command::Json;

namespace {

QString tx(const char* text) {
    return QCoreApplication::translate("wabcut::app::AudioMixerBridge", text);
}

constexpr double kNegInf = -std::numeric_limits<double>::infinity();
constexpr const char* kEqMatchEffect = "wabcut.audio.graphicEq30";

QVariant numberOrNull(double v) {
    return std::isfinite(v) ? QVariant(v) : QVariant();
}

QVariant numberOrNull(const Json& v) {
    return v.is_number() ? QVariant(v.get<double>()) : QVariant();
}

QString errorText(const command::CommandResult& result) {
    return result.ok() ? QString() : QString::fromStdString(result.error->message);
}

/// The EQ Match instance of a clip's stack (the Graphic Equalizer the command marked with effect data), or null.
const EffectInstance* eqMatchOf(const Clip& clip) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == kEqMatchEffect && e.findData("eqMatch") != nullptr)
            return &e;
    }
    return nullptr;
}

} // namespace

// ---- Show/Hide Tracks, Meter Input(s) Only (card 032) -----------------------------------------------------------------

int AudioMixerBridge::hiddenCount() const {
    int n = 0;
    for (const QVariantList* list : {&m_strips, &m_submixes}) {
        for (const QVariant& v : *list) {
            if (v.toMap().value(QStringLiteral("hidden")).toBool())
                ++n;
        }
    }
    return n;
}

void AudioMixerBridge::setTrackHidden(const QString& key, bool hidden) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return;
    const auto known = [&](const QVariantList& list) {
        return std::ranges::any_of(list, [&](const QVariant& v) { return v.toMap().value(QStringLiteral("key")).toString() == key; });
    };
    if (!known(m_strips) && !known(m_submixes))
        return;
    std::set<QString>& set = m_hiddenStrips[seq->id.value];
    if (hidden)
        set.insert(key);
    else
        set.erase(key);
    rebuild(); // the strips carry `hidden`: they change, stripsChanged follows
}

bool AudioMixerBridge::isTrackHidden(const QString& key) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto it = seq ? m_hiddenStrips.find(seq->id.value) : m_hiddenStrips.end();
    return it != m_hiddenStrips.end() && it->second.count(key) != 0;
}

void AudioMixerBridge::showAllTracks() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || m_hiddenStrips.erase(seq->id.value) == 0)
        return;
    rebuild();
}

void AudioMixerBridge::setVoiceOver(VoiceOverBridge* voiceOver) {
    if (m_voiceOver == voiceOver)
        return;
    if (m_voiceOver)
        disconnect(m_voiceOver, nullptr, this, nullptr);
    m_voiceOver = voiceOver;
    if (voiceOver) {
        // The meters read the input in C++ when the bridge says there are new levels.
        connect(voiceOver, &VoiceOverBridge::levelsChanged, this, [this] {
            if (m_meterInputsOnly)
                emit metersUpdated();
        });
        connect(voiceOver, &VoiceOverBridge::inputChanged, this, [this] {
            if (m_meterInputsOnly)
                emit metersUpdated();
        });
    }
}

void AudioMixerBridge::setMeterInputsOnly(bool on) {
    if (m_meterInputsOnly == on)
        return;
    m_meterInputsOnly = on;
    emit meterInputsOnlyChanged();
    emit metersUpdated();
}

bool AudioMixerBridge::isArmedStrip(const QString& key) const {
    return std::ranges::any_of(m_strips, [&](const QVariant& v) {
        const QVariantMap strip = v.toMap();
        return strip.value(QStringLiteral("key")).toString() == key && strip.value(QStringLiteral("recordArmed")).toBool();
    });
}

std::optional<AudioMixerBridge::MeterLevels> AudioMixerBridge::inputNow() const {
    if (m_inputSource)
        return m_inputSource();
    if (m_voiceOver && m_voiceOver->inputOpen())
        return inputLevels();
    return std::nullopt;
}

AudioMixerBridge::MeterLevels AudioMixerBridge::inputLevels() const {
    MeterLevels levels;
    levels.channels = 2;
    levels.peakDb.fill(kNegInf);
    levels.rmsDb.fill(kNegInf);
    levels.holdDb.fill(kNegInf);
    const QVariantMap in = m_voiceOver ? m_voiceOver->levels() : QVariantMap{};
    if (in.contains(QStringLiteral("peakDb"))) {
        const double peak = in.value(QStringLiteral("peakDb")).toDouble();
        const double rms = in.value(QStringLiteral("rmsDb")).toDouble();
        for (std::size_t ch = 0; ch < 2; ++ch) {
            levels.peakDb[ch] = peak;
            levels.rmsDb[ch] = rms;
            levels.holdDb[ch] = peak;
        }
        levels.clipped = in.value(QStringLiteral("clipped")).toBool();
    }
    return levels;
}

// ---- The Loudness Radar (cards 104, 141) -----------------------------------------------------------------------------

void AudioMixerBridge::setRadarLap(int seconds) {
    static constexpr int kLaps[] = {60, 300, 600, 1800, 3600};
    const int lap = *std::ranges::min_element(kLaps, {}, [&](int l) { return std::abs(l - seconds); });
    if (lap == m_radarLap)
        return;
    m_radarLap = lap;
    updateRadar(true);
    emit radarChanged();
}

void AudioMixerBridge::setRadarMode(const QString& mode) {
    const QString next = mode == QLatin1String("programme") ? mode : QStringLiteral("live");
    if (next == m_radarMode)
        return;
    m_radarMode = next;
    m_radarSeen = 0;
    updateRadar(true);
    emit radarChanged();
}

void AudioMixerBridge::setRadarShown(bool shown) {
    if (shown == m_radarShown)
        return;
    m_radarShown = shown;
    m_radarSeen = 0;
    updateRadar(true);
    emit radarChanged();
}

void AudioMixerBridge::updateRadar(bool force) {
    if (!m_radarShown)
        return;
    const bool live = m_radarMode == QLatin1String("live");
    audio::edit::RadarFrame frame;
    audio::edit::LoudnessBand band;
    double lra = 0.0;
    double seconds = 0.0;
    bool stale = false;
    if (live) {
        // The history grows once a second: look for it twice a second, not at every meter tick.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!force && now - m_radarLastMs < 500)
            return;
        m_radarLastMs = now;
        std::vector<float> history;
        if (m_loudnessHistorySource) {
            history = m_loudnessHistorySource();
        } else {
            editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr;
            if (playback::TimelineAudioPlayer* player = session ? session->audioPlayer() : nullptr)
                history = player->loudnessHistory();
        }
        if (!force && history.size() == m_radarSeen)
            return;
        m_radarSeen = history.size();
        frame = audio::edit::foldLive(history, m_radarLap);
        band = audio::edit::loudnessBand(history);
        lra = m_liveRange;
        seconds = static_cast<double>(history.size());
    } else {
        frame = audio::edit::foldProgramme(m_mixSeries, 1.0);
        band = audio::edit::loudnessBand(m_mixSeries);
        lra = m_mixRange;
        seconds = static_cast<double>(m_mixSeries.size());
        stale = m_controller && m_controller->session().revision() != m_mixSeriesRevision;
    }
    QVariantList values;
    values.reserve(static_cast<qsizetype>(frame.values.size()));
    bool any = false;
    for (const float v : frame.values) {
        const bool known = std::isfinite(v);
        any = any || known;
        values.append(known ? QVariant(static_cast<double>(v)) : QVariant());
    }
    QVariantMap out{{QStringLiteral("valid"), any},
                    {QStringLiteral("mode"), m_radarMode},
                    {QStringLiteral("lap"), m_radarLap},
                    {QStringLiteral("slots"), static_cast<int>(frame.values.size())},
                    {QStringLiteral("values"), values},
                    {QStringLiteral("secondsPerSlot"), frame.secondsPerSlot},
                    {QStringLiteral("head"), frame.head},
                    {QStringLiteral("seconds"), seconds},
                    {QStringLiteral("target"), targetLufs()},
                    {QStringLiteral("lra"), lra > 0.0 ? QVariant(lra) : QVariant()},
                    {QStringLiteral("lowLufs"), band.valid ? QVariant(band.lowLufs) : QVariant()},
                    {QStringLiteral("highLufs"), band.valid ? QVariant(band.highLufs) : QVariant()},
                    {QStringLiteral("stale"), stale}};
    if (any && band.valid) {
        const QLocale loc;
        out.insert(QStringLiteral("text"),
                   tx("Range %1 LU · %2 to %3 LUFS")
                       .arg(loc.toString(lra > 0.0 ? lra : band.rangeLu(), 'f', 1), loc.toString(band.lowLufs, 'f', 1),
                            loc.toString(band.highLufs, 'f', 1)));
    } else {
        out.insert(QStringLiteral("text"), live ? tx("Play the sequence to draw the radar") : tx("Analyze the mix to draw its radar"));
    }
    if (out == m_radar)
        return;
    m_radar = out;
    emit radarChanged();
}

// ---- Match (card 045) -------------------------------------------------------------------------------------------------

QVariantList AudioMixerBridge::matchMedia() const {
    QVariantList out;
    if (!m_controller)
        return out;
    for (const MediaPtr& item : m_controller->snapshot()->media) {
        if (!item || item->info.audio.empty())
            continue;
        out.append(QVariantMap{{QStringLiteral("id"), QString::number(item->id.value)},
                               {QStringLiteral("name"), QString::fromStdString(item->name)},
                               {QStringLiteral("seconds"), item->info.duration.toSeconds()}});
        if (out.size() >= 500)
            break;
    }
    return out;
}

QString AudioMixerBridge::matchUseSelectedClip() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return tx("No project");
    for (const ClipId id : m_controller->selection()) {
        const auto loc = seq->findClip(id);
        if (!loc || loc->track->kind != TrackKind::Audio)
            continue;
        m_match.referenceSet = true;
        m_match.referenceKind = QStringLiteral("clip");
        m_match.referenceId = id.value;
        m_match.referenceFrom.reset();
        m_match.referenceTo.reset();
        m_match.referenceLabel = QString::fromStdString(std::string(loc->clip().name()));
        m_match.learned = false;
        emit matchChanged();
        return {};
    }
    return tx("Select an audio clip to use as the reference");
}

QString AudioMixerBridge::matchUseRange() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return tx("No project");
    const std::optional<Ticks> in = m_controller->markIn();
    const std::optional<Ticks> out = m_controller->markOut();
    if (!in || !out || !(*in < *out))
        return tx("Mark an In and an Out point on the timeline first");
    m_match.referenceSet = true;
    m_match.referenceKind = QStringLiteral("range");
    m_match.referenceId = 0;
    m_match.referenceFrom = in->value;
    m_match.referenceTo = out->value;
    m_match.referenceLabel = tx("In to Out: %1 – %2").arg(m_controller->formatTimecode(*in), m_controller->formatTimecode(*out));
    m_match.learned = false;
    emit matchChanged();
    return {};
}

QString AudioMixerBridge::matchUseMedia(const QString& mediaId) {
    if (!m_controller)
        return tx("No project");
    bool ok = false;
    const std::uint64_t id = mediaId.toULongLong(&ok);
    const MediaItem* item = ok ? m_controller->snapshot()->findMedia(MediaId{id}) : nullptr;
    if (!item || item->info.audio.empty())
        return tx("Choose a media file with audio");
    m_match.referenceSet = true;
    m_match.referenceKind = QStringLiteral("media");
    m_match.referenceId = id;
    m_match.referenceFrom.reset();
    m_match.referenceTo.reset();
    m_match.referenceLabel = QString::fromStdString(item->name);
    m_match.learned = false;
    emit matchChanged();
    return {};
}

QString AudioMixerBridge::matchUseSelectedTargets() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return tx("No project");
    std::vector<ClipId> clips;
    QString label;
    for (const ClipId id : m_controller->selection()) {
        const auto loc = seq->findClip(id);
        if (!loc)
            continue;
        // A video clip stands for its linked audio (the command resolves it); a clip without audio is skipped here.
        if (loc->track->kind != TrackKind::Audio && !loc->clip().linkGroup.isValid())
            continue;
        clips.push_back(id);
        if (label.isEmpty())
            label = QString::fromStdString(std::string(loc->clip().name()));
    }
    if (clips.empty())
        return tx("Select the clips to match on the timeline");
    m_match.targets = std::move(clips);
    m_match.targetLabel = label;
    m_match.learned = false;
    emit matchChanged();
    return {};
}

void AudioMixerBridge::setMatchOption(const QString& name, const QVariant& value) {
    bool changed = false;
    if (name == QLatin1String("amount")) {
        const double v = std::clamp(value.toDouble(), 0.0, 100.0);
        changed = v != m_match.amount;
        m_match.amount = v;
    } else if (name == QLatin1String("smoothing")) {
        const double v = std::clamp(value.toDouble(), 0.0, 100.0);
        changed = v != m_match.smoothing;
        m_match.smoothing = v;
    } else if (name == QLatin1String("headroom")) {
        const double v = std::clamp(value.toDouble(), 0.0, 24.0);
        changed = v != m_match.headroom;
        m_match.headroom = v;
    } else if (name == QLatin1String("mode")) {
        const QString v = value.toString() == QLatin1String("peak") ? QStringLiteral("peak") : QStringLiteral("loudness");
        changed = v != m_match.mode;
        m_match.mode = v;
    } else if (name == QLatin1String("limit")) {
        changed = value.toBool() != m_match.limit;
        m_match.limit = value.toBool();
    }
    if (changed)
        emit matchChanged();
}

command::CommandRequest AudioMixerBridge::matchRequest(const char* command) const {
    command::CommandRequest request;
    request.command = command;
    Json params = Json::object();
    if (m_match.referenceKind == QLatin1String("clip")) {
        params["reference"] = Json{{"clip", m_match.referenceId}};
    } else if (m_match.referenceKind == QLatin1String("media")) {
        params["reference"] = Json{{"media", m_match.referenceId}};
    } else {
        params["reference"] = "mix";
    }
    if (m_match.referenceFrom)
        params["referenceFrom"] = *m_match.referenceFrom;
    if (m_match.referenceTo)
        params["referenceTo"] = *m_match.referenceTo;
    Json clips = Json::array();
    for (const ClipId id : m_match.targets)
        clips.push_back(id.value);
    params["clips"] = std::move(clips);
    request.params = std::move(params);
    return request;
}

QString AudioMixerBridge::runMatch(const QString& kind, const char* command, bool apply) {
    if (!m_match.referenceSet)
        return tx("Choose a reference first");
    if (m_match.targets.empty())
        return tx("Choose the clips to match first");
    command::CommandRequest request = matchRequest(command);
    if (kind == QLatin1String("matchLevel")) {
        request.params["mode"] = m_match.mode.toStdString();
        request.params["headroom"] = m_match.headroom;
        request.params["limit"] = m_match.limit;
    } else {
        request.params["amount"] = m_match.amount;
        request.params["smoothing"] = m_match.smoothing;
        request.params["apply"] = apply;
    }
    m_match.message.clear();
    emit matchChanged();
    return startJob(kind, {request});
}

QString AudioMixerBridge::matchLearn() {
    return runMatch(QStringLiteral("matchLearn"), "audio.eqMatch", false);
}

QString AudioMixerBridge::matchApplyEq() {
    return runMatch(QStringLiteral("matchEq"), "audio.eqMatch", true);
}

QString AudioMixerBridge::matchApplyLevel() {
    return runMatch(QStringLiteral("matchLevel"), "audio.levelMatch", true);
}

QString AudioMixerBridge::matchReset() {
    if (m_match.targets.empty())
        return tx("Choose the clips first");
    command::CommandRequest request;
    request.command = "audio.eqMatchReset";
    Json clips = Json::array();
    for (const ClipId id : m_match.targets)
        clips.push_back(id.value);
    request.params = Json{{"clips", std::move(clips)}};
    const QString error = errorText(runBatch({request}, tx("Reset EQ Match").toStdString()));
    emit matchChanged();
    return error;
}

QString AudioMixerBridge::matchTogglePreview() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return tx("No project");
    std::vector<command::CommandRequest> requests;
    bool anyOn = false;
    std::vector<std::pair<ClipId, EffectInstanceId>> found;
    for (const ClipId id : m_match.targets) {
        const auto loc = seq->findClip(id);
        const EffectInstance* e = loc ? eqMatchOf(loc->clip()) : nullptr;
        if (!e)
            continue;
        found.emplace_back(id, e->id);
        anyOn = anyOn || e->enabled;
    }
    if (found.empty())
        return tx("There is no EQ Match on these clips yet: apply it first");
    for (const auto& [clip, effect] : found) {
        command::CommandRequest request;
        request.command = "clip.setEffect";
        request.params = Json{{"clip", clip.value}, {"effect", effect.value}, {"enabled", !anyOn}};
        requests.push_back(std::move(request));
    }
    const QString error =
        errorText(runBatch(std::move(requests), (anyOn ? tx("Bypass EQ Match") : tx("Enable EQ Match")).toStdString()));
    emit matchChanged();
    return error;
}

namespace {

QVariantMap statsOf(const Json& s) {
    return QVariantMap{{QStringLiteral("integrated"), numberOrNull(s.value("integratedLufs", Json()))},
                       {QStringLiteral("truePeak"), numberOrNull(s.value("truePeakDbtp", Json()))},
                       {QStringLiteral("range"), numberOrNull(s.value("loudnessRangeLu", Json()))},
                       {QStringLiteral("rms"), numberOrNull(s.value("rmsDbfs", Json()))},
                       {QStringLiteral("crest"), numberOrNull(s.value("crestDb", Json()))},
                       {QStringLiteral("seconds"), numberOrNull(s.value("seconds", Json()))}};
}

double numberAt(const Json& j, const char* key, double fallback) {
    const Json& v = j.value(key, Json());
    return v.is_number() ? v.get<double>() : fallback;
}

} // namespace

void AudioMixerBridge::matchFinish(const QString& kind, const std::vector<command::CommandResult>& results,
                                   std::uint64_t baseRevision) {
    const Json& data = results.front().data;
    if (kind == QLatin1String("matchEq") || kind == QLatin1String("matchLevel")) {
        // The measuring ran on a copy of the project; its result is committed as one undo step when the project is still
        // the one it measured.
        const bool level = kind == QLatin1String("matchLevel");
        if (!results.front().preview) {
            m_match.message = tx("Nothing to change");
        } else if (!m_controller || m_controller->session().revision() != baseRevision) {
            m_match.message = tx("The project changed while it was measuring: try again");
        } else if (auto committed = m_controller->session().commit((level ? tx("Level Match") : tx("EQ Match")).toStdString(),
                                                                    *results.front().preview);
                   !committed) {
            m_match.message = QString::fromStdString(committed.error().message);
        } else {
            m_match.message = level ? tx("Level matched") : tx("EQ Match applied");
            for (const std::string& w : results.front().warnings)
                m_match.message += QStringLiteral(" · ") + QString::fromStdString(w);
        }
        if (level) {
            QVariantList report;
            for (const Json& r : data.value("results", Json::array())) {
                const Json& target = r.value("target", Json());
                QString name = target.is_string() ? tx("Mix") : QString::fromStdString(target.dump());
                if (target.is_object() && target.contains("clip") && target["clip"].is_number_unsigned()) {
                    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
                    if (const auto loc = seq ? seq->findClip(ClipId{target["clip"].get<std::uint64_t>()}) : std::nullopt)
                        name = QString::fromStdString(std::string(loc->clip().name()));
                } else if (target.is_object() && target.contains("track")) {
                    name = QString::fromStdString(target["track"].is_string() ? target["track"].get<std::string>() : target["track"].dump());
                }
                QVariantMap row{{QStringLiteral("target"), name},
                                {QStringLiteral("gainDb"), numberOrNull(r.value("gainDb", Json()))},
                                {QStringLiteral("before"), statsOf(r.value("before", Json::object()))},
                                {QStringLiteral("after"), statsOf(r.value("after", Json::object()))},
                                {QStringLiteral("limited"), r.value("limited", false)}};
                report.append(row);
            }
            m_match.report = report;
            // The target sounds as the report's "after" now: its levels are those (the spectra stay: only the gain moved).
            if (!report.isEmpty() && !report.first().toMap().value(QStringLiteral("after")).toMap().value(QStringLiteral("integrated")).isNull())
                m_match.targetStats = report.first().toMap().value(QStringLiteral("after")).toMap();
        }
        emit matchChanged();
        return;
    }
    if (kind == QLatin1String("matchLearn")) {
        const Json& curve = data.value("curve", Json::object());
        audio::edit::SpectrumProfile reference;
        audio::edit::SpectrumProfile target;
        for (const Json& band : curve.value("bands", Json::array())) {
            const int b = band.value("band", 0) - 1;
            if (b < 0 || b >= audio::edit::kMatchBandCount)
                continue;
            reference.levelDb[static_cast<std::size_t>(b)] = numberAt(band, "referenceDb", std::numeric_limits<double>::quiet_NaN());
            target.levelDb[static_cast<std::size_t>(b)] = numberAt(band, "targetDb", std::numeric_limits<double>::quiet_NaN());
        }
        const Json& refStats = data.value("reference", Json::object()).value("stats", Json::object());
        const Json& tgtStats = data.value("target", Json::object()).value("stats", Json::object());
        reference.activeSeconds = numberAt(refStats, "audibleSeconds", 0.0);
        reference.seconds = numberAt(refStats, "seconds", 0.0);
        target.activeSeconds = numberAt(tgtStats, "audibleSeconds", 0.0);
        target.seconds = numberAt(tgtStats, "seconds", 0.0);
        m_match.reference = reference;
        m_match.target = target;
        m_match.referenceStats = statsOf(refStats);
        m_match.targetStats = statsOf(tgtStats);
        m_match.learned = true;
        m_match.learnedRevision = m_controller ? m_controller->session().revision() : 0;
        m_match.message = tx("Learned. The EQ curve and the level difference are below.");
        for (const std::string& w : results.front().warnings)
            m_match.message += QStringLiteral(" ") + QString::fromStdString(w);
    }
    emit matchChanged();
}

QVariantMap AudioMixerBridge::match() const {
    QVariantMap out;
    out.insert(QStringLiteral("reference"), QVariantMap{{QStringLiteral("set"), m_match.referenceSet},
                                                        {QStringLiteral("kind"), m_match.referenceKind},
                                                        {QStringLiteral("label"), m_match.referenceLabel}});
    out.insert(QStringLiteral("target"), QVariantMap{{QStringLiteral("count"), static_cast<int>(m_match.targets.size())},
                                                     {QStringLiteral("label"), m_match.targetLabel}});
    out.insert(QStringLiteral("options"), QVariantMap{{QStringLiteral("amount"), m_match.amount},
                                                      {QStringLiteral("smoothing"), m_match.smoothing},
                                                      {QStringLiteral("mode"), m_match.mode},
                                                      {QStringLiteral("headroom"), m_match.headroom},
                                                      {QStringLiteral("limit"), m_match.limit}});
    out.insert(QStringLiteral("busy"), m_busy == QLatin1String("matchLearn") || m_busy == QLatin1String("matchEq") ||
                                           m_busy == QLatin1String("matchLevel"));
    out.insert(QStringLiteral("learned"), m_match.learned);
    out.insert(QStringLiteral("message"), m_match.message);
    out.insert(QStringLiteral("report"), m_match.report);
    out.insert(QStringLiteral("referenceStats"), m_match.referenceStats);
    out.insert(QStringLiteral("targetStats"), m_match.targetStats);

    QVariantList bands;
    double offset = 0.0;
    if (m_match.learned) {
        audio::edit::EqMatchOptions options;
        options.amount = m_match.amount / 100.0;
        options.smoothing = m_match.smoothing / 100.0;
        const audio::edit::EqMatchCurve curve = audio::edit::fitEqMatch(m_match.reference, m_match.target, options);
        offset = curve.levelOffsetDb;
        for (int b = 0; b < audio::edit::kMatchBandCount; ++b) {
            const auto i = static_cast<std::size_t>(b);
            bands.append(QVariantMap{{QStringLiteral("hz"), audio::edit::matchBandHz(b)},
                                     {QStringLiteral("referenceDb"), numberOrNull(m_match.reference.levelDb[i])},
                                     {QStringLiteral("targetDb"), numberOrNull(m_match.target.levelDb[i])},
                                     {QStringLiteral("gainDb"), curve.valid ? QVariant(curve.gainDb[i]) : QVariant()}});
        }
        const bool peak = m_match.mode == QLatin1String("peak");
        const QVariant ref = m_match.referenceStats.value(peak ? QStringLiteral("truePeak") : QStringLiteral("integrated"));
        const QVariant tgt = m_match.targetStats.value(peak ? QStringLiteral("truePeak") : QStringLiteral("integrated"));
        out.insert(QStringLiteral("levelGainDb"), !ref.isNull() && !tgt.isNull() ? QVariant(ref.toDouble() - tgt.toDouble()) : QVariant());
    }
    out.insert(QStringLiteral("bands"), bands);
    out.insert(QStringLiteral("levelOffsetDb"), offset);

    // Where the EQ Match already is on the target clips.
    int applied = 0;
    int bypassed = 0;
    if (const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr) {
        for (const ClipId id : m_match.targets) {
            const auto loc = seq->findClip(id);
            if (const EffectInstance* e = loc ? eqMatchOf(loc->clip()) : nullptr) {
                ++applied;
                if (!e->enabled)
                    ++bypassed;
            }
        }
    }
    out.insert(QStringLiteral("applied"), applied);
    out.insert(QStringLiteral("bypassed"), bypassed);
    return out;
}

} // namespace wabcut::app
