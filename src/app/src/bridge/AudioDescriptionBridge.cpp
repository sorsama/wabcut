#include "AudioDescriptionBridge.h"

#include "AiController.h"
#include "CommandJob.h"
#include "GlobalBridge.h"
#include "ProgramToolsBridge.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/Provider.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/command/Engine.h>

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QQmlEngine>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <stop_token>

namespace wabcut::app {

using command::Json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

constexpr int kFrameWidth = 768;

/// The first chat model the user's preferences resolve to that takes pictures, or null.
struct Pick {
    std::string id;
    std::string name;
    bool local = false;
};

std::optional<Pick> pickVisionModel(::AiController* ai) {
    if (!ai)
        return std::nullopt;
    auto resolved = ai->platform().resolve(ai::Capability::Chat);
    if (!resolved)
        return std::nullopt;
    for (const auto& candidate : *resolved) {
        if (candidate.model && candidate.model->caps.vision)
            return Pick{candidate.model->id, candidate.model->name.empty() ? candidate.model->id : candidate.model->name,
                        ai->runsLocally(candidate.model->id)};
    }
    return std::nullopt;
}

/// A picture of the sequence at `at` as a JPEG, or empty.
ai::ImagePart pictureOf(const ProjectState& state, SequenceId sequence, Ticks at) {
    ai::ImagePart part;
    const auto frame = ProgramToolsBridge::renderStateFrame(state, sequence, at, kFrameWidth);
    if (!frame)
        return part;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!frame->save(&buffer, "JPG", 85))
        return part;
    part.mediaType = "image/jpeg";
    part.bytes.assign(bytes.begin(), bytes.end());
    return part;
}

} // namespace

struct AudioDescriptionBridge::Impl {
    std::thread worker;
    std::stop_source stop;
    std::shared_ptr<ai::IChatModel> model;
    std::expected<ai::dub::DescribeResult, ai::AiError> result{ai::dub::DescribeResult{}};
};

AudioDescriptionBridge::AudioDescriptionBridge(QObject* parent) : QObject(parent) {}

AudioDescriptionBridge::~AudioDescriptionBridge() {
    if (m_job) {
        m_job->stop.request_stop();
        if (m_job->worker.joinable())
            m_job->worker.join();
    }
}

AudioDescriptionBridge* AudioDescriptionBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new AudioDescriptionBridge;
    bridge->setEngine(qmlEngine);
    return bridge;
}

void AudioDescriptionBridge::setEngine(QQmlEngine* engine) {
    m_engine = engine;
    if (!engine)
        return;
    m_controller = engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    m_transcripts = engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    m_ai = engine->singletonInstance<::AiController*>("Wabcut", "AiController");
}

void AudioDescriptionBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

QVariantList AudioDescriptionBridge::gaps() const {
    QVariantList out;
    if (!m_controller)
        return out;
    for (std::size_t i = 0; i < m_gaps.size(); ++i) {
        const ai::dub::AdGap& g = m_gaps[i];
        out.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                               {QStringLiteral("startText"), m_controller->formatTimecode(g.range.start)},
                               {QStringLiteral("endText"), m_controller->formatTimecode(g.range.end())},
                               {QStringLiteral("seconds"), std::round(g.range.duration.toSeconds() * 10.0) / 10.0},
                               {QStringLiteral("maxWords"), g.maxWords},
                               {QStringLiteral("before"), qs(g.before)},
                               {QStringLiteral("after"), qs(g.after)}});
    }
    return out;
}

QVariantList AudioDescriptionBridge::lines() const {
    QVariantList out;
    if (!m_controller)
        return out;
    for (std::size_t i = 0; i < m_lines.size() && i < m_gaps.size(); ++i) {
        const ai::dub::AdLine& l = m_lines[i];
        const int words = ai::dub::wordCount(l.text);
        out.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                               {QStringLiteral("startText"), m_controller->formatTimecode(l.start)},
                               {QStringLiteral("seconds"), std::round(l.duration.toSeconds() * 10.0) / 10.0},
                               {QStringLiteral("text"), qs(l.text)},
                               {QStringLiteral("maxWords"), l.maxWords},
                               {QStringLiteral("words"), words},
                               {QStringLiteral("over"), words > l.maxWords},
                               {QStringLiteral("before"), qs(m_gaps[i].before)},
                               {QStringLiteral("after"), qs(m_gaps[i].after)}});
    }
    return out;
}

