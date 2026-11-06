#pragma once

// Data tracks of a media file (0.7, stabilizing with the camera's gyro): the timed metadata cameras write beside the
// picture - GoPro's 'gpmd', Sony's 'rtmd', DJI's 'djmd' - listed and read packet by packet through the demuxer. Only the
// bytes are handed out; wabcut/gyro/Gyro.h parses them. Untrusted input: every read is bounded (packets, total bytes),
// cancellable, and a damaged file gives an Error, never a crash.

#include <wabcut/model/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

namespace wabcut::media {

struct DataStreamInfo {
    int index = -1;            ///< the container's stream index
    std::string tag;           ///< the sample entry's four characters ("gpmd"); empty when the container has none
    std::string handler;       ///< the track's handler name ("GoPro MET"), empty when it has none
    std::int64_t packets = -1; ///< sample count when the container knows it
};

/// The data streams of `path` (UTF-8). Headers only. Errors as media::probe.
std::expected<std::vector<DataStreamInfo>, Error> listDataStreams(const std::string& path);

struct DataPacket {
    double seconds = 0.0;  ///< presentation time on the source clock of the file's pictures (the same zero as a clip's source time)
    double duration = 0.0; ///< seconds; where the container gives none, the gap to the next packet (the last one repeats the gap before)
    std::vector<std::uint8_t> bytes;
};

struct DataReadOptions {
    std::size_t maxPackets = 4'000'000;
    std::size_t maxBytes = std::size_t{768} << 20;
    std::size_t firstPackets = 0; ///< when > 0, stop after this many packets (a quick look at the start of a track)
    std::function<bool()> cancelled; ///< polled between packets; true stops with ErrorCode::Cancelled
};

/// Hands every packet of data stream `streamIndex` (a DataStreamInfo::index) to `each`, in order, one at a time (the packet's
/// bytes are valid during the call); `each` returns false to stop. The other streams are not read, so a long recording's data
/// track is read without going through its pictures, and without holding its bytes.
std::expected<void, Error> forEachDataPacket(const std::string& path, int streamIndex, const std::function<bool(const DataPacket&)>& each,
                                             const DataReadOptions& options = {});

/// All packets of the stream in memory (a quick look at a short track, tests): forEachDataPacket() collected, bounded by the
/// options' maxPackets and maxBytes.
std::expected<std::vector<DataPacket>, Error> readDataPackets(const std::string& path, int streamIndex, const DataReadOptions& options = {});

} // namespace wabcut::media
