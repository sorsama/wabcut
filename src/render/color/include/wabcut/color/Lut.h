#pragma once

// Colour look-up tables: the .cube format (Adobe Cube LUT Specification 1.0, plus DaVinci Resolve's shaper form) and
// the CPU reference of how the grade applies a LUT (docs/color.md, "LUTs").
//
// A Lut holds an optional 1D table (a per-channel curve; in Resolve's files a shaper in front of the 3D table) and an
// optional 3D table (a lattice of `size`^3 RGB points, red fastest). Inputs are mapped from the table's input range to
// [0, 1] and clamped there, so values outside the domain take the edge entries, as the specification says.
//
// Parsing is defensive (files come from anywhere): CRLF and LF line ends, a UTF-8 byte-order mark, '#' comments,
// blank lines, keywords in any order before the data, TITLE in quotes. It rejects, with the line number:
//   * a 3D size above kMaxLut3dSize (65) or a 1D size above kMaxLut1dSize (65536, the specification's limit);
//   * a wrong number of data lines, a data line without exactly three numbers, non-finite numbers;
//   * a domain whose minimum is not below its maximum, keywords after the data, a repeated keyword;
//   * a file larger than kMaxCubeBytes.
// Unknown keywords before the data are skipped (other tools write their own). Memory is bounded by the size limits:
// at most 65^3 + 65536 RGB entries are ever allocated.

#include <wabcut/model/Error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::color {

using Rgb = std::array<double, 3>;

inline constexpr int kMaxLut3dSize = 65;
inline constexpr int kMaxLut1dSize = 65536;
inline constexpr std::size_t kMaxCubeBytes = 64u * 1024u * 1024u;

/// How a 3D table is sampled between lattice points.
enum class LutInterpolation : std::uint8_t {
    Tetrahedral, ///< the default: exact on the grey axis, no hue shifts between lattice points
    Trilinear,   ///< for comparison
};

struct Lut {
    std::string title;
    /// 1D table: `size1d` RGB entries (0 = none). Its input range is domain1dMin..domain1dMax per channel.
    int size1d = 0;
    std::vector<float> table1d; ///< size1d x 3, entry-major
    Rgb domain1dMin{0, 0, 0};
    Rgb domain1dMax{1, 1, 1};
    /// 3D table: `size3d`^3 RGB entries, red fastest (index = r + size*g + size*size*b); 0 = none.
    int size3d = 0;
    std::vector<float> table3d; ///< size3d^3 x 3
    Rgb domain3dMin{0, 0, 0};
    Rgb domain3dMax{1, 1, 1};

    bool has1d() const { return size1d >= 2; }
    bool has3d() const { return size3d >= 2; }
    bool empty() const { return !has1d() && !has3d(); }
    /// The 1D entry `i` (clamped) as RGB.
    Rgb entry1d(int i) const;
    /// The 3D lattice point (r, g, b) (clamped) as RGB.
    Rgb entry3d(int r, int g, int b) const;
};

/// Parses .cube text. A file must have a 1D or a 3D table (or both, Resolve's shaper form: 1D first).
std::expected<Lut, Error> parseCube(std::string_view text);
/// Reads and parses a .cube file (at most kMaxCubeBytes).
std::expected<Lut, Error> loadCube(const std::filesystem::path& file);
/// .cube text of a LUT (TITLE, domains, sizes, data with 9 significant digits); parseCube reads it back exactly.
std::string writeCube(const Lut& lut);

/// The LUT applied to one RGB value: the 1D table, then the 3D table, each mapping its input range to [0, 1]
/// (clamped) first. Values are display-referred code values in the grade (docs/color.md).
Rgb applyLut(const Lut& lut, const Rgb& in, LutInterpolation interpolation = LutInterpolation::Tetrahedral);
/// Only the 1D table (identity without one).
Rgb apply1d(const Lut& lut, const Rgb& in);
/// Only the 3D table (identity without one).
Rgb apply3d(const Lut& lut, const Rgb& in, LutInterpolation interpolation = LutInterpolation::Tetrahedral);

/// A 3D identity lattice of `size` points per side (tests, generated looks).
Lut identityLut3d(int size);

/// The 1D table resampled to `entries` points over its domain (the GPU stores at most this many per row): exact when
/// `entries` equals size1d, linear interpolation of the original otherwise.
std::vector<float> resampled1d(const Lut& lut, int entries);

} // namespace wabcut::color
