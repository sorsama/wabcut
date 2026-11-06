#pragma once

// The OpenFX video effect plug-ins this computer has (0.7, docs/plugins.md, "OpenFX video plug-ins"): scanned by the
// wabcut-ofxhost helper, one helper per plug-in binary so a plug-in that crashes or hangs while it is described only goes on
// the blocklist; cached between runs; and offered to the model as effect descriptors ("plugin.ofx.<plug-in id>",
// Effects.h's EffectDescriptorProvider) whose parameters are the plug-in's, keyframable where the plug-in allows.
//
// How an OpenFX parameter becomes effect parameters (keys are the plug-in's parameter name, suffixed for the ones with
// several values):
//   Double   name              Amount (Degrees for an angle, Factor for a scale, Pixels for a canvas position)
//   Int      name              Amount, whole values
//   Boolean  name              a switch (0 / 1), not keyframable
//   Choice   name              a menu (the option index), not keyframable
//   RGB(A)   name.r .g .b (.a) Fraction, 0..1
//   2D, 3D   name.x .y (.z)    Amount (Fraction when the plug-in's coordinates are normalised)
//   String   name              a text setting (EffectData text)
// Parameters the plug-in hides (secret), buttons, groups, pages, custom and parametric parameters are not offered.
//
// Threading: every member may be called from any thread (an internal lock); scan() takes seconds and belongs on a worker
// (startScan() runs one).

#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wabcut::ofxhost {

/// Effect ids of OpenFX plug-ins: "plugin.ofx.<plug-in identifier>".
inline constexpr std::string_view kOfxEffectPrefix = "plugin.ofx.";
/// EffectData key holding the plug-in's name for the missing-plug-in badge (the audio plug-ins' key too).
inline constexpr std::string_view kOfxNameKey = "plugin.name";

enum class OfxParamKind : std::uint8_t { Double, Int, Bool, Choice, Rgb, Rgba, Double2D, Int2D, Double3D, Int3D, String };

struct OfxParamInfo {
    std::string name;  ///< the plug-in's own parameter name (the key of single-value kinds)
    OfxParamKind kind = OfxParamKind::Double;
    std::string label; ///< what the plug-in shows
    std::string hint;
    bool animates = true;
    /// One entry per component (Double 1, RGB 3, RGBA 4, 2D 2, 3D 3): the plug-in's default and hard limits, and the range it
    /// shows (display limits).
    std::vector<double> defaults;
    std::vector<double> mins;
    std::vector<double> maxs;
    std::vector<double> displayMins;
    std::vector<double> displayMaxs;
    std::vector<std::string> options; ///< Choice: the labels
    std::string textDefault;          ///< String
    bool multiline = false;           ///< String
    bool angle = false;               ///< Double: an angle in degrees
    bool scale = false;               ///< Double: a scale factor
    bool spatial = false;             ///< Double, 2D: a position in normalised coordinates (a fraction of the picture)
    bool pixels = false;              ///< Double, 2D: a position in pixels of the picture

    int components() const;
    /// The effect parameter keys: `name`, or `name.r` ... (EffectParamInfo::key); empty for String.
    std::vector<std::string> keys() const;
};

struct OfxPluginInfo {
    std::string id;           ///< the plug-in's identifier ("net.sf.openfx.Invert")
    std::string name;
    std::string group;        ///< what the plug-in files itself under ("Color")
    std::string description;
    std::string version;
    std::string context;      ///< "filter" or "general"
    std::string depth;        ///< "float", "short" or "byte": the pixels it asked for
    std::string components;   ///< "rgba" or "rgb"
    bool singleInstance = false;
    std::filesystem::path path; ///< the .ofx binary
    std::vector<OfxParamInfo> params;
    std::string problem;      ///< why it cannot be used here, when it cannot

    std::string effectId() const { return std::string(kOfxEffectPrefix) + id; }
    const OfxParamInfo* param(std::string_view name) const;
};

