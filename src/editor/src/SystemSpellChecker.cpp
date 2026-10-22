#include <wabcut/editor/SystemSpellChecker.h>

#include <algorithm>
#include <cctype>
#include <map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <spellcheck.h>
#endif

namespace wabcut::editor {

namespace {

#ifdef _WIN32

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// UTF-8 -> UTF-16, and the byte offset of every UTF-16 unit (plus the end).
struct Wide {
    std::wstring text;
    std::vector<std::size_t> bytes;
};

Wide widen(std::string_view s) {
    Wide out;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        const std::size_t n = b0 < 0x80 ? 1 : (b0 >> 5) == 0x6 ? 2 : (b0 >> 4) == 0xE ? 3 : (b0 >> 3) == 0x1E ? 4 : 0;
        char32_t c = n == 1 ? b0 : n == 2 ? (b0 & 0x1F) : n == 3 ? (b0 & 0x0F) : (b0 & 0x07);
        bool ok = n > 0 && i + n <= s.size();
        for (std::size_t k = 1; ok && k < n; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            ok = (b & 0xC0) == 0x80;
            c = (c << 6) | (b & 0x3F);
        }
        const std::size_t len = ok ? n : 1;
        if (!ok) {
            c = 0xFFFD;
        }
        if (c >= 0x10000) {
            const char32_t v = c - 0x10000;
            out.text.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            out.bytes.push_back(i);
            out.text.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
            out.bytes.push_back(i);
        } else {
            out.text.push_back(static_cast<wchar_t>(c));
            out.bytes.push_back(i);
        }
        i += len;
    }
    out.bytes.push_back(s.size());
    return out;
}

std::string narrow(const wchar_t* w) {
    if (!w) {
        return {};
    }
    const int length = static_cast<int>(wcslen(w));
    if (length == 0) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, w, length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w, length, out.data(), size, nullptr, nullptr);
    }
    return out;
}

std::wstring wideTag(std::string_view tag) {
    std::wstring out;
    for (const char ch : tag) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return out;
}

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p(o.p) { o.p = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            reset();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~Com() { reset(); }
    void reset() {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

/// COM on this thread for as long as the thread lives (balanced on exit when this code initialised it). A thread that
/// is already in an apartment (the app's GUI thread: Qt's OLE) keeps it.
struct ComThread {
    bool owned = false;
    ComThread() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr); // S_OK or S_FALSE: ours to balance; RPC_E_CHANGED_MODE: an STA thread, fine
    }
    ~ComThread() {
        if (owned) {
            CoUninitialize();
        }
    }
};

/// This thread's factory and checkers by language tag.
struct ThreadCheckers {
    ComThread com;
    Com<ISpellCheckerFactory> factory;
    bool tried = false;
    std::map<std::wstring, Com<ISpellChecker>> checkers;
    std::map<std::string, std::wstring> resolved; // requested -> installed tag ("" when none)

    ISpellCheckerFactory* get() {
        if (!tried) {
            tried = true;
            CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, __uuidof(ISpellCheckerFactory),
                             reinterpret_cast<void**>(factory.put()));
        }
        return factory.p;
    }
};

ThreadCheckers& threadCheckers() {
    thread_local ThreadCheckers checkers;
    return checkers;
}

std::vector<std::string> supportedLanguages(ISpellCheckerFactory* factory) {
    std::vector<std::string> out;
    if (!factory) {
        return out;
    }
    Com<IEnumString> languages;
    if (FAILED(factory->get_SupportedLanguages(languages.put())) || !languages) {
        return out;
    }
    LPOLESTR tag = nullptr;
    ULONG fetched = 0;
    while (languages->Next(1, &tag, &fetched) == S_OK && fetched == 1) {
        out.push_back(narrow(tag));
        CoTaskMemFree(tag);
        tag = nullptr;
    }
    return out;
}

std::string userLanguage() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
        return narrow(name);
    }
    return "en-US";
}

