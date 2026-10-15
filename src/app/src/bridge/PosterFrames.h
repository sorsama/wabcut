#pragma once

// Library pictures (M5k).
//
// PosterImageProvider ("image://wabcut-posters/…"): a media item's poster frame (MediaItem::posterFrame), decoded on
// a QThreadPool thread with ai::search::VideoPictureReader (the frame the viewer sees: sample aspect and rotation
// applied) and kept in a small in-memory cache. The URL is opaque to QML (principle 9): LibraryBridge builds it.
//
// FilmstripScrub (QML `FilmstripScrub`): hover scrub on a Library card. It shows the filmstrip tile nearest the pointer
// from the timeline's thumbnail atlases (ProjectController::filmstripSource, MediaFilmstripSource: extracted on its
// worker thread), as a sub-rectangle of the atlas texture, so moving the pointer never decodes and never uploads more
// than the atlas once. Tiles whose frame a source redaction covers are not shown (review P2-9: the atlases are made
// from the raw file).

#include "FilmstripSource.h"

#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <QImage>
#include <QPointer>
#include <QQuickAsyncImageProvider>
#include <QQuickItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

namespace wabcut::app {

class ProjectController;

class PosterImageProvider final : public QQuickAsyncImageProvider {
public:
    static constexpr const char* kProviderId = "wabcut-posters";

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    /// "image://wabcut-posters/…" for the frame at `time` of the video stream `containerStream` of `path` (UTF-8).
    static QString urlFor(const QString& path, Ticks time, int containerStream);
};

class FilmstripScrub : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    /// The media item ("m7").
    Q_PROPERTY(QString media READ media WRITE setMedia NOTIFY mediaChanged FINAL)
    /// Where the pointer is across the card, 0..1 (of a subclip's range for a subclip).
    Q_PROPERTY(double position READ position WRITE setPosition NOTIFY positionChanged FINAL)
    /// A thumbnail is shown (the atlas has arrived and no redaction hides it).
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged FINAL)
    /// The frame under the pointer is covered by a source redaction, so nothing is shown.
    Q_PROPERTY(bool redacted READ redacted NOTIFY stateChanged FINAL)
    /// The media timecode under the pointer.
    Q_PROPERTY(QString timecode READ timecode NOTIFY positionChanged FINAL)

public:
    explicit FilmstripScrub(QQuickItem* parent = nullptr);
    ~FilmstripScrub() override;

    QString media() const { return m_media; }
    void setMedia(const QString& media);
    double position() const { return m_position; }
    void setPosition(double position);
    bool ready() const { return m_ready; }
    bool redacted() const { return m_redacted; }
    QString timecode() const;
    /// Tests: the controller and the atlases (by default the QML singleton's).
    void setController(ProjectController* controller);
    void setSource(std::shared_ptr<const FilmstripSource> source);

signals:
    void mediaChanged();
    void positionChanged();
    void stateChanged();

protected:
    void componentComplete() override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void releaseResources() override;

private:
    /// The source time under the pointer, or nothing (no such media).
    std::optional<Ticks> sourceTime() const;
    void evaluate();

    QPointer<ProjectController> m_controller;
    std::shared_ptr<const FilmstripSource> m_source;
    QString m_media;
    double m_position = 0.0;
    bool m_ready = false;
    bool m_redacted = false;
    // What to draw (GUI thread) and the uploaded atlas (render thread, under the scene graph's sync).
    std::shared_ptr<const FilmstripAtlas> m_atlas;
    int m_tile = -1;
    std::uint64_t m_textureSerial = 0;
    bool m_injected = false; // setSource() from a test: keep it
};

} // namespace wabcut::app
