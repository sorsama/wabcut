#pragma once

// MulticamGrid (M5l): the angle grid of the Program Monitor's multi-camera view, every angle of a multicam source in
// one picture, drawn by the export renderer (render::AsyncOffscreenRenderer: the monitor's Compositor and colour
// pipeline, Principle 5) at a reduced resolution.
//
// multicamGridState(): a private copy of the project in which the multicam source sequence shows all its angles at
// once. Each angle's video track is scaled to 1/columns and moved into its cell (row-major, angle 1 top left); the
// other video tracks, the audio tracks and the master video stack are dropped (a multicam angle has no master stage).
// One composite per frame then draws every angle, one decoder per angle, so the grid costs one offscreen frame.
//
// MulticamGridRenderer: a worker thread. It keeps one rendered source open (forward decoding) and renders the frame
// nearest the time it was last given, skipping what it cannot keep up with while playing; a jump back or far ahead
// reopens the source. Each published frame carries its cost: the request-to-readback time of that composite.

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <QImage>
#include <QSize>
#include <QString>

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace wabcut::app {

/// Columns of the grid for `angles` angles: 2 up to 4, 3 up to 9, 4 beyond (Premiere's 2x2 to 4x4).
int multicamGridColumns(std::size_t angles);

/// The project with `source` (a multicam source sequence) drawing all its angles in a `columns` x `columns` grid.
/// Errors: NotFound, InvalidArgument (not a multicam source).
std::expected<ProjectState, Error> multicamGridState(const ProjectState& state, SequenceId source, int columns);

struct MulticamGridFrame {
    QImage image;           ///< RGBA8, the grid at the render size
    Ticks time;             ///< the source sequence time drawn
    double milliseconds = 0; ///< request to readback for this frame (composite + upload + decode of new frames)
    bool opened = false;     ///< the source was opened for this frame (its cost includes that)
    QString backend;        ///< the renderer's description ("OffscreenRenderer D3D11 · ...")
};

/// One grid frame at `time`, rendered on the calling thread (paused previews, tests, the bench).
std::expected<MulticamGridFrame, Error> renderMulticamGrid(const ProjectState& grid, SequenceId source, Ticks time,
                                                           QSize size, const std::filesystem::path& projectFile = {});

class MulticamGridRenderer {
public:
    /// Called on the worker thread with each rendered frame (or an error text).
    using Publish = std::function<void(std::expected<MulticamGridFrame, QString> frame, std::uint64_t generation)>;

    explicit MulticamGridRenderer(Publish publish);
    ~MulticamGridRenderer();

    /// What to draw: the grid project, its source sequence and the render size. A new generation drops the open source.
    void setSource(std::shared_ptr<const ProjectState> grid, SequenceId source, QSize size, std::filesystem::path projectFile,
                   std::uint64_t generation);
    /// The source time to draw, and whether it advances (playing). Cheap: the GUI calls it on every playhead change.
    void setTime(Ticks time, bool playing);
    void clear();

private:
    struct Request {
        std::shared_ptr<const ProjectState> grid;
        SequenceId source;
        QSize size;
        std::filesystem::path projectFile;
        std::uint64_t generation = 0;
        Ticks time;
        bool playing = false;
        std::uint64_t serial = 0; ///< bumped by every change
    };
    void run(std::stop_token stop);

    Publish m_publish;
    std::mutex m_mutex;
    std::condition_variable_any m_changed;
    Request m_request;
    std::jthread m_thread;
};

} // namespace wabcut::app
