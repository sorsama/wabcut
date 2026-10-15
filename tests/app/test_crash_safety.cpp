// I.7 crash safety (docs/release.md "Crash reports"): a deliberate crash in a helper process that installs the handler
// the way main() does leaves a report (and on Windows a minidump) in the folder, with the log redacted - no paths, keys,
// tokens, e-mail addresses or secret environment values - and nothing uploaded; the next start's CrashNotice finds it,
// says what happened, copies a diagnostic summary with only the version, system, GPU, driver and backends, and stops
// asking once dismissed. An exit without a clean shutdown is noticed too; a clean exit is not. Then the notice in the
// real window (CrashNoticeDialog, Main.qml) with its buttons.

#include "ShellFixture.h"

#include "AiController.h"
#include "CrashHandler.h"
#include "CrashNotice.h"
#include "CrashReports.h"

#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickItem>

#include <nlohmann/json.hpp>

#include <wabcut/ai/Redact.h>

#include <filesystem>
#include <functional>
#include <fstream>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

fs::path freshFolder(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "crash" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

/// Runs the helper; returns its exit code (non-zero after a crash).
int runHelper(const fs::path& folder, const char* mode) {
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("WABCUT_TEST_API_KEY"), QStringLiteral("supersecretvalue123"));
    process.setProcessEnvironment(env);
    process.start(QStringLiteral(WABCUT_CRASH_HELPER),
                  {QString::fromStdU16String(folder.u16string()), QString::fromLatin1(mode)});
    REQUIRE(process.waitForFinished(120'000));
    INFO("stderr: " << process.readAllStandardError().toStdString());
    return process.exitCode();
}

std::vector<fs::path> filesWith(const fs::path& folder, const std::string& extension) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        if (e.path().extension() == extension) {
            out.push_back(e.path());
        }
    }
    return out;
}

/// A control of a dialog (a Popup): its QObject children, else the visual tree under its content item (Repeater
/// delegates are only there).
QObject* inDialog(QObject* dialog, const char* name) {
    const QString wanted = QString::fromLatin1(name);
    if (QObject* found = dialog->findChild<QObject*>(wanted)) {
        return found;
    }
    const std::function<QObject*(QQuickItem*)> walk = [&](QQuickItem* item) -> QObject* {
        if (!item) {
            return nullptr;
        }
        if (item->objectName() == wanted) {
            return item;
        }
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = walk(child)) {
                return found;
            }
        }
        return nullptr;
    };
    return walk(dialog->property("contentItem").value<QQuickItem*>());
}

nlohmann::json readJson(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return nlohmann::json::parse(in);
}

} // namespace

