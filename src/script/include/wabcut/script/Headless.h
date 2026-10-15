#pragma once

// One extension command run without the app (`wabcut-cli ext run`, tests): starts the extension on a session, runs
// the command, answers its prompts from the given answers (or the forms' defaults), and reports what happened.

#include <wabcut/script/Host.h>

#include <chrono>
#include <expected>
#include <vector>

namespace wabcut::script {

struct HeadlessRun {
    ExtensionBundle bundle;
    std::string command; ///< a registered command's id
    Json args = Json::object();
    /// Answers to the prompts in the order they open: an object of field values, or null to cancel. Prompts past
    /// the end get their forms' default values.
    Json answers = Json::array();
    command::CapabilitySet granted;
    EditSession* session = nullptr;
    const command::CommandRegistry* registry = nullptr;
    std::shared_ptr<command::CommandServices> services;
    std::filesystem::path helper;      ///< empty: scriptHostPath()
    std::filesystem::path storageFile; ///< empty: nothing is kept
    std::string wabcutVersion;
    HostLimits limits;
    /// The whole run (loading, the command, its prompts and promises).
    std::chrono::milliseconds timeout{120'000};
};

struct HeadlessReport {
    CallResult call;
    std::vector<Notification> notifications;
    std::vector<Json> prompts; ///< {"form", "answer"} as they were answered
    std::vector<LogLine> log;
    std::optional<StopReport> stop; ///< when the extension did not stay up
    std::uint64_t revisionBefore = 0;
    std::uint64_t revisionAfter = 0;
    std::vector<std::string> undoLabels; ///< the steps the run added, oldest first

    bool ok() const { return call.ok; }
};

/// Errors: those of ExtensionHost::start, and InvalidArgument for answers that are not an array.
std::expected<HeadlessReport, Error> runHeadless(HeadlessRun run);

Json toJson(const HeadlessReport& report);

} // namespace wabcut::script
