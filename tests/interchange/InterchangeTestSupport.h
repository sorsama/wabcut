#pragma once

// Helpers for the interchange tests: fixture access, imports that must succeed and validate, and compact
// textual views of tracks so tests can assert whole timelines exactly:
//
//     CHECK(clips(seq, "V1") == Strings{"0+150 A001.mov @30", "150+150 A002.mov @100 off"});
//
// Each clip reads "<start>+<duration> <name> @<source In>[ off]" in frames of the sequence's rate (a
// value that is not a whole frame is printed as "t<ticks>"); " off" marks a disabled clip.

#include <wabcut/interchange/Import.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::test {

using Strings = std::vector<std::string>;

std::filesystem::path fixturePath(std::string_view name);
std::string readFixture(std::string_view name);
std::string readFile(const std::filesystem::path& path);

/// Imports a fixture file; REQUIREs success and validate(Strict).
ImportResult importFixture(std::string_view name, const ImportOptions& options = {});
/// Imports in-memory content; REQUIREs success and validate(Strict).
ImportResult importText(std::string_view text, Format format, const ImportOptions& options = {},
                        std::string_view sourceName = "inline");

const Sequence& sequenceAt(const ImportResult& result, std::size_t index);
/// The bins between the import bin and `bin` ("Edits/Selects"); "" for the import bin itself (X.1).
std::string binPath(const ImportResult& result, BinId bin);
const Track& track(const Sequence& sequence, std::string_view label);

/// The clips of a track ("V1", "A2", ...) in the format described above.
Strings clips(const Sequence& sequence, std::string_view label);
/// Link groups as "V1@0+A1@0" (members in track order, groups sorted).
Strings links(const Sequence& sequence);
/// Media item names of the result, in import order.
Strings mediaNames(const ImportResult& result);
const MediaItem& mediaNamed(const ImportResult& result, std::string_view name);

/// Ticks of `frames` at `rate`.
Ticks frames(std::int64_t count, Rate rate);

/// True when the report has an entry with this outcome and category whose reason contains `text`.
bool hasEntry(const ImportReport& report, ReportOutcome outcome, ReportCategory category,
              std::string_view text);
/// The entry as above (REQUIREs it exists).
const ReportEntry& entry(const ImportReport& report, ReportOutcome outcome, ReportCategory category,
                         std::string_view text);
/// Reasons of all entries with this outcome and category.
Strings reasons(const ImportReport& report, ReportOutcome outcome, ReportCategory category);

/// 1-based line of the first line of `text` containing `needle` (0 when absent).
std::int32_t lineOf(std::string_view text, std::string_view needle, std::int32_t occurrence = 1);

/// A fresh, empty directory for this test under the build's test-output folder.
std::filesystem::path scratchDir(std::string_view name);
/// Creates (or overwrites) a file with `bytes` bytes, creating parent folders.
void writeFile(const std::filesystem::path& path, std::string_view contents);

} // namespace wabcut::interchange::test