TEST_CASE("Crash safety: a crash leaves a redacted local report and a dump; the next start notices it once",
          "[app][crash]") {
    struct Mode {
        const char* name;
        const char* what; ///< part of the report's "what"
    };
    const Mode modes[] = {{"segv", "access violation"},
                          {"abort", "abort"},
                          {"throw", "an exception nobody caught"},
                          {"stack", "stack overflow"}};
    for (const Mode& mode : modes) {
        DYNAMIC_SECTION(mode.name) {
            const fs::path folder = freshFolder(mode.name);
            CHECK(runHelper(folder, mode.name) != 0);

            const auto reports = filesWith(folder, ".json");
            REQUIRE(reports.size() == 1);
            const nlohmann::json report = readJson(reports.front());
            CHECK(report["version"] == "9.9.9 (test)");
            CHECK(report["what"].get<std::string>().find(mode.what) != std::string::npos);
            CHECK(report["diagnostics"]["gpu"] == "Test GPU 3000");
#ifdef _WIN32
            // A minidump: the "MDMP" signature, stacks and modules (a few KB to a few MB).
            REQUIRE(report["dump"].is_string());
            const fs::path dump = folder / report["dump"].get<std::string>();
            REQUIRE(fs::exists(dump));
            CHECK(fs::file_size(dump) > 1024);
            std::ifstream in(dump, std::ios::binary);
            char signature[4] = {};
            in.read(signature, 4);
            CHECK(std::string(signature, 4) == "MDMP");
#endif
            // The log is in the report, redacted.
            std::string log;
            for (const auto& line : report["log"]) {
                log += line.get<std::string>() + "\n";
            }
            INFO(log);
            CHECK(log.find("plain line 42") != std::string::npos);
            for (const char* secret : {"alice", "Wedding", "interview", "sk-test", "abc.def.ghi", "example.com",
                                       "supersecretvalue123", "Videos"}) {
                INFO("leaked: " << secret);
                CHECK(log.find(secret) == std::string::npos);
            }
            CHECK(log.find("<path>.mp4") != std::string::npos);
            CHECK(log.find("<path>.mov") != std::string::npos);
            CHECK(log.find("<redacted>") != std::string::npos);
            CHECK(log.find("<email>") != std::string::npos);

            // The next start.
            CrashNotice notice;
            notice.setFolder(folder);
            REQUIRE(notice.pending());
            CHECK(notice.count() == 1);
            CHECK(notice.detail().contains(QStringLiteral("crashed")));
            CHECK(notice.hasDump() == (fs::exists(folder / report.value("dump", std::string("-")))));
            const QString summary = notice.summary();
            INFO(summary.toStdString());
            for (const char* expected : {"9.9.9 (test)", "Test GPU 3000", "1.2.3.4", "D3D11", "d3d11va"}) {
                CHECK(summary.contains(QString::fromLatin1(expected)));
            }
            for (const char* absent : {"alice", "<path>", "plain line", "sk-test", "Wedding"}) {
                CHECK_FALSE(summary.contains(QString::fromLatin1(absent)));
            }
            // The Windows clipboard is shared with every process on the desktop (other test runs too): while one holds
            // it, a copy fails, so try a few times.
            bool copied = false;
            for (int attempt = 0; attempt < 20 && !copied; ++attempt) {
                notice.copyDiagnosticSummary();
                copied = QGuiApplication::clipboard()->text() == summary;
                if (!copied) {
                    spin(100);
                }
            }
            CHECK(copied);
            notice.dismiss();
            CHECK_FALSE(notice.pending());
            CrashNotice later;
            later.setFolder(folder);
            CHECK_FALSE(later.pending()); // once
            CHECK(fs::exists(reports.front())); // the report stays for the user to share or delete
        }
    }
}

TEST_CASE("Crash safety: an exit without a clean shutdown is noticed; a clean one is not", "[app][crash]") {
    const fs::path unclean = freshFolder("exit");
    CHECK(runHelper(unclean, "exit") == 0);
    CHECK(filesWith(unclean, ".json").empty()); // no crash report
    CrashNotice notice;
    notice.setFolder(unclean);
    REQUIRE(notice.pending());
    CHECK_FALSE(notice.hasDump());
    CHECK(notice.detail().contains(QStringLiteral("without saying why")));
    CHECK(notice.summary().contains(QStringLiteral("Test GPU 3000")));
    notice.dismiss();
    CrashNotice again;
    again.setFolder(unclean);
    CHECK_FALSE(again.pending());

    const fs::path clean = freshFolder("clean");
    CHECK(runHelper(clean, "clean") == 0);
    CrashNotice none;
    none.setFolder(clean);
    CHECK_FALSE(none.pending());
}

TEST_CASE("Crash safety: the next start keeps the newest ten reports with their dumps", "[app][crash]") {
    const fs::path folder = freshFolder("prune");
    fs::create_directories(folder);
    for (int i = 0; i < 12; ++i) {
        const std::string stem = "wabcut-20261012T1000" + std::to_string(10 + i) + "Z-" + std::to_string(100 + i);
        std::ofstream(folder / (stem + ".json")) << R"({"what": "test", "time": "2026-10-12T10:00:00Z"})";
        std::ofstream(folder / (stem + ".dmp")) << "dump";
    }
    std::ofstream(folder / "notes.txt") << "the user's own file";
    CrashNotice notice;
    notice.setFolder(folder);
    CHECK(filesWith(folder, ".json").size() == 10);
    CHECK(filesWith(folder, ".dmp").size() == 10);
    CHECK_FALSE(fs::exists(folder / "wabcut-20261012T100010Z-100.json")); // the two oldest went
    CHECK_FALSE(fs::exists(folder / "wabcut-20261012T100011Z-101.dmp"));
    CHECK(fs::exists(folder / "wabcut-20261012T100021Z-111.dmp"));
    CHECK(fs::exists(folder / "notes.txt"));
}

