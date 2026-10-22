#pragma once

// Private (0.2): smart rendering. Frames an intra-only export (ProRes, DNxHR / DNxHD) can take as they are, without
// rendering and encoding them: Use Previews (rendered preview segments that still show what the sequence shows, when
// the export's format is the previews') and Smart Render (a single source clip shown untouched: full frame, no Motion,
// Opacity, effects, transitions, redactions or colour conversion, its own size and rate). Their packets are copied
// from the preview file or the source file into the export, which must then encode with no frame delay (slice
// threads), so copied and encoded packets interleave in order.
//
// The plan only copies what it can prove: the packet's codec parameters (codec, profile / FourCC, size, pixel format,
// field order, colour tags) equal the export encoder's, and the file's index has the frame. Everything else renders.

#include "Encode.h"

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::exporter::detail {

/// Where a copied frame comes from.
struct CopyFrame {
    int file = -1;              ///< index into SmartRenderPlan::files(); -1: the frame renders
    int stream = 0;             ///< the container stream
    std::int64_t timestamp = 0; ///< the packet's pts, in its stream's time base
    bool preview = false;       ///< from a preview file (Use Previews), else a source file (Smart Render)
};

/// Whether an export can smart render at all: ProRes or DNxHR / DNxHD, no picture effects, burned-in captions or
/// alpha, and Smart Render or Use Previews on.
bool smartRenderEligible(const ExportSettings& settings);

class SmartRenderPlan {
public:
    struct Inputs {
        const ProjectState* project = nullptr;  ///< locators at the original files (what renders)
        const ProjectState* snapshot = nullptr; ///< the project as the preview cache hashes it (Use Previews)
        const Sequence* sequence = nullptr;
        const ResolvedExport* resolved = nullptr;
        const ExportSettings* settings = nullptr;
        const AVCodecParameters* encoder = nullptr; ///< the opened export encoder's parameters
        PreviewCache* previews = nullptr;           ///< Use Previews (may be null)
        std::filesystem::path projectFile;
    };

    /// Decides frame by frame (cheap: evaluation and file indexes, nothing is decoded). Never fails: what it cannot
    /// prove renders.
    static SmartRenderPlan build(const Inputs& inputs, const std::stop_token& stop);

    std::int64_t copiedFrames() const { return m_copied; }
    std::int64_t previewFrames() const { return m_previewCopied; }
    /// The output frames that render, in order.
    std::vector<std::int64_t> renderedFrames() const;
    /// The copy of output frame k, or null when it renders.
    const CopyFrame* copyFor(std::int64_t k) const;
    const std::vector<std::filesystem::path>& files() const { return m_files; }

private:
    std::vector<CopyFrame> m_frames; ///< one per output frame
    std::vector<std::filesystem::path> m_files;
    std::int64_t m_copied = 0;
    std::int64_t m_previewCopied = 0;
};

/// Reads the planned packets (each file opened once, read forward; a jump seeks).
class PacketCopier {
public:
    explicit PacketCopier(const SmartRenderPlan& plan);
    ~PacketCopier();
    PacketCopier(const PacketCopier&) = delete;
    PacketCopier& operator=(const PacketCopier&) = delete;

    /// The packet of `copy`, stamped as output frame `k` (pts = dts = k, duration 1, key frame) in `timeBase`.
    /// Errors: IoError, DecoderError (the packet is not in the file).
    std::expected<PacketPtr, Error> packet(const CopyFrame& copy, std::int64_t k);

private:
    struct Input;
    const SmartRenderPlan& m_plan;
    std::vector<std::unique_ptr<Input>> m_inputs;
};

} // namespace wabcut::exporter::detail
