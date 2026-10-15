#pragma once

// Built-in LUTs and the LUT library (docs/color.md, "Built-in looks and input LUTs").
//
// Every built-in LUT is generated here from math - no third-party LUT files are shipped - and referenced as
// "builtin:<name>" from a grade's EffectData:
//   looks (Creative > Look; self-made, display-referred Rec.709 in and out, 33^3):
//     teal-orange, warm-film, cool-night, bleach-bypass, mono-silver
//   input LUTs (Basic > Input LUT; 65^3) from the manufacturers' published transfer functions and primaries, decoding
//   the code values of a legal-range file (code = 64 + 876 v) to scene linear, converting the gamut to Rec.709 and
//   rendering it with Wabcut's own highlight roll-off (an exponential shoulder above 18 % grey, grey unchanged):
//     slog3-sgamut3cine-to-rec709  Sony S-Log3 / S-Gamut3.Cine (Sony, "Technical Summary for S-Gamut3.Cine/S-Log3")
//     vlog-vgamut-to-rec709        Panasonic V-Log / V-Gamut (Panasonic, "V-Log/V-Gamut Reference Manual")
//   Canon Log 3 and Apple Log are not included: their published constants could not be verified for this release.
//
// LutLibrary loads .cube files of project assets once per file version (path, size, modification time) and resolves
// grade references for the evaluator.

#include <wabcut/color/Grade.h>
#include <wabcut/color/Lut.h>
#include <wabcut/model/ProjectState.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wabcut::color {

struct BuiltinLut {
    std::string_view name;        ///< the part after "builtin:"
    std::string_view displayName;
    bool input = false;           ///< an input LUT (Basic) rather than a look (Creative)
    std::string_view description;
    std::string_view source;      ///< where the math comes from, and its licence
};

std::span<const BuiltinLut> builtinLuts();
/// The generated LUT for a built-in name (or "builtin:<name>"); null for an unknown name. Generated once.
std::shared_ptr<const Lut> builtinLut(std::string_view name);

inline constexpr std::string_view kBuiltinPrefix = "builtin:";

/// The scene-linear -> display rendering of the input LUTs: an exponential shoulder above 0.18 (slope 1 there),
/// approaching 1; identity below.
double highlightRolloff(double linear);

/// Sony S-Log3 code value (0..1 of the full 10-bit scale) -> scene linear (18 % grey = 0.18), and back.
double slog3ToLinear(double code);
double linearToSlog3(double linear);
/// Panasonic V-Log code value -> scene linear, and back.
double vlogToLinear(double code);
double linearToVlog(double linear);
/// Linear RGB in the gamut of the given xy primaries (D65 white) -> linear Rec.709.
Mat3 gamutToRec709(double rx, double ry, double gx, double gy, double bx, double by);

/// Loads and caches the .cube files of project assets.
class LutLibrary {
public:
    static LutLibrary& shared();

    /// The LUT of a file, loaded once per (path, size, mtime); null with `error` set when it cannot be read.
    std::shared_ptr<const Lut> load(const std::filesystem::path& file, std::string* error = nullptr);
    /// Resolves "builtin:<name>" and "asset:<id>" (an asset of `state` whose file is online) for evaluateGrade.
    LutResolver resolver(const ProjectState& state);
    void clear();

private:
    struct Entry {
        std::int64_t size = -1;
        std::int64_t mtime = 0;
        std::shared_ptr<const Lut> lut;
        std::string error;
    };
    std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace wabcut::color
