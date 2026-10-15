// A stand-in for wabcut-plughost (review S5-P2-6, test_plughost_channel.cpp). It speaks the control protocol, but before
// every reply it writes lines that are not replies to anything (what a plug-in's prints or a corrupt helper could put
// on the channel), and its scan answers carry fields of the wrong type. What it does comes from the plugin id of a
// "create" request and from the name of the file it scans:
//   --serve, create "refuse":   refuses ({"ok": false, "error": "the fake helper hosts no plugins"});
//   --serve, create "accept":   accepts; its replies to the instance's requests carry fields of the wrong type;
//   --serve, create "endless":  writes a line that never ends (more than kMaxLineBytes), then waits;
//   --scan <name>.clap:         first the malformed answer <name> stands for, then a valid empty one.

#include <wabcut/platform/DllSearch.h>
#include <wabcut/plughost/Ipc.h>
#include <wabcut/plughost/Protocol.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <io.h>

using nlohmann::json;

namespace {

void writeLine(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/// Lines that are not replies to anything the editor asked: ids of the wrong type, ids nobody waits for, values that
/// are not objects, text that is not JSON, an empty line, and the request's own id as a string.
void writeNoise(std::uint64_t requestId) {
    for (const char* line : {R"({"id":"x"})", R"({"id":null})", R"({"id":-1})", R"({"id":1.5})", R"({"id":{"n":1}})",
                             R"({"id":[1]})", R"({"id":true})", R"({"id":18446744073709551615,"ok":true})", R"([1,2,3])",
                             R"("a string")", "not json at all {", R"({"event":"stateChanged","instance":"x"})", ""}) {
        writeLine(line);
    }
    writeLine(json{{"id", std::to_string(requestId)}, {"ok", true}}.dump());
}

/// More than the editor reads as one line, without a newline: the editor must give the channel up (as a crash).
void writeEndlessLine() {
    const std::string chunk(64 * 1024, 'x');
    for (std::size_t written = 0; written <= wabcut::plughost::kMaxLineBytes + chunk.size() * 16; written += chunk.size()) {
        if (std::fwrite(chunk.data(), 1, chunk.size(), stdout) != chunk.size()) {
            return; // the editor closed the pipe (it ended this process)
        }
    }
    std::fflush(stdout);
}

int serve() {
    std::string line;
    while (std::getline(std::cin, line)) {
        const json request = json::parse(line, nullptr, false);
        if (!request.is_object()) {
            continue;
        }
        const bool hasId = request.contains("id");
        const std::uint64_t id = hasId && request.at("id").is_number_unsigned() ? request.at("id").get<std::uint64_t>() : 0;
        const std::string op = request.contains("op") && request.at("op").is_string() ? request.at("op").get<std::string>() : "";
        writeNoise(id);
        json reply{{"ok", true}};
        if (op == "hello") {
            reply["version"] = wabcut::plughost::kProtocolVersion;
            reply["pid"] = "not a number";
        } else if (op == "create") {
            const std::string mode =
                request.contains("plugin") && request.at("plugin").is_string() ? request.at("plugin").get<std::string>() : "";
            if (mode == "endless") {
                writeEndlessLine();
                continue;
            }
            if (mode == "accept") {
                reply["latency"] = "x";
                reply["editor"] = "yes"; // not a bool: read as false
            } else {
                reply = json{{"ok", false}, {"error", "the fake helper hosts no plugins"}};
            }
        } else if (op == "saveState") {
            reply["state"] = 12; // not a string: read as no state
        } else if (op == "params") {
            // One well-formed entry among entries that are not.
            reply["params"] = json::parse(R"([{"id":"x","value":"y"},{"id":3,"value":0.5},7,null,{"id":4}])");
        }
        if (hasId) {
            reply["id"] = request.at("id");
        }
        writeLine(reply.dump());
        if (op == "quit") {
            return 0;
        }
    }
    return 0;
}

/// The scan answer the file's name stands for, as a plug-in printing first would have made it the scan's answer.
int scan(const std::filesystem::path& file) {
    const std::string name = file.stem().string();
    std::string answer = R"({"ok":false,"error":"the fake helper does not know this file"})";
    if (name == "latency-string") {
        answer = R"({"ok":true,"plugins":[{"format":"clap","id":"org.fake.a","name":"A","latency":"x"}]})";
    } else if (name == "param-id-string") {
        answer = R"({"ok":true,"plugins":[{"format":"clap","id":"org.fake.b","name":"B","params":[{"id":"x"}]}]})";
    } else if (name == "name-number") {
        answer = R"({"ok":true,"plugins":[{"format":"clap","id":"org.fake.c","name":7}]})";
    } else if (name == "ok-string") {
        answer = R"({"ok":"yes","plugins":[]})";
    } else if (name == "plugins-object") {
        answer = R"({"ok":true,"plugins":{"a":1}})";
    }
    writeLine(answer);
    writeLine(R"({"ok":true,"plugins":[]})"); // what the helper itself would have answered after the plug-in's line
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    _setmode(_fileno(stdout), _O_BINARY);
    const std::vector<std::string> args(argv, argv + argc);
    if (args.size() >= 3 && args[1] == "--scan") {
        return scan(std::filesystem::path(args[2]));
    }
    if (args.size() >= 2 && args[1] == "--serve") {
        return serve();
    }
    std::fprintf(stderr, "usage: wabcut_fake_plughost --serve | --scan <plugin file>\n");
    return 1;
}
