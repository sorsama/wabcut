#pragma once

// The command catalog the command and MCP harnesses serve: every built-in command except those that read or write
// files named in their parameters (a fuzzer must not write over the machine's files or read its devices).

#include <wabcut/command/Registry.h>

#include <array>
#include <cstdlib>
#include <string_view>

namespace wabcut::fuzz {

inline const command::CommandRegistry& safeCommands() {
    static const command::CommandRegistry registry = [] {
        constexpr std::array<std::string_view, 5> kFileCommands{"history.export", "review.exportPackage",
                                                                "review.importPackage", "project.merge",
                                                                "history.restore"};
        command::CommandRegistry r;
        for (const command::CommandSpec& spec : command::builtinCommands().all()) {
            bool files = spec.touchesFiles; // C.12's flag: cdl/lut import, plugins.scan, lottie.import (C.13), ...
            for (const std::string_view name : kFileCommands) {
                files = files || spec.name == name;
            }
            if (!files && !r.add(spec)) {
                std::abort();
            }
        }
        return r;
    }();
    return registry;
}

} // namespace wabcut::fuzz
