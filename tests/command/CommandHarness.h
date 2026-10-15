#pragma once

// A Timeline picture (see tests/model/TimelineFixture.h) in an EditSession with a CommandEngine on it.

#include "TimelineFixture.h"

#include <wabcut/command/Engine.h>
#include <wabcut/model/EditSession.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace wabcut::test {

using command::Json;

struct CommandHarness {
    explicit CommandHarness(Timeline timeline, command::EnginePolicy policy = {},
                            const command::CommandRegistry& registry = command::builtinCommands())
        : tl(std::move(timeline)), session(tl.state), engine(session, registry, policy) {}
    explicit CommandHarness(std::string_view picture, command::EnginePolicy policy = {},
                            const command::CommandRegistry& registry = command::builtinCommands())
        : CommandHarness(Timeline(picture), policy, registry) {}

    /// Runs a request or batch document.
    command::CommandResult run(const Json& document) { return engine.executeJson(document); }

    /// Runs a single command and requires it to succeed.
    command::CommandResult ok(std::string_view name, Json params = Json::object()) {
        auto result = run(Json{{"command", name}, {"params", std::move(params)}});
        INFO(command::toJson(result).dump());
        REQUIRE(result.ok());
        return result;
    }

    /// Runs a single command and returns its error code ("" on success).
    std::string errorOf(std::string_view name, Json params = Json::object()) {
        const auto result = run(Json{{"command", name}, {"params", std::move(params)}});
        return result.ok() ? std::string() : std::string(command::toString(result.error->code));
    }

    ProjectSnapshot state() const { return session.snapshot(); }
    ClipId clip(std::string_view ref) const { return clipAt(*session.snapshot(), tl.seq, ref); }
    const Sequence& sequence(const ProjectSnapshot& s) const { return *s->findSequence(tl.seq); }

    Timeline tl;
    EditSession session;
    command::CommandEngine engine;
};

/// "00:00:00:04" at the fixture's 25 fps.
inline std::string tc(int frames) {
    const int ff = frames % 25;
    const int ss = (frames / 25) % 60;
    const int mm = frames / (25 * 60);
    char text[16];
    std::snprintf(text, sizeof text, "00:%02d:%02d:%02d", mm, ss, ff);
    return text;
}

} // namespace wabcut::test

#define CHECK_SESSION(harness, picture) CHECK_TIMELINE(*(harness).state(), (harness).tl.seq, picture)
