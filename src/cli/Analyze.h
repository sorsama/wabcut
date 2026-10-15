#pragma once

// Media analysis from the command line (docs/ai.md, "Media analysis"):
//
//   wabcut-cli analyze <media> [--shots] [--beats] [--json] [--detector combined|content|adaptive|threshold]
//                      [--threshold <n>] [--min-bpm <n>] [--max-bpm <n>] [--cache <dir> | --no-cache]
//
// Without --shots/--beats both run. Results are cached per source fingerprint in the analysis cache (the same one
// the editor and the analysis.* commands use).

#include <wabcut/command/ExecutionContext.h>

#include <filesystem>
#include <memory>

/// The analysis services for a project (decoders + the user's analysis cache); wabcut-cli's cmd and mcp serve the
/// analysis.* commands with it.
std::shared_ptr<wabcut::command::CommandServices> analysisServicesFor(const std::filesystem::path& project);

int runAnalyze(int argc, char** argv);
