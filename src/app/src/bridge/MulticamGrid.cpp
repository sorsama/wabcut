#include "MulticamGrid.h"

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/media/FrameHandle.h>

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wabcut::app {

namespace {

/// `value` * mul + add, for a constant or every key of a curve.
ParamValue mapParam(const ParamValue& value, double mul, double add) {
    if (const Curve* curve = value.curve()) {
        Curve out = *curve;
        for (Keyframe& k : out.keys) {
            k.value = k.value * mul + add;
            k.in.slope *= mul;
            k.out.slope *= mul;
        }
        return ParamValue(std::move(out));
    }
    return ParamValue(value.constant() * mul + add);
}

QImage toImage(const media::FrameHandle& picture) {
    const media::CpuPlanes* cpu = picture.cpu();
    if (!cpu || picture.format != media::PixelFormat::RGBA8 || cpu->planes.empty())
        return {};
    QImage image(picture.width, picture.height, QImage::Format_RGBA8888);
    const media::Plane& plane = cpu->planes[0];
    for (int y = 0; y < picture.height; ++y) {
        std::memcpy(image.scanLine(y), plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride,
                    static_cast<std::size_t>(picture.width) * 4);
    }
    return image;
}

exporter::VideoSourceFormat formatFor(const Sequence& seq, QSize size, Ticks start, std::int64_t frames) {
    exporter::VideoSourceFormat format;
    format.width = std::max(16, size.width() & ~1);
    format.height = std::max(16, size.height() & ~1);
    format.frameRate = seq.frameRate.isValid() ? seq.frameRate : Rate{30, 1};
    format.start = snapToFrame(std::max(start, Ticks{}), format.frameRate);
    format.frameCount = std::max<std::int64_t>(1, frames);
    return format;
}

exporter::RenderedSourceOptions gridOptions() {
    exporter::RenderedSourceOptions options;
    options.renderThreads = 1;
    options.maxInFlight = 2;
    options.maxDecoders = 16; // one per angle
    return options;
}

} // namespace

int multicamGridColumns(std::size_t angles) {
    if (angles <= 4)
        return 2;
    if (angles <= 9)
        return 3;
    return 4;
}

std::expected<ProjectState, Error> multicamGridState(const ProjectState& state, SequenceId sourceId, int columns) {
    const Sequence* source = state.findSequence(sourceId);
    if (!source)
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    if (!source->multicam)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "'" + source->name + "' is not a multicam source"});
    columns = std::clamp(columns, 1, 8);
    auto grid = std::make_shared<Sequence>(*source);
    grid->audioTracks.clear();
    grid->captionTracks.clear();
    grid->effects.clear(); // no master stage: a multicam angle shows its track alone
    const double cellW = static_cast<double>(source->width) / columns;
    const double cellH = static_cast<double>(source->height) / columns;
    const double mul = 1.0 / columns;
    std::vector<TrackPtr> tracks;
    const auto& angles = source->multicam->angles;
    for (std::size_t a = 0; a < angles.size() && a < static_cast<std::size_t>(columns * columns); ++a) {
        if (!angles[a].video.isValid())
            continue;
        const Track* angleTrack = source->findTrack(angles[a].video);
        if (!angleTrack)
            continue;
        const double dx = (static_cast<double>(a % columns) + 0.5) * cellW - source->width / 2.0;
        const double dy = (static_cast<double>(a / columns) + 0.5) * cellH - source->height / 2.0;
        auto track = std::make_shared<Track>(*angleTrack);
        track->enabled = true; // the multicam monitor reads every angle, whatever its flags
        track->transitions.clear();
        track->effects.clear();
        std::vector<Clip> clips;
        for (const Clip& c : angleTrack->clips) {
            Clip copy = c;
            ClipContent content = c.content();
            content.properties[ParamId::Scale] = mapParam(content.properties[ParamId::Scale], mul, 0.0);
            content.properties[ParamId::PositionX] = mapParam(content.properties[ParamId::PositionX], mul, dx);
            content.properties[ParamId::PositionY] = mapParam(content.properties[ParamId::PositionY], mul, dy);
            copy.setContent(std::move(content));
            clips.push_back(std::move(copy));
        }
        track->clips = ClipList(std::move(clips));
        tracks.push_back(std::move(track));
    }
    grid->videoTracks = std::move(tracks);
    return state.withSequence(std::move(grid));
}

std::expected<MulticamGridFrame, Error> renderMulticamGrid(const ProjectState& grid, SequenceId sourceId, Ticks time,
                                                           QSize size, const std::filesystem::path& projectFile) {
    const ProjectState checked = editor::withMediaChecked(grid, projectFile);
    const Sequence* seq = checked.findSequence(sourceId);
    if (!seq)
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    QElapsedTimer timer;
    timer.start();
    auto source = exporter::renderedVideoSourceFactory(gridOptions())(checked, *seq, formatFor(*seq, size, time, 1));
    if (!source)
        return std::unexpected(source.error());
    if (auto r = (*source)->request(0); !r)
        return std::unexpected(r.error());
    auto picture = (*source)->next();
    MulticamGridFrame frame;
    frame.backend = QString::fromStdString((*source)->description());
    (*source)->stop();
    if (!picture)
        return std::unexpected(picture.error());
    frame.image = toImage(*picture);
    if (frame.image.isNull())
        return std::unexpected(Error{ErrorCode::Unsupported, "The renderer delivered no RGBA picture"});
    frame.time = snapToFrame(std::max(time, Ticks{}), seq->frameRate.isValid() ? seq->frameRate : Rate{30, 1});
    frame.milliseconds = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    frame.opened = true;
    return frame;
}

