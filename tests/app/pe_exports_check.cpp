// Issue #10: proves that wabcut.exe itself exports the hybrid-GPU driver hints (src/app/src/main.cpp), with their
// values. NVIDIA's Optimus and AMD's PowerXpress drivers read NvOptimusEnablement and AmdPowerXpressRequestHighPerformance
// from the export table of the executable that started the process; a DLL's or a dropped static library's copy does not
// count. This reads the PE file directly (no Windows API, no DbgHelp):
//
//   wabcut_pe_exports_check <wabcut.exe> NvOptimusEnablement=1 AmdPowerXpressRequestHighPerformance=1
//
// prints each export it finds with its value and exits 0 when every named export is there with that 32-bit value.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Section {
    std::uint32_t virtualAddress = 0;
    std::uint32_t virtualSize = 0;
    std::uint32_t rawOffset = 0;
    std::uint32_t rawSize = 0;
};

struct Image {
    std::vector<unsigned char> bytes;
    std::vector<Section> sections;

    template <typename T> std::optional<T> at(std::size_t offset) const {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
            return std::nullopt;
        }
        T value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value; // PE is little-endian, as every machine this runs on
    }
    /// The file offset of a relative virtual address, if a section holds it in the file.
    std::optional<std::size_t> offsetOf(std::uint32_t rva) const {
        for (const Section& s : sections) {
            const std::uint32_t size = s.virtualSize != 0 ? s.virtualSize : s.rawSize;
            if (rva >= s.virtualAddress && rva - s.virtualAddress < size) {
                const std::uint32_t delta = rva - s.virtualAddress;
                if (delta >= s.rawSize) {
                    return std::nullopt; // uninitialised data: nothing in the file
                }
                return static_cast<std::size_t>(s.rawOffset) + delta;
            }
        }
        return std::nullopt;
    }
    std::optional<std::string> stringAt(std::uint32_t rva) const {
        const auto offset = offsetOf(rva);
        if (!offset) {
            return std::nullopt;
        }
        std::string text;
        for (std::size_t i = *offset; i < bytes.size() && bytes[i] != 0; ++i) {
            text.push_back(static_cast<char>(bytes[i]));
        }
        return text;
    }
};

int fail(const char* message) {
    std::fprintf(stderr, "pe_exports_check: %s\n", message);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return fail("usage: wabcut_pe_exports_check <exe> Name=value...");
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        return fail("cannot open the executable");
    }
    Image image;
    image.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

    const auto peOffset = image.at<std::uint32_t>(0x3C);
    if (image.bytes.size() < 0x40 || image.bytes[0] != 'M' || image.bytes[1] != 'Z' || !peOffset ||
        image.at<std::uint32_t>(*peOffset) != 0x00004550u) {
        return fail("not a PE executable");
    }
    const std::size_t coff = *peOffset + 4;
    const auto sectionCount = image.at<std::uint16_t>(coff + 2);
    const auto optionalSize = image.at<std::uint16_t>(coff + 16);
    const std::size_t optional = coff + 20;
    const auto magic = image.at<std::uint16_t>(optional);
    if (!sectionCount || !optionalSize || !magic || (*magic != 0x10B && *magic != 0x20B)) {
        return fail("unknown optional header");
    }
    // Data directory 0 (exports): at 96 in PE32, 112 in PE32+.
    const std::size_t directories = optional + (*magic == 0x20B ? 112 : 96);
    const auto exportRva = image.at<std::uint32_t>(directories);
    const std::size_t sectionTable = optional + *optionalSize;
    for (std::uint16_t i = 0; i < *sectionCount; ++i) {
        const std::size_t s = sectionTable + static_cast<std::size_t>(i) * 40;
        const auto vsize = image.at<std::uint32_t>(s + 8);
        const auto va = image.at<std::uint32_t>(s + 12);
        const auto rawSize = image.at<std::uint32_t>(s + 16);
        const auto rawOffset = image.at<std::uint32_t>(s + 20);
        if (!vsize || !va || !rawSize || !rawOffset) {
            return fail("truncated section table");
        }
        image.sections.push_back({*va, *vsize, *rawOffset, *rawSize});
    }
    if (!exportRva || *exportRva == 0) {
        return fail("the executable has no export table");
    }
    const auto dir = image.offsetOf(*exportRva);
    if (!dir) {
        return fail("the export table is outside the file");
    }
    const auto nameCount = image.at<std::uint32_t>(*dir + 24);
    const auto functionsRva = image.at<std::uint32_t>(*dir + 28);
    const auto namesRva = image.at<std::uint32_t>(*dir + 32);
    const auto ordinalsRva = image.at<std::uint32_t>(*dir + 36);
    if (!nameCount || !functionsRva || !namesRva || !ordinalsRva) {
        return fail("truncated export directory");
    }
    const auto functions = image.offsetOf(*functionsRva);
    const auto names = image.offsetOf(*namesRva);
    const auto ordinals = image.offsetOf(*ordinalsRva);
    if (!functions || !names || !ordinals) {
        return fail("export arrays outside the file");
    }

    int missing = 0;
    for (int a = 2; a < argc; ++a) {
        const std::string wanted = argv[a];
        const std::size_t eq = wanted.find('=');
        const std::string name = wanted.substr(0, eq);
        const std::uint32_t expected = eq == std::string::npos ? 1u : static_cast<std::uint32_t>(std::stoul(wanted.substr(eq + 1)));
        bool found = false;
        for (std::uint32_t i = 0; i < *nameCount && !found; ++i) {
            const auto nameRva = image.at<std::uint32_t>(*names + static_cast<std::size_t>(i) * 4);
            const auto exportName = nameRva ? image.stringAt(*nameRva) : std::nullopt;
            if (!exportName || *exportName != name) {
                continue;
            }
            found = true;
            const auto ordinal = image.at<std::uint16_t>(*ordinals + static_cast<std::size_t>(i) * 2);
            const auto dataRva = ordinal ? image.at<std::uint32_t>(*functions + static_cast<std::size_t>(*ordinal) * 4)
                                         : std::nullopt;
            const auto dataOffset = dataRva ? image.offsetOf(*dataRva) : std::nullopt;
            const auto value = dataOffset ? image.at<std::uint32_t>(*dataOffset) : std::nullopt;
            if (!value) {
                std::printf("%s: exported, but its value is not in the file\n", name.c_str());
                ++missing;
            } else {
                std::printf("%s = %u (expected %u)\n", name.c_str(), *value, expected);
                if (*value != expected) {
                    ++missing;
                }
            }
        }
        if (!found) {
            std::printf("%s: not exported\n", name.c_str());
            ++missing;
        }
    }
    return missing == 0 ? 0 : 1;
}