TEST_CASE("Crash safety: redaction keeps what helps and drops what identifies", "[app][crash]") {
    using crash::redact;
    CHECK(redact("loaded C:\\Users\\bob\\My Projects\\Film.wabcut ok") == "loaded <path>.wabcut ok");
    CHECK(redact("opened C:\\Users\\alice\\Videos\\Wedding Day.mp4 for the timeline") ==
          "opened <path>.mp4 for the timeline");
    CHECK(redact("relinked /Users/bob/My Movies/take 2.mov, done") == "relinked <path>.mov, done");
    CHECK(redact("\\\\nas\\share\\clips\\a.mxf missing") == "<path>.mxf missing");
    CHECK(redact("file:///Users/bob/Movies/x.mov") == "<path>.mov");
    CHECK(redact("~/footage/day1") == "<path>");
    CHECK(redact("key=abcd1234efgh&x=1") == "key=<redacted>&x=1");
    CHECK(redact("x-api-key: AIzaSyA1234567890abcdefghijklmnopqrstu") == "x-api-key: <redacted>");
    CHECK(redact("hash 0123456789abcdef0123456789abcdef0123") == "hash <redacted>");
    CHECK(redact("frame 1/2 at 25 fps, decoder d3d11va") == "frame 1/2 at 25 fps, decoder d3d11va");
    CHECK(redact(std::string(2000, 'x')).size() <= crash::kMaxLogLine);
}

TEST_CASE("Crash safety: the next start shows the notice in the window, with Recovery and the summary",
          "[app][crash][qml]") {
    const fs::path folder = freshFolder("notice");
    REQUIRE(runHelper(folder, "segv") != 0);
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* notice = shell.engine.singletonInstance<CrashNotice*>("Wabcut", "CrashNotice");
    REQUIRE(notice);
    notice->setFolder(folder);
    QObject* dialog = shell.object("crashNoticeDialog");
    REQUIRE(dialog);
    QMetaObject::invokeMethod(dialog, "show");
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    for (const char* button : {"crashCopySummary", "crashShowReports", "crashOpenRecovery", "crashDismiss"}) {
        INFO(button);
        CHECK(inDialog(dialog, button) != nullptr);
    }
    // Open Recovery: dismissed, and the Recovery dashboard opens.
    QObject* recover = inDialog(dialog, "crashOpenRecovery");
    QMetaObject::invokeMethod(recover, "clicked");
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));
    CHECK_FALSE(notice->pending());
    QObject* recovery = shell.object("recoveryDialog");
    REQUIRE(recovery);
    CHECK(waitFor([&] { return recovery->property("opened").toBool(); }));
}

// Review wave 5, S5-P2-10: the report's redaction knew key patterns and the environment, not the keys the AI key ring
// registered, and it cut a long line before redacting, so a key across the cut survived in part.
TEST_CASE("Crash safety: keys the AI key ring registered never reach a report, also where a line is cut", "[app][crash]") {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    AiController ai; // the AI platform: its key ring's secrets are redacted in the crash log from now on
    ai::Redactor::global().addSecret("Zq7-custom-key-1234");
    CHECK(crash::redact("provider said: Zq7-custom-key-1234 invalid") == "provider said: <redacted> invalid");
    // The key starts 1593 bytes in: the old cut at 1600 left "Zq7-cus", and the path before it shrank to "<path>.mov".
    const std::string line = "C:\\Users\\bob\\" + std::string(1570, 'p') + "\\clip.mov Zq7-custom-key-1234 rejected";
    const std::string cut = crash::redact(line);
    INFO(cut);
    CHECK(cut.find("Zq7") == std::string::npos);
}
