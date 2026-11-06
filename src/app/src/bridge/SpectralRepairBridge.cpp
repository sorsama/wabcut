#include "SpectralRepairBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "SpectralSources.h"

#include <wabcut/audio/dsp/SpectralRepair.h>
#include <QCoreApplication>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <functional>

namespace wabcut::app {

using command::Json;
namespace dsp = wabcut::audio::dsp;

namespace {

/// What the analysis cache may hold: tiles of 8-bit levels (about 260 kB each).
constexpr std::size_t kBudgetBytes = 96U << 20;
constexpr double kMinSpanSeconds = 0.05;

int snapFft(int size) { return size <= 1536 ? 1024 : size <= 3072 ? 2048 : 4096; }

Ticks ticksOf(double seconds) {
    return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
}

/// The audio clip a selected clip stands for: itself, or the audio clip linked to a video clip.
const Clip* audioClipFor(const Sequence& seq, ClipId id) {
    const auto at = seq.findClip(id);
    if (!at) {
        return nullptr;
    }
    const Clip& clip = at->clip();
    if (clip.stream.kind == StreamKind::Audio) {
        return &clip;
    }
    if (clip.linkGroup.isValid()) {
        for (const TrackPtr& t : seq.audioTracks) {
            for (const Clip& other : t->clips) {
                if (other.linkGroup == clip.linkGroup) {
                    return &other;
                }
            }
        }
    }
    return nullptr;
}

QString kindLabel(SpectralKind k) { return QString::fromLatin1(toString(k)); }

} // namespace

SpectralRepairBridge::SpectralRepairBridge(QObject* parent) : QObject(parent) {}

SpectralRepairBridge* SpectralRepairBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new SpectralRepairBridge;
    if (qmlEngine) {
        bridge->m_playback = qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    }
    return bridge;
}

void SpectralRepairBridge::setController(ProjectController* controller) {
    if (m_controller == controller) {
        return;
    }
    if (m_controller) {
        disconnect(m_controller, nullptr, this, nullptr);
    }
    m_controller = controller;
    if (m_controller) {
        const auto all = [this] {
            refreshClip();
            refreshEdits();
            refreshPlayhead();
        };
        connect(m_controller, &ProjectController::revisionChanged, this, all);
        connect(m_controller, &ProjectController::sequenceChanged, this, all);
        connect(m_controller, &ProjectController::selectionChanged, this, all);
        connect(m_controller, &ProjectController::selectedClipChanged, this, all);
        connect(m_controller, &ProjectController::playheadChanged, this, &SpectralRepairBridge::refreshPlayhead);
    }
    refreshClip();
    refreshEdits();
    refreshPlayhead();
}

void SpectralRepairBridge::setSourceFactory(SourceFactory factory) {
    m_sourceFactory = std::move(factory);
    invalidateCaches(true, true);
}

void SpectralRepairBridge::setClipOverride(std::optional<SpectralClip> clip) {
    m_override = std::move(clip);
    refreshClip();
    refreshEdits();
}

QString SpectralRepairBridge::clip() const { return m_available ? clipKey(m_clip.clip) : QString(); }

void SpectralRepairBridge::setMessage(const QString& message) {
    if (m_message == message) {
        return;
    }
    m_message = message;
    emit messageChanged();
}

void SpectralRepairBridge::invalidateCaches(bool original, bool result) {
    if (original) {
        m_original.reset();
    }
    if (result) {
        m_result.reset();
        m_resultHash = 0;
    }
    ++m_revision;
    emit analysisChanged();
}

void SpectralRepairBridge::clampView() {
    const double total = std::max(m_clip.rangeEnd - m_clip.rangeStart, kMinSpanSeconds);
    m_viewSpan = std::clamp(m_viewSpan, kMinSpanSeconds, total);
    m_viewStart = std::clamp(m_viewStart, m_clip.rangeStart, std::max(m_clip.rangeStart, m_clip.rangeEnd - m_viewSpan));
}

