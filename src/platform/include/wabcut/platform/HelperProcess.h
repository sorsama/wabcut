#pragma once

// What a helper process gets from the editor on Windows (review wave 5, S5-P2-5): an explicit, minimal environment and
// its own folder as the working directory, never the editor's whole environment (a provider key set as an environment
// variable would reach the script host and every plug-in's process) or its current directory (a project folder opened
// from Explorer, where a planted DLL would be found). Header-only, shared by wabcut_ai's ChildProcess and the plug-in
// host's ChildProcess (wabcut_plughost_common links nothing of Wabcut's): wabcut_target_defaults() puts
// src/platform/include on every Wabcut target's include path. helperEnvironment() is plain C++ (tested anywhere); the
// functions that read this process's environment are Windows only.

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wabcut::platform {

namespace detail {

inline std::wstring upperAscii(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) {
        if (c >= L'a' && c <= L'z') {
            c = static_cast<wchar_t>(c - L'a' + L'A');
        }
    }
    return out;
}

/// A name made of credential words: one of its parts (split at '_', '-' or '.') is KEY, TOKEN, SECRET, ..., or it ends
/// in one (OPENAIKEY).
inline bool credentialName(const std::wstring& upper) {
    static constexpr std::array<std::wstring_view, 12> kWords{L"KEY",    L"KEYS",       L"APIKEY",     L"TOKEN",
                                                             L"TOKENS", L"SECRET",     L"SECRETS",    L"PASSWORD",
                                                             L"PASSWD", L"CREDENTIAL", L"CREDENTIALS", L"PAT"};
    const std::wstring_view name(upper);
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = name.find_first_of(L"_-.", start);
        const std::wstring_view part = name.substr(start, end - start); // the rest when end is npos
        if (std::ranges::find(kWords, part) != kWords.end()) {
            return true;
        }
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1;
    }
    return name.ends_with(L"KEY") || name.ends_with(L"TOKEN") || name.ends_with(L"SECRET") || name.ends_with(L"PASSWORD");
}

} // namespace detail

/// Whether a helper may see the editor's variable `name` (case-insensitive, as Windows names are): the system's own
/// (SystemRoot, TEMP, the user's folders, the processor), the locale's, Qt's (QT_*), ggml's and the sanitizers' options
/// and Wabcut's settings (WABCUT_*), but never one named like a credential (WABCUT_OPENAI_API_KEY, OPENAI_API_KEY,
/// GITHUB_TOKEN). PATH is not inherited: helperEnvironment() writes its own.
inline bool helperMayInherit(std::wstring_view name) {
    static constexpr std::array<std::wstring_view, 37> kNames{
        L"SYSTEMROOT", L"SYSTEMDRIVE", L"WINDIR", L"COMSPEC", L"PATHEXT", L"TEMP", L"TMP", L"USERPROFILE", L"HOMEDRIVE",
        L"HOMEPATH", L"LOCALAPPDATA", L"APPDATA", L"PROGRAMDATA", L"ALLUSERSPROFILE", L"PUBLIC", L"PROGRAMFILES",
        L"PROGRAMFILES(X86)", L"PROGRAMW6432", L"COMMONPROGRAMFILES", L"COMMONPROGRAMFILES(X86)", L"COMMONPROGRAMW6432",
        L"NUMBER_OF_PROCESSORS", L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER", L"PROCESSOR_LEVEL",
        L"PROCESSOR_REVISION", L"OS", L"LANG", L"LANGUAGE", L"LC_ALL", L"LC_CTYPE", L"LC_MESSAGES", L"TZ",
        L"CUDA_VISIBLE_DEVICES", L"ASAN_OPTIONS", L"UBSAN_OPTIONS", L"HOME"};
    static constexpr std::array<std::wstring_view, 3> kPrefixes{L"QT_", L"WABCUT_", L"GGML_"};
    const std::wstring upper = detail::upperAscii(name);
    if (upper.empty() || detail::credentialName(upper)) {
        return false;
    }
    if (std::ranges::find(kNames, std::wstring_view(upper)) != kNames.end()) {
        return true;
    }
    return std::ranges::any_of(kPrefixes, [&](std::wstring_view prefix) { return upper.starts_with(prefix); });
}

