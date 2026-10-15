#include "EvalHarness.h"

#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/agent/Verification.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <set>
#include <stdexcept>

namespace wabcut::ai::eval {

using namespace wabcut::test;

namespace {

std::string runOf(char letter, int frames) {
    std::string s(1, letter);
    s.append(static_cast<std::size_t>(frames - 1), static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));
    return s;
}

/// Runs fixture-building commands with every capability.
class Builder {
public:
    explicit Builder(ProjectState state) : m_session(std::move(state)), m_engine(m_session, edit::editingCommands()) {}
    command::CommandResult run(std::string command, Json params) {
        command::CommandRequest r;
        r.command = std::move(command);
        r.params = std::move(params);
        command::CommandResult result = m_engine.execute(r);
        if (!result.ok()) {
            throw std::runtime_error("fixture command " + r.command + " failed: " + result.error->message);
        }
        return result;
    }
    ProjectState state() const { return m_session.stateForSave(); }

private:
    EditSession m_session;
    command::CommandEngine m_engine;
};

/// The timeline with only `keep` media left in the library.
Fixture fromTimeline(std::string name, Timeline& tl, std::string_view keep) {
    Fixture fx;
    fx.name = std::move(name);
    fx.seq = tl.seq;
    ProjectState state = tl.state;
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        if (keep.find(letter) == std::string_view::npos) {
            auto removed = wabcut::edit::removeMedia(state, tl.media(letter));
            if (!removed) {
                throw std::runtime_error("fixture: could not remove media " + std::string(1, letter));
            }
            state = std::move(*removed);
        } else {
            fx.media[letter] = tl.media(letter);
        }
    }
    fx.state = std::move(state);
    return fx;
}

Fixture interview(std::string name = "interview") {
    const std::string video = runOf('A', 125) + std::string(25, '.') + runOf('B', 125) + runOf('C', 125);
    Timeline tl("V1 tgt |" + video + "|\nA1 tgt dialogue |" + video + "|\nA2 nosync music |" + runOf('M', 400) + "|");
    tl.linkMatching();
    Fixture fx = fromTimeline(std::move(name), tl, "ABCM");
    Builder b(fx.state);
    b.run("marker.add", {{"name", "Hook"}, {"time", "00:00:00:10"}});
    b.run("marker.add", {{"name", "Outro"}, {"time", "00:00:14:00"}});
    fx.state = b.state();
    return fx;
}

Fixture captions() {
    Fixture fx = interview("captions");
    Builder b(fx.state);
    b.run("captions.addTrack", {{"name", "English"}, {"language", "en"}});
    b.run("captions.addCue", {{"start", "00:00:00:00"}, {"end", "00:00:01:12"}, {"text", "Welcome to the show."}});
    b.run("captions.addCue", {{"start", "00:00:02:00"}, {"end", "00:00:04:00"}, {"text", "Today we talk about editing."}});
    b.run("captions.addCue", {{"start", "00:00:06:00"}, {"end", "00:00:08:00"}, {"text", "Let's get startd"}});
    b.run("captions.addCue", {{"start", "00:00:10:00"}, {"end", "00:00:12:00"}, {"text", "Thanks for watching."}});
    fx.state = b.state();
    return fx;
}

Fixture multicam() {
    Timeline tl("V1 tgt |" + std::string(10, '.') + "|\nA1 tgt dialogue |" + std::string(10, '.') + "|");
    Fixture fx = fromTimeline("multicam", tl, "DEF");
    Builder b(fx.state);
    b.run("multicam.create", {{"media", Json::array({fx.media['D'].value, fx.media['E'].value, fx.media['F'].value})},
                              {"name", "Podcast"},
                              {"angleNames", Json::array({"Host", "Guest", "Wide"})},
                              {"sync", "manual"},
                              {"offsets", Json::array({0, 0, 0})}});
    SequenceId source;
    for (const SequencePtr& s : b.state().sequences) {
        if (s->id != fx.seq) {
            source = s->id;
        }
    }
    b.run("clip.overwrite", {{"sequence", fx.seq.value}, {"sourceSequence", source.value}, {"at", "00:00:00:00"},
                             {"video", "V1"}, {"audio", Json::array({"A1"})}});
    fx.state = b.state();
    return fx;
}