// ---- MulticamGridRenderer ---------------------------------------------------------------------------------------

MulticamGridRenderer::MulticamGridRenderer(Publish publish) : m_publish(std::move(publish)) {
    m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

MulticamGridRenderer::~MulticamGridRenderer() {
    m_thread.request_stop();
    m_changed.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void MulticamGridRenderer::setSource(std::shared_ptr<const ProjectState> grid, SequenceId source, QSize size,
                                     std::filesystem::path projectFile, std::uint64_t generation) {
    {
        std::lock_guard lock(m_mutex);
        m_request.grid = std::move(grid);
        m_request.source = source;
        m_request.size = size;
        m_request.projectFile = std::move(projectFile);
        m_request.generation = generation;
        ++m_request.serial;
    }
    m_changed.notify_all();
}

void MulticamGridRenderer::setTime(Ticks time, bool playing) {
    {
        std::lock_guard lock(m_mutex);
        if (m_request.time == time && m_request.playing == playing)
            return;
        m_request.time = time;
        m_request.playing = playing;
        ++m_request.serial;
    }
    m_changed.notify_all();
}

void MulticamGridRenderer::clear() {
    setSource(nullptr, {}, {}, {}, 0);
}

void MulticamGridRenderer::run(std::stop_token stop) {
    std::uint64_t done = 0; // the last request serial drawn
    // The source kept open while playing.
    std::unique_ptr<exporter::IVideoFrameSource> stream;
    std::uint64_t streamGeneration = 0;
    Ticks streamStart;
    Rate streamRate{30, 1};
    std::int64_t nextIndex = 0;
    QString backend;
    std::shared_ptr<const ProjectState> checked;
    std::shared_ptr<const ProjectState> checkedFrom;
    const auto closeStream = [&] {
        if (stream)
            stream->stop();
        stream.reset();
    };
    while (!stop.stop_requested()) {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            m_changed.wait(lock, stop, [&] { return m_request.serial != done; });
            if (stop.stop_requested())
                break;
            request = m_request;
        }
        done = request.serial;
        if (!request.grid) {
            closeStream();
            continue;
        }
        if (checkedFrom != request.grid) {
            checked = std::make_shared<const ProjectState>(editor::withMediaChecked(*request.grid, request.projectFile));
            checkedFrom = request.grid;
            closeStream();
        }
        const Sequence* seq = checked->findSequence(request.source);
        if (!seq) {
            m_publish(std::unexpected(QStringLiteral("The multicam source is gone")), request.generation);
            continue;
        }
        // The frame at the playhead from the open source (forward decoding, playing or stepping on while paused);
        // reopened when the playhead jumped back or far ahead, or the source changed.
        const Rate rate = seq->frameRate.isValid() ? seq->frameRate : Rate{30, 1};
        const auto frameAt = [&](Ticks t) { return ticksToFrames(t - streamStart, rate, Rounding::Floor).value_or(0); };
        const std::int64_t twoSeconds = 2 * rate.num / std::max<std::int64_t>(1, rate.den);
        const bool sameStream = stream && streamGeneration == request.generation;
        const std::int64_t wanted = sameStream ? frameAt(request.time) : -1;
        if (sameStream && wanted == nextIndex - 1)
            continue; // still on the frame drawn last
        QElapsedTimer timer;
        timer.start();
        bool reopened = false;
        if (!sameStream || wanted < nextIndex - 1 || wanted > nextIndex + twoSeconds) {
            reopened = true;
            closeStream();
            const exporter::VideoSourceFormat format = formatFor(*seq, request.size, request.time, 10 * 60 * 60);
            auto opened = exporter::renderedVideoSourceFactory(gridOptions())(*checked, *seq, format);
            if (!opened) {
                m_publish(std::unexpected(QString::fromStdString(opened.error().message)), request.generation);
                continue;
            }
            stream = std::move(*opened);
            streamGeneration = request.generation;
            streamStart = format.start;
            streamRate = rate;
            nextIndex = 0;
            backend = QString::fromStdString(stream->description());
        }
        const std::int64_t index = std::max(nextIndex, frameAt(request.time));
        if (auto r = stream->request(index); !r) {
            closeStream();
            m_publish(std::unexpected(QString::fromStdString(r.error().message)), request.generation);
            continue;
        }
        auto picture = stream->next();
        if (!picture) {
            closeStream();
            m_publish(std::unexpected(QString::fromStdString(picture.error().message)), request.generation);
            continue;
        }
        nextIndex = index + 1;
        MulticamGridFrame frame;
        frame.image = toImage(*picture);
        frame.time = streamStart + framesToTicks(index, streamRate);
        frame.milliseconds = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        frame.opened = reopened;
        frame.backend = backend;
        m_publish(std::move(frame), request.generation);
        // The next frame is drawn when the playhead moves on (setTime), never ahead of it.
    }
    closeStream();
}

} // namespace wabcut::app
