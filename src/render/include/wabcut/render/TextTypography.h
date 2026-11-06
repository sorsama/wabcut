#pragma once

// Typography beyond Qt's switches (0.7, card #138): OpenType features and optical kerning for graphics text.
//
//   * Features. QTextLayout shapes text with HarfBuzz, which already applies a script's required and default features:
//     the joining forms (init, medi, fina, isol), contextual alternates (calt), required ligatures (rlig, the Nastaliq and
//     Naskh fonts' mandatory shapes) and mark positioning for Arabic and Persian; ligatures (liga, clig) and kerning for
//     Latin. A TextStyle's `features` list (Graphics.h, parseFontFeatures) turns further ones on - discretionary ligatures
//     (dlig), stylistic sets (ss01..ss20), swashes (swsh), old-style or tabular figures (onum, tnum) - or a default one off,
//     through QFont::setFeature (Qt 6.7+). Values set that way override Qt's own choices (kerning(), the ligature rules
//     for letter-spaced text).
//   * Optical kerning. Qt has the font's own pairs only (GPOS / kern). Premiere's Optical kerning spaces each pair by the
//     shapes of its two letters instead: opticalSpacings() measures the white space between the ink of neighbouring glyphs
//     over the x-height (or cap-height) band and evens it out against the spacing of round and straight letters in the same
//     font (n, o), so "AV", "To" and "r." tighten while "nn" and "oo" stay put. It applies to Latin, Greek and Cyrillic text;
//     joined scripts keep the shaper's spacing, since their letters are not set one after another.

#include <QFont>
#include <QList>
#include <QString>
#include <QTextCharFormat>
#include <QTextLayout>

#include <string_view>
#include <vector>

namespace wabcut::render {

/// Sets the OpenType features of `features` (parseFontFeatures' syntax) on `font`; an unreadable list sets none (the model
/// refuses such a list when it is saved).
void applyFontFeatures(QFont& font, std::string_view features);

/// Extra space after each UTF-16 unit of `text` (pixels at `font`'s pixel size; positive opens the pair, negative tightens it)
/// for optical kerning, or an empty list when the text has letters of a joined script (Arabic, Indic, ...), the font cannot
/// give glyphs, or the text is a single letter. The adjustments are measured on the glyph outlines at 96 px and scaled, kept
/// within -0.18 .. +0.12 em. Safe on any thread (the outlines are cached).
std::vector<double> opticalSpacings(const QFont& font, const QString& text);

/// Appends the format ranges for characters [from, to) of `text`: one range with `format`, or with `optical` one per
/// character, each with its letter spacing set to `trackingPx` plus its optical adjustment (opticalSpacings). The layout's
/// shaping runs per range, so a run set optically does not form ligatures.
void appendTextRanges(QList<QTextLayout::FormatRange>& out, const QString& text, int from, int to, const QTextCharFormat& format,
                      bool optical, double trackingPx);

} // namespace wabcut::render
