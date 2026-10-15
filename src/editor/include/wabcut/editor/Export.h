#pragma once

// The export seam: how the editor (File > Export, the e2e test) asks for a sequence to be rendered to a file.
//
// An implementation is registered with setExporter(); until then exportSequence() fails with Unsupported and the
// UI says export is not available yet. makeExportJobExporter() is the export module's ExportJob (M6) behind it.
// The request carries only what the editor decides (sequence, range, output, preset id); encoding settings belong
// to the exporter.

#include <wabcut/export/ExportJob.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace wabcut::editor {

struct ExportRequest {
    SequenceId sequence;
    /// Default: the work area when set, else the whole sequence.
    std::optional<TimeRange> range;
    std::filesystem::path output;
    /// Exporter-defined preset id; empty = the exporter's default.
    std::string preset;
};

struct ExportResult {
    std::filesystem::path output;
    std::int64_t videoFrames = 0;
    std::int64_t audioSamples = 0;
};

/// Progress in [0, 1]; return false to cancel (the exporter then fails with Cancelled).
using ExportProgress = std::function<bool(double fraction)>;

class Exporter {
public:
    virtual ~Exporter() = default;
    /// "wabcut-export (h264_nvenc)" etc., for the UI.
    virtual std::string name() const = 0;
    /// Renders `request` from the snapshot `project` (immutable: the user may keep editing meanwhile).
    virtual std::expected<ExportResult, Error> run(std::shared_ptr<const ProjectState> project,
                                                   const ExportRequest& request, const ExportProgress& progress) = 0;
};

/// Process-wide exporter (null = none). Thread-safe.
void setExporter(std::shared_ptr<Exporter> exporter);
std::shared_ptr<Exporter> exporter();

/// The export module's ExportJob as an Exporter: `ExportRequest::preset` names a built-in preset ("match-source"
/// when empty), a range becomes a custom range, progress callbacks can cancel. `videoSource` empty = the export
/// module's default (the program renderer once M6 phase 2 lands; Unsupported before); tests pass
/// exporter::syntheticVideoSourceFactory(). Audio is always the real timeline mix.
std::shared_ptr<Exporter> makeExportJobExporter(exporter::VideoSourceFactory videoSource = {});

/// Exports from the session's current snapshot with the registered exporter. Unsupported when none is registered,
/// NotFound for an unknown sequence. Runs on the calling thread (call it from a worker for a responsive UI).
std::expected<ExportResult, Error> exportSequence(const EditSession& session, ExportRequest request,
                                                  const ExportProgress& progress = {});

} // namespace wabcut::editor