Fixture library() {
    Fixture fx = interview("library");
    Builder b(fx.state);
    b.run("bin.create", {{"name", "Footage"}});
    b.run("bin.create", {{"name", "Music"}});
    fx.state = b.state();
    for (const Bin& bin : fx.state.bins) {
        fx.bins[bin.name] = bin.id;
    }
    return fx;
}

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const auto index = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size()))) - 1;
    return v[std::min(index, v.size() - 1)];
}

} // namespace

Fixture makeFixture(std::string_view name) {
    if (name == "interview") {
        return interview();
    }
    if (name == "captions") {
        return captions();
    }
    if (name == "multicam") {
        return multicam();
    }
    if (name == "library") {
        return library();
    }
    throw std::invalid_argument("unknown fixture " + std::string(name));
}

// --- OracleModel ---------------------------------------------------------------------------------------------

OracleModel::OracleModel(const ModelSpec& spec, const EvalCase& c, const Fixture& fixture, OracleStyle style)
    : m_spec(spec), m_case(c), m_fixture(fixture), m_style(style) {}

namespace {
/// "$media:A" and "$bin:Footage" in a reference: the fixture's ids.
Json resolve(const Json& j, const Fixture& fx) {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s.starts_with("$media:") && s.size() == 8) {
            return fx.media.at(s[7]).value;
        }
        if (s.starts_with("$bin:")) {
            return fx.bins.at(s.substr(5)).value;
        }
        return j;
    }
    if (j.is_array() || j.is_object()) {
        Json out = j;
        for (auto it = out.begin(); it != out.end(); ++it) {
            *it = resolve(*it, fx);
        }
        return out;
    }
    return j;
}
} // namespace

std::expected<ChatResponse, AiError> OracleModel::chat(const ChatRequest& request, const ChatEventHandler& onEvent, std::stop_token) {
    const auto step = static_cast<std::size_t>(
        std::count_if(request.messages.begin(), request.messages.end(), [](const ChatMessage& m) { return m.role == Role::Assistant; }));
    const bool constrained = request.responseFormat && request.responseFormat->name == "tool_call";
    std::vector<std::string> offered;
    if (constrained) {
        for (const Json& branch : request.responseFormat->schema.value("anyOf", Json::array())) {
            const Json& props = branch.value("properties", Json::object());
            if (props.contains("name")) {
                offered.push_back(props["name"].value("const", std::string()));
            }
        }
    } else {
        for (const ToolSpec& t : request.tools) {
            offered.push_back(t.name);
        }
    }
    ChatResponse r;
    r.model = m_spec.id;
    r.message.role = Role::Assistant;
    std::string text;
    auto answer = [&](const std::string& words) {
        text = constrained && m_style == OracleStyle::Clean ? Json{{"answer", words}}.dump() : words;
        r.message.parts.push_back(TextPart{text});
        r.stopReason = StopReason::EndTurn;
    };
    if (m_style == OracleStyle::Lazy || step >= m_case.reference.size()) {
        answer(m_style == OracleStyle::Lazy ? "Done." : m_case.referenceAnswer);
    } else {
        ToolUse use = m_case.reference[step];
        use.arguments = resolve(use.arguments, m_fixture);
        if (std::ranges::find(offered, use.tool) == offered.end()) {
            m_routingMiss = true;
            answer("I cannot do that with the tools I have.");
        } else if (constrained) {
            const std::string clean = Json{{"name", use.tool}, {"parameters", use.arguments}}.dump();
            if (m_style == OracleStyle::Clean) {
                text = clean;
            } else {
                switch (m_variant++ % 5) {
                case 0: text = "```json\n" + clean + "\n```"; break;
                case 1: text = Json{{"tool", use.tool}, {"arguments", use.arguments}}.dump(); break;
                case 2: text = Json{{"name", use.tool}, {"parameters", use.arguments.dump()}}.dump(); break;
                case 3: text = "Sure, here is the call: " + clean; break;
                default: text = clean.substr(0, clean.size() - 1); break; // cut off before the last brace
                }
            }
            r.message.parts.push_back(TextPart{text});
            r.stopReason = StopReason::EndTurn;
        } else {
            ToolCall call;
            call.id = "oracle_" + std::to_string(step);
            call.name = use.tool;
            call.arguments = use.arguments;
            call.argumentsText = use.arguments.dump();
            text = call.argumentsText;
            r.message.parts.push_back(std::move(call));
            r.stopReason = StopReason::ToolUse;
        }
    }
    r.usage.inputTokens = estimateInputTokens(request);
    r.usage.outputTokens = static_cast<std::int64_t>((text.size() + 3) / 4);
    if (onEvent) {
        onEvent(TextDelta{0, text});
    }
    return r;
}

