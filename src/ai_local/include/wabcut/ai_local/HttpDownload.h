#pragma once

// A minimal IDownloadTransport on the OS HTTP stack (WinHTTP on Windows), for model downloads from the CLI and
// until the Qt transport (src/ai_net) is injected by the app. HTTPS only; follows redirects; no telemetry, no
// cookies, no credentials.

#include <wabcut/ai/ModelStore.h>

#include <memory>

namespace wabcut::ai_local {

/// nullptr on platforms without an implementation yet (only Windows has one).
std::unique_ptr<ai::IDownloadTransport> makeHttpDownloadTransport();

} // namespace wabcut::ai_local