void SpectralRepairBridge::refreshClip() {
    std::optional<SpectralClip> found;
    QString reason;
    if (m_override) {
        found = m_override;
    } else if (m_controller) {
        const SequencePtr seq = m_controller->currentSequence();
        const ProjectSnapshot snap = m_controller->snapshot();
        reason = tr("Select an audio clip on the timeline to repair its sound");
        if (seq && snap) {
            for (const ClipId id : m_controller->selection()) {
                const Clip* clip = audioClipFor(*seq, id);
                if (clip == nullptr) {
                    continue;
                }
                if (clip->isNested() || clip->hasUnknownSource()) {
                    reason = tr("Spectral Repair works on a clip of a media file, not on a nested sequence");
                    continue;
                }
                const MediaItem* media = snap->findMedia(clip->media);
                if (media == nullptr || clip->stream.index < 0 || static_cast<std::size_t>(clip->stream.index) >= media->info.audio.size()) {
                    reason = tr("This clip's media has no audio");
                    continue;
                }
                const AudioStreamInfo& stream = media->info.audio[static_cast<std::size_t>(clip->stream.index)];
                SpectralClip c;
                c.clip = clip->id;
                c.media = clip->media;
                c.streamOrdinal = clip->stream.index;
                c.sampleRate = std::clamp(static_cast<int>(std::lround(stream.sampleRate.toDouble())), 8000, 192000);
                c.channels = std::clamp(stream.channels > 0 ? stream.channels : 2, 1, 8);
                const Ticks duration = stream.duration.value > 0 ? stream.duration : media->info.duration;
                c.lengthSamples = std::max<std::int64_t>(ticksToSamples(duration, Rate{c.sampleRate, 1}), 0);
                const TimeRange shown = clip->shownSourceRange(clip->start(), clip->end());
                c.rangeStart = std::max(0.0, shown.start.toSeconds());
                c.rangeEnd = std::max(c.rangeStart + kMinSpanSeconds, shown.end().toSeconds());
                c.name = QString::fromStdString(media->name);
                found = c;
                reason.clear();
                break;
            }
        }
    }
    if (!found && !m_available && !m_reason.isEmpty() && (m_override || m_controller)) {
        const QString again = m_override ? QString() : (reason.isEmpty() ? m_reason : reason);
        if (again == m_reason) {
            return; // still nothing to repair, for the same reason
        }
    }
    const bool same = found && m_available && found->clip == m_clip.clip && found->media == m_clip.media &&
                      found->streamOrdinal == m_clip.streamOrdinal && found->sampleRate == m_clip.sampleRate &&
                      found->channels == m_clip.channels && found->lengthSamples == m_clip.lengthSamples &&
                      found->rangeStart == m_clip.rangeStart && found->rangeEnd == m_clip.rangeEnd;
    if (same) {
        return;
    }
    const bool sameSound = found && m_available && found->media == m_clip.media && found->streamOrdinal == m_clip.streamOrdinal &&
                           found->sampleRate == m_clip.sampleRate && found->channels == m_clip.channels &&
                           found->lengthSamples == m_clip.lengthSamples;
    m_available = found.has_value();
    m_reason = m_available ? QString() : (reason.isEmpty() ? tr("Select an audio clip on the timeline to repair its sound") : reason);
    if (found) {
        m_clip = *found;
    } else {
        m_clip = SpectralClip{};
    }
    if (!sameSound) {
        m_channel = std::min(m_channel, std::max(m_clip.channels - 1, -1));
        invalidateCaches(true, true);
    }
    // Another clip (or another part of it): the whole of what it plays.
    m_viewStart = m_clip.rangeStart;
    m_viewSpan = std::max(m_clip.rangeEnd - m_clip.rangeStart, kMinSpanSeconds);
    clampView();
    emit clipChanged();
    emit viewChanged();
    refreshEdits();
    refreshPlayhead();
}

