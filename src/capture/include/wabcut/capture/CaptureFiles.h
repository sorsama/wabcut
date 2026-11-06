#pragma once

// The files of a capture (0.7, docs/capture.md): the names they get in the project's Capture folder, the journal that marks
// a file as a capture still being written, and the repair of a capture the program stopped in the middle of.
//
// While recording, `<file>.capture` sits next to the file and says who writes it (process id and start time) and what the
// capture was for (the recorder's provenance note: the project and bin it goes to). A normal stop removes it. After a
// crash, a power cut or a kill, findInterruptedCaptures() finds the files whose writer is gone, and recoverCapture() copies
// the packets that reached the disk into a clean file (index, duration) in place of the broken one.

#include <wabcut/capture/CaptureTypes.h>

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::capture {

/// A file name made of what the user typed: UTF-8 kept, characters Windows forbids (< > : " / \ | ? * and controls) dropped,
/// no leading or trailing spaces or dots, never a reserved device name (CON, NUL, COM1, ...), at most 100 bytes; "Capture"
/// when nothing is left.
std::string safeFileName(std::string_view text);
/// "Capture 1", "Capture 2", ...: the first clip name no file or journal in `folder` uses.
std::string defaultClipName(const std::filesystem::path& folder);
/// `folder / safeFileName(name) + extension` ("mkv"); "name (2)" and up while a file or journal of that name exists.
std::filesystem::path captureFilePath(const std::filesystem::path& folder, std::string_view name, std::string_view extension = "mkv");
/// The file extension (without the dot) of a container the capture presets use.
std::string_view captureExtension();

/// The nominal frame rate for a rate a device reports: 23.976 / 29.97 / 59.94 / 119.88 and 24 / 25 / 30 / 50 / 60 / 120 / 15 /
/// 10 for a rate within 0.05 % of one of them (29.97 and 30 are 0.1 % apart), else the reduced ratio.
Rate snapFrameRate(Rate reported);

// ---- The journal ----------------------------------------------------------------------------------------------------

struct CaptureJournal {
    std::int64_t pid = 0;          ///< the writing process
    std::int64_t processStamp = 0; ///< its start time (Windows: the creation FILETIME), 0 when unknown
    std::string startedAt;         ///< ISO 8601 UTC, informational
    std::string note;              ///< the recorder's provenance, verbatim
};

std::filesystem::path journalPath(const std::filesystem::path& file);
/// Writes the journal of `file` for this process. IoError when it cannot be written.
std::expected<void, Error> writeJournal(const std::filesystem::path& file, const std::string& note);
std::optional<CaptureJournal> readJournal(const std::filesystem::path& file);
void removeJournal(const std::filesystem::path& file);

/// Whether process `pid` (started at `stamp`, see CaptureJournal) is running.
bool processIsRunning(std::int64_t pid, std::int64_t stamp);
std::int64_t currentProcessId();
std::int64_t processStartStamp(std::int64_t pid);

struct InterruptedCapture {
    std::filesystem::path file;
    CaptureJournal journal;
    std::uintmax_t bytes = 0;
};

/// The captures in `folder` (not its subfolders) whose journal exists, whose file exists and whose writer is gone, by name.
/// A writer is gone when its process is not running, or is this one and the file is not one of `activeFiles` (a capture of
/// this process that was ended without being finished). Links and files with more than one name are skipped.
std::vector<InterruptedCapture> findInterruptedCaptures(const std::filesystem::path& folder,
                                                        const std::vector<std::filesystem::path>& activeFiles = {});

struct RecoveredCapture {
    std::filesystem::path file; ///< the repaired capture (the interrupted file's own path)
    double seconds = 0.0;       ///< length of the picture recovered
    std::int64_t videoFrames = 0;
    std::int64_t audioPackets = 0;
};

/// Copies the packets of an interrupted capture into a clean file and puts it in place of the broken one; its journal goes.
/// Errors: NotFound (no picture reached the disk: the broken file and its journal are removed), IoError, DecoderError.
std::expected<RecoveredCapture, Error> recoverCapture(const InterruptedCapture& capture);

} // namespace wabcut::capture
