#include <wabcut/editor/PreviewRender.h>

#include <wabcut/editor/Production.h>

#include <stop_token>
#include <system_error>
#include <utility>

namespace wabcut::editor {

namespace {

class ExportPreviewRenderer final : public PreviewRenderer {
public:
    explicit ExportPreviewRenderer(PreviewRenderOptions options) : m_options(std::move(options)) {}

    std::expected<std::int64_t, Error> render(const ProjectState& state, SequenceId sequence, TimeRange range,
                                              const std::filesystem::path& file,
                                              const std::function<bool(double)>& progress) override {
        if (range.duration.value <= 0) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "a preview segment must not be empty"});
        }
        // The cache folder ("<project> Preview Files") does not exist before a project's first render (load() and
        // render.previewInOut do not make it); the export refuses a missing output folder.
        std::error_code folderError;
        std::filesystem::create_directories(file.parent_path(), folderError);
        exporter::ExportRequest job;
        job.snapshot = std::make_shared<const ProjectState>(state);
        job.sequence = sequence;
        job.settings = previewExportSettings(m_options.codec, range);
        job.output = file;
        job.projectFile = m_options.projectFile;
        job.videoSource = m_options.videoSource;
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
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(result->output, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::IoError, "the preview file is missing after its render: " + ec.message()});
        }
        return static_cast<std::int64_t>(bytes);
    }

private:
    PreviewRenderOptions m_options;
};

} // namespace

exporter::ExportSettings previewExportSettings(PreviewCodec codec, TimeRange range) {
    exporter::ExportSettings s;
    s.presetId = codec == PreviewCodec::ProResLt ? "wabcut-preview-prores-lt" : "wabcut-preview-dnxhr-lb";
    s.container = exporter::Container::Mov;
    s.video.enabled = true;
    if (codec == PreviewCodec::ProResLt) {
        s.video.codec = exporter::VideoCodec::ProRes;
        s.video.proresProfile = exporter::ProResProfile::Lt;
        s.video.pixelFormat = exporter::OutputPixelFormat::Yuv422p10;
    } else {
        s.video.codec = exporter::VideoCodec::DnxHR;
        s.video.dnxhrProfile = exporter::DnxhrProfile::Lb;
        s.video.pixelFormat = exporter::OutputPixelFormat::Yuv422p;
    }
    s.video.encoder = "auto"; // software on Windows: neither codec has a hardware encoder there
    s.audio.enabled = false;
    s.range = exporter::RangeMode::Custom;
    s.customRange = range;
    s.captions.mode = exporter::CaptionExport::None;
    return s;
}

std::shared_ptr<PreviewRenderer> makeExportPreviewRenderer(PreviewRenderOptions options) {
    return std::make_shared<ExportPreviewRenderer>(std::move(options));
}

std::filesystem::path previewFolderFor(const std::filesystem::path& projectFile) {
    // 0.2 (P.3): a production's scratch disk for previews (its Production Settings) holds every project's previews.
    if (const auto production = productionOf(projectFile)) {
        if (std::filesystem::path shared = production->previewFolderFor(projectFile); !shared.empty()) {
            return shared;
        }
    }
    std::filesystem::path stem = projectFile.stem();
    stem += " Preview Files";
    return projectFile.parent_path() / stem;
}

} // namespace wabcut::editor
