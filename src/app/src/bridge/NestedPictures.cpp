#include "NestedPictures.h"

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/media/FrameHandle.h>

#include <QMetaObject>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace wabcut::app {

namespace {

std::uint64_t mix(std::uint64_t key, std::uint64_t value) {
    key ^= value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
    return key * 0xFF51AFD7ED558CCDull;
}

std::uint64_t address(const void* p) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
}

void stampOf(const ProjectState& state, SequenceId id, std::uint64_t& key, std::unordered_set<std::uint64_t>& seen) {
    if (!seen.insert(id.value).second)
        return; // shown twice (or a cycle the model refuses anyway): once is enough
    const SequencePtr sequence = state.sequencePtr(id);
    key = mix(key, address(sequence.get()));
    if (!sequence)
        return;
    for (const auto& track : sequence->videoTracks) {
        if (!track)
            continue;
        for (const Clip& clip : track->clips) {
            if (clip.isNested())
                stampOf(state, clip.sequence, key, seen);
            else
                key = mix(key, address(state.findMedia(clip.media))); // a relink, proxy or new metadata replaces the item
        }
    }
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

} // namespace

std::uint64_t sequencePictureStamp(const ProjectState& state, SequenceId sequence) {
    if (!state.findSequence(sequence))
        return 0;
    std::uint64_t key = mix(0x5EC0'0000ull, sequence.value);
    std::unordered_set<std::uint64_t> seen;
    stampOf(state, sequence, key, seen);
    // Source redactions reach any clip of their media (I.6): their list is shared between revisions until it changes.
    key = mix(key, address(state.redactions.get().data()));
    key = mix(key, state.redactions.size());
    return key == 0 ? 1 : key;
}

std::optional<SequencePictures> renderSequencePictures(const ProjectState& state, const Sequence& sequence, int count,
                                                       int height, const std::stop_token& stop) {
    const std::int64_t ms = static_cast<std::int64_t>(std::floor(sequence.duration.toSeconds() * 1000.0));
    if (count <= 0 || ms <= 0 || sequence.width <= 0 || sequence.height <= 0 || height <= 0)
        return std::nullopt;
    // One source, one frame per part: a "frame rate" of `count` frames over the duration, starting half a part in.
    const std::optional<Rate> rate = Rate::make(std::int64_t{count} * 1000, ms);
    if (!rate)
        return std::nullopt;
    const int h = std::max(2, height & ~1);
    const int w = std::max(2, static_cast<int>(std::lround(static_cast<double>(h) * sequence.width / sequence.height)) & ~1);
    exporter::RenderedSourceOptions options;
    options.hardwareDecode = true; // a thumbnail: fast beats bit-exact
    options.renderThreads = 1;
    options.maxInFlight = 2;
    options.maxDecoders = 8;
    exporter::VideoSourceFormat format;
    format.width = w;
    format.height = h;
    format.frameRate = *rate;
    format.start = Ticks{framesToTicks(1, *rate).value / 2};
    format.frameCount = count;
    auto source = exporter::renderedVideoSourceFactory(options)(state, sequence, format);
    if (!source)
        return std::nullopt;
    SequencePictures out;
    std::int64_t requested = 0;
    const auto fail = [&] {
        (*source)->stop();
        return std::nullopt;
    };
    for (std::int64_t i = 0; i < count; ++i) {
        while (requested < count && requested - i < (*source)->maxInFlight()) {
            if (!(*source)->request(requested))
                return fail();
            ++requested;
        }
        if (stop.stop_requested())
            return fail();
        auto picture = (*source)->next();
        if (!picture)
            return fail();
        QImage image = toImage(*picture);
        if (image.isNull())
            return fail();
        out.images.push_back(std::move(image));
        out.times.push_back(format.start + framesToTicks(i, *rate));
    }
    (*source)->stop();
    return out;
}

// ---- Library thumbnails of sequences ---------------------------------------------------------------------------------

void PictureProject::set(ProjectSnapshot snapshot, std::filesystem::path projectFile) {
    std::lock_guard lock(m_mutex);
    m_snapshot = std::move(snapshot);
    m_file = std::move(projectFile);
}

std::pair<ProjectSnapshot, std::filesystem::path> PictureProject::get() const {
    std::lock_guard lock(m_mutex);
    return {m_snapshot, m_file};
}

namespace {

class SequenceThumbnailResponse final : public QQuickImageResponse {
public:
    SequenceThumbnailResponse(ProjectSnapshot snapshot, std::filesystem::path file, SequenceId sequence, int height)
        : m_link(std::make_shared<Link>()) {
        m_link->target = this;
        QThreadPool::globalInstance()->start([link = m_link, snapshot = std::move(snapshot), file = std::move(file),
                                              sequence, height] {
            QImage image;
            QString error;
            if (!link->stop.stop_requested() && snapshot) {
                const ProjectState checked = editor::withMediaChecked(*snapshot, file);
                if (const Sequence* seq = checked.findSequence(sequence)) {
                    if (auto pictures = renderSequencePictures(checked, *seq, 1, height, link->stop.get_token());
                        pictures && !pictures->images.empty()) {
                        image = std::move(pictures->images.front());
                    }
                }
            }
            if (image.isNull()) {
                // Nothing rendered (offline or unreadable media, a cancelled request): a 1 x 1 transparent image, not an
                // error, so the card keeps its icon without a QML warning (LibraryCard.hasPicture).
                image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
            }
            std::lock_guard lock(link->mutex);
            if (SequenceThumbnailResponse* target = link->target) {
                QMetaObject::invokeMethod(
                    target,
                    [target, image = std::move(image), error = std::move(error)] {
                        target->m_image = image;
                        target->m_error = error;
                        emit target->finished();
                    },
                    Qt::QueuedConnection);
            }
        });
    }

    ~SequenceThumbnailResponse() override {
        m_link->stop.request_stop();
        std::lock_guard lock(m_link->mutex);
        m_link->target = nullptr;
    }

    QQuickTextureFactory* textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(m_image); }
    QString errorString() const override { return m_error; }
    void cancel() override { m_link->stop.request_stop(); }

private:
    struct Link {
        std::mutex mutex;
        SequenceThumbnailResponse* target = nullptr;
        std::stop_source stop;
    };
    std::shared_ptr<Link> m_link;
    QImage m_image;
    QString m_error;
};

} // namespace

SequenceThumbnailProvider::SequenceThumbnailProvider(std::shared_ptr<const PictureProject> project)
    : m_project(std::move(project)) {}

QQuickImageResponse* SequenceThumbnailProvider::requestImageResponse(const QString& id, const QSize& requestedSize) {
    // "<sequence id>/<stamp>": the stamp only makes an edited sequence a new URL; the picture is the current one.
    const SequenceId sequence{id.section(QLatin1Char('/'), 0, 0).toULongLong()};
    const int height = std::clamp(requestedSize.height() > 0 ? requestedSize.height() : 180, 16, 1080);
    auto [snapshot, file] = m_project ? m_project->get() : std::pair<ProjectSnapshot, std::filesystem::path>{};
    return new SequenceThumbnailResponse(std::move(snapshot), std::move(file), sequence, height);
}

QString SequenceThumbnailProvider::urlFor(const ProjectState& state, SequenceId sequence) {
    const std::uint64_t stamp = sequencePictureStamp(state, sequence);
    if (stamp == 0)
        return {};
    return QStringLiteral("image://%1/%2/%3").arg(QLatin1String(kProviderId)).arg(sequence.value).arg(stamp, 0, 16);
}

} // namespace wabcut::app
