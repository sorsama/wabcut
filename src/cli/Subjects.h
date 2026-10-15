#pragma once

// Subject analysis from the command line and for `cmd` / `mcp` (AI-9, docs/ai.md "Auto reframe"):
//
//   wabcut-cli subjects <media> [--device auto|gpu|cpu] [--threads <n>] [--no-cache] [--json]
//                       [--face-model <yunet.onnx> --object-model <yolox.onnx>] [--helper <wabcut-embed>]
//
// Faces (YuNet) and people (YOLOX) run in the wabcut-embed helper next to the CLI, with the detectors from the model
// store (or the files given); without them only salient regions are found. Prints what was found and the throughput
// (analysed seconds per second, samples per second), so it doubles as the perf probe of DirectML against the CPU.

#include <wabcut/command/ExecutionContext.h>

#include <filesystem>
#include <memory>

/// The reframe services for a project: subjects of its media (decoded, detected in wabcut-embed, cached). wabcut-cli's
/// cmd and mcp serve analysis.subjects, reframe.* and shorts.* with them, and (AI-13) analysis.speakers and
/// multicam.autoSwitch with those faces and the microphones' levels.
std::shared_ptr<wabcut::command::CommandServices> reframeServicesFor(const std::filesystem::path& project);

int runSubjects(int argc, char** argv);
