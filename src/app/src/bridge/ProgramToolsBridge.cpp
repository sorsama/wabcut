#include "ProgramToolsBridge.h"

#include "ProjectController.h"

#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Timecode.h>

#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wabcut::app {

using command::Json;

ProgramToolsBridge::ProgramToolsBridge(QObject* parent) : QObject(parent) {}

ProgramToolsBridge* ProgramToolsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* tools = new ProgramToolsBridge;
    if (qmlEngine)
        tools->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return tools;
}

void ProgramToolsBridge::setController(ProjectController* controller) {
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::marksChanged, this, &ProgramToolsBridge::stateChanged);
        connect(controller, &ProjectController::sequenceChanged, this, &ProgramToolsBridge::stateChanged);
    }
    emit stateChanged();
}

bool ProgramToolsBridge::canLift() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    return seq && seq->markedRange().has_value();
}

void ProgramToolsBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

command::CommandResult ProgramToolsBridge::run(const std::string& name, Json params) {
    command::CommandResult result;
    if (!m_controller || !m_controller->currentSequenceId().isValid()) {
        result.error = command::CommandError{command::ErrorCode::InvalidRequest, "no sequence", nullptr};
        return result;
    }
    command::CommandEngine engine(m_controller->session());
    command::CommandRequest request;
    request.command = name;
    params["sequence"] = m_controller->currentSequenceId().value;
    request.params = std::move(params);
    return engine.execute(request);
}

namespace {
QString errorText(const command::CommandResult& r) {
    return r.ok() ? QString() : QString::fromStdString(r.error->message);
}
} // namespace

bool ProgramToolsBridge::lift() {
    if (!canLift()) {
        setMessage(tr("Set an In and an Out in the sequence first"));
        return false;
    }
    const auto r = run("sequence.lift", Json::object());
    setMessage(errorText(r));
    return r.ok();
}

bool ProgramToolsBridge::extract() {
    if (!canLift()) {
        setMessage(tr("Set an In and an Out in the sequence first"));
        return false;
    }
    const auto r = run("sequence.extract", Json::object());
    setMessage(errorText(r));
    return r.ok();
}

bool ProgramToolsBridge::addMarker() {
    if (!m_controller)
        return false;
    const auto r = run("marker.add", Json{{"time", m_controller->playhead().value}});
    setMessage(errorText(r));
    return r.ok();
}

std::expected<QImage, QString> ProgramToolsBridge::renderFrame(SequenceId sequence, Ticks at) const {
    if (!m_controller)
        return std::unexpected(tr("No project"));
    return renderStateFrame(*m_controller->snapshot(), sequence, at);
}

std::expected<QImage, QString> ProgramToolsBridge::renderStateFrame(const ProjectState& state, SequenceId sequence,
                                                                    Ticks at, int width) {
    const Sequence* seq = state.findSequence(sequence);
    if (!seq)
        return std::unexpected(tr("No sequence"));
    const Rate rate = seq->frameRate.isValid() ? seq->frameRate : kRate30;
    exporter::RenderedSourceOptions options;
    options.renderThreads = 1;
    options.maxInFlight = 1;
    int w = seq->width;
    int h = seq->height;
    if (width > 0 && width < w && w > 0) {
        h = std::max(2, static_cast<int>(std::lround(static_cast<double>(h) * width / w)) & ~1);
        w = width & ~1;
    }
    auto source = exporter::renderedVideoSourceFactory(options)(
        state, *seq, exporter::VideoSourceFormat{w, h, rate, snapToFrame(at, rate), 1, {}});
    if (!source)
        return std::unexpected(QString::fromStdString(source.error().message));
    if (auto requested = (*source)->request(0); !requested)
        return std::unexpected(QString::fromStdString(requested.error().message));
    auto picture = (*source)->next();
    (*source)->stop();
    if (!picture)
        return std::unexpected(QString::fromStdString(picture.error().message));
    const media::CpuPlanes* cpu = picture->cpu();
    if (!cpu || picture->format != media::PixelFormat::RGBA8)
        return std::unexpected(tr("The renderer delivered no RGBA picture"));
    QImage image(picture->width, picture->height, QImage::Format_RGBA8888);
    const media::Plane& plane = cpu->planes[0];
    for (int y = 0; y < picture->height; ++y) {
        std::memcpy(image.scanLine(y), plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride,
                    static_cast<std::size_t>(picture->width) * 4);
    }
    return image;
}

QString ProgramToolsBridge::exportFrame(const QUrl& folder) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq) {
        setMessage(tr("No sequence"));
        return {};
    }
    const QString dir = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        setMessage(tr("Choose a folder for the frame"));
        return {};
    }
    const Ticks at = m_controller->playhead();
    auto image = renderFrame(seq->id, at);
    if (!image) {
        setMessage(image.error());
        return {};
    }
    // "Rough cut 00;01;02;03.png": timecode separators that are valid in file names everywhere.
    QString timecode = m_controller->formatTimecode(at);
    timecode.replace(QLatin1Char(':'), QLatin1Char(';'));
    QString base = QString::fromStdString(seq->name).trimmed();
    for (const QChar bad : {QLatin1Char('/'), QLatin1Char('\\'), QLatin1Char(':'), QLatin1Char('*'), QLatin1Char('?'),
                            QLatin1Char('"'), QLatin1Char('<'), QLatin1Char('>'), QLatin1Char('|')})
        base.replace(bad, QLatin1Char('_'));
    if (base.isEmpty())
        base = tr("Frame");
    QString path = QDir(dir).filePath(QStringLiteral("%1 %2.png").arg(base, timecode));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QDir(dir).filePath(QStringLiteral("%1 %2 (%3).png").arg(base, timecode).arg(n));
    if (!image->save(path, "PNG")) {
        setMessage(tr("Could not write %1").arg(QDir::toNativeSeparators(path)));
        return {};
    }
    setMessage(tr("Frame saved: %1").arg(QFileInfo(path).fileName()));
    emit frameExported(path);
    return path;
}

} // namespace wabcut::app