/// The plug-in identifier of an effect id ("plugin.ofx.<id>" -> "<id>"), or nullopt for any other id.
std::optional<std::string> ofxPluginIdOf(std::string_view effectId);

struct OfxBlocked {
    std::filesystem::path path;
    std::string reason; ///< "crashed while being scanned (exit code ...)", "did not finish scanning within 30 s"
};

struct OfxCatalogOptions {
    /// Where the catalog is kept between runs (JSON). Empty: not kept.
    std::filesystem::path cacheFile;
    /// The helper (default: wabcut-ofxhost next to this executable).
    std::filesystem::path helper;
    /// The folders scanned; empty: the standard ones (standardFolders()) and the user's.
    std::vector<std::filesystem::path> folders;
    /// How long one plug-in binary may take to scan.
    std::chrono::milliseconds scanTimeout{30000};
};

struct OfxScanReport {
    int files = 0;   ///< plug-in binaries found
    int scanned = 0; ///< scanned now (new or changed)
    int cached = 0;  ///< unchanged since a previous scan
    int skipped = 0; ///< on the blocklist
    int plugins = 0; ///< plug-ins known after the scan
    int added = 0;   ///< plug-ins new in this scan
    std::vector<OfxBlocked> blocked;
    std::vector<std::string> errors; ///< files that are not plug-ins, or failed to load (not blocked)
    bool cancelled = false;
};

class OfxCatalog {
public:
    explicit OfxCatalog(OfxCatalogOptions options = {});
    ~OfxCatalog();
    OfxCatalog(const OfxCatalog&) = delete;
    OfxCatalog& operator=(const OfxCatalog&) = delete;

    /// OpenFX's standard folder (Windows: %COMMONPROGRAMFILES%\OFX\Plugins) and the folders of the OFX_PLUGIN_PATH
    /// environment variable (semicolon separated), those that exist.
    static std::vector<std::filesystem::path> standardFolders();
    /// The default cache file: %LOCALAPPDATA%\Wabcut\ofx-plugins.json.
    static std::filesystem::path defaultCacheFile();
    static std::filesystem::path defaultHelper();

    /// Reads the cache (plug-ins, blocklist, user folders). False when there is none or it cannot be read.
    bool load();
    /// Scans the folders: new and changed binaries through the helper, one at a time, each in its own process. Unchanged
    /// binaries keep their cached result; blocklisted ones are skipped. `rescan` scans every binary again.
    OfxScanReport scan(std::stop_token stop = {}, const std::function<void(double, std::string_view)>& progress = {},
                       bool rescan = false);
    /// Runs scan() on a worker; `done` is called on it when finished.
    void startScan(std::function<void(const OfxScanReport&)> done = {}, bool rescan = false);
    bool scanning() const;

    std::vector<OfxPluginInfo> plugins() const;
    std::optional<OfxPluginInfo> find(std::string_view effectId) const;
    std::vector<OfxBlocked> blocked() const;
    /// Takes a binary off the blocklist (it is scanned again next time).
    void unblock(const std::filesystem::path& path);
    void clearBlocklist();

    std::vector<std::filesystem::path> userFolders() const;
    void addUserFolder(const std::filesystem::path& folder);
    void removeUserFolder(const std::filesystem::path& folder);
    /// The folders a scan visits: the standard ones (or the options') then the user's.
    std::vector<std::filesystem::path> folders() const;

    /// The model's view of the plug-ins (Effects.h): installed with setSecondaryEffectDescriptorProvider().
    std::shared_ptr<const EffectDescriptorProvider> descriptors() const;
    /// Called (on the thread that changed it) whenever the plug-ins known change: a scan finished, the cache was loaded.
    void setChangedCallback(std::function<void()> callback);

    const OfxCatalogOptions& options() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
    std::jthread m_worker;
};

} // namespace wabcut::ofxhost
