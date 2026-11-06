#pragma once

// The generated half of the `wabcut` Python package (python/wabcut, docs/python.md, 0.7): the command catalog as data
// and as typed stubs, so a script's `session.clip.addEffect(...)` is checked by an editor before it runs and validated
// again by the package and by the server when it does.
//
//   catalog.json    every command's newest version: name, title, capability, description, aliases, parameter JSON
//                   Schema, example, whether it runs an AI model or touches files. The package validates a call's
//                   parameters against it before sending anything, builds help() text from it, and resolves
//                   session.<group>.<command> from it.
//   _commands.pyi   the same catalog as Python type stubs: one class per group of commands (`session.clip`,
//                   `session.export.queue`), keyword-only parameters with the schema's types (Literal for enums,
//                   TypedDict for object parameters), and the commands' descriptions as docstrings.
//
// Pure data and text: deterministic for a registry (sorted by name), no clock, no version. `wabcut_command_docgen
// --python python/wabcut` writes them; the test "the Python package's generated files are up to date" compares.

#include <wabcut/command/Registry.h>

#include <string>
#include <vector>

namespace wabcut::command {

struct PythonFile {
    std::string path; ///< relative to the package folder ("catalog.json")
    std::string text;
};

/// The generated files of the package for `registry` (the catalog wabcut-cli serves), in a fixed order.
std::vector<PythonFile> pythonApiFiles(const CommandRegistry& registry);

/// Python's reserved words and `self`: parameter names the stubs and the package spell with a trailing underscore
/// (`from` -> `from_`).
bool isPythonReserved(const std::string& name);

} // namespace wabcut::command