QString AudioDescriptionBridge::analyze(const QVariantMap& options) {
    if (!m_controller)
        return tr("No project");
    if (m_running)
        return tr("Busy");
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq) {
        setMessage(tr("Open a sequence first"));
        return m_message;
    }
    ai::dub::AdOptions timing;
    if (options.contains(QStringLiteral("minGap")))
        timing.minGap = Ticks{static_cast<std::int64_t>(std::llround(std::clamp(options.value(QStringLiteral("minGap")).toDouble(), 0.5, 60.0) * static_cast<double>(kTicksPerSecond)))};
    if (options.contains(QStringLiteral("wordsPerSecond")))
        timing.wordsPerSecond = std::clamp(options.value(QStringLiteral("wordsPerSecond")).toDouble(), 0.5, 6.0);
    std::vector<ai::dub::AdGap> found;
    if (options.value(QStringLiteral("silent")).toBool()) {
        found = ai::dub::findGaps(std::span<const TimeRange>{}, seq->duration, timing, nullptr);
    } else {
        static const ai::edit::SequenceTranscript kNoTranscript;
        const ai::edit::SequenceTranscript& transcript = m_transcripts ? m_transcripts->sequenceTranscript() : kNoTranscript;
        if (!m_transcripts || transcript.empty()) {
            m_gaps.clear();
            m_lines.clear();
            emit gapsChanged();
            emit linesChanged();
            setMessage(tr("Transcribe the sequence first (the Transcript tab), or say that this video has no speech"));
            return m_message;
        }
        found = ai::dub::findGaps(transcript, seq->duration, timing);
    }
    m_gaps = std::move(found);
    m_lines.clear();
    for (const ai::dub::AdGap& g : m_gaps)
        m_lines.push_back(ai::dub::AdLine{g.range.start, g.range.duration, {}, g.maxWords});
    m_sequence = seq->id;
    m_track = 0;
    emit gapsChanged();
    emit linesChanged();
    setMessage(m_gaps.empty() ? tr("No gap in the dialogue is long enough for a description") : QString());
    return {};
}

ai::dub::DescribeOptions AudioDescriptionBridge::describeOptions(const QVariantMap& options) const {
    ai::dub::DescribeOptions out;
    if (const QString language = options.value(QStringLiteral("language")).toString().trimmed(); !language.isEmpty())
        out.language = language.toStdString();
    out.style = options.value(QStringLiteral("style")).toString().trimmed().left(300).toStdString();
    return out;
}

