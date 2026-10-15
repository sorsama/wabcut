#pragma once

// A project as content-addressed chunks (C.9, the version history's storage; History.h). The project root holds the
// media list and its sequences as {"$chunk": key} references, each sequence holds its tracks the same way, so a
// revision shares every unchanged sequence and track with the revisions before it. The chunk texts are the project
// file's JSON for those parts: a revision loads through the same migrations and checks as a project file.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::io::detail {

/// Where writeChunks puts the chunks.
class ChunkSink {
public:
    virtual ~ChunkSink() = default;
    /// The key of `node` (a sequence or track, shared between revisions) when it was stored before, else nullopt:
    /// unchanged nodes are not serialised again.
    virtual std::optional<std::string> known(const std::shared_ptr<const void>& node) = 0;
    /// Stores a chunk (for `node`, or null for the root and the media list) and returns its key.
    virtual std::string put(const std::shared_ptr<const void>& node, std::string text) = 0;
};

/// Writes `state` as chunks; returns the root's key.
std::expected<std::string, Error> writeChunks(const ProjectState& state, ChunkSink& sink);

/// The text of the chunk with a key.
using ChunkSource = std::function<std::expected<std::string, Error>(const std::string& key)>;

/// Puts a project back together from its root chunk (migrating it like a project file and validating its structure).
std::expected<ProjectState, Error> readChunks(const std::string& rootKey, const ChunkSource& get);

/// The keys a chunk refers to: the root's media list and sequences, a sequence's tracks.
std::expected<std::vector<std::string>, Error> chunkReferences(std::string_view text);

} // namespace wabcut::io::detail
