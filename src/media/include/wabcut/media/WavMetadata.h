#pragma once

// Channel names of a polyphonic WAV (0.7): field recorders name the tracks of a multichannel file ("Boom", "Lav A",
// "Mix L") in its iXML chunk - <BWFXML><TRACK_LIST><TRACK><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX><NAME>Boom</NAME>... -
// and some also in the description of the Broadcast Wave `bext` chunk. The file is untrusted: every read here is
// bounded, the XML is parsed without entity expansion or external references, and a name that is not plain UTF-8 text is
// dropped rather than repaired.
//
// iXML: http://www.gallery.co.uk/ixml/object_Details.html (TRACK_LIST, TRACK_COUNT, TRACK, CHANNEL_INDEX - the
// recorder's input number -, INTERLEAVE_INDEX - the track's position in the interleaved file, both counted from 1 -,
// NAME, FUNCTION; "All parameters and sub objects are optional", XML "in any unicode encoding").

#include <wabcut/model/MediaInfo.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::media {

/// At most this many channels get a name (the project file keeps them): AudioStreamInfo's bound.
inline constexpr int kMaxNamedChannels = 256;
/// A channel name is cut to this many bytes (on a character boundary).
inline constexpr std::size_t kMaxChannelNameBytes = 128;
static_assert(kMaxNamedChannels == static_cast<int>(kMaxChannelNames) && kMaxChannelNameBytes == kMaxChannelNameLength);
/// An iXML chunk larger than this is not read (real ones are a few KiB).
inline constexpr std::size_t kMaxIxmlBytes = 1024 * 1024;

/// The names of the `channels` channels of a file, from the iXML document `xml`: element i is the name of channel i + 1
/// of the interleaved file, empty when it has none. A TRACK without INTERLEAVE_INDEX counts by its place in the list.
/// A track whose index is outside 1..channels, a second track for the same channel and a name that is empty, not UTF-8
/// or only control characters are ignored. Empty result (no vector at all) when no channel got a name.
std::vector<std::string> parseIxmlTrackNames(std::string_view xml, int channels);

/// The names a Broadcast Wave `bext` description gives as lines "sTRK<n>=<name>" (n counted from 1; the keyword
/// convention of the BWF description field that recorders and Avid use), same rules as above.
std::vector<std::string> parseBextTrackNames(std::string_view description, int channels);

/// Reads the RIFF / RF64 / BW64 chunks of a WAV file (only their headers; the sound is never read) and returns the
/// channel names of its iXML chunk, else of its bext description. Empty on any problem, a file that is not a WAV and a
/// file with no names. Reads at most kMaxIxmlBytes of iXML and 256 bytes of bext.
std::vector<std::string> readWavChannelNames(const std::filesystem::path& file, int channels);

} // namespace wabcut::media
