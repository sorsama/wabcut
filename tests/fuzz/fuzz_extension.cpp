// Fuzz harness: what an extension brings or says (C.12), all untrusted - its manifest (wabcut-extension.json), the
// helper's protocol lines, prompt forms and their answers, and the permission file the editor reads back.
// What parses must parse again from its own output to the same thing, and a form's defaults must fit the form.

#include "FuzzSupport.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/script/Manifest.h>
#include <wabcut/script/Permissions.h>
#include <wabcut/script/Protocol.h>

using namespace wabcut;
using namespace wabcut::script;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text(fuzz::text(data, size));

    // A manifest: checked fields, then the same manifest from its own JSON.
    if (const auto manifest = parseManifest(text)) {
        WABCUT_FUZZ_CHECK(isValidExtensionId(manifest->id), "manifest id");
        WABCUT_FUZZ_CHECK((manifest->requested().bits & ~grantableCapabilities().bits) == 0, "grantable capabilities");
        for (const std::string& file : manifest->files()) {
            WABCUT_FUZZ_CHECK(isValidScriptPath(file), "script path " + file);
        }
        const auto again = parseManifest(toJson(*manifest).dump());
        WABCUT_FUZZ_CHECK(again, again ? std::string() : again.error().summary());
        WABCUT_FUZZ_CHECK(toJson(*again) == toJson(*manifest), "manifest round trip");
    }

    // A line from the helper: its checked body is a valid message in turn, unchanged (sanitising is idempotent).
    if (const auto message = parseHostMessage(text)) {
        const auto again = parseHostMessage(message->body.dump());
        WABCUT_FUZZ_CHECK(again, again ? std::string() : again.error());
        WABCUT_FUZZ_CHECK(again->type == message->type && again->body == message->body, "message round trip");
    }

    const Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !io::jsonNestingWithin(text)) {
        return 0;
    }
    // A prompt form: normalised once and for all; its defaults are an answer it accepts; any JSON answer is checked
    // without throwing.
    if (const auto form = validatePromptForm(json)) {
        const auto renormalised = validatePromptForm(*form);
        WABCUT_FUZZ_CHECK(renormalised && *renormalised == *form, "form normalisation is idempotent");
        const auto defaults = validatePromptAnswer(*form, defaultPromptAnswer(*form));
        WABCUT_FUZZ_CHECK(defaults, defaults ? std::string() : defaults.error());
        if (json.contains("answer")) {
            (void)validatePromptAnswer(*form, json["answer"]);
        }
    }
    // The permission file: what loads writes back the same.
    if (auto store = PermissionStore::fromJson(json)) {
        const auto again = PermissionStore::fromJson(store->toJson());
        WABCUT_FUZZ_CHECK(again && again->toJson() == store->toJson(), "permission file round trip");
    }
    return 0;
}
