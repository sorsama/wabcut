#include "PosterFrames.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/ai/search/MediaSearchAccess.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/RedactionMedia.h>

#include <QCache>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlEngine>
#include <QQuickImageResponse>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGImageNode>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace wabcut::app {

namespace {

/// Decoded posters by URL id and width: a poster shows on a card many times while the library scrolls.
QCache<QString, QImage>& posterCache() {
    static QCache<QString, QImage> cache(32 * 1024 * 1024); // cost = bytes
    return cache;
}
QMutex& posterCacheMutex() {
    static QMutex mutex;
    return mutex;
}

struct PosterRequest {
    QString path;
    Ticks time;
    int stream = -1;
};

std::optional<PosterRequest> parsePoster(const QString& id) {
    const QStringList parts = id.split(QLatin1Char('|'));
    if (parts.size() != 3)
        return std::nullopt;
    bool okTime = false;
    bool okStream = false;
    PosterRequest out;
    out.path = QUrl::fromPercentEncoding(parts[0].toUtf8());
    out.time = Ticks{parts[1].toLongLong(&okTime)};
    out.stream = parts[2].toInt(&okStream);
    if (!okTime || !okStream || out.path.isEmpty() || out.time.value < 0)
        return std::nullopt;
    return out;
}

class PosterResponse final : public QQuickImageResponse {
public:
    PosterResponse(const QString& id, int width) : m_link(std::make_shared<Link>()) {
        m_link->target = this;
        const QString key = id + QLatin1Char('@') + QString::number(width);
        {
            QMutexLocker lock(&posterCacheMutex());
            if (const QImage* cached = posterCache().object(key)) {
                m_image = *cached;
                QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
                return;
            }
        }
        const auto request = parsePoster(id);
        if (!request) {
            m_error = QStringLiteral("bad poster id");
            QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
            return;
        }
        QThreadPool::globalInstance()->start([link = m_link, job = *request, width, key] {
            QImage image;
            QString error;
            auto reader = ai::search::VideoPictureReader::open(job.path.toStdString(), job.stream, width);
            if (reader) {
                if (auto picture = (*reader)->at(job.time); picture && picture->valid()) {
                    image = QImage(picture->rgb.data(), picture->width, picture->height, picture->width * 3,
                                   QImage::Format_RGB888)
                                .copy();
                    QMutexLocker lock(&posterCacheMutex());
                    posterCache().insert(key, new QImage(image), static_cast<qsizetype>(image.sizeInBytes()));
                } else {
                    error = picture ? QStringLiteral("no picture") : QString::fromStdString(picture.error().message);
                }
            } else {
                error = QString::fromStdString(reader.error().message);
            }
            std::lock_guard lock(link->mutex);
            if (PosterResponse* target = link->target) {
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

    ~PosterResponse() override {
        std::lock_guard lock(m_link->mutex);
        m_link->target = nullptr;
    }

    QQuickTextureFactory* textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(m_image); }
    QString errorString() const override { return m_error; }

private:
    struct Link {
        std::mutex mutex;
        PosterResponse* target = nullptr;
    };
    std::shared_ptr<Link> m_link;
    QImage m_image;
    QString m_error;
};

} // namespace

QQuickImageResponse* PosterImageProvider::requestImageResponse(const QString& id, const QSize& requestedSize) {
    const int width = std::clamp(requestedSize.width() > 0 ? requestedSize.width() : 320, 16, 1920);
    return new PosterResponse(id, width);
}

QString PosterImageProvider::urlFor(const QString& path, Ticks time, int containerStream) {
    return QStringLiteral("image://%1/%2|%3|%4")
        .arg(QLatin1String(kProviderId), QString::fromLatin1(QUrl::toPercentEncoding(path)))
        .arg(time.value)
        .arg(containerStream);
}

// ---- FilmstripScrub ------------------------------------------------------------------------------------------------

FilmstripScrub::FilmstripScrub(QQuickItem* parent) : QQuickItem(parent) { setFlag(ItemHasContents); }

FilmstripScrub::~FilmstripScrub() = default;

void FilmstripScrub::componentComplete() {
    QQuickItem::componentComplete();
    if (!m_controller) {
        if (QQmlEngine* engine = qmlEngine(this))
            setController(engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    }
}

void FilmstripScrub::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        // The source is replaced when the cache directory changes, and has new atlases when they arrive.
        connect(controller, &ProjectController::filmstripsChanged, this, [this] {
            if (!m_injected && m_controller)
                m_source = m_controller->filmstripSource();
            evaluate();
        });
        connect(controller, &ProjectController::revisionChanged, this, &FilmstripScrub::evaluate);
        if (!m_injected)
            m_source = controller->filmstripSource();
    }
    evaluate();
}

void FilmstripScrub::setSource(std::shared_ptr<const FilmstripSource> source) {
    m_source = std::move(source);
    m_injected = true;
    evaluate();
}

void FilmstripScrub::setMedia(const QString& media) {
    if (media == m_media)
        return;
    m_media = media;
    emit mediaChanged();
    evaluate();
}

void FilmstripScrub::setPosition(double position) {
    position = std::clamp(position, 0.0, 1.0);
    if (qFuzzyCompare(position + 1.0, m_position + 1.0))
        return;
    m_position = position;
    emit positionChanged();
    evaluate();
}

std::optional<Ticks> FilmstripScrub::sourceTime() const {
    const auto id = detail::parseId<MediaId>(QLatin1Char('m'), m_media);
    if (!id || !m_controller)
        return std::nullopt;
    const MediaItem* item = m_controller->snapshot()->findMedia(*id);
    if (!item)
        return std::nullopt;
    const TimeRange span = item->subclip ? item->subclip->range : TimeRange{Ticks{}, item->info.duration};
    if (span.duration.value <= 0)
        return std::nullopt;
    return span.start + Ticks{static_cast<std::int64_t>(m_position * static_cast<double>(span.duration.value - 1))};
}

QString FilmstripScrub::timecode() const {
    const auto t = sourceTime();
    const auto id = detail::parseId<MediaId>(QLatin1Char('m'), m_media);
    if (!t || !id || !m_controller)
        return {};
    const MediaItem* item = m_controller->snapshot()->findMedia(*id);
    const Rate rate = item && item->info.hasVideo() && item->info.video.front().frameRate.isValid()
                          ? item->info.video.front().frameRate
                          : kRate30;
    return QString::fromStdString(ticksToTimecode(*t, rate, false));
}

void FilmstripScrub::evaluate() {
    const bool wasReady = m_ready;
    const bool wasRedacted = m_redacted;
    m_ready = false;
    m_redacted = false;
    m_atlas.reset();
    m_tile = -1;
    const auto id = detail::parseId<MediaId>(QLatin1Char('m'), m_media);
    const auto t = sourceTime();
    if (id && t && m_source && m_controller) {
        const FilmstripSource::Lookup look = m_source->lookup(*id); // never blocks; a miss starts the extraction
        if (look.atlas && look.atlas->count() > 0) {
            const int tile = look.atlas->tileAt(*t);
            const Ticks shown = look.atlas->times[static_cast<std::size_t>(tile)];
            const ProjectSnapshot snap = m_controller->snapshot();
            const std::vector<const Redaction*> covering = redactionsCovering(*snap, *id); // a subclip's master's too
            m_redacted = std::any_of(covering.begin(), covering.end(), [&](const Redaction* r) {
                return r->enabled && (r->source.contains(*t) || r->source.contains(shown));
            });
            if (!m_redacted) {
                m_atlas = look.atlas;
                m_tile = tile;
                m_ready = true;
            }
        }
    }
    if (wasReady != m_ready || wasRedacted != m_redacted)
        emit stateChanged();
    update();
}

QSGNode* FilmstripScrub::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGImageNode*>(oldNode);
    if (!m_atlas || m_tile < 0 || width() <= 0 || height() <= 0 || !window()) {
        delete node;
        m_textureSerial = 0;
        return nullptr;
    }
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
        m_textureSerial = 0;
    }
    if (m_textureSerial != m_atlas->serial || !node->texture()) {
        // One upload per atlas; moving the pointer only changes the source rectangle.
        node->setTexture(window()->createTextureFromImage(m_atlas->atlas));
        m_textureSerial = m_atlas->serial;
    }
    const QRect tile = m_atlas->tileRect(m_tile);
    // Cover the item, cropping the tile's longer side (the card's thumbnail does the same).
    const double scale = std::max(width() / tile.width(), height() / tile.height());
    const double sw = width() / scale;
    const double sh = height() / scale;
    node->setSourceRect(QRectF(tile.x() + (tile.width() - sw) / 2, tile.y() + (tile.height() - sh) / 2, sw, sh));
    node->setRect(QRectF(0, 0, width(), height()));
    node->setFiltering(QSGTexture::Linear);
    return node;
}

void FilmstripScrub::releaseResources() { m_textureSerial = 0; }

} // namespace wabcut::app
