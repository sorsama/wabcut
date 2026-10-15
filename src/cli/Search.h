#pragma once

// wabcut-cli's smart search (AI-8, docs/ai.md "Smart search"): the services of search.footage and search.indexMedia for
// `cmd` and `mcp` — the shared search index in the user cache, the project's media, and SigLIP 2 in the wabcut-embed
// helper next to the CLI when it is built and the model is downloaded (else transcripts only). Opened on first use.

#include <wabcut/command/ExecutionContext.h>

#include <filesystem>
#include <memory>

std::shared_ptr<wabcut::command::CommandServices> searchServicesFor(const std::filesystem::path& project);
