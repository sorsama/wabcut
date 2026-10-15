#pragma once

// `wabcut-cli ext ...`: extensions without the app (docs/extensions.md, "Headless automation").
//   wabcut-cli ext run <extension> <command> <project.wabcut> [--grant <capabilities> | --grant-requested]
//                      [--trust] [--args '<json>'] [--answers '<json array>'] [--save] [--timeout <s>]
//   wabcut-cli ext validate <folder>
//   wabcut-cli ext list [<project.wabcut>]

#include <wabcut/command/ExecutionContext.h>

#include <filesystem>
#include <functional>
#include <memory>

using ExtensionServicesFor =
    std::function<std::shared_ptr<wabcut::command::CommandServices>(const std::filesystem::path& project)>;

int runExtensions(int argc, char** argv, const char* version, const ExtensionServicesFor& servicesFor);