void SpectralRepairBridge::refreshEdits() {
    std::vector<SpectralEdit> edits;
    bool has = false;
    bool enabled = true;
    int listen = 0;
    int fft = m_fftSize;
    if (m_available && m_controller) {
        if (const SequencePtr seq = m_controller->currentSequence()) {
            if (const auto at = seq->findClip(m_clip.clip)) {
                for (const EffectInstance& e : at->clip().effects()) {
                    if (e.effect != kSpectralRepairEffect) {
                        continue;
                    }
                    has = true;
                    enabled = e.enabled;
                    edits = unpackSpectralEdits(e);
                    const auto choice = [&](const char* key, double fallback) {
                        const ParamValue* v = e.find(key);
                        return static_cast<int>(std::lround(v != nullptr ? v->constant() : fallback));
                    };
                    listen = std::clamp(choice("listen", 0.0), 0, 2);
                    const int size = std::clamp(choice("fftSize", 1.0), 0, 2);
                    fft = size == 0 ? 1024 : size == 1 ? 2048 : 4096;
                    break;
                }
            }
        }
    }
    std::string joined;
    for (const SpectralEdit& e : edits) {
        joined += encodeSpectralEdit(e);
        joined += '\n';
    }
    joined += enabled ? '1' : '0';
    const std::uint64_t hash = std::hash<std::string>{}(joined);
    const bool editsMoved = hash != m_editsHash || has != m_hasRepair || listen != m_listen;
    const bool fftMoved = has && fft != m_fftSize;
    m_edits = std::move(edits);
    m_hasRepair = has;
    m_repairEnabled = enabled;
    m_listen = listen;
    m_editsHash = hash;
    if (fftMoved) {
        m_fftSize = fft;
        invalidateCaches(true, true);
        emit viewChanged();
    }
    if (!editsMoved && !fftMoved) {
        return;
    }
    m_editsVariant.clear();
    for (std::size_t i = 0; i < m_edits.size(); ++i) {
        const SpectralEdit& e = m_edits[i];
        const SpectralBounds b = spectralBounds(e);
        m_editsVariant.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                                          {QStringLiteral("kind"), kindLabel(e.kind)},
                                          {QStringLiteral("shape"), QString::fromLatin1(toString(e.shape))},
                                          {QStringLiteral("start"), b.start},
                                          {QStringLiteral("end"), b.end},
                                          {QStringLiteral("low"), b.low},
                                          {QStringLiteral("high"), b.high},
                                          {QStringLiteral("gainDb"), e.gainDb},
                                          {QStringLiteral("amount"), e.amount},
                                          {QStringLiteral("harmonics"), e.harmonics},
                                          {QStringLiteral("hz"), e.path.empty() ? 0.0 : e.path.front().hz}});
    }
    if (m_showResult && m_result) {
        m_result.reset();
        m_resultHash = 0;
        ++m_revision;
        emit analysisChanged();
    }
    emit editsChanged();
}

void SpectralRepairBridge::refreshPlayhead() {
    double at = -1.0;
    if (m_available && m_controller) {
        if (const SequencePtr seq = m_controller->currentSequence()) {
            if (const auto found = seq->findClip(m_clip.clip)) {
                const Clip& clip = found->clip();
                const Ticks ph = m_controller->playhead();
                if (ph >= clip.start() && ph < clip.end()) {
                    at = clip.curveTimeAt(ph).toSeconds();
                }
            }
        }
    }
    if (at != m_playhead) {
        m_playhead = at;
        emit playheadChanged();
    }
}

std::shared_ptr<dsp::SampleSource> SpectralRepairBridge::makeSource(int channel) {
    if (m_sourceFactory) {
        return m_sourceFactory(m_clip, channel);
    }
    if (!m_controller) {
        return nullptr;
    }
    const ProjectSnapshot snap = m_controller->snapshot();
    if (!snap) {
        return nullptr;
    }
    return std::make_shared<FeedSource>(std::make_shared<ProjectFeed>(*snap), m_clip.media, m_clip.streamOrdinal, m_clip.sampleRate,
                                        m_clip.lengthSamples, m_clip.channels, channel);
}

