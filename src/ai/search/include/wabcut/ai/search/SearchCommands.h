#pragma once

// The smart-search commands of the catalog (docs/commands.md):
//
//   search.footage     Read           the moments of the project's media that show or say what a query describes:
//                                     timecoded source ranges with a score, the matched words and a thumbnail time
//   search.indexMedia  MediaAnalysis  brings the search index up to date for some or all media (a job: decodes
//                                     keyframes, runs the embedding model, reads transcripts)
//
// They need a FootageSearch, which the host provides as SearchServices (alone or in a command::ServiceBundle); without
// it they fail with InvalidRequest. A hit's range goes straight into clip.insert / clip.overwrite (sourceIn,
// sourceOut).

#include <wabcut/ai/search/FootageSearch.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <expected>
#include <functional>
#include <memory>
#include <mutex>

namespace wabcut::ai::search {

class SearchServices : public command::CommandServices {
public:
    using Opener = std::function<std::expected<std::shared_ptr<FootageSearch>, Error>()>;

    explicit SearchServices(std::shared_ptr<FootageSearch> search) : m_search(std::move(search)) {}
    /// Opens the search on first use: hosts that run many commands and rarely search (wabcut-cli) do not open the
    /// index for every command. A failed open is retried on the next use.
    explicit SearchServices(Opener open) : m_open(std::move(open)) {}

    /// The search, opened if needed. Errors: the opener's.
    std::expected<std::shared_ptr<FootageSearch>, Error> search();

private:
    std::mutex m_mutex;
    std::shared_ptr<FootageSearch> m_search;
    Opener m_open;
};

/// Adds search.footage and search.indexMedia.
void registerSearchCommands(command::CommandRegistry& registry);

/// A hit as the commands report it (also the app's result model).
command::Json hitToJson(const SearchHit& hit);

} // namespace wabcut::ai::search
