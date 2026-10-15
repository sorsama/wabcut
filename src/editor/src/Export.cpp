#include <wabcut/editor/Export.h>

#include <mutex>
#include <stop_token>
#include <utility>

namespace wabcut::editor {

namespace {

std::mutex g_exporterMutex;
std::shared_ptr<Exporter> g_exporter;

class ExportJobExporter final : public Exporter {
public:
    explicit ExportJobExporter(exporter::VideoSourceFactory videoSource) : m_videoSource(std::move(videoSource)) {}

    std::string name() const override { return "wabcut-export"; }

    std::expected<ExportResult, Error> run(std::shared_ptr<const ProjectState> project, const ExportRequest& request,
                                           const ExportProgress& progress) override {
        const std::string presetId = request.preset.empty() ? std::string("match-source") : request.preset;
        const std::optional<exporter::Preset> preset = exporter::findPreset(presetId);
        if (!preset) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "unknown export preset: " + presetId});
        }
        exporter::ExportRequest job;
        job.snapshot = std::move(project);
        job.sequence = request.sequence;
        job.settings = preset->settings;
        if (request.range) {
            job.settings.range = exporter::RangeMode::Custom;
            job.settings.customRange = request.range;
        }
        job.output = request.output;
        job.videoSource = m_videoSource;
        std::stop_source stop;
        if (progress) {
            job.onProgress = [&](const exporter::ExportProgress& p) {
                if (!progress(p.fraction())) {
                    stop.request_stop();
                }
            };
        }
        exporter::ExportJob exportJob(std::move(job));
        auto result = exportJob.run(stop.get_token());
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        return ExportResult{result->output, result->frames, result->audioSamples};
    }

private:
    exporter::VideoSourceFactory m_videoSource;
};

} // namespace

std::shared_ptr<Exporter> makeExportJobExporter(exporter::VideoSourceFactory videoSource) {
    return std::make_shared<ExportJobExporter>(std::move(videoSource));
}

void setExporter(std::shared_ptr<Exporter> exporter) {
    std::lock_guard lock(g_exporterMutex);
    g_exporter = std::move(exporter);
}

std::shared_ptr<Exporter> exporter() {
    std::lock_guard lock(g_exporterMutex);
    return g_exporter;
}

std::expected<ExportResult, Error> exportSequence(const EditSession& session, ExportRequest request,
                                                  const ExportProgress& progress) {
    const std::shared_ptr<Exporter> impl = exporter();
    if (!impl) {
        return std::unexpected(Error{ErrorCode::Unsupported, "export is not available in this build yet"});
    }
    const ProjectSnapshot project = session.snapshot();
    const Sequence* sequence = project->findSequence(request.sequence);
    if (!sequence) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    if (!request.range) {
        request.range = sequence->workArea ? *sequence->workArea : TimeRange{Ticks{}, sequence->duration};
    }
    return impl->run(project, request, progress);
}

} // namespace wabcut::editor