std::shared_ptr<dsp::SpectrogramCache> SpectralRepairBridge::displayCache() {
    if (!m_available) {
        return nullptr;
    }
    if (!m_original) {
        const auto source = makeSource(m_channel);
        if (!source) {
            return nullptr;
        }
        m_original = std::make_shared<dsp::SpectrogramCache>(source, m_clip.sampleRate, m_fftSize, kBudgetBytes);
    }
    if (!m_showResult) {
        return m_original;
    }
    if (!m_result || m_resultHash != m_editsHash) {
        const auto base = makeSource(m_channel);
        if (!base) {
            return m_original;
        }
        std::vector<SpectralEdit> edits = m_repairEnabled ? m_edits : std::vector<SpectralEdit>{};
        m_result = std::make_shared<dsp::SpectrogramCache>(
            std::make_shared<RepairedSource>(base, std::move(edits), m_clip.sampleRate, m_fftSize), m_clip.sampleRate, m_fftSize,
            kBudgetBytes);
        m_resultHash = m_editsHash;
    }
    return m_result;
}

void SpectralRepairBridge::setFftSize(int size) {
    size = snapFft(size);
    if (size == m_fftSize) {
        return;
    }
    const int before = m_fftSize;
    m_fftSize = size;
    if (m_hasRepair) {
        const QString error = run("audio.spectralRepairOptions", Json{{"clip", m_clip.clip.value}, {"fftSize", size}});
        if (!error.isEmpty()) {
            m_fftSize = before;
            return;
        }
    }
    invalidateCaches(true, true);
    emit viewChanged();
}

void SpectralRepairBridge::setLogFrequency(bool log) {
    if (log == m_log) {
        return;
    }
    m_log = log;
    emit viewChanged();
}

void SpectralRepairBridge::setShowResult(bool show) {
    if (show == m_showResult) {
        return;
    }
    m_showResult = show;
    ++m_revision;
    emit viewChanged();
    emit analysisChanged();
}

void SpectralRepairBridge::setChannel(int channel) {
    channel = std::clamp(channel, -1, std::max(m_clip.channels - 1, -1));
    if (channel == m_channel) {
        return;
    }
    m_channel = channel;
    invalidateCaches(true, true);
    emit viewChanged();
}

void SpectralRepairBridge::setFloorDb(double db) {
    db = std::clamp(db, -120.0, m_ceilingDb - 6.0);
    if (db == m_floorDb) {
        return;
    }
    m_floorDb = db;
    emit viewChanged();
}

void SpectralRepairBridge::setCeilingDb(double db) {
    db = std::clamp(db, m_floorDb + 6.0, 0.0);
    if (db == m_ceilingDb) {
        return;
    }
    m_ceilingDb = db;
    emit viewChanged();
}

void SpectralRepairBridge::setViewStart(double seconds) {
    const double before = m_viewStart;
    m_viewStart = seconds;
    clampView();
    if (m_viewStart != before) {
        emit viewChanged();
    }
}

void SpectralRepairBridge::setViewSpan(double seconds) {
    const double before = m_viewSpan;
    m_viewSpan = seconds;
    clampView();
    if (m_viewSpan != before) {
        emit viewChanged();
    }
}

void SpectralRepairBridge::zoomView(double factor, double anchorSeconds) {
    if (!(factor > 0.0) || !m_available) {
        return;
    }
    const double anchor = std::clamp(anchorSeconds, m_viewStart, m_viewStart + m_viewSpan);
    const double fraction = m_viewSpan > 0.0 ? (anchor - m_viewStart) / m_viewSpan : 0.5;
    const double span = std::clamp(m_viewSpan / factor, kMinSpanSeconds, std::max(m_clip.rangeEnd - m_clip.rangeStart, kMinSpanSeconds));
    m_viewSpan = span;
    m_viewStart = anchor - fraction * span;
    clampView();
    emit viewChanged();
}

