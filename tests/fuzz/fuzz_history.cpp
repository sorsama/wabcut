// Fuzz harness: a version history folder (History.h) - untrusted when a project and its .history folder come from
// someone else. The first byte picks the file:
//   0  index.json (parseHistoryIndex)
//   1  an object file: "WCH1", length, zlib data (decodeHistoryObject); what decodes encodes and decodes back
//   2  a revision's chunks, '\0'-separated, keyed "0", "1", ... with the last one the root (writeChunks writes it
//      last; readChunks); what loads is structurally valid and comes back the same through writeChunks + readChunks

#include "FuzzSupport.h"

#include "HistoryFormat.h"
#include "ProjectChunks.h"

#include <wabcut/model/ProjectState.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;

namespace {

/// Chunks kept in memory, keyed by number, for the round trip.
class MemorySink final : public io::detail::ChunkSink {
public:
    std::optional<std::string> known(const std::shared_ptr<const void>&) override { return std::nullopt; }
    std::string put(const std::shared_ptr<const void>&, std::string text) override {
        std::string key = std::to_string(chunks.size());
        chunks.emplace(key, std::move(text));
        return key;
    }
    std::map<std::string, std::string> chunks;
};

void index(std::string_view text) {
    const auto parsed = detail::parseHistoryIndex(text);
    if (!parsed) {
        return;
    }
    WABCUT_FUZZ_CHECK(parsed->revisions.size() <= kMaxHistoryRevisions, std::string(text));
    for (std::size_t i = 1; i < parsed->revisions.size(); ++i) {
        WABCUT_FUZZ_CHECK(parsed->revisions[i - 1].id < parsed->revisions[i].id, std::string(text));
    }
    WABCUT_FUZZ_CHECK(parsed->revisions.empty() || parsed->nextRevision > parsed->revisions.back().id, std::string(text));
}

void object(std::string_view file) {
    const auto text = detail::decodeHistoryObject(file);
    if (!text) {
        return;
    }
    const auto encoded = detail::encodeHistoryObject(*text);
    WABCUT_FUZZ_CHECK(encoded, encoded ? std::string() : encoded.error().message);
    const auto decoded = detail::decodeHistoryObject(*encoded);
    WABCUT_FUZZ_CHECK(decoded && *decoded == *text, "object round trip");
}

void chunks(std::string_view data) {
    std::map<std::string, std::string> pieces;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= data.size(); ++i) {
        if (i == data.size() || data[i] == '\0') {
            pieces.emplace(std::to_string(pieces.size()), std::string(data.substr(start, i - start)));
            start = i + 1;
        }
    }
    const auto source = [&](const std::map<std::string, std::string>& from) {
        return [&from](const std::string& key) -> std::expected<std::string, Error> {
            const auto it = from.find(key);
            if (it == from.end()) {
                return std::unexpected(Error{ErrorCode::NotFound, "no chunk " + key});
            }
            return it->second;
        };
    };
    const auto state = io::detail::readChunks(std::to_string(pieces.size() - 1), source(pieces));
    if (!state) {
        return;
    }
    const auto valid = validate(*state, Validation::Structure);
    WABCUT_FUZZ_CHECK(valid, valid ? std::string() : valid.error().message);
    MemorySink sink;
    const auto root = io::detail::writeChunks(*state, sink);
    WABCUT_FUZZ_CHECK(root, root ? std::string() : root.error().message);
    const auto again = io::detail::readChunks(*root, source(sink.chunks));
    WABCUT_FUZZ_CHECK(again, again ? std::string() : again.error().message);
    WABCUT_FUZZ_CHECK(*again == *state, "chunk round trip");
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::string_view rest = fuzz::text(data + 1, size - 1);
    switch (data[0] % 3) {
    case 0:
        index(rest);
        break;
    case 1:
        object(rest);
        break;
    default:
        chunks(rest);
        break;
    }
    return 0;
}
