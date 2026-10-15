#include "Search.h"

#include <wabcut/ai/search/MediaSearchAccess.h>
#include <wabcut/ai/search/SearchCommands.h>

std::shared_ptr<wabcut::command::CommandServices> searchServicesFor(const std::filesystem::path& project) {
    namespace search = wabcut::ai::search;
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(project, ec);
    return std::make_shared<search::SearchServices>([absolute] {
        search::SearchHostOptions options;
        options.projectFile = absolute;
        return search::openFootageSearch(options);
    });
}
