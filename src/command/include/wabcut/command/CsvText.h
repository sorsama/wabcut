#pragma once

// CSV text cells that a spreadsheet cannot run (card #84 and its follow-ups). Marker names, comments, speaker names and
// transcript words come from files, camera metadata, projects and recordings other people made, and Excel, Google
// Sheets and LibreOffice run a cell that starts with = + - @ (or a tab or carriage return) as a formula when the file
// is opened (CSV injection: OWASP "CSV Injection"). Such a cell gets a single quote in front, so it stays text; then
// the cell is quoted as RFC 4180 asks (a comma, a quote or a line break). A cell that already starts with quotes
// followed by one of those characters gets one more, so csvTextUnguarded() - what a Wabcut reader of these files
// would use - gives back exactly what was written.

#include <cstddef>
#include <string>
#include <string_view>

namespace wabcut::command {

/// Whether a spreadsheet would read a cell starting with `c` as a formula.
constexpr bool isCsvFormulaStart(char c) {
    return c == '=' || c == '+' || c == '-' || c == '@' || c == '\t' || c == '\r';
}

/// Whether `text` needs the guard: it starts, after any apostrophes, with a formula character.
constexpr bool needsCsvGuard(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && text[i] == '\'') {
        ++i;
    }
    return i < text.size() && isCsvFormulaStart(text[i]);
}

/// One text cell, guarded against formulas and quoted when it holds a comma, a quote or a line break.
inline std::string csvText(std::string_view text) {
    std::string cell = needsCsvGuard(text) ? "'" + std::string(text) : std::string(text);
    if (cell.find_first_of(",\"\r\n") == std::string::npos) {
        return cell;
    }
    std::string out = "\"";
    for (const char ch : cell) {
        out += ch;
        if (ch == '"') {
            out += '"';
        }
    }
    return out + "\"";
}

/// The text a cell written by csvText() holds, with its quoting already removed: the guard apostrophe taken off.
inline std::string csvTextUnguarded(std::string_view cell) {
    if (!cell.empty() && cell.front() == '\'' && needsCsvGuard(cell)) {
        cell.remove_prefix(1);
    }
    return std::string(cell);
}

} // namespace wabcut::command