/// The installed tag for `language` ("" = the user's; "en" -> the first "en-*"), or "" when there is none.
std::wstring resolveTag(ThreadCheckers& t, std::string_view language) {
    ISpellCheckerFactory* factory = t.get();
    if (!factory) {
        return {};
    }
    const std::string key(language);
    if (const auto it = t.resolved.find(key); it != t.resolved.end()) {
        return it->second;
    }
    const std::string wanted = language.empty() ? userLanguage() : std::string(language);
    std::wstring found;
    BOOL supported = FALSE;
    if (SUCCEEDED(factory->IsSupported(wideTag(wanted).c_str(), &supported)) && supported) {
        found = wideTag(wanted);
    } else {
        const std::string primary = lowerAscii(wanted.substr(0, wanted.find_first_of("-_")));
        for (const std::string& tag : supportedLanguages(factory)) {
            if (lowerAscii(tag.substr(0, tag.find('-'))) == primary) {
                found = wideTag(tag);
                break;
            }
        }
        // The user's language has no dictionary: English, which Windows always has.
        if (found.empty() && language.empty()) {
            if (SUCCEEDED(factory->IsSupported(L"en-US", &supported)) && supported) {
                found = L"en-US";
            }
        }
    }
    t.resolved[key] = found;
    return found;
}

ISpellChecker* checkerFor(std::string_view language) {
    ThreadCheckers& t = threadCheckers();
    const std::wstring tag = resolveTag(t, language);
    if (tag.empty()) {
        return nullptr;
    }
    auto it = t.checkers.find(tag);
    if (it == t.checkers.end()) {
        Com<ISpellChecker> checker;
        if (FAILED(t.factory->CreateSpellChecker(tag.c_str(), checker.put()))) {
            checker.reset();
        }
        it = t.checkers.emplace(tag, std::move(checker)).first;
    }
    return it->second.p;
}

class WindowsSpellChecker final : public SpellChecker {
public:
    bool supports(std::string_view language) const override { return checkerFor(language) != nullptr; }

    std::vector<SpellIssue> check(std::string_view text, std::string_view language) const override {
        std::vector<SpellIssue> out;
        ISpellChecker* checker = checkerFor(language);
        if (!checker || text.empty()) {
            return out;
        }
        const Wide wide = widen(text);
        Com<IEnumSpellingError> errors;
        if (FAILED(checker->Check(wide.text.c_str(), errors.put())) || !errors) {
            return out;
        }
        for (;;) {
            Com<ISpellingError> error;
            if (errors->Next(error.put()) != S_OK || !error) {
                break;
            }
            ULONG start = 0;
            ULONG length = 0;
            CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
            error->get_StartIndex(&start);
            error->get_Length(&length);
            error->get_CorrectiveAction(&action);
            if (action == CORRECTIVE_ACTION_NONE || length == 0 || start + length > wide.text.size()) {
                continue;
            }
            SpellIssue issue;
            issue.start = wide.bytes[start];
            issue.end = wide.bytes[start + length];
            issue.word = std::string(text.substr(issue.start, issue.end - issue.start));
            if (action == CORRECTIVE_ACTION_REPLACE) {
                issue.action = SpellAction::Replace;
                LPWSTR replacement = nullptr;
                if (SUCCEEDED(error->get_Replacement(&replacement)) && replacement) {
                    issue.suggestions.push_back(narrow(replacement));
                    CoTaskMemFree(replacement);
                }
            } else if (action == CORRECTIVE_ACTION_DELETE) {
                issue.action = SpellAction::Delete;
            }
            out.push_back(std::move(issue));
        }
        return out;
    }

    std::vector<std::string> suggest(std::string_view word, std::string_view language) const override {
        std::vector<std::string> out;
        ISpellChecker* checker = checkerFor(language);
        if (!checker || word.empty()) {
            return out;
        }
        Com<IEnumString> suggestions;
        const HRESULT hr = checker->Suggest(widen(word).text.c_str(), suggestions.put());
        if (hr != S_OK || !suggestions) {
            return out; // S_FALSE: the word is right
        }
        LPOLESTR s = nullptr;
        ULONG fetched = 0;
        while (out.size() < kMaxSpellSuggestions && suggestions->Next(1, &s, &fetched) == S_OK && fetched == 1) {
            out.push_back(narrow(s));
            CoTaskMemFree(s);
            s = nullptr;
        }
        return out;
    }
};

#endif

} // namespace

std::shared_ptr<const SpellChecker> systemSpellChecker() {
#ifdef _WIN32
    static const auto checker = std::make_shared<const WindowsSpellChecker>();
    return checker;
#else
    return std::shared_ptr<const SpellChecker>(std::shared_ptr<const SpellChecker>{}, &noSpellChecker());
#endif
}

std::vector<std::string> systemSpellingLanguages() {
#ifdef _WIN32
    return supportedLanguages(threadCheckers().get());
#else
    return {};
#endif
}

std::filesystem::path spellingDictionaryIn(const std::filesystem::path& folder) {
    return folder / "spelling-dictionary.txt";
}

} // namespace wabcut::editor
