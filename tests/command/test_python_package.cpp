// Runs the Python package's own tests (python/tests: unit tests against a fake Wabcut, end-to-end tests against the real
// `wabcut-cli mcp`) with a Python found at run time. Skipped, with a message, when there is none; CI has one.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#else
#include <sys/wait.h>
#endif

namespace {

std::string envValue(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

/// Runs a shell command line and returns its exit code (-1 when it did not exit normally).
int shell(const std::string& line) {
#ifdef _WIN32
    const int code = std::system(("\"" + line + "\"").c_str()); // cmd /c strips one pair of outer quotes
    return code;
#else
    const int status = std::system(line.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

/// A Python 3.11 or newer: $WABCUT_TEST_PYTHON, else python3 / python / py -3 on the PATH.
std::optional<std::string> findPython() {
    std::vector<std::string> candidates;
    // $WABCUT_TEST_PYTHON goes through the shell: only a plain path to an existing file is taken (no quotes or shell
    // operators), anything else is ignored.
    if (const std::string given = envValue("WABCUT_TEST_PYTHON"); !given.empty() &&
        given.find_first_of("\"'`$&|;<>^%!\r\n") == std::string::npos && std::filesystem::is_regular_file(given)) {
        candidates.push_back(quoted(given));
    }
#ifdef _WIN32
    candidates.insert(candidates.end(), {"py -3", "python", "python3"});
    const char* nothing = "nul";
#else
    candidates.insert(candidates.end(), {"python3", "python"});
    const char* nothing = "/dev/null";
#endif
    for (const std::string& candidate : candidates) {
        const std::string check = candidate + " -c \"import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)\" > " +
                                  nothing + " 2>&1";
        if (shell(check) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("the Python package's own tests pass against wabcut-cli", "[command][python][stdio]") {
    const auto python = findPython();
    if (!python) {
        SKIP("no Python 3.11 or newer was found (put python3 on the PATH or set WABCUT_TEST_PYTHON to its full path): the "
             "Python package tests did not run");
    }
    const std::filesystem::path packageRoot = std::filesystem::path(WABCUT_PYTHON_PACKAGE_DIR).parent_path();
    REQUIRE(std::filesystem::exists(packageRoot / "tests"));
    const auto outputDir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "command" / "python";
    std::filesystem::create_directories(outputDir);
    const auto log = outputDir / "unittest.log";

    setEnv("WABCUT_CLI", WABCUT_CLI_PATH);
    setEnv("PYTHONDONTWRITEBYTECODE", "1"); // the source tree stays clean
    setEnv("PYTHONIOENCODING", "utf-8");
#ifdef _WIN32
    const std::string cd = "cd /d " + quoted(packageRoot.string()) + " && ";
#else
    const std::string cd = "cd " + quoted(packageRoot.string()) + " && ";
#endif
    const int code = shell(cd + *python + " -X utf8 -m unittest discover -s tests > " + quoted(log.string()) + " 2>&1");

    std::ifstream in(log, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const std::string output = text.str();
    INFO("the Python test output (" << log.string() << "):\n" << output.substr(output.size() > 6000 ? output.size() - 6000 : 0));
    CHECK(code == 0);
    // unittest ends with "OK" (or "OK (skipped=n)") only when every test passed; a run that executed nothing is a failure.
    CHECK(output.find("\nOK") != std::string::npos);
    CHECK(output.find("Ran 0 tests") == std::string::npos);
}
