#pragma once

// Windows.Data.Pdf (WinRT) behind a plain C++ interface: the PDF-compatible part of an Adobe Illustrator file drawn at any size.
// Windows only; elsewhere an installed Ghostscript does this (VectorImage.cpp). No Qt types, so the C++/WinRT headers stay
// in VectorPdfWin.cpp.

#ifdef _WIN32

#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::render::detail {

/// The first page's size in points (1/72 inch) - or why the document cannot be read (not a PDF, encrypted, no pages).
std::expected<std::pair<double, double>, std::string> pdfPageSizePoints(const std::vector<std::uint8_t>& pdf);

/// The first page drawn into a `width` x `height` picture on a transparent background, as PNG bytes.
std::expected<std::vector<std::uint8_t>, std::string> pdfRenderPng(const std::vector<std::uint8_t>& pdf, int width, int height);

} // namespace wabcut::render::detail

#endif
