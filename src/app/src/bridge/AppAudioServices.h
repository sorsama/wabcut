#pragma once

// The command services of the app's audio tools (M5l), for the bridges that run these commands from the UI
// (ClipAudioBridge, MulticamEditBridge), bundled like the assistant's (AgentController::services()):
//   * the media-backed audio analysis: loudness, speech for Auto-Duck, and audio sync (audio.syncClips, A.5);
//   * Enhance Speech and stem separation on this computer (A.4), first-use downloads through the AI network stack;
//   * multicam.create's audio sync (C.8 MulticamServices on the same analysis).
// Takes and stems go next to a saved project (<project>.wabcut-media/enhanced and /stems), so they travel with it;
// an unsaved project uses the user cache.

#include <filesystem>
#include <memory>
#include <string_view>

class AiController;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class TranscriptBridge;

struct AppAudioServiceOptions {
    std::filesystem::path projectFile;
    TranscriptBridge* transcripts = nullptr; ///< speech for Auto-Duck (optional)
    AiController* ai = nullptr;              ///< first-use model downloads; null: the models must be installed
};

std::shared_ptr<command::CommandServices> appAudioServices(const AppAudioServiceOptions& options);

/// <dir>/<project name>.wabcut-media/<leaf> for a saved project; empty for an unsaved one.
std::filesystem::path projectMediaDirectory(const std::filesystem::path& projectFile, std::string_view leaf);

} // namespace wabcut::app