QVariantMap AudioDescriptionBridge::quote(const QVariantMap& options) const {
    QVariantMap q{{QStringLiteral("available"), false}};
    if (!m_ai || !m_controller) {
        q.insert(QStringLiteral("reason"), tr("AI is not available"));
        return q;
    }
    const auto pick = pickVisionModel(m_ai);
    if (!pick) {
        q.insert(QStringLiteral("reason"),
                 tr("No chat model that can see pictures is set up, AI is offline, or this project is no-cloud (Settings > AI)"));
        return q;
    }
    const int pictures = std::clamp(options.value(QStringLiteral("frames"), 1).toInt(), 1, 3);
    const int perGap = pictures;
    q.insert(QStringLiteral("model"), qs(pick->id));
    q.insert(QStringLiteral("name"), qs(pick->name));
    q.insert(QStringLiteral("local"), pick->local);
    q.insert(QStringLiteral("pictures"), static_cast<int>(m_gaps.size()) * perGap);
    // The price of one request (a typical picture) times the gaps.
    ai::ImagePart sample;
    sample.mediaType = "image/jpeg";
    sample.bytes.assign(60'000, 0);
    ai::dub::AdGap gap = m_gaps.empty() ? ai::dub::AdGap{} : m_gaps.front();
    gap.before = std::string(80, 'a');
    gap.after = std::string(80, 'a');
    const ai::dub::DescribeOptions forModel = describeOptions(options);
    std::vector<ai::ImagePart> frames(static_cast<std::size_t>(perGap), sample);
    const ai::ChatRequest request = ai::dub::describeRequest(gap, std::move(frames), forModel, {});
    if (auto p = m_ai->platform().preflightChat(pick->id, request)) {
        const auto count = static_cast<ai::MicroUsd>(std::max<std::size_t>(1, m_gaps.size()));
        q.insert(QStringLiteral("priced"), p->cost.priced);
        q.insert(QStringLiteral("cost"), pick->local ? tr("free: it runs on this computer")
                                         : p->cost.priced ? tr("about %1, at most %2").arg(qs(ai::formatUsd(p->cost.expected * count)), qs(ai::formatUsd(p->cost.ceiling * count)))
                                                          : tr("this model's price is not known"));
    } else {
        q.insert(QStringLiteral("reason"), qs(p.error().message));
        return q;
    }
    const QString what = pick->local ? QString()
                                     : tr("%n picture(s) of your video (small JPEG frames) and the words said around each gap, as text", "",
                                          static_cast<int>(m_gaps.size()) * perGap);
    const QVariantMap prompt = m_ai->consentPrompt(qs(pick->id), QStringLiteral("chat"), what);
    q.insert(QStringLiteral("needed"), !pick->local && prompt.value(QStringLiteral("needed")).toBool());
    q.insert(QStringLiteral("prompt"), prompt);
    q.insert(QStringLiteral("available"), true);
    return q;
}

QString AudioDescriptionBridge::describe(const QVariantMap& options) {
    if (!m_controller || !m_ai)
        return tr("No project");
    if (m_running)
        return tr("Busy");
    if (m_gaps.empty()) {
        setMessage(tr("Find the gaps first"));
        return m_message;
    }
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq || seq->id != m_sequence) {
        setMessage(tr("The sequence changed: find the gaps again"));
        return m_message;
    }
    const auto pick = pickVisionModel(m_ai);
    if (!pick) {
        setMessage(tr("No chat model that can see pictures is set up, AI is offline, or this project is no-cloud (Settings > AI)"));
        return m_message;
    }
    const bool allowOnce = options.value(QStringLiteral("allowOnce")).toBool();
    // Never a quiet fallback: without the user's consent nothing is sent.
    if (!allowOnce && quote(options).value(QStringLiteral("needed")).toBool()) {
        setMessage(tr("Describing with your AI needs your consent first: nothing was sent"));
        return m_message;
    }
    auto model = m_ai->platform().chat(pick->id, ai::CallOptions{allowOnce, {}});
    if (!model) {
        setMessage(qs(model.error().message));
        return m_message;
    }
    if (m_job && m_job->worker.joinable())
        m_job->worker.join();
    m_job = std::make_shared<Impl>();
    m_job->model = std::shared_ptr<ai::IChatModel>(std::move(*model));
    m_cancel = false;
    m_running = true;
    m_progress = 0.0;
    emit runningChanged();
    setMessage({});

    const ProjectSnapshot snapshot = m_controller->snapshot();
    const SequenceId sequence = seq->id;
    const std::vector<ai::dub::AdGap> gapList = m_gaps;
    const ai::dub::DescribeOptions forModel = describeOptions(options);
    const int pictures = std::clamp(options.value(QStringLiteral("frames"), 1).toInt(), 1, 3);
    std::shared_ptr<Impl> job = m_job;
    job->worker = std::thread([this, job, snapshot, sequence, gapList, forModel, pictures] {
        const ai::dub::FrameSource frames = [&](const ai::dub::AdGap& gap) {
            std::vector<ai::ImagePart> out;
            for (int k = 0; k < pictures; ++k) {
                const Ticks at = gap.range.start + Ticks{gap.range.duration.value * (2 * k + 1) / (2 * pictures)};
                ai::ImagePart part = pictureOf(*snapshot, sequence, at);
                if (!part.bytes.empty())
                    out.push_back(std::move(part));
            }
            return out;
        };
        const auto progress = [this](double p) {
            QMetaObject::invokeMethod(this, [this, p] {
                m_progress = p;
                emit runningChanged();
            }, Qt::QueuedConnection);
            return !m_cancel.load();
        };
        job->result = ai::dub::describeGaps(gapList, *job->model, frames, forModel, progress, job->stop.get_token());
        QMetaObject::invokeMethod(this, [this] { finishDescribe(); }, Qt::QueuedConnection);
    });
    return {};
}

