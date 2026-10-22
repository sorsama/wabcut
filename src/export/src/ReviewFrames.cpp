#include <wabcut/export/ReviewFrames.h>

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/playback/video/Evaluate.h>

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QImageWriter>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <vector>

namespace wabcut::exporter {

RenderedReviewFrames::RenderedReviewFrames(ReviewFrameOptions options) : m_options(options) {}

RenderedReviewFrames::~RenderedReviewFrames() = default;

std::expected<QImage, Error> RenderedReviewFrames::picture(const ProjectState& state, SequenceId sequenceId, Ticks time,
                                                           int width) const {
    std::lock_guard lock(m_mutex);
    const Sequence* sequence = state.findSequence(sequenceId);
    if (!sequence || sequence->width <= 0 || sequence->height <= 0) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    // As an export: an offline original anywhere in the picture (nested too) is an error, not a thumbnail without it.
    for (const ResolvedLeaf& leaf : resolveSequenceLeaves(state, sequenceId, StreamKind::Video, time)) {
        const MediaItem* item = leaf.clip ? state.findMedia(leaf.clip->media) : nullptr;
        if (item && !item->isGenerator() && (item->locator.offline || item->locator.path.empty())) {
            return std::unexpected(Error{ErrorCode::NotFound, std::format("{} is offline", item->name)});
        }
    }
    if (!m_renderer) {
        render::OffscreenOptions options;
        options.backend = m_options.backend;
        options.role = render::OutputRole::Export;
        options.alpha = render::OutputAlpha::Opaque;
        options.compositor = render::CompositorOptions{render::TexturePool::Export, 1, 48};
        auto created = render::OffscreenRenderer::create(options);
        if (!created) {
            return std::unexpected(created.error());
        }
        m_renderer = std::move(*created);
    }

    // The export's picture at `time` (playback evaluator, originals, burned-in captions left out).
    std::vector<playback::LayerSpec> specs;
    playback::evaluateVideoLayers(state, *sequence, time, specs, true, nullptr, nullptr);
    std::vector<media::VideoFrame> frames(specs.size());
    std::map<std::uint64_t, std::unique_ptr<media::VideoDecoder>> decoders;
    for (std::size_t j = 0; j < specs.size(); ++j) {
        const playback::LayerSpec& spec = specs[j];
        if (!playback::needsDecode(spec)) {
            continue;
        }
        std::unique_ptr<media::VideoDecoder>& decoder = decoders[playback::sessionKey(spec)];
        if (!decoder) {
            const std::string* path = playback::sourcePathOf(*spec.item, false);
            if (!path) {
                return std::unexpected(Error{ErrorCode::NotFound, std::format("{} is offline", spec.item->name)});
            }
            media::DecoderOptions options;
            options.allowHardware = m_options.hardwareDecode;
            options.snapToStreamTicks = true; // sequence frames, as playback shows them
            auto opened = media::VideoDecoder::open(*path, spec.containerStream, options);
            if (!opened) {
                return std::unexpected(Error{opened.error().code, std::format("Cannot decode {}: {}", spec.item->name,
                                                                              opened.error().message)});
            }
            decoder = std::move(*opened);
        }
        Ticks wanted = spec.sourceTime;
        if (spec.interpolationSecond) {
            // P.2 Time Interpolation: the frame after the group's first one.
            if (j == 0 || !frames[j - 1].isValid()) {
                continue;
            }
            wanted = playback::interpolationNextTime(frames[j - 1]);
        }
        auto decoded = decoder->frameAt(wanted);
        if (!decoded) {
            if (decoded.error().code == ErrorCode::NotFound) {
                continue; // past the end of the media: the export shows nothing there either
            }
            return std::unexpected(Error{decoded.error().code, std::format("Cannot decode {}: {}", spec.item->name,
                                                                           decoded.error().message)});
        }
        frames[j] = std::move(*decoded);
    }
    auto composite = std::make_shared<render::PreparedComposite>();
    composite->targetTime = time;
    composite->canvas = {sequence->width, sequence->height};
    composite->color = m_options.color;
    composite->color.management = state.colorManagement; // as the monitor shows it (I.9; an HDR project tone-mapped)
    playback::buildCompositeLayers(specs, frames, composite->layers, &playback::computedMotion(), composite->canvas);

    // At most `width` pixels wide, at the sequence's aspect (and never larger than the sequence).
    const int w = std::clamp(width, 16, std::max(16, sequence->width));
    const int h = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * sequence->height / sequence->width)));
    auto image = m_renderer->render(composite, QSize(w, h));
    if (!image) {
        return std::unexpected(image.error());
    }
    // A deep copy: the rendered bytes go away with `image`.
    return QImage(image->rgba.data(), image->width, image->height, image->width * 4, QImage::Format_RGBA8888).copy();
}

std::expected<command::ReviewFrame, Error> RenderedReviewFrames::frame(const ProjectState& state, SequenceId sequenceId,
                                                                        Ticks time, int width) const {
    const auto rendered = picture(state, sequenceId, time, width);
    if (!rendered) {
        return std::unexpected(rendered.error());
    }
    const QImage& image = *rendered;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    command::ReviewFrame out;
    const bool jpeg = QImageWriter::supportedImageFormats().contains("jpeg") &&
                      image.convertToFormat(QImage::Format_RGB888).save(&buffer, "JPEG", std::clamp(m_options.jpegQuality, 1, 100));
    if (jpeg) {
        out.mime = "image/jpeg";
    } else {
        bytes.clear();
        buffer.seek(0);
        if (!image.save(&buffer, "PNG")) {
            return std::unexpected(Error{ErrorCode::IoError, "could not encode the thumbnail"});
        }
        out.mime = "image/png";
    }
    out.bytes.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    return out;
}

} // namespace wabcut::exporter
