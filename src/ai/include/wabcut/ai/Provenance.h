#pragma once
#include <wabcut/ai/Chat.h>
#include <wabcut/model/ProjectState.h>
#include <filesystem>
namespace wabcut::ai {
// Unsigned C2PA-compatible assertion JSON, NOT a signed credential. Times use exact timeline ticks.
Json provenanceReport(const ProjectState&, SequenceId, std::optional<TimeRange> exportRange = {});
std::string provenanceXmp(const GeneratedProvenance&);
std::expected<void, Error> writeProvenanceSidecars(const std::filesystem::path& asset, const GeneratedProvenance&);
std::expected<void, Error> writeExportProvenance(const std::filesystem::path& output, const ProjectState&, SequenceId);
}