void SpectralRepairBridge::scrollView(double seconds) {
    const double before = m_viewStart;
    m_viewStart += seconds;
    clampView();
    if (m_viewStart != before) {
        emit viewChanged();
    }
}

void SpectralRepairBridge::fitView() {
    m_viewStart = m_clip.rangeStart;
    m_viewSpan = std::max(m_clip.rangeEnd - m_clip.rangeStart, kMinSpanSeconds);
    clampView();
    emit viewChanged();
}

Json SpectralRepairBridge::clipParams() const { return Json{{"clip", m_clip.clip.value}}; }

QString SpectralRepairBridge::run(const std::string& command, Json params) {
    if (!m_controller || !m_available) {
        return tr("Select an audio clip first");
    }
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, command, std::move(params));
    const QString error = commandErrorText(result);
    setMessage(error);
    return error;
}

QString SpectralRepairBridge::addEdit(const QString& kind, const QString& shape, const QVariantList& points, const QVariantMap& options) {
    if (!m_available) {
        return tr("Select an audio clip first");
    }
    static const std::vector<std::pair<QString, std::string>> commands{{QStringLiteral("attenuate"), "audio.spectralAttenuate"},
                                                                       {QStringLiteral("heal"), "audio.spectralHeal"},
                                                                       {QStringLiteral("noiseFloor"), "audio.spectralNoiseFloor"},
                                                                       {QStringLiteral("tone"), "audio.spectralRemoveTone"}};
    const auto name = std::ranges::find(commands, kind, &std::pair<QString, std::string>::first);
    if (name == commands.end()) {
        return tr("Unknown repair");
    }
    const double nyquist = m_clip.sampleRate / 2.0;
    const auto clampT = [&](double t) { return std::clamp(t, m_clip.rangeStart, m_clip.rangeEnd); };
    const auto clampF = [&](double f) { return std::clamp(f, 20.0, nyquist); };
    Json params = clipParams();
    Json list = Json::array();
    double t0 = 1.0e18, t1 = -1.0e18, f0 = 1.0e18, f1 = -1.0e18;
    for (const QVariant& v : points) {
        const QVariantMap p = v.toMap();
        const double t = clampT(p.value(QStringLiteral("time")).toDouble());
        const double f = clampF(p.value(QStringLiteral("hz")).toDouble());
        t0 = std::min(t0, t);
        t1 = std::max(t1, t);
        f0 = std::min(f0, f);
        f1 = std::max(f1, f);
        list.push_back(Json{{"time", t}, {"hz", f}});
    }
    params["shape"] = shape.toStdString();
    if (kind == QLatin1String("tone")) {
        // A tone: the trace (points), or a steady frequency over the time range; the range is the selection's, else the clip's.
        const double start = clampT(options.contains(QStringLiteral("start")) ? options.value(QStringLiteral("start")).toDouble() : (list.empty() ? m_clip.rangeStart : t0));
        const double end = clampT(options.contains(QStringLiteral("end")) ? options.value(QStringLiteral("end")).toDouble() : (list.empty() ? m_clip.rangeEnd : t1));
        params["start"] = start;
        params["end"] = std::max(end, start + 0.01);
        params["shape"] = "rectangle";
        if (options.contains(QStringLiteral("hz")) && options.value(QStringLiteral("hz")).toDouble() > 0.0) {
            params["hz"] = clampF(options.value(QStringLiteral("hz")).toDouble());
        } else if (!list.empty()) {
            params["points"] = list;
        }
    } else if (shape == QLatin1String("rectangle")) {
        if (list.size() < 2) {
            return tr("Drag a rectangle on the spectrogram first");
        }
        params["start"] = t0;
        params["end"] = std::max(t1, t0 + 0.001);
        params["low"] = f0;
        params["high"] = std::max(f1, f0 + 1.0);
    } else {
        params["points"] = list;
    }
    for (const char* key : {"gainDb", "amount", "radiusSeconds", "radiusOctaves", "widthHz"}) {
        const QString k = QString::fromLatin1(key);
        if (options.contains(k)) {
            params[key] = options.value(k).toDouble();
        }
    }
    if (options.contains(QStringLiteral("harmonics"))) {
        params["harmonics"] = options.value(QStringLiteral("harmonics")).toInt();
    }
    if (options.contains(QStringLiteral("logFrequency"))) {
        params["logFrequency"] = options.value(QStringLiteral("logFrequency")).toBool();
    }
    return run(name->second, std::move(params));
}

