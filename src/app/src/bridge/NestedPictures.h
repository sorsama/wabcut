#pragma once

// Pictures of sequences (I.9): filmstrips of nested and multicam clips on the timeline, and Library thumbnails of
// sequences. They are rendered through the export path (exporter::renderedVideoSourceFactory: the export's evaluator
// and compositor, so grades, track and master effects, captions-free pictures and redactions are what an export shows,
// nested levels included), never on the GUI thread, and cached by a stamp of everything the picture depends on.

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace wabcut::app {

/// A stamp of what `sequence`'s pictures depend on: the sequence and every sequence it nests (structural sharing: an
/// edit replaces the pointer), the media items their clips show and the project's redactions. 0 when it does not exist.
std::uint64_t sequencePictureStamp(const ProjectState& state, SequenceId sequence);

/// Pictures of a sequence and the sequence times they show.
struct SequencePictures {
    std::vector<QImage> images;
    std::vector<Ticks> times;
};

/// `count` frames of `sequence` (of `state`, media already checked on disk) spread over its duration (the middle of
/// each of `count` equal parts: one frame is the middle frame), `height` pixels high at the sequence's aspect, rendered
/// by one export renderer source (call from a worker, never the GUI thread). Stops early, returning nullopt, when
/// `stop` is requested; nullopt too when a frame cannot be rendered (an offline layer: never a picture without what it
/// holds).
std::optional<SequencePictures> renderSequencePictures(const ProjectState& state, const Sequence& sequence, int count,
                                                       int height, const std::stop_token& stop = {});

/// The project pictures are rendered from: set on the GUI thread, read by the threads that render.
class PictureProject {
public:
    void set(ProjectSnapshot snapshot, std::filesystem::path projectFile);
    std::pair<ProjectSnapshot, std::filesystem::path> get() const;

private:
    mutable std::mutex m_mutex;
    ProjectSnapshot m_snapshot;
    std::filesystem::path m_file;
};

/// "image://wabcut-sequences/<sequence id>/<stamp>": a sequence's Library thumbnail, its middle frame rendered like an
/// export, on a thread of its own. The stamp changes with anything the picture depends on, so an edit is a new URL (a
/// new picture).
class SequenceThumbnailProvider final : public QQuickAsyncImageProvider {
public:
    static constexpr const char* kProviderId = "wabcut-sequences";

    explicit SequenceThumbnailProvider(std::shared_ptr<const PictureProject> project);
    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    static QString urlFor(const ProjectState& state, SequenceId sequence);

private:
    std::shared_ptr<const PictureProject> m_project;
};

} // namespace wabcut::app