// --- TimedChat -----------------------------------------------------------------------------------------------

std::expected<ChatResponse, AiError> TimedChat::chat(const ChatRequest& request, const ChatEventHandler& onEvent, std::stop_token stop) {
    const auto start = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    double first = -1;
    const ChatEventHandler timed = [&](const ChatEvent& e) {
        if (first < 0 && !std::holds_alternative<UsageUpdate>(e)) {
            first = since();
        }
        if (onEvent) {
            onEvent(e);
        }
    };
    auto response = m_inner.chat(request, timed, stop);
    CallTiming t;
    t.seconds = since();
    t.firstToken = first < 0 ? t.seconds : first;
    if (response) {
        t.outputTokens = response->usage.outputTokens;
        t.inputTokens = response->usage.inputTokens;
    }
    std::lock_guard lock(m_mutex);
    m_calls.push_back(t);
    return response;
}

std::vector<CallTiming> TimedChat::take() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_calls, {});
}

// --- Runs ----------------------------------------------------------------------------------------------------

int EvalSummary::passed() const {
    return static_cast<int>(std::ranges::count_if(cases, [](const CaseResult& c) { return c.success; }));
}

double EvalSummary::successRate() const { return cases.empty() ? 0.0 : static_cast<double>(passed()) / static_cast<double>(cases.size()); }

double EvalSummary::validToolCallRate() const {
    int calls = 0;
    int invalid = 0;
    for (const CaseResult& c : cases) {
        calls += c.toolCalls + c.malformedReplies;
        invalid += c.invalidToolCalls + c.malformedReplies;
    }
    return calls == 0 ? 1.0 : static_cast<double>(calls - invalid) / static_cast<double>(calls);
}

double EvalSummary::routingRecall() const {
    if (cases.empty()) {
        return 0.0;
    }
    return static_cast<double>(std::ranges::count_if(cases, [](const CaseResult& c) { return c.routedReference; })) /
           static_cast<double>(cases.size());
}

double EvalSummary::medianStepSeconds() const {
    std::vector<double> v;
    for (const CaseResult& c : cases) {
        for (const CallTiming& t : c.calls) {
            v.push_back(t.seconds);
        }
    }
    return median(std::move(v));
}

double EvalSummary::p90StepSeconds() const {
    std::vector<double> v;
    for (const CaseResult& c : cases) {
        for (const CallTiming& t : c.calls) {
            v.push_back(t.seconds);
        }
    }
    return percentile(std::move(v), 0.9);
}

double EvalSummary::medianFirstTokenSeconds() const {
    std::vector<double> v;
    for (const CaseResult& c : cases) {
        for (const CallTiming& t : c.calls) {
            v.push_back(t.firstToken);
        }
    }
    return median(std::move(v));
}

double EvalSummary::decodeTokensPerSecond() const {
    double tokens = 0;
    double seconds = 0;
    for (const CaseResult& c : cases) {
        for (const CallTiming& t : c.calls) {
            if (t.outputTokens > 1 && t.seconds > t.firstToken) {
                tokens += static_cast<double>(t.outputTokens - 1); // the first token arrives with the prompt
                seconds += t.seconds - t.firstToken;
            }
        }
    }
    return seconds > 0 ? tokens / seconds : 0.0;
}

std::map<std::string, std::pair<int, int>> EvalSummary::byCategory() const {
    std::map<std::string, std::pair<int, int>> out;
    for (const CaseResult& c : cases) {
        auto& [passed, total] = out[c.category];
        passed += c.success ? 1 : 0;
        ++total;
    }
    return out;
}

