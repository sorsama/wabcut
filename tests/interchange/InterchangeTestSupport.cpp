#include "InterchangeTestSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <map>
#include <sstream>

namespace wabcut::interchange::test {

namespace fs = std::filesystem;

fs::path fixturePath(std::string_view name) {
    const std::u8string utf8(name.begin(), name.end());
    return fs::path(WABCUT_INTERCHANGE_FIXTURES) / fs::path(utf8);
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string readFixture(std::string_view name) {
    return readFile(fixturePath(name));
}

namespace {

ImportResult requireValid(std::expected<ImportResult, Error> result) {
    if (!result) {
        FAIL("import failed: " << result.error().message);
    }
    const auto valid = validate(result->project, Validation::Strict);
    if (!valid) {
        FAIL("invalid project: " << valid.error().message);
    }
    return std::move(*result);
}

} // namespace

ImportResult importFixture(std::string_view name, const ImportOptions& options) {
    return requireValid(ImportRouter::importFile(fixturePath(name), options));
}

std::string binPath(const ImportResult& result, BinId bin) {
    std::string path;
    for (int depth = 0; bin.isValid() && bin != result.bin && depth < 64; ++depth) {
        const Bin* b = result.project.findBin(bin);
        if (!b) {
            return "?";
        }
        path = b->name + (path.empty() ? "" : "/") + path;
        bin = b->parent;
    }
    return bin == result.bin ? path : "outside";
}

ImportResult importText(std::string_view text, Format format, const ImportOptions& options,
                        std::string_view sourceName) {
    return requireValid(ImportRouter::importData(text, format, sourceName, options));
}

const Sequence& sequenceAt(const ImportResult& result, std::size_t index) {
    REQUIRE(index < result.sequences.size());
    const Sequence* seq = result.project.findSequence(result.sequences[index]);
    REQUIRE(seq);
    return *seq;
}

const Track& track(const Sequence& sequence, std::string_view label) {
    REQUIRE(label.size() >= 2);
    const auto& list = label[0] == 'V' ? sequence.videoTracks : sequence.audioTracks;
    const auto index = static_cast<std::size_t>(std::stoi(std::string(label.substr(1)))) - 1;
    REQUIRE(index < list.size());
    return *list[index];
}

namespace {

std::string inFrames(Ticks t, Rate rate) {
    const std::int64_t perFrame = rate.ticksPerUnit();
    if (t.value % perFrame == 0) {
        return std::to_string(t.value / perFrame);
    }
    return "t" + std::to_string(t.value);
}

std::string labelOf(const Sequence& sequence, TrackId id) {
    const auto pos = sequence.trackPosition(id);
    REQUIRE(pos);
    return (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1);
}

} // namespace

Strings clips(const Sequence& sequence, std::string_view label) {
    Strings out;
    for (const Clip& c : track(sequence, label).clips) {
        out.push_back(inFrames(c.start(), sequence.frameRate) + "+" +
                      inFrames(c.duration(), sequence.frameRate) + " " + c.name() + " @" +
                      inFrames(c.sourceIn, sequence.frameRate) + (c.enabled ? "" : " off"));
    }
    return out;
}

Strings links(const Sequence& sequence) {
    std::map<std::uint64_t, std::vector<std::string>> groups;
    sequence.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.linkGroup.isValid()) {
                groups[c.linkGroup.value].push_back(labelOf(sequence, t.id) + "@" +
                                                    inFrames(c.start(), sequence.frameRate));
            }
        }
    });
    Strings out;
    for (const auto& [group, members] : groups) {
        std::string joined;
        for (const auto& m : members) {
            joined += (joined.empty() ? "" : "+") + m;
        }
        out.push_back(joined);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Strings mediaNames(const ImportResult& result) {
    Strings out;
    for (const MediaId id : result.media) {
        const MediaItem* m = result.project.findMedia(id);
        REQUIRE(m);
        out.push_back(m->name);
    }
    return out;
}

const MediaItem& mediaNamed(const ImportResult& result, std::string_view name) {
    for (const MediaId id : result.media) {
        const MediaItem* m = result.project.findMedia(id);
        if (m && m->name == name) {
            return *m;
        }
    }
    FAIL("no media named " << name);
    throw; // unreachable
}

Ticks frames(std::int64_t count, Rate rate) {
    return framesToTicks(count, rate);
}

bool hasEntry(const ImportReport& report, ReportOutcome outcome, ReportCategory category,
              std::string_view text) {
    return std::any_of(report.entries().begin(), report.entries().end(), [&](const ReportEntry& e) {
        return e.outcome == outcome && e.category == category && e.reason.find(text) != std::string::npos;
    });
}

const ReportEntry& entry(const ImportReport& report, ReportOutcome outcome, ReportCategory category,
                         std::string_view text) {
    for (const ReportEntry& e : report.entries()) {
        if (e.outcome == outcome && e.category == category && e.reason.find(text) != std::string::npos) {
            return e;
        }
    }
    FAIL("no " << toString(outcome) << "/" << toString(category) << " entry containing: " << text);
    throw; // unreachable
}

Strings reasons(const ImportReport& report, ReportOutcome outcome, ReportCategory category) {
    Strings out;
    for (const ReportEntry& e : report.entries()) {
        if (e.outcome == outcome && e.category == category) {
            out.push_back(e.reason);
        }
    }
    return out;
}

std::int32_t lineOf(std::string_view text, std::string_view needle, std::int32_t occurrence) {
    std::int32_t line = 1;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const std::string_view current =
            text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (current.find(needle) != std::string_view::npos && --occurrence == 0) {
            return line;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
        ++line;
    }
    return 0;
}

fs::path scratchDir(std::string_view name) {
    const fs::path dir =
        fs::path(WABCUT_TEST_OUTPUT_DIR) / "interchange" / fs::path(std::u8string(name.begin(), name.end()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

} // namespace wabcut::interchange::test
