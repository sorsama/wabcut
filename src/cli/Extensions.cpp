#include "Extensions.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/script/Headless.h>
#include <wabcut/script/Manager.h>
#include <wabcut/script/Storage.h>

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
namespace sc = wabcut::script;
using sc::Json;

int usage() {
    std::fprintf(stderr,
                 "usage: wabcut-cli ext run <extension> <command> <project.wabcut> [--grant <capabilities> | "
                 "--grant-requested]\n"
                 "                          [--trust] [--args '<json>'] [--answers '<json array>'] [--save] "
                 "[--timeout <s>]\n"
                 "       wabcut-cli ext validate <folder>\n"
                 "       wabcut-cli ext list [<project.wabcut>]\n"
                 "<extension> is an extension folder or the id of an installed one. Capabilities: Read, EditSequence,\n"
                 "EditProject, MediaAnalysis (comma-separated, within what the manifest asks for). Without --grant the\n"
                 "grants given in Wabcut apply. A per-project extension also needs your trust (in Wabcut, or --trust).\n"
                 "Nothing is saved without --save. WABCUT_DATA_DIR overrides where extensions and grants live.\n");
    return 2;
}

int fail(int code, const std::string& message) {
    const char* kind = code == 2 ? "usage" : "refused";
    std::printf("%s\n", Json{{"ok", false}, {"error", {{"kind", kind}, {"message", message}}}}.dump(2).c_str());
    std::fprintf(stderr, "wabcut-cli ext: %s\n", message.c_str());
    return code;
}

fs::path dataDir() {
    if (const char* env = std::getenv("WABCUT_DATA_DIR"); env != nullptr && *env != '\0') {
        return fs::path(env);
    }
    QCoreApplication::setOrganizationName(QStringLiteral("Wabcut"));
    QCoreApplication::setApplicationName(QStringLiteral("Wabcut"));
    return fs::path(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdWString());
}

bool isInside(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(path, ec);
    const fs::path r = fs::weakly_canonical(root, ec);
    auto pi = p.begin();
    for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi) {
        if (pi == p.end() || *pi != *ri) {
            return false;
        }
    }
    return true;
}

std::expected<sc::ExtensionBundle, wabcut::Error> resolve(const std::string& name, const fs::path& project,
                                                          const sc::ExtensionPaths& paths,
                                                          const std::optional<sc::Version>& version) {
    std::error_code ec;
    if (fs::is_directory(fs::path(name), ec)) {
        const bool inProject = !project.empty() && isInside(fs::path(name), sc::projectExtensionsRoot(project));
        return sc::loadExtension(fs::path(name), inProject ? sc::ExtensionScope::Project : sc::ExtensionScope::User,
                                 version);
    }
    if (!sc::isValidExtensionId(name)) {
        return std::unexpected(wabcut::Error{wabcut::ErrorCode::NotFound, "no extension folder or id " + name});
    }
    if (fs::is_directory(paths.userExtensions / name, ec)) {
        return sc::loadExtension(paths.userExtensions / name, sc::ExtensionScope::User, version);
    }
    if (!project.empty() && fs::is_directory(sc::projectExtensionsRoot(project) / name, ec)) {
        return sc::loadExtension(sc::projectExtensionsRoot(project) / name, sc::ExtensionScope::Project, version);
    }
    return std::unexpected(wabcut::Error{wabcut::ErrorCode::NotFound, "no installed extension " + name});
}

Json capabilitiesJson(wabcut::command::CapabilitySet set) {
    Json out = Json::array();
    for (const auto c : {wabcut::command::Capability::Read, wabcut::command::Capability::EditSequence,
                         wabcut::command::Capability::EditProject, wabcut::command::Capability::MediaAnalysis}) {
        if (set.has(c)) {
            out.push_back(wabcut::command::toString(c));
        }
    }
    return out;
}