void AudioDescriptionBridge::finishDescribe() {
    if (!m_job)
        return;
    if (m_job->worker.joinable())
        m_job->worker.join();
    m_running = false;
    m_progress = 1.0;
    if (!m_job->result) {
        setMessage(qs(m_job->result.error().message));
    } else {
        int written = 0;
        for (const ai::dub::AdLine& line : m_job->result->lines) {
            // The line belongs to the gap that starts where it does.
            for (std::size_t i = 0; i < m_gaps.size(); ++i) {
                if (m_gaps[i].range.start == line.start) {
                    m_lines[i].text = line.text;
                    ++written;
                    break;
                }
            }
        }
        const int silent = static_cast<int>(m_gaps.size()) - written;
        QString text = tr("%n line(s) written", "", written);
        if (silent > 0)
            text += QStringLiteral(". ") + tr("%n gap(s) left silent", "", silent);
        if (m_cancel)
            text = tr("Stopped. %1").arg(text);
        setMessage(text);
    }
    m_job->model.reset();
    emit runningChanged();
    emit linesChanged();
}

void AudioDescriptionBridge::cancel() {
    m_cancel = true;
    if (m_job)
        m_job->stop.request_stop();
}

QString AudioDescriptionBridge::setLineText(int index, const QString& text) {
    if (index < 0 || index >= static_cast<int>(m_lines.size()))
        return tr("No such line");
    m_lines[static_cast<std::size_t>(index)].text = text.simplified().toStdString();
    emit linesChanged();
    return {};
}

void AudioDescriptionBridge::removeLine(int index) {
    if (index >= 0 && index < static_cast<int>(m_lines.size())) {
        m_lines[static_cast<std::size_t>(index)].text.clear();
        emit linesChanged();
    }
}

void AudioDescriptionBridge::clearLines() {
    for (ai::dub::AdLine& line : m_lines)
        line.text.clear();
    emit linesChanged();
}

QString AudioDescriptionBridge::apply(const QString& language) {
    if (!m_controller)
        return tr("No project");
    Json list = Json::array();
    for (const ai::dub::AdLine& line : m_lines) {
        if (line.text.empty())
            continue;
        list.push_back(Json{{"start", line.start.value}, {"duration", line.duration.value}, {"text", line.text}});
    }
    if (list.empty()) {
        setMessage(tr("No line written yet"));
        return m_message;
    }
    Json params{{"lines", std::move(list)}};
    if (!language.trimmed().isEmpty())
        params["language"] = language.trimmed().toStdString();
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "audioDescription.createTrack", std::move(params));
    const QString error = commandErrorText(result);
    setMessage(error);
    if (error.isEmpty() && result.data.contains("captionTrack"))
        m_track = result.data["captionTrack"].get<std::int64_t>();
    return error;
}

QString AudioDescriptionBridge::voice(const QString& language) {
    if (m_track == 0) {
        setMessage(tr("Put the lines on the track first"));
        return m_message;
    }
    auto* dub = m_engine ? m_engine->singletonInstance<DubBridge*>("Wabcut", "Dub") : nullptr;
    if (!dub) {
        setMessage(tr("Voice-over is not available"));
        return m_message;
    }
    QVariantMap proposal{{QStringLiteral("kind"), QStringLiteral("dub")},
                         {QStringLiteral("captionTrack"), static_cast<qlonglong>(m_track)},
                         {QStringLiteral("name"), QStringLiteral("Audio Description")},
                         {QStringLiteral("duck"), -12.0}};
    if (!language.trimmed().isEmpty())
        proposal.insert(QStringLiteral("language"), language.trimmed());
    dub->propose(proposal);
    return {};
}

QString AudioDescriptionBridge::scriptText() const {
    if (!m_controller)
        return {};
    const SequencePtr seq = m_controller->currentSequence();
    QString out = tr("Audio description script: %1").arg(seq ? qs(seq->name) : QString()) + QStringLiteral("\n\n");
    for (const ai::dub::AdLine& line : m_lines) {
        if (line.text.empty())
            continue;
        out += m_controller->formatTimecode(line.start) + QStringLiteral(" - ") + m_controller->formatTimecode(line.start + line.duration) +
               QStringLiteral("  (") + tr("%1 s").arg(std::round(line.duration.toSeconds() * 10.0) / 10.0) + QStringLiteral(")\n") +
               qs(line.text) + QStringLiteral("\n\n");
    }
    return out;
}

QString AudioDescriptionBridge::exportScript(const QUrl& file) const {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const QString text = scriptText();
    if (path.isEmpty() || ai::dub::wordCount(text.toStdString()) == 0)
        return tr("Nothing to write");
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text) || out.write(text.toUtf8()) < 0 || !out.commit())
        return tr("Cannot write %1").arg(QFileInfo(path).fileName());
    return {};
}

} // namespace wabcut::app
