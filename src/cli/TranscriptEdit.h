#pragma once

// Transcript-first editing from the command line (docs/ai.md, "Text-based editing"):
//
//   wabcut-cli transcript-edit <project> [--remove-fillers] [--remove-silences <s>] [--min-confidence <0..1>]
//                              [--padding <s>] [--room-tone <s>] [--sequence <id>] [--review] [--dry-run] [--save]
//   wabcut-cli captions <project> [--sequence <id>] [--srt <out.srt>] [--vtt <out.vtt>] [--max-chars <n>]
//                       [--max-lines <n>] [--max-duration <s>] [--save]
//
// transcript-edit runs transcript.cleanup (or transcript.review with --review) and prints the command result;
// --dry-run previews the cuts without changing anything, --save writes the edited project back. captions writes
// SRT/WebVTT files and, with --save, stores the caption track in the project (captions.generate).
//
// Both take the transcripts from the analysis cache (what `transcribe` or the app stored for the media), from
// --cache <dir>, or from files: --transcript <media id or name>[:<audio stream>]=<transcript.json>.

#include <wabcut/command/ExecutionContext.h>

#include <filesystem>
#include <memory>

/// The transcript services for a project: the analysis cache (WABCUT_CACHE_DIR or the user cache) and the
/// audio-energy probe (AudioEnergyService). wabcut-cli's cmd and mcp subcommands serve the transcript commands with it.
std::shared_ptr<wabcut::command::CommandServices> transcriptServicesFor(const std::filesystem::path& project);

/// transcript.transcribe for a project: ai::TranscribeService with local transcription (when built) and the analysis
/// cache; no cloud (a headless request cannot answer the consent prompt).
std::shared_ptr<wabcut::command::CommandServices> transcribeServicesFor(const std::filesystem::path& project);

/// The audio analysis of the audio.* commands for a project: loudness on the render path and speech from the cached
/// transcripts or the audio level, results in the same analysis cache; and multicam.create's audio sync on it.
std::shared_ptr<wabcut::command::CommandServices> audioServicesFor(const std::filesystem::path& project);

int runTranscriptEdit(int argc, char** argv);
int runCaptions(int argc, char** argv);
