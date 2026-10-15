#pragma once

// DLL search hardening for Wabcut's executables (review wave 5, S5-P2-5). main() calls hardenDllSearch() first, so a
// DLL loaded by name at run time comes from the application's folder, System32 or a folder added with
// AddDllDirectory(), never from the current directory (a project folder opened from Explorer) or PATH. The executables'
// static imports are restricted at link time (/DEPENDENTLOADFLAG, cmake/WabcutCompilerOptions.cmake).
//
// Header-only and without <windows.h>, so its macros stay out of the mains: the two kernel32 functions are declared
// here exactly as the SDK declares them (a compatible redeclaration where <windows.h> is included too).
// wabcut_target_defaults() puts src/platform/include on every Wabcut target's include path.

#ifdef _WIN32
extern "C" {
__declspec(dllimport) int __stdcall SetDefaultDllDirectories(unsigned long DirectoryFlags);
__declspec(dllimport) int __stdcall SetDllDirectoryW(const wchar_t* lpPathName);
}
#endif

namespace wabcut::platform {

/// Restricts run-time DLL loading by name to the application's folder, System32 and AddDllDirectory() folders, and takes
/// the current directory out of the search order of LoadLibrary calls that pass their own flags. Call it first in main(),
/// before anything loads a library. Does nothing off Windows.
inline void hardenDllSearch() noexcept {
#ifdef _WIN32
    constexpr unsigned long kApplicationDir = 0x00000200; // LOAD_LIBRARY_SEARCH_APPLICATION_DIR
    constexpr unsigned long kUserDirs = 0x00000400;       // LOAD_LIBRARY_SEARCH_USER_DIRS
    constexpr unsigned long kSystem32 = 0x00000800;       // LOAD_LIBRARY_SEARCH_SYSTEM32
    (void)SetDefaultDllDirectories(kApplicationDir | kUserDirs | kSystem32);
    (void)SetDllDirectoryW(L"");
#endif
}

} // namespace wabcut::platform
