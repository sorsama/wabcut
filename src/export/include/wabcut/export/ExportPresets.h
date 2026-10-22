#pragma once

// 0.2 (Premiere parity): export settings as JSON, the override keys every front end shares (the Export workspace, Quick
// Export, the export.* commands, wabcut-cli render), and user presets - Premiere's Save Preset, Import Preset, Delete
// and Preset Manager - as Wabcut's own JSON files in a folder (the app: "Export Presets" in the user's config folder).
//
// Preset files and settings JSON come from anywhere (shared presets, commands): every value is type-checked and
// bounded, unknown keys are ignored (a newer Wabcut's preset still opens), and the result must pass validate(). A file
// is at most kMaxPresetFileBytes.

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/Error.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::exporter {

using Json = nlohmann::json;

/// Every field of `settings` (the caption style excepted: exports use the caption track's own). The format of a preset
/// file's "settings" and of the export commands' "settings".
Json settingsToJson(const ExportSettings& settings);
/// Settings from JSON: missing keys keep ExportSettings{}'s defaults, unknown keys are ignored, wrong types and
/// out-of-range values are refused, then validate(). Errors: InvalidArgument ("<key path>: <problem>").
std::expected<ExportSettings, Error> settingsFromJson(const Json& json);

/// Applies override keys to `settings` (null values are ignored). The keys of ExportController.queueExport and the
/// export commands: container, videoCodec, video, audio, encoder, quality, bitrateMbps, maxBitrateMbps, frameRate,
/// width, height, scaleMode, range, customRange {start, end} (ticks), captions, audioCodec, audioChannels, sampleRate,
/// audioBitrateKbps, dynamicRange; 0.2: pixelFormat, alpha (Include Alpha Channel), proresProfile, dnxhrProfile,
/// exportAsSequence, subtitles (bool), effects / metadata / render (objects in settingsToJson's format, replacing those
/// sections). A container or codec change also moves the fields that depend on it (pixel format, audio codec). Does not
/// validate. Errors: InvalidArgument ("Unsupported export option <key> = <value>").
std::expected<void, Error> applyOverrides(ExportSettings& settings, const Json& overrides);

/// "proxy", "lt", "standard", "hq", "4444", "4444xq".
std::string_view proresProfileName(ProResProfile profile);
/// "lb", "sq", "hq", "hqx", "444", "dnxhd-lb", "dnxhd-sq", "dnxhd-hq", "dnxhd-hqx".
std::string_view dnxhrProfileName(DnxhrProfile profile);

inline constexpr std::size_t kMaxPresetFileBytes = 1024 * 1024;
inline constexpr std::size_t kMaxPresets = 1000;
inline constexpr std::string_view kPresetFileFormat = "wabcut-export-preset";

/// A preset file's text: {"format": "wabcut-export-preset", "version": 1, "id", "name", "description", "settings"}.
std::string presetToJson(const Preset& preset);
/// Reads a preset file's text (untrusted). Errors: InvalidArgument / ParseError.
std::expected<Preset, Error> presetFromJson(std::string_view text);

/// "user-" + the name in lowercase letters, digits and dashes (at most 64), e.g. "My YouTube" -> "user-my-youtube".
std::string userPresetId(std::string_view name);

/// The user's presets: one "<id>.json" per preset in `folder` (created on the first save). Thread-safe.
class PresetStore {
public:
    explicit PresetStore(std::filesystem::path folder);

    const std::filesystem::path& folder() const { return m_folder; }

    /// The presets in the folder, sorted by name; files that do not read are left out (problems() says why).
    std::vector<Preset> list() const;
    std::vector<std::string> problems() const;
    std::optional<Preset> find(std::string_view id) const;

    /// Save Preset: stores `settings` under `name` (id = userPresetId(name)); an existing preset of that id is
    /// replaced only with `overwrite`. Errors: InvalidArgument (empty / too long name, invalid settings, the id exists),
    /// IoError.
    std::expected<Preset, Error> save(std::string_view name, std::string_view description, ExportSettings settings,
                                      bool overwrite);
    /// Import Preset: reads a preset file from anywhere and stores it (under its name's id). Errors: as save, NotFound,
    /// ParseError.
    std::expected<Preset, Error> importFile(const std::filesystem::path& file, bool overwrite);
    /// Export Preset: writes the preset `id` (a user's or a built-in one) to `file`.
    std::expected<void, Error> exportFile(std::string_view id, const std::filesystem::path& file) const;
    /// Delete: removes the user preset `id`. Errors: NotFound, IoError.
    std::expected<void, Error> remove(std::string_view id);

    /// A built-in preset or one of this store's.
    std::optional<Preset> findAny(std::string_view id) const;

private:
    std::filesystem::path fileFor(std::string_view id) const;
    std::filesystem::path m_folder;
    mutable std::mutex m_mutex;
    mutable std::vector<std::string> m_problems;
};

} // namespace wabcut::exporter