QString SpectralRepairBridge::removeEdit(int index) {
    Json p = clipParams();
    p["index"] = index;
    return run("audio.spectralRepairRemove", std::move(p));
}

QString SpectralRepairBridge::clearEdits() {
    Json p = clipParams();
    p["all"] = true;
    return run("audio.spectralRepairRemove", std::move(p));
}

QString SpectralRepairBridge::setRepairEnabled(bool enabled) {
    Json p = clipParams();
    p["enabled"] = enabled;
    return run("audio.spectralRepairOptions", std::move(p));
}

QString SpectralRepairBridge::setListen(int mode) {
    Json p = clipParams();
    p["listen"] = mode == 1 ? "selection" : mode == 2 ? "removed" : "off";
    return run("audio.spectralRepairOptions", std::move(p));
}

namespace {

/// A column of the analysis at `seconds`: from the finest level whose tile is already computed (null tile: none is).
struct CachedColumn {
    std::shared_ptr<const dsp::SpectrogramTile> tile;
    int column = 0;
};

CachedColumn cachedColumn(const dsp::SpectrogramCache& cache, double seconds) {
    const double base = cache.columnSeconds();
    for (int level = 0; level <= cache.maxLevel(); ++level) {
        const double levelSeconds = base * static_cast<double>(std::int64_t{1} << level);
        const auto c = static_cast<std::int64_t>(std::floor((seconds + 0.5 * base) / levelSeconds));
        if (c < 0 || c >= cache.columns(level)) {
            continue;
        }
        if (auto tile = cache.peek(level, c / dsp::kSpectrogramTileColumns)) {
            const int column = static_cast<int>(c % dsp::kSpectrogramTileColumns);
            if (column < tile->columns) {
                return {std::move(tile), column};
            }
        }
    }
    return {};
}

} // namespace

double SpectralRepairBridge::findTone(double start, double end, double low, double high) {
    if (!m_available || !m_original || end <= start) {
        return 0.0;
    }
    const dsp::SpectrogramCache& cache = *m_original;
    const double binHz = static_cast<double>(m_clip.sampleRate) / cache.fftSize();
    const int b0 = std::max(1, static_cast<int>(std::floor(low / binHz)));
    const int b1 = std::min(cache.bins() - 2, static_cast<int>(std::ceil(high / binHz)));
    if (b1 <= b0) {
        return 0.0;
    }
    std::vector<double> power(static_cast<std::size_t>(b1 - b0 + 3), 0.0); // bins b0-1 .. b1+1
    int used = 0;
    constexpr int kSamples = 96;
    for (int k = 0; k < kSamples; ++k) {
        const double t = start + (end - start) * (k + 0.5) / kSamples;
        const CachedColumn c = cachedColumn(cache, t);
        if (!c.tile) {
            continue; // not analysed yet
        }
        for (int b = b0 - 1; b <= b1 + 1; ++b) {
            power[static_cast<std::size_t>(b - b0 + 1)] += std::pow(10.0, dsp::spectrogramDb(c.tile->at(c.column, b)) / 10.0);
        }
        ++used;
    }
    if (used < 3) {
        return 0.0;
    }
    std::vector<double> db(power.size());
    for (std::size_t i = 0; i < power.size(); ++i) {
        db[i] = 10.0 * std::log10(power[i] / static_cast<double>(used) + 1e-18);
    }
    std::size_t best = 1;
    for (std::size_t i = 1; i + 1 < db.size(); ++i) {
        if (db[i] > db[best]) {
            best = i;
        }
    }
    std::vector<double> sorted(db.begin() + 1, db.end() - 1);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    if (db[best] < sorted[sorted.size() / 2] + 6.0) {
        return 0.0; // nothing stands out
    }
    // A parabola through the peak and its neighbours (in dB) gives the frequency between bins.
    const double a = db[best - 1];
    const double b = db[best];
    const double c = db[best + 1];
    const double denominator = a - 2.0 * b + c;
    const double shift = denominator != 0.0 ? std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5) : 0.0;
    return (static_cast<double>(b0 - 1 + static_cast<int>(best)) + shift) * binHz;
}