int runRun(int argc, char** argv, const char* version, const ExtensionServicesFor& servicesFor) {
    if (argc < 6) {
        return usage();
    }
    const std::string extension = argv[3];
    const std::string command = argv[4];
    const fs::path project = argv[5];
    std::optional<std::string> grant;
    bool grantRequested = false;
    bool trust = false;
    bool save = false;
    Json args = Json::object();
    Json answers = Json::array();
    int timeoutSeconds = 120;
    for (int i = 6; i < argc; ++i) {
        const std::string_view a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--grant" && hasValue) {
            grant = argv[++i];
        } else if (a == "--grant-requested") {
            grantRequested = true;
        } else if (a == "--trust") {
            trust = true;
        } else if (a == "--save") {
            save = true;
        } else if (a == "--args" && hasValue) {
            args = Json::parse(argv[++i], nullptr, false);
            if (!args.is_object()) {
                return fail(2, "--args must be a JSON object");
            }
        } else if (a == "--answers" && hasValue) {
            answers = Json::parse(argv[++i], nullptr, false);
            if (!answers.is_array()) {
                return fail(2, "--answers must be a JSON array (an object of field values, or null, per prompt)");
            }
        } else if (a == "--timeout" && hasValue) {
            const std::string_view v = argv[++i];
            const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), timeoutSeconds);
            if (ec != std::errc{} || end != v.data() + v.size() || timeoutSeconds < 1 || timeoutSeconds > 3600) {
                return fail(2, "--timeout takes 1 to 3600 seconds");
            }
        } else {
            return usage();
        }
    }
    if (grant && grantRequested) {
        return fail(2, "--grant and --grant-requested exclude each other");
    }

    const auto running = sc::parseVersion(version);
    const sc::ExtensionPaths paths = sc::extensionPathsUnder(dataDir());
    sc::PermissionStore store = sc::PermissionStore::load(paths.permissions);
    auto bundle = resolve(extension, project, paths, running);
    if (!bundle) {
        return fail(3, bundle.error().message);
    }
    const sc::Manifest manifest = bundle->manifest; // a copy: the bundle moves into the run below
    const wabcut::command::CapabilitySet allowed{manifest.requested().bits & sc::grantableCapabilities().bits};
    wabcut::command::CapabilitySet granted;
    if (grant) {
        const auto parsed = sc::parseCapabilityList(*grant);
        if (!parsed) {
            return fail(2, "--grant: unknown capability in \"" + *grant + "\"");
        }
        if ((parsed->bits & ~allowed.bits) != 0) {
            return fail(2, manifest.name + " asks for " + sc::capabilityList(allowed) + "; --grant cannot add " +
                               sc::capabilityList({parsed->bits & ~allowed.bits}));
        }
        granted = *parsed;
    } else if (grantRequested) {
        granted = allowed;
    } else if (store.isEnabled(manifest.id) && store.isReviewed(*bundle)) {
        // Only the reviewed files in their folder: a copy elsewhere claiming the id gets nothing (S5-P2-4).
        granted = store.effective(*bundle);
    } else {
        return fail(3, manifest.name + " has no grants here: enable it in Wabcut, or pass --grant <capabilities> "
                                       "(it asks for " + sc::capabilityList(allowed) + ")");
    }
    if (bundle->scope == sc::ExtensionScope::Project && !trust && !store.isTrusted(project, *bundle)) {
        return fail(3, manifest.name + " comes with the project and is not trusted: review it, then trust it in "
                                       "Wabcut or pass --trust");
    }

    auto loaded = wabcut::io::loadProject(project);
    if (!loaded) {
        return fail(3, "cannot open " + wabcut::pathToUtf8(project) + ": " + loaded.error().message);
    }
    wabcut::EditSession session(std::move(*loaded));

    sc::HeadlessRun run;
    run.bundle = std::move(*bundle);
    run.command = command;
    run.args = std::move(args);
    run.answers = std::move(answers);
    run.granted = granted;
    run.session = &session;
    run.registry = &wabcut::ai::edit::editingCommands();
    run.services = servicesFor(project);
    run.helper = paths.helper;
    // An installed extension's storage only for that installed folder: a folder elsewhere claiming its id starts empty.
    const auto sameFolder = [](const fs::path& a, const fs::path& b) {
        std::error_code ec;
        return fs::equivalent(a, b, ec);
    };
    const fs::path idFolder = wabcut::pathFromUtf8(manifest.id);
    const bool installed = sameFolder(run.bundle.folder, paths.userExtensions / idFolder) ||
                           (!project.empty() && sameFolder(run.bundle.folder, sc::projectExtensionsRoot(project) / idFolder));
    run.storageFile = installed ? sc::storageFileFor(paths.storage, manifest.id) : fs::path{};
    run.wabcutVersion = version;
    run.timeout = std::chrono::seconds(timeoutSeconds);
    const Json about{{"id", manifest.id},
                     {"name", manifest.name},
                     {"version", sc::toString(manifest.version)},
                     {"scope", sc::toString(run.bundle.scope)},
                     {"granted", capabilitiesJson(granted)}};
    auto report = sc::runHeadless(std::move(run));
    if (!report) {
        return fail(3, "cannot start the extension: " + report.error().message);
    }
    Json out = sc::toJson(*report);
    out["extension"] = about;
    out["saved"] = false;
    if (save && report->revisionAfter != report->revisionBefore) {
        if (auto saved = wabcut::io::saveProject(session.stateForSave(), project); !saved) {
            out["saveError"] = saved.error().message;
        } else {
            out["saved"] = true;
        }
    }
    std::printf("%s\n", out.dump(2, ' ', false, Json::error_handler_t::replace).c_str());
    if (report->ok()) {
        return out.contains("saveError") ? 3 : 0;
    }
    return report->stop && report->stop->state != sc::HostState::Stopped ? 3 : 1;
}

