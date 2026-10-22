#pragma once

// Word documents (.docx, Office Open XML WordprocessingML) for transcripts (0.2 graphics): transcript.export writes one,
// captions.import reads one back (Premiere's transcript export and import).
//
//   * writeDocx makes a minimal, valid document: [Content_Types].xml, the package and document relationships,
//     word/document.xml with one w:p per paragraph (a '\n' inside one is a w:br; runs bold or grey as asked), core
//     properties with the title. Stored ZIP entries (no compression) - Word, LibreOffice and Google Docs open them.
//   * readDocxText reads the text of a document: every w:p of word/document.xml as a line (w:br and w:cr are line
//     breaks, w:tab a tab; tracked deletions and field codes are left out), in document order, tables included.
//     Untrusted input: the bounded ZIP reader (Zip.h) and an XML parser that loads no external entities; at most
//     16 MiB of document.xml.

#include <wabcut/model/Error.h>

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange {

struct DocxRun {
    std::string text; ///< UTF-8; '\n' is a line break inside the paragraph
    bool bold = false;
    bool muted = false; ///< grey (timecodes)
};

struct DocxParagraph {
    std::vector<DocxRun> runs;
    /// Space after the paragraph, in points (Word's "Spacing After").
    int spaceAfter = 0;
};

/// The .docx file's bytes.
std::string writeDocx(std::span<const DocxParagraph> paragraphs, std::string_view title);

/// The document's text, one line per paragraph and line break. Errors: ParseError (not a ZIP, no word/document.xml,
/// malformed XML, beyond the limits).
std::expected<std::string, Error> readDocxText(std::string_view data);

} // namespace wabcut::interchange
