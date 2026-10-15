#pragma once

// WavWriter: a PCM WAV written while it is recorded (A.8, voice-over takes), crash-safe.
//
// The file is a canonical RIFF/WAVE: "fmt " (PCM, 16 or 24 bits, or 32-bit float) then "data". Samples are appended as
// they arrive; every `syncFrames` the RIFF and data sizes in the header are rewritten to cover what was written and
// the stream is flushed, so after a crash the file is a valid WAV of everything up to the last sync, and
// recoverWav() restores the rest from the file's length (whole frames; a frame cut in half by the crash is dropped).
//
// The journal. create() also writes `<file>.journal` (wavJournalPath): it marks the file as a take this writer made,
// the only kind recoverWav() changes (a WAV from elsewhere, with chunks after "data", is never rewritten), and keeps the
// creator's note (the recorder's provenance: what the take was for). finish() leaves it; the take's owner removes it
// with the file, or when the take needs no recovery any more (removeWavJournal).
//
// Not RT-safe (file I/O): the recorder's writer thread owns it.

#include <wabcut/model/Error.h>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::audio {

struct WavFormat {
    int sampleRate = 48000;
    int channels = 1;
    int bitsPerSample = 24; ///< 16 or 24 (PCM), or 32 (IEEE float)
    bool operator==(const WavFormat&) const = default;
};

class WavWriter {
public:
    /// Creates (or truncates) `file`, writes the header and the file's journal, which keeps `journalNote` verbatim.
    /// Errors: InvalidArgument (format), IoError.
    static std::expected<std::unique_ptr<WavWriter>, Error> create(const std::filesystem::path& file, WavFormat format,
                                                                   std::int64_t syncFrames = 24000,
                                                                   std::string_view journalNote = {});
    ~WavWriter(); ///< finish()es if it was not.

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    /// Appends whole frames of interleaved float samples (clamped to [-1, 1], rounded to the bit depth). IoError.
    std::expected<void, Error> write(std::span<const float> interleaved);
    /// Writes the final sizes and closes the file. Idempotent.
    std::expected<void, Error> finish();

    std::int64_t frames() const noexcept { return m_frames; }
    const WavFormat& format() const noexcept { return m_format; }
    const std::filesystem::path& path() const noexcept { return m_path; }

private:
    WavWriter(std::FILE* file, std::filesystem::path path, WavFormat format, std::int64_t syncFrames);
    std::expected<void, Error> writeSizes();

    std::FILE* m_file = nullptr;
    std::filesystem::path m_path;
    WavFormat m_format;
    std::int64_t m_syncFrames;
    std::int64_t m_frames = 0;
    std::int64_t m_unsynced = 0;
};

/// What recoverWav() (or inspectWav()) found.
struct RecoveredWav {
    WavFormat format;
    std::int64_t frames = 0; ///< whole frames in the file now (inspectWav: once repaired)
    bool repaired = false;   ///< the header did not cover the data (a crash while recording) and was rewritten
    bool needsRepair = false; ///< the header did not cover the data (inspectWav: recoverWav() would rewrite it)
    std::int64_t droppedBytes = 0; ///< a partial frame cut off at the end
};

/// Reads a canonical PCM / float WAV and, when it is a take this writer made (its journal is there, of the same
/// format), makes its header match its length: the data size from the file size, rounded down to whole frames (the tail
/// of a partial frame is truncated). A file whose header already covers it is reported as it is. Any other file is left
/// untouched. Errors: NotFound, IoError, InvalidArgument (not such a WAV; its header does not cover it and this writer
/// did not make it; larger than a WAV header can describe, 4 GiB).
std::expected<RecoveredWav, Error> recoverWav(const std::filesystem::path& file);
/// What recoverWav() would do, read only: the same checks and errors, the file never opened for writing (listing what
/// could be recovered; recoverWav() only for the take the user picks).
std::expected<RecoveredWav, Error> inspectWav(const std::filesystem::path& file);

/// `<file>.journal`: the journal create() writes next to `file`.
std::filesystem::path wavJournalPath(const std::filesystem::path& file);

struct WavJournal {
    WavFormat format; ///< the format create() wrote
    std::string note; ///< create()'s journalNote
};

/// The journal of `file`; nullopt when it has none (or one this writer did not write).
std::optional<WavJournal> readWavJournal(const std::filesystem::path& file);
/// Removes the journal of `file` (nothing when there is none).
void removeWavJournal(const std::filesystem::path& file);

} // namespace wabcut::audio
