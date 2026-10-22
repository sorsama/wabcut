#pragma once

// XMP of media files (0.2, P.3: Premiere's Metadata panel File section and its links to the Project panel's clip
// metadata columns).
//
//   * Reading: a sidecar next to the file ("<name>.xmp", as Premiere writes it for formats it cannot write into, or
//     "<name>.<ext>.xmp") wins over a packet inside the file. Inside the file: MP4 / QuickTime keep it in Adobe's
//     top-level 'uuid' box or in moov/udta/'XMP_' (found by walking the boxes, never reading the media data); other
//     formats (JPEG, PNG, TIFF, WAV, MXF, AVI, ...) by scanning for the packet in the first and last
//     kXmpScanBytes of the file (the XMP specification's packet scanning). Files are untrusted: every read is bounded.
//   * Parsing (pugixml, no DTDs or entities): RDF/XML properties written as attributes of rdf:Description, as simple
//     elements, language alternatives (x-default first), bags and sequences (joined with ", ") and structures (one
//     level, as "name/field"). Namespaces are resolved, so any prefix the writer chose is read.
//   * Clip metadata: Premiere links Scene = xmpDM:scene, Shot = xmpDM:shotName, Take = xmpDM:takeNumber, Log Note =
//     xmpDM:logComment, Good = xmpDM:good, Tape Name = xmpDM:tapeName, Client = xmpDM:client, Comment =
//     xmpDM:comment, Description = dc:description (clipFieldsFromXmp / writeXmpSidecar).
//   * Writing: a sidecar ("<name>.xmp"; an existing one keeps every property not written), written atomically; and, when
//     the user turns it on, into MP4 / MOV files (embedXmp): a checked copy replaces the file, never the file edited in
//     place, so a failure leaves it as it was.
//   * Camera fields: Camera Angle / Label / Model, Shot Date and Shot Location are linked like Scene
//     (xmpDM:cameraAngle, cameraLabel, cameraModel, shotDate, shotLocation).

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange {

/// Bytes scanned at each end of a file that is not ISO-BMFF (and the most an XMP box or sidecar may hold).
inline constexpr std::size_t kXmpScanBytes = 8 * 1024 * 1024;
/// Bound on the properties read from one packet.
inline constexpr std::size_t kMaxXmpProperties = 4096;

struct XmpProperty {
    std::string ns;     ///< the namespace URI
    std::string prefix; ///< the usual prefix of that namespace ("dc", "xmp", "xmpDM", ...), else the file's
    std::string name;   ///< the local name; a structure's field as "name/field"
    std::string label;  ///< for people: "Scene", "Shot Name", ... (the name when not known)
    std::string group;  ///< the schema for people: "Dublin Core", "Basic", "Dynamic Media", ... (the prefix when not known)
    std::string value;  ///< the text (a language alternative's x-default, a bag's items joined with ", ")

    bool operator==(const XmpProperty&) const = default;
};

/// Where a packet came from.
enum class XmpSource : std::uint8_t { Sidecar, Embedded };

struct XmpPacket {
    XmpSource source = XmpSource::Embedded;
    std::filesystem::path file; ///< the sidecar, or the media file
    std::vector<XmpProperty> properties;
};

/// Parses an XMP packet (the x:xmpmeta element, the rdf:RDF element or a whole sidecar). ParseError when it is not XML.
std::expected<std::vector<XmpProperty>, Error> parseXmp(std::string_view packet);

/// The XMP sidecar of `media` Premiere reads and writes: "<dir>/<stem>.xmp".
std::filesystem::path xmpSidecarPath(const std::filesystem::path& media);

struct XmpReadOptions {
    /// Packet scanning (formats other than ISO-BMFF) only for files up to this size: import passes a small bound so a
    /// large MXF or AVI is not read at both ends for every file; sidecars and MP4 / MOV boxes are always read.
    std::uintmax_t scanFilesUpTo = ~std::uintmax_t{0};
};

/// Reads the XMP of a media file (see the file comment); nullopt when it has none. IoError when the file cannot be read.
std::expected<std::optional<XmpPacket>, Error> readMediaXmp(const std::filesystem::path& media,
                                                            const XmpReadOptions& options = {});

/// The packet text inside raw file bytes (scanning for <x:xmpmeta ... </x:xmpmeta>, else <rdf:RDF ... </rdf:RDF>);
/// nullopt when there is none. Exposed for tests.
std::optional<std::string_view> findXmpPacket(std::string_view bytes);

/// The clip metadata an XMP packet carries (Premiere's links); fields the packet does not have stay unset.
struct XmpClipFields {
    std::optional<std::string> scene;
    std::optional<std::string> shot;
    std::optional<std::string> take;
    std::optional<std::string> logNote;
    std::optional<std::string> client;
    std::optional<bool> good;
    std::optional<std::string> tapeName;
    std::optional<std::string> comment;
    std::optional<std::string> description;
    std::optional<std::string> cameraAngle;
    std::optional<std::string> cameraLabel;
    std::optional<std::string> cameraModel;
    std::optional<std::string> shotDate;
    std::optional<std::string> shotLocation;

    bool empty() const {
        return !scene && !shot && !take && !logNote && !client && !good && !tapeName && !comment && !description &&
               !cameraAngle && !cameraLabel && !cameraModel && !shotDate && !shotLocation;
    }
};
XmpClipFields clipFieldsFromXmp(const std::vector<XmpProperty>& properties);
/// Puts the fields into a media item (an item being imported: Premiere fills its linked columns from the file's
/// XMP). With `overwrite` false only empty fields are set. Values over the model's bounds are cut at a UTF-8 boundary.
void applyXmpClipFields(MediaItem& item, const XmpClipFields& fields, bool overwrite);

/// The XMP of a media item's clip metadata (a fresh packet: dc:title, dc:description and the linked xmpDM fields).
std::string xmpPacketFor(const MediaItem& item);

/// One property the Metadata panel's File section sets: its namespace, the usual prefix, the local name and the text
/// ("" removes it). Language alternatives (dc:title, dc:description, dc:rights) keep their x-default form, bags and
/// sequences (dc:subject, dc:creator) take items separated by ", "; a structure's fields ("name/field") cannot be set.
struct XmpEdit {
    std::string ns;
    std::string prefix;
    std::string name;
    std::string value;

    bool operator==(const XmpEdit&) const = default;
};

/// The properties the File section offers to add (Wabcut's known table, labelled and grouped), value empty.
std::vector<XmpProperty> knownXmpProperties();
/// The linked clip metadata of `item` as edits: what writeXmpSidecar and embedXmp write for it.
std::vector<XmpEdit> linkedXmpEdits(const MediaItem& item);
/// `edits` applied to an XMP packet (`existing`; empty: a new one), the rest kept. ParseError when `existing` is not XMP.
std::expected<std::string, Error> editedXmpPacket(std::string_view existing, const std::vector<XmpEdit>& edits);
/// Sets `edits` in the sidecar of `media` (default "<stem>.xmp", made when missing), keeping what else it holds.
/// Returns the file written.
std::expected<std::filesystem::path, Error> editXmpSidecar(const std::filesystem::path& media,
                                                           const std::vector<XmpEdit>& edits,
                                                           const std::filesystem::path& sidecar = {});
/// Whether `media` is an ISO-BMFF file (MP4, MOV, M4A, 3GP) embedXmp can write into.
bool canEmbedXmp(const std::filesystem::path& media);
/// Writes `edits` into the XMP inside an MP4 / MOV file (Adobe's top-level 'uuid' box), keeping its other properties.
/// A copy is made next to the file; in the copy the old XMP boxes become 'free' boxes (nothing before the media data
/// moves, so every sample offset stays valid), the new box is appended (a trailing old one is cut off first), the copy
/// is read back and checked, and only then does it replace the file. Any failure removes the copy and leaves the file
/// as it was; a file another program holds open cannot be replaced (IoError). The file's size and tail change, so its
/// fingerprint does too: the caller records the new one.
std::expected<void, Error> embedXmp(const std::filesystem::path& media, const std::vector<XmpEdit>& edits);

/// Writes the item's clip metadata into `sidecar` (default: xmpSidecarPath of its file), keeping what an existing
/// sidecar holds besides the linked properties. Returns the file written. InvalidArgument for an item without a file
/// (titles, synthetic items); IoError / ParseError when an existing sidecar cannot be read or is not XMP.
std::expected<std::filesystem::path, Error> writeXmpSidecar(const MediaItem& item,
                                                            const std::filesystem::path& sidecar = {});

} // namespace wabcut::interchange