double SpectralRepairBridge::peakLevel(double start, double end, double low, double high) {
    if (!m_available || !m_original || end <= start) {
        return -1000.0;
    }
    const dsp::SpectrogramCache& cache = *m_original;
    const double binHz = static_cast<double>(m_clip.sampleRate) / cache.fftSize();
    const int b0 = std::max(0, static_cast<int>(std::floor(low / binHz)));
    const int b1 = std::min(cache.bins() - 1, static_cast<int>(std::ceil(high / binHz)));
    double best = -1000.0;
    constexpr int kSamples = 96;
    for (int k = 0; k < kSamples; ++k) {
        const CachedColumn c = cachedColumn(cache, start + (end - start) * (k + 0.5) / kSamples);
        if (!c.tile) {
            continue;
        }
        for (int b = b0; b <= b1; ++b) {
            best = std::max(best, dsp::spectrogramDb(c.tile->at(c.column, b)));
        }
    }
    return best;
}

void SpectralRepairBridge::seekTo(double sourceSeconds) {
    if (!m_available || !m_controller) {
        return;
    }
    const SequencePtr seq = m_controller->currentSequence();
    const auto at = seq ? seq->findClip(m_clip.clip) : std::nullopt;
    if (!at) {
        return;
    }
    if (const auto t = at->clip().timelineTimeOfSourceTime(ticksOf(sourceSeconds))) {
        m_controller->setPlayhead(*t);
    }
}

QString SpectralRepairBridge::playSelection(double startSeconds, double endSeconds) {
    if (!m_available || !m_controller) {
        return tr("Select an audio clip first");
    }
    if (!m_playback) {
        return tr("Playback is not available here");
    }
    const SequencePtr seq = m_controller->currentSequence();
    const auto at = seq ? seq->findClip(m_clip.clip) : std::nullopt;
    if (!at) {
        return tr("The clip is gone");
    }
    const Clip& clip = at->clip();
    const auto from = clip.timelineTimeOfSourceTime(ticksOf(startSeconds));
    const auto to = clip.timelineTimeOfSourceTime(ticksOf(endSeconds));
    if (!from) {
        return tr("The selection is outside the clip");
    }
    const Ticks end = to ? *to : clip.end();
    m_playback->playRange(*from, std::min(std::max(end, *from + Ticks{kTicksPerSecond / 20}), clip.end()));
    return {};
}

void SpectralRepairBridge::open() { emit openRequested(); }

QString SpectralRepairBridge::durationText(double seconds) const {
    if (seconds < 1.0) {
        return tr("%1 ms").arg(static_cast<int>(std::lround(seconds * 1000.0)));
    }
    return tr("%1 s").arg(seconds, 0, 'f', seconds < 10.0 ? 2 : 1);
}

} // namespace wabcut::app