int runValidate(int argc, char** argv, const char* version) {
    if (argc != 4) {
        return usage();
    }
    auto bundle = sc::loadExtension(fs::path(argv[3]), sc::ExtensionScope::User, sc::parseVersion(version));
    if (!bundle) {
        std::printf("%s\n", Json{{"ok", false}, {"error", bundle.error().message}}.dump(2).c_str());
        return 1;
    }
    Json files = Json::array();
    for (const sc::ScriptSource& s : bundle->scripts) {
        files.push_back({{"path", s.path}, {"bytes", s.source.size()}});
    }
    std::printf("%s\n", Json{{"ok", true},
                             {"manifest", sc::toJson(bundle->manifest)},
                             {"files", files},
                             {"bytes", bundle->bytes},
                             {"contentHash", bundle->contentHash},
                             {"warnings", bundle->manifest.warnings}}
                            .dump(2)
                            .c_str());
    return 0;
}

int runList(int argc, char** argv, const char* version) {
    if (argc > 4) {
        return usage();
    }
    const fs::path project = argc == 4 ? fs::path(argv[3]) : fs::path{};
    const auto running = sc::parseVersion(version);
    const sc::ExtensionPaths paths = sc::extensionPathsUnder(dataDir());
    const sc::PermissionStore store = sc::PermissionStore::load(paths.permissions);
    Json list = Json::array();
    const auto add = [&](std::vector<sc::FoundExtension> found, sc::ExtensionScope scope) {
        for (sc::FoundExtension& f : found) {
            Json e{{"folder", wabcut::pathToUtf8(f.folder)}, {"scope", sc::toString(scope)}};
            if (!f.bundle) {
                e["status"] = "invalid";
                e["error"] = f.bundle.error().message;
            } else {
                const sc::Manifest& m = f.bundle->manifest;
                e["id"] = m.id;
                e["name"] = m.name;
                e["version"] = sc::toString(m.version);
                e["asks"] = capabilitiesJson(m.requested());
                e["granted"] = capabilitiesJson(store.effective(*f.bundle));
                e["status"] = !store.isReviewed(*f.bundle) ? "needsReview"
                              : !store.isEnabled(m.id)     ? "disabled"
                              : scope == sc::ExtensionScope::Project && !store.isTrusted(project, *f.bundle)
                                  ? "needsTrust"
                                  : "enabled";
            }
            list.push_back(std::move(e));
        }
    };
    add(sc::findExtensions(paths.userExtensions, sc::ExtensionScope::User, running), sc::ExtensionScope::User);
    if (!project.empty()) {
        add(sc::findExtensions(sc::projectExtensionsRoot(project), sc::ExtensionScope::Project, running),
            sc::ExtensionScope::Project);
    }
    std::printf("%s\n", Json{{"extensions", list}, {"folder", wabcut::pathToUtf8(paths.userExtensions)}}.dump(2).c_str());
    return 0;
}

} // namespace

int runExtensions(int argc, char** argv, const char* version, const ExtensionServicesFor& servicesFor) {
    if (argc < 3) {
        return usage();
    }
    const std::string_view verb = argv[2];
    if (verb == "run") {
        return runRun(argc, argv, version, servicesFor);
    }
    if (verb == "validate") {
        return runValidate(argc, argv, version);
    }
    if (verb == "list") {
        return runList(argc, argv, version);
    }
    return usage();
}
