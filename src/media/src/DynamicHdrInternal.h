#pragma once

// Private helpers of DynamicHdr.cpp that the probe also uses (it already has the file open). Never include from a public
// header.

#include <wabcut/media/DynamicHdr.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <optional>
#include <stop_token>
#include <string>

namespace wabcut::media::detail {

/// The Dolby Vision configuration record in a stream's coded side data (the container's 'dvcC' / 'dvvC' box), or nullopt.
std::optional<DolbyVisionInfo> dolbyVisionConfigOf(const AVCodecParameters* par);

/// scanDynamicHdr over an input that is already open (it reads packets from `fmt`: the position moves).
std::expected<DynamicHdrReport, Error> scanDynamicHdrIn(AVFormatContext* fmt, AVStream* stream, const std::string& source,
                                                        int maxFrames, std::stop_token stop);

} // namespace wabcut::media::detail
