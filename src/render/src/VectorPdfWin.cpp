// Windows.Data.Pdf through C++/WinRT: https://learn.microsoft.com/uwp/api/windows.data.pdf.pdfpage (PdfPage.Size is in
// device-independent pixels, 1/96 inch; PdfPageRenderOptions.DestinationWidth / DestinationHeight / BackgroundColor choose
// the picture and RenderToStreamAsync writes it as PNG). A desktop (unpackaged) app may use the API.
//
// The calls run on a thread of their own that joins the multi-threaded apartment: Qt's threads may be STA, where blocking
// on an async operation can deadlock.

#include "VectorPdf.h"

#ifdef _WIN32

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>

#include <exception>
#include <thread>

namespace wabcut::render::detail {

namespace {

using namespace winrt;
using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Storage::Streams;

/// Runs `work` on a fresh MTA thread and returns its result.
template <class Result, class Work>
std::expected<Result, std::string> onMtaThread(Work work) {
    std::expected<Result, std::string> out = std::unexpected(std::string("the PDF reader did not run"));
    std::thread worker([&] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            out = work();
            winrt::uninit_apartment();
        } catch (const winrt::hresult_error& e) {
            out = std::unexpected("Windows could not read the PDF: " + winrt::to_string(e.message()));
        } catch (const std::exception& e) {
            out = std::unexpected(std::string(e.what()));
        }
    });
    worker.join();
    return out;
}

/// The document in memory, ready to open (a password-protected one throws hresult_error).
PdfDocument openDocument(const std::vector<std::uint8_t>& pdf) {
    InMemoryRandomAccessStream stream;
    {
        DataWriter writer(stream);
        writer.WriteBytes(winrt::array_view<const std::uint8_t>(pdf.data(), pdf.data() + pdf.size()));
        writer.StoreAsync().get();
        writer.DetachStream();
    }
    stream.Seek(0);
    return PdfDocument::LoadFromStreamAsync(stream).get();
}

} // namespace

std::expected<std::pair<double, double>, std::string> pdfPageSizePoints(const std::vector<std::uint8_t>& pdf) {
    return onMtaThread<std::pair<double, double>>([&]() -> std::expected<std::pair<double, double>, std::string> {
        const PdfDocument document = openDocument(pdf);
        if (document.PageCount() == 0) {
            return std::unexpected(std::string("the PDF has no pages"));
        }
        const PdfPage page = document.GetPage(0);
        const auto size = page.Size(); // DIPs
        return std::pair{static_cast<double>(size.Width) * 72.0 / 96.0, static_cast<double>(size.Height) * 72.0 / 96.0};
    });
}

std::expected<std::vector<std::uint8_t>, std::string> pdfRenderPng(const std::vector<std::uint8_t>& pdf, int width, int height) {
    return onMtaThread<std::vector<std::uint8_t>>([&]() -> std::expected<std::vector<std::uint8_t>, std::string> {
        const PdfDocument document = openDocument(pdf);
        if (document.PageCount() == 0) {
            return std::unexpected(std::string("the PDF has no pages"));
        }
        const PdfPage page = document.GetPage(0);
        PdfPageRenderOptions options;
        options.DestinationWidth(static_cast<std::uint32_t>(width));
        options.DestinationHeight(static_cast<std::uint32_t>(height));
        options.BackgroundColor(winrt::Windows::UI::Color{0, 255, 255, 255}); // transparent
        InMemoryRandomAccessStream out;
        page.RenderToStreamAsync(out, options).get();
        const std::uint64_t bytes = out.Size();
        if (bytes == 0 || bytes > (std::uint64_t{1} << 30)) {
            return std::unexpected(std::string("Windows drew nothing"));
        }
        DataReader reader(out.GetInputStreamAt(0));
        reader.LoadAsync(static_cast<std::uint32_t>(bytes)).get();
        std::vector<std::uint8_t> png(static_cast<std::size_t>(bytes));
        reader.ReadBytes(winrt::array_view<std::uint8_t>(png.data(), png.data() + png.size()));
        return png;
    });
}

} // namespace wabcut::render::detail

#endif
