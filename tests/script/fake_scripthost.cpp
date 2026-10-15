// A misbehaving stand-in for wabcut-scripthost (tests/script/test_host_robustness.cpp): what a broken or compromised
// helper could do, chosen by the last part of the extension id it is started with
// (wabcut_fake_scripthost --extension com.test.<mode>):
//   garbage      answers the hello with a line that is not JSON
//   hang         ready; ignores every invoke and cancel (sleeps): the editor has to kill it
//   crash        ready; aborts on the first invoke
//   flood        ready; writes 30,000 log lines at once
//   nostart      never sends ready
//   exit         exits right after the hello
//   huge         ready; then one message larger than the editor reads
//   forge        ready; on invoke, asks the editor to run the command document in its args and returns the reply
//   badprompt    ready; sends a prompt the editor cannot show (it would be a script error in the real helper)
//   chatty       ready; on invoke, logs a line every 100 ms forever, never answering and ignoring cancel (S5-P2-1)
//   busy         ready; on invoke, asks the editor to run a read command over and over, never answering (S5-P2-1)
//   storageflood ready; then 10,000 storage writes of 2 KB at once (S5-P2-1)
//   model        ready; on invoke, asks the editor to run "slow.model" and returns the reply (S5-P2-1)
//   prompter     ready; a prompt while no call runs; on invoke, seven prompts one after the other (S5-P2-2)
// Pure C++: it builds in the core preset too.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using Json = nlohmann::json;

namespace {

void send(const Json& j) {
    const std::string line = j.dump() + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

void ready() {
    send({{"type", "registerCommand"}, {"id", "go"}, {"title", "Go"}});
    send({{"type", "ready"}});
}

bool next(Json& message) {
    std::string line;
    while (std::getline(std::cin, line)) {
        message = Json::parse(line, nullptr, false);
        if (message.is_object()) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string id;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--extension") {
            id = argv[i + 1];
        }
    }
    const std::string mode = id.substr(id.rfind('.') + 1);
    Json m;
    if (!next(m) || m.value("type", "") != "hello") {
        return 2;
    }
    if (mode == "garbage") {
        std::fputs("this is not a message\n", stdout);
        std::fflush(stdout);
    } else if (mode == "exit") {
        return 0;
    } else if (mode == "nostart") {
        // nothing
    } else {
        ready();
    }
    if (mode == "flood") {
        for (int i = 0; i < 30'000; ++i) {
            send({{"type", "log"}, {"level", "info"}, {"message", "line " + std::to_string(i)}});
        }
    }
    if (mode == "huge") {
        send({{"type", "log"}, {"message", std::string(3 * 1024 * 1024, 'x')}});
    }
    if (mode == "storageflood") {
        for (int i = 0; i < 10'000; ++i) {
            send({{"type", "storageSet"}, {"key", "k" + std::to_string(i % 200)}, {"value", std::string(2000, 'v')}});
        }
        send({{"type", "registerCommand"}, {"id", "flooded"}, {"title", "Flooded"}});
    }
    const Json numberForm{{"title", "How many?"},
                          {"fields", Json::array({{{"id", "n"}, {"type", "number"}, {"label", "N"}, {"value", 2}}})}};
    if (mode == "prompter") {
        send({{"type", "prompt"}, {"prompt", 100}, {"form", numberForm}});
    }
    if (mode == "badprompt") {
        send({{"type", "prompt"}, {"prompt", 1}, {"form", {{"title", "Pick"}, {"fields", Json::array({{{"id", "f"}, {"type", "file"}, {"label", "File"}}})}}}});
    }
    std::uint64_t request = 1;
    while (next(m)) {
        const std::string type = m.value("type", "");
        if (type == "shutdown") {
            return 0;
        }
        if (type == "ping") {
            send({{"type", "pong"}, {"id", m.value("id", 0)}});
        }
        if (type != "invoke") {
            continue;
        }
        if (mode == "hang") {
            std::this_thread::sleep_for(std::chrono::hours(1)); // a native hang: no watchdog, no cancel
        }
        if (mode == "crash") {
            std::abort();
        }
        if (mode == "chatty") {
            for (int i = 0;; ++i) { // never answers; the log lines are not progress on the call
                send({{"type", "log"}, {"level", "info"}, {"message", "still busy " + std::to_string(i)}});
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (mode == "busy") {
            for (;;) { // every request resets nothing past the call's hard limit
                send({{"type", "request"}, {"id", request++}, {"method", "commands.run"},
                      {"params", {{"document", {{"command", "project.summary"}}}}}});
                Json reply;
                while (next(reply) && reply.value("type", "") != "reply") {
                }
                if (!std::cin) {
                    return 0;
                }
            }
        }
        if (mode == "model") {
            send({{"type", "request"}, {"id", request++}, {"method", "commands.run"},
                  {"params", {{"document", {{"command", "slow.model"}}}}}});
            Json reply;
            while (next(reply) && reply.value("type", "") != "reply") {
            }
            send({{"type", "done"}, {"call", m["call"]}, {"result", reply}});
            continue;
        }
        if (mode == "prompter") {
            for (std::uint64_t p = 1; p <= 7; ++p) {
                send({{"type", "prompt"}, {"prompt", p}, {"form", numberForm}});
                Json answer;
                while (next(answer) && answer.value("type", "") != "promptAnswer") {
                }
            }
            send({{"type", "done"}, {"call", m["call"]}, {"result", "prompted"}});
            continue;
        }
        if (mode == "forge") {
            // A compromised helper asks for whatever it likes; the editor decides.
            const std::uint64_t requestId = request++;
            send({{"type", "request"}, {"id", requestId}, {"method", "commands.run"},
                  {"params", {{"document", m["args"].value("document", Json::object())}}}});
            Json reply;
            while (next(reply) && reply.value("type", "") != "reply") {
            }
            send({{"type", "done"}, {"call", m["call"]}, {"result", reply}});
            continue;
        }
        send({{"type", "done"}, {"call", m["call"]}, {"result", "ok"}});
    }
    return 0;
}
