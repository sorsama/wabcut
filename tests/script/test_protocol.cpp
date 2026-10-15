// The scripthost protocol (C.12): the helper runs untrusted code, so every line it sends is checked for its shape and
// bounds, and prompt forms and answers are checked before anyone renders or uses them.

#include <wabcut/script/Protocol.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::script;

namespace {

std::string why(const Json& message) {
    auto parsed = parseHostMessage(message.dump());
    return parsed ? std::string() : parsed.error();
}

Json form() {
    return Json{{"title", "Add Markers"},
                {"fields", Json::array({{{"id", "every"}, {"type", "choice"}, {"label", "Mark"},
                                         {"options", Json::array({{{"value", "beat"}, {"label", "Every beat"}},
                                                                  {{"value", "bar"}, {"label", "Every bar"}}})}},
                                        {{"id", "bpm"}, {"type", "number"}, {"label", "Tempo"}, {"value", 120},
                                         {"min", 20}, {"max", 300}},
                                        {{"id", "name"}, {"type", "text"}, {"label", "Name"}, {"maxLength", 10}},
                                        {{"id", "result"}, {"type", "text"}, {"label", "Result"}, {"value", "0:00 Intro"},
                                         {"readOnly", true}, {"multiline", true}},
                                        {{"id", "all"}, {"type", "checkbox"}, {"label", "Every sequence"}}})}};
}

} // namespace

TEST_CASE("Every message type from the helper is checked for its shape", "[script][protocol]") {
    CHECK(why({{"type", "ready"}}).empty());
    CHECK(why({{"type", "request"}, {"id", 1}, {"method", "commands.run"}, {"params", {{"document", {{"command", "project.summary"}}}}}}).empty());
    CHECK(why({{"type", "request"}, {"id", 1}, {"method", "commands.list"}}).empty());
    CHECK(why({{"type", "request"}, {"id", 1}, {"method", "commands.describe"}, {"params", {{"name", "marker.add"}}}}).empty());
    CHECK(why({{"type", "done"}, {"call", 3}, {"result", {{"markers", 4}}}}).empty());
    CHECK(why({{"type", "done"}, {"call", 3}, {"error", {{"kind", "timeLimit"}, {"message", "stopped"}}}}).empty());
    CHECK(why({{"type", "notify"}, {"level", "warning"}, {"message", "No beats"}}).empty());
    CHECK(why({{"type", "toast"}, {"message", "Done"}}).empty());
    CHECK(why({{"type", "prompt"}, {"prompt", 1}, {"form", form()}}).empty());
    CHECK(why({{"type", "registerCommand"}, {"id", "addBeatMarkers"}, {"title", "Add Markers"}}).empty());
    CHECK(why({{"type", "subscribe"}, {"event", "playheadMoved"}, {"on", false}}).empty());
    CHECK(why({{"type", "storageSet"}, {"key", "k"}, {"value", Json::array({1})}}).empty());
    CHECK(why({{"type", "storageRemove"}, {"key", "k"}}).empty());
    CHECK(why({{"type", "log"}, {"level", "warn"}, {"message", "hello"}}).empty());
    CHECK(why({{"type", "pong"}, {"id", 9}}).empty());

    // Broken ones say why, and never throw (members of other types, missing members, unknown values).
    CHECK_FALSE(why({{"type", "open a file"}}).empty());
    CHECK_FALSE(why({{"type", 5}}).empty());
    CHECK_FALSE(why({{"type", "request"}, {"id", -1}, {"method", "commands.run"}}).empty());
    CHECK_FALSE(why({{"type", "request"}, {"id", 1}, {"method", "fs.readFile"}}).empty());
    CHECK_FALSE(why({{"type", "request"}, {"id", 1}, {"method", "commands.run"}, {"params", {{"document", "x"}}}}).empty());
    CHECK_FALSE(why({{"type", "request"}, {"id", std::uint64_t{1} << 60}, {"method", "commands.list"}}).empty());
    CHECK_FALSE(why({{"type", "done"}, {"call", "3"}}).empty());
    CHECK_FALSE(why({{"type", "done"}, {"call", 3}, {"error", {{"kind", "segfault"}, {"message", "x"}}}}).empty());
    CHECK_FALSE(why({{"type", "notify"}, {"level", "critical"}, {"message", "x"}}).empty());
    CHECK_FALSE(why({{"type", "notify"}, {"level", 3}, {"message", "x"}}).empty());
    CHECK_FALSE(why({{"type", "notify"}, {"message", std::string(kMaxNotifyBytes + 1, 'x')}}).empty());
    CHECK_FALSE(why({{"type", "toast"}, {"message", ""}}).empty());
    CHECK_FALSE(why({{"type", "registerCommand"}, {"id", "../x"}, {"title", "X"}}).empty());
    CHECK_FALSE(why({{"type", "subscribe"}, {"event", "keyPressed"}}).empty());
    CHECK_FALSE(why({{"type", "subscribe"}, {"event", "playheadMoved"}, {"on", "yes"}}).empty());
    CHECK_FALSE(why({{"type", "storageSet"}, {"key", "k"}}).empty());
    CHECK_FALSE(why({{"type", "prompt"}, {"prompt", 1}, {"form", {{"title", 7}}}}).empty());
    CHECK_FALSE(why({{"type", "pong"}}).empty());

    // Bounds before parsing: size and nesting; and what is not a JSON object.
    CHECK_FALSE(parseHostMessage(std::string(kMaxHostMessageBytes + 1, ' ')).has_value());
    CHECK_FALSE(parseHostMessage(std::string(1000, '[') + std::string(1000, ']')).has_value());
    CHECK_FALSE(parseHostMessage("{\"type\": \"ready\"").has_value());
    CHECK_FALSE(parseHostMessage("[\"ready\"]").has_value());
}

