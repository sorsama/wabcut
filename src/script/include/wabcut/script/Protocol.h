#pragma once

// The wabcut-scripthost protocol (docs/extensions.md): newline-delimited JSON over the helper's stdin and stdout, one
// object per line, UTF-8, like wabcut-infer's and wabcut-embed's. Each extension runs in its own helper process.
//
// The helper runs untrusted code, so the editor treats every line it sends as untrusted: parseHostMessage() checks
// the shape and bounds of each message, and the editor checks again what a message asks for (capabilities,
// storage bounds, prompt forms) before acting on it.
//
// Editor -> helper:
//   {"type":"hello","protocol":1,"wabcutVersion":"0.9.0","extension":{"id","name","version","capabilities":[...]},
//    "limits":{"callMs":5000},"scripts":[{"path":"main.js","source":"..."}],"storage":{...}}
//   {"type":"invoke","call":7,"command":"addBeatMarkers","args":{...}}     run a registered command's handler
//   {"type":"event","event":"playheadMoved","data":{...},"coalesced":3}  to the extension's listeners
//   {"type":"reply","id":4,"result":{...}}                                answer to a request (a command's result)
//   {"type":"promptAnswer","prompt":2,"values":{...}}                     the user's answers; null = cancelled
//   {"type":"cancel","call":7}                                             give up a call (interrupts its script)
//   {"type":"ping","id":9}
//   {"type":"shutdown"}
// Helper -> editor:
//   {"type":"ready"}  or  {"type":"loadFailed","message":"..."}          after evaluating the scripts
//   {"type":"request","id":4,"method":"commands.run","params":{"document":{...}}}
//        methods: commands.run (a request or batch document), commands.list, commands.describe {"name"}
//   {"type":"done","call":7,"result":...}  or  {"type":"done","call":7,"error":{"kind":"exception","message":"..."}}
//        kind: exception, timeLimit, cancelled, result (the result could not be sent)
//   {"type":"notify","level":"info","message":"..."}   (info, warning, error)   {"type":"toast","message":"..."}
//   {"type":"prompt","prompt":2,"form":{...}}          (validatePromptForm)
//   {"type":"registerCommand","id":"addBeatMarkers","title":"Add Markers at Every Beat"}
//   {"type":"subscribe","event":"playheadMoved","on":true}
//   {"type":"storageSet","key":"k","value":...}   {"type":"storageRemove","key":"k"}
//   {"type":"log","level":"info","message":"..."}       (log, info, warn, error: console.*)
//   {"type":"pong","id":9}

#include <wabcut/script/Events.h>
#include <wabcut/script/Manifest.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace wabcut::script {

inline constexpr int kScriptProtocolVersion = 1;
/// One message from the helper. A command document or a handler's result fits easily; more is a broken helper.
inline constexpr std::size_t kMaxHostMessageBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kMaxNotifyBytes = 2000;
inline constexpr std::size_t kMaxToastBytes = 300;
inline constexpr std::size_t kMaxLogBytes = 4000;
inline constexpr std::size_t kMaxPromptFormBytes = 64 * 1024;
inline constexpr std::size_t kMaxPromptFields = 16;
inline constexpr std::size_t kMaxRegisteredCommands = 64;
/// Ids and call numbers are plain JSON integers below 2^53 (what JavaScript numbers hold exactly).
inline constexpr std::uint64_t kMaxMessageId = (std::uint64_t{1} << 53) - 1;

enum class HostMessageType {
    Ready,
    LoadFailed,
    Request,
    Done,
    Notify,
    Toast,
    Prompt,
    RegisterCommand,
    Subscribe,
    StorageSet,
    StorageRemove,
    Log,
    Pong,
};

/// A checked message from the helper: `body` is the whole object, its fields of the documented types and sizes.
struct HostMessage {
    HostMessageType type = HostMessageType::Log;
    Json body;
};

std::expected<HostMessage, std::string> parseHostMessage(std::string_view line);

/// A prompt form (wabcut.ui.prompt): {"title", "message"?, "submitLabel"?, "fields": [...]}, each field
///   {"id", "type": "text" | "number" | "checkbox" | "choice", "label", "value"?, ...}
///   text:     "placeholder"?, "maxLength"? (<= 20000), "multiline"?, "readOnly"? (a copyable result)
///   number:   "min"?, "max"?, "step"?
///   choice:   "options": [{"value", "label"}] (1..50)
/// The normalised form (defaults filled in), or why it is not one.
std::expected<Json, std::string> validatePromptForm(const Json& form);
/// The user's (or a script's, headless) answers checked against the form: every field present with a value of its
/// type within its bounds (a missing one takes the field's value). Read-only fields keep their value.
std::expected<Json, std::string> validatePromptAnswer(const Json& form, const Json& values);
/// The answers a form gives when nobody changes it (headless runs).
Json defaultPromptAnswer(const Json& form);

/// The hello message the editor sends first.
Json helloMessage(const Manifest& manifest, command::CapabilitySet granted, std::string_view wabcutVersion,
                  int callMs, const Json& scripts, const Json& storage);

} // namespace wabcut::script