Json EvalSummary::toJson() const {
    Json rows = Json::array();
    for (const CaseResult& c : cases) {
        Json calls = Json::array();
        for (const CallTiming& t : c.calls) {
            calls.push_back({{"seconds", t.seconds}, {"firstToken", t.firstToken}, {"in", t.inputTokens}, {"out", t.outputTokens}});
        }
        rows.push_back({{"id", c.id}, {"category", c.category}, {"success", c.success}, {"failure", c.failure},
                        {"status", c.status}, {"modelCalls", c.modelCalls}, {"toolCalls", c.toolCalls},
                        {"invalidToolCalls", c.invalidToolCalls}, {"malformedReplies", c.malformedReplies},
                        {"routedReference", c.routedReference}, {"seconds", c.seconds}, {"tools", c.tools}, {"calls", calls}});
    }
    Json categories = Json::object();
    for (const auto& [name, counts] : byCategory()) {
        categories[name] = {{"passed", counts.first}, {"total", counts.second}};
    }
    return {{"model", model}, {"mode", mode}, {"cases", cases.size()}, {"passed", passed()}, {"successRate", successRate()},
            {"validToolCallRate", validToolCallRate()}, {"routingRecall", routingRecall()},
            {"medianStepSeconds", medianStepSeconds()}, {"p90StepSeconds", p90StepSeconds()},
            {"medianFirstTokenSeconds", medianFirstTokenSeconds()}, {"decodeTokensPerSecond", decodeTokensPerSecond()},
            {"byCategory", categories}, {"results", rows}};
}

EvalSummary runEval(const std::function<IChatModel&(const EvalCase&, const Fixture&)>& modelFor, const RunOptions& options) {
    EvalSummary summary;
    std::map<std::string, Fixture> fixtures;
    for (const EvalCase& c : evalCases()) {
        if (!options.only.empty() && std::ranges::none_of(options.only, [&](const std::string& o) { return o == c.id || o == c.category; })) {
            continue;
        }
        auto it = fixtures.find(c.fixture);
        if (it == fixtures.end()) {
            it = fixtures.emplace(c.fixture, makeFixture(c.fixture)).first;
        }
        const Fixture& fx = it->second;
        IChatModel& model = modelFor(c, fx);
        summary.model = model.spec().id;
        summary.mode = options.constrained.value_or(model.spec().caps.constrainedTools) ? "constrained" : "native";
        TimedChat timed(model);

        agent::AgentOptions agentOptions;
        agentOptions.limits.maxSteps = options.maxSteps;
        agentOptions.limits.timeout = options.timeout;
        agentOptions.constrainedTools = options.constrained;
        agent::AgentRequest request;
        request.prompt = c.prompt;
        request.sequence = fx.seq;
        request.base = std::make_shared<const ProjectState>(fx.state);
        request.constraints = agent::parseConstraints(c.prompt, fx.state, fx.seq);
        const auto begin = std::chrono::steady_clock::now();
        const agent::AgentOutcome outcome = agent::EditAgent(agentOptions).run(timed, request, {}, {});

        CaseResult r;
        r.id = c.id;
        r.category = c.category;
        r.status = std::string(agent::toString(outcome.status));
        r.modelCalls = outcome.modelCalls;
        r.toolCalls = outcome.toolCalls;
        r.invalidToolCalls = outcome.invalidToolCalls;
        r.malformedReplies = outcome.malformedReplies;
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        r.calls = timed.take();
        for (const agent::AgentStep& s : outcome.steps) {
            if (!s.tool.empty()) {
                r.tools.push_back(s.tool + (s.ok ? "" : " (error)"));
            }
        }
        for (const ToolUse& use : c.reference) {
            if (std::ranges::find(outcome.offeredTools, use.tool) == outcome.offeredTools.end()) {
                r.routedReference = false;
            }
        }
        const agent::AgentStatus expected = c.question ? agent::AgentStatus::NoChanges : agent::AgentStatus::Staged;
        if (outcome.status != expected) {
            r.failure = "status " + r.status + (outcome.error.empty() ? "" : ": " + outcome.error);
            if (outcome.status == agent::AgentStatus::VerificationFailed) {
                r.failure += " (" + outcome.verification.failuresForModel() + ")";
            }
        } else if (!outcome.staged) {
            r.failure = "nothing staged";
        } else if (auto finding = c.check(fx, *outcome.staged, outcome)) {
            r.failure = *finding;
        }
        r.success = r.failure.empty();
        if (options.progress) {
            options.progress(r);
        }
        summary.cases.push_back(std::move(r));
    }
    return summary;
}

} // namespace wabcut::ai::eval