TEST_CASE("Text from an extension cannot pose as the editor's", "[script][protocol]") {
    // A bidirectional override and an invisible character in a notification become U+FFFD; new lines stay in a
    // notification and go from a toast.
    auto n = parseHostMessage(Json{{"type", "notify"}, {"message", "Saved\xE2\x80\xAE" "evil\nline two"}}.dump());
    REQUIRE(n.has_value());
    CHECK(n->body["message"] == "Saved\xEF\xBF\xBD" "evil\nline two");
    auto t = parseHostMessage(Json{{"type", "toast"}, {"message", "a\nb\x01" "c"}}.dump());
    REQUIRE(t.has_value());
    CHECK(t->body["message"] == "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c");
    auto r = parseHostMessage(Json{{"type", "registerCommand"}, {"id", "x"}, {"title", "Delete \xE2\x80\x8B" "all"}}.dump());
    REQUIRE(r.has_value());
    CHECK(r->body["title"] == "Delete \xEF\xBF\xBD" "all");
    // A log line is cut to its bound.
    auto l = parseHostMessage(Json{{"type", "log"}, {"message", std::string(kMaxLogBytes * 2, 'x')}}.dump());
    REQUIRE(l.has_value());
    CHECK(l->body["message"].get<std::string>().size() == kMaxLogBytes);
}

TEST_CASE("Prompt forms are normalised; and answers must fit them", "[script][protocol]") {
    auto f = validatePromptForm(form());
    REQUIRE(f.has_value());
    CHECK((*f)["submitLabel"] == "OK");
    CHECK((*f)["fields"][0]["value"] == "beat"); // a choice defaults to its first option
    CHECK((*f)["fields"][2]["value"] == "");
    CHECK((*f)["fields"][4]["value"] == false);
    CHECK(defaultPromptAnswer(*f) ==
          Json{{"every", "beat"}, {"bpm", 120.0}, {"name", ""}, {"result", "0:00 Intro"}, {"all", false}});

    const auto answer = [&](Json values) { return validatePromptAnswer(*f, values); };
    auto ok = answer({{"every", "bar"}, {"bpm", 90}, {"name", "Beat"}, {"all", true}});
    REQUIRE(ok.has_value());
    CHECK((*ok)["every"] == "bar");
    CHECK((*ok)["result"] == "0:00 Intro"); // missing: the field's value
    // A read-only field keeps its value whatever comes back.
    CHECK((*answer({{"result", "changed"}}))["result"] == "0:00 Intro");
    CHECK_FALSE(answer({{"every", "never"}}).has_value());
    CHECK_FALSE(answer({{"bpm", 1000}}).has_value());
    CHECK_FALSE(answer({{"bpm", "fast"}}).has_value());
    CHECK_FALSE(answer({{"name", "far too long a name"}}).has_value());
    CHECK_FALSE(answer({{"all", 1}}).has_value());
    CHECK_FALSE(answer({{"unknown", 1}}).has_value());
    CHECK_FALSE(answer(Json::array()).has_value());

    // Forms that cannot be shown.
    const auto bad = [](Json mutate) {
        Json g = form();
        g.merge_patch(mutate);
        return validatePromptForm(g).has_value();
    };
    CHECK_FALSE(bad({{"title", ""}}));
    CHECK_FALSE(bad({{"title", std::string(101, 't')}}));
    CHECK_FALSE(bad({{"fields", Json::array({{{"id", "a"}, {"type", "file"}, {"label", "Pick a file"}}})}}));
    CHECK_FALSE(bad({{"fields", Json::array({{{"id", "a"}, {"type", "text"}, {"label", "A"}},
                                             {{"id", "a"}, {"type", "text"}, {"label", "B"}}})}}));
    CHECK_FALSE(bad({{"fields", Json::array({{{"id", "n"}, {"type", "number"}, {"label", "N"}, {"min", 5}, {"max", 1}}})}}));
    CHECK_FALSE(bad({{"fields", Json::array({{{"id", "c"}, {"type", "choice"}, {"label", "C"}, {"options", Json::array()}}})}}));
    CHECK_FALSE(bad({{"fields", Json::array({{{"id", "t"}, {"type", "text"}, {"label", "T"}, {"readOnly", "yes"}}})}}));
    Json many = form();
    many["fields"] = Json::array();
    for (std::size_t i = 0; i <= kMaxPromptFields; ++i) {
        many["fields"].push_back({{"id", "f" + std::to_string(i)}, {"type", "checkbox"}, {"label", "F"}});
    }
    CHECK_FALSE(validatePromptForm(many).has_value());
}