/// The environment block of a helper, for CreateProcessW with CREATE_UNICODE_ENVIRONMENT (NAME=value entries, each
/// NUL-terminated, sorted by name as Windows keeps them, then a final NUL), from the editor's `variables` ("NAME=value";
/// the hidden "=C:=..." ones are dropped): the ones helperMayInherit() lets through, SystemRoot (from `systemDir`'s
/// folder when the editor has none) and a PATH of the helper's folder, the application's folder, `systemDir` (System32)
/// and Windows' own folder.
inline std::wstring helperEnvironment(const std::vector<std::wstring>& variables, const std::filesystem::path& helperDir,
                                      const std::filesystem::path& appDir, const std::filesystem::path& systemDir) {
    std::vector<std::wstring> entries;
    bool systemRoot = false;
    for (const std::wstring& v : variables) {
        const std::size_t equals = v.find(L'=', 1);
        if (v.empty() || v.front() == L'=' || equals == std::wstring::npos) {
            continue;
        }
        const std::wstring_view name = std::wstring_view(v).substr(0, equals);
        if (!helperMayInherit(name)) {
            continue;
        }
        systemRoot = systemRoot || detail::upperAscii(name) == L"SYSTEMROOT";
        entries.push_back(v);
    }
    const std::filesystem::path windowsDir = systemDir.parent_path();
    if (!systemRoot && !windowsDir.empty()) {
        entries.push_back(L"SystemRoot=" + windowsDir.wstring());
    }
    std::wstring path;
    std::vector<std::wstring> seen;
    for (const std::filesystem::path& dir : {helperDir, appDir, systemDir, windowsDir}) {
        const std::wstring text = dir.wstring();
        std::wstring key = detail::upperAscii(text);
        if (text.empty() || std::ranges::find(seen, key) != seen.end()) {
            continue;
        }
        seen.push_back(std::move(key));
        path += path.empty() ? text : L";" + text;
    }
    entries.push_back(L"PATH=" + path);
    const auto nameOf = [](const std::wstring& entry) {
        return detail::upperAscii(std::wstring_view(entry).substr(0, entry.find(L'=')));
    };
    std::ranges::stable_sort(entries, [&](const std::wstring& a, const std::wstring& b) { return nameOf(a) < nameOf(b); });
    std::wstring block;
    for (const std::wstring& e : entries) {
        block += e;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

#ifdef _WIN32

/// This process's environment variables ("NAME=value").
inline std::vector<std::wstring> currentEnvironment() {
    std::vector<std::wstring> out;
    wchar_t* strings = GetEnvironmentStringsW();
    if (strings == nullptr) {
        return out;
    }
    for (const wchar_t* p = strings; *p != L'\0'; p += std::wcslen(p) + 1) {
        out.emplace_back(p);
    }
    FreeEnvironmentStringsW(strings);
    return out;
}

/// The folder of this process's executable (the application's folder).
inline std::filesystem::path thisExecutableDir() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return {};
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

/// System32.
inline std::filesystem::path system32Dir() {
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    const UINT n = GetSystemDirectoryW(buffer.data(), MAX_PATH);
    return n > 0 && n <= MAX_PATH ? std::filesystem::path(std::wstring(buffer.data(), n)) : std::filesystem::path();
}

/// helperEnvironment() of this process for the helper `executable` (an absolute path).
inline std::wstring helperEnvironmentFor(const std::filesystem::path& executable) {
    return helperEnvironment(currentEnvironment(), executable.parent_path(), thisExecutableDir(), system32Dir());
}

#endif

} // namespace wabcut::platform
