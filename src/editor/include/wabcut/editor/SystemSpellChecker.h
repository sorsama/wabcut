#pragma once

// The operating system's spell checker (0.2 graphics, model/Spelling.h) for the app and wabcut-cli.
//
// Windows: the Spelling Checking API (spellcheck.h, ISpellCheckerFactory), the same dictionaries as Windows' own text
// boxes: every language the user has installed a dictionary for (en-US is always there). A language given as ISO 639-1
// ("en") takes the first installed dictionary of that language ("en-US"); "" is the user's display language. The API
// tokenises and checks the text itself (repeated words come back as Delete, known autocorrections as Replace) and
// suggests corrections. Words are never added to the operating system's dictionary: PersonalSpelling keeps Wabcut's.
// Other platforms: a checker that supports no language (nothing is underlined), until a Hunspell-based one exists.
//
// Thread-safe: each thread gets its own COM objects (COM is initialised on threads that have not done so).

#include <wabcut/model/Spelling.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::editor {

/// The system checker (one per process; cheap to call from any thread).
std::shared_ptr<const SpellChecker> systemSpellChecker();

/// The languages the system can check (BCP 47 tags, e.g. "en-US", "th-TH"); empty where there is no checker.
std::vector<std::string> systemSpellingLanguages();

/// The user's dictionary file: `folder`/spelling-dictionary.txt (the app: its data folder; wabcut-cli: the
/// WABCUT_SPELLING_DICTIONARY file when set).
std::filesystem::path spellingDictionaryIn(const std::filesystem::path& folder);

} // namespace wabcut::editor
