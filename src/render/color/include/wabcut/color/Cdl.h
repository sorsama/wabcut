#pragma once

// ASC CDL (American Society of Cinematographers Color Decision List, v1.2) - the grade's colour wheels are exactly a
// CDL: slope = gain - lift, offset = lift + offset, power = gamma, plus saturation (docs/color.md, "Wheels").
//
// Reads .cc (one ColorCorrection), .ccc (ColorCorrectionCollection) and .cdl (ColorDecisionList) XML, with or
// without the urn:ASC:CDL namespace, SatNode or the older SATNode; writes .cc, .ccc and .cdl. Values must be finite;
// slope and power non-negative (power above 0), saturation non-negative.

#include <wabcut/color/Grade.h>
#include <wabcut/model/Error.h>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::color {

struct Cdl {
    std::string id;          ///< ColorCorrection id (optional)
    std::string description; ///< SOPNode Description (optional)
    Rgb slope{1, 1, 1};
    Rgb offset{0, 0, 0};
    Rgb power{1, 1, 1};
    double saturation = 1.0;

    bool operator==(const Cdl&) const = default;
    bool isIdentity() const;
};

enum class CdlFormat { Cc, Ccc, Cdl };

/// The format of a file name's extension (.cc, .ccc, .cdl; else Cc).
CdlFormat cdlFormatOf(const std::filesystem::path& file);

/// Every ColorCorrection in the text (in document order).
std::expected<std::vector<Cdl>, Error> parseCdl(std::string_view xml);
std::expected<std::vector<Cdl>, Error> loadCdl(const std::filesystem::path& file);
std::string writeCdl(const std::vector<Cdl>& corrections, CdlFormat format);
std::expected<void, Error> saveCdl(const std::vector<Cdl>& corrections, const std::filesystem::path& file);

/// The grade's wheels as a CDL, and a CDL as wheel parameters (lift 0, gain = slope, offset, gamma = power, sat).
Cdl cdlOf(const GradeParams& params);
void applyCdl(GradeParams& params, const Cdl& cdl);
/// The CPU reference of a CDL on a display-referred value (clamped "ASC" style, like the grade's wheels).
Rgb applyCdlToPixel(const Cdl& cdl, const Rgb& in);

} // namespace wabcut::color
