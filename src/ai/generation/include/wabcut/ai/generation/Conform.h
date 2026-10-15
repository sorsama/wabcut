#pragma once
#include <wabcut/ai/generation/GenerationCommands.h>
#include <wabcut/media/ThumbnailService.h>
#include <filesystem>
namespace wabcut::ai::generation {
std::expected<media::Thumbnail, Error> boundaryFrame(const std::string& path, Ticks sourceTime, std::stop_token stop = {});
// Decodes through VideoDecoder, resamples on the exact target frame clock, scales and encodes via ExportJob.
std::expected<MediaInfo, Error> conformVideo(const std::filesystem::path& input, const std::filesystem::path& output,
    const Sequence& target, Ticks duration, bool reverse, std::stop_token stop,
    const std::function<void(float)>& progress = {});
}
