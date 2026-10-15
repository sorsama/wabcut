// P.1: paths::FileSet (the originals' set ProxyManager::refresh builds once) answers exactly what sameFile() against
// each member answers, for every way two paths can name one file: the same spelling, another spelling, a hard link, a
// junction or a symlink on the way, a file that does not exist, an empty path.

#include "PathSafety.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path fileSetDir() {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / "fileset";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void touch(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

bool makeJunction(const fs::path& link, const fs::path& target) {
#ifdef _WIN32
    const std::wstring command = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >NUL 2>&1";
    return _wsystem(command.c_str()) == 0 && fs::exists(link);
#else
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#endif
}

} // namespace

TEST_CASE("FileSet answers like sameFile() against each of its files", "[editor][paths]") {
    const fs::path dir = fileSetDir();
    const fs::path a = dir / "footage" / "A001.mov";
    const fs::path b = dir / "footage" / "B002.mov";
    const fs::path missing = dir / "elsewhere" / "C003.mov"; // a member that is offline
    touch(a, "a");
    touch(b, "b");
    std::error_code ec;
    const fs::path hardLink = dir / "proxies" / "A001-link.mov";
    fs::create_directories(hardLink.parent_path());
    fs::create_hard_link(a, hardLink, ec);
    const bool haveHardLink = !ec;
    const fs::path junction = dir / "via";
    const bool haveJunction = makeJunction(junction, dir / "footage");

    const std::vector<fs::path> members{a, b, missing, fs::path{}};
    paths::FileSet set;
    for (const fs::path& m : members) {
        set.add(m);
    }

    std::vector<fs::path> questions{
        a,
        b,
        missing,
        dir / "footage" / "." / "A001.mov",
        dir / "footage" / ".." / "footage" / "B002.mov",
        dir / "FOOTAGE" / "a001.MOV", // another spelling (case-insensitive where the file system is)
        dir / "footage" / "Z999.mov", // does not exist, not a member
        dir / "proxies",               // a folder
        fs::path{},
    };
    if (haveHardLink) {
        questions.push_back(hardLink);
    } else {
        WARN("no hard link here: " << ec.message());
    }
    if (haveJunction) {
        questions.push_back(junction / "A001.mov");
    } else {
        WARN("no junction here");
    }
    const fs::path unrelated = dir / "proxies" / "other.mov";
    touch(unrelated, "other");
    questions.push_back(unrelated);

    for (const fs::path& q : questions) {
        bool expected = false;
        for (const fs::path& m : members) {
            expected = expected || paths::sameFile(q, m);
        }
        INFO("question: " << q.string());
        CHECK(set.contains(q) == expected);
    }
    if (haveHardLink) {
        CHECK(set.contains(hardLink)); // the case the identity lookup exists for
    }
    CHECK_FALSE(set.contains(unrelated));
}
