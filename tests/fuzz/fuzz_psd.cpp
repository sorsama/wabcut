// Fuzz harness: layered Photoshop files (0.2, media/Psd.h) - files from anywhere, so untrusted. The bytes are read as a
// PSD / PSB: its layer records, then the composite of every visible layer on the document's canvas and of the first
// layers on their own bounds. Every call returns a document, a picture or an error; a picture is as large as it says.

#include "FuzzSupport.h"

#include <wabcut/media/Psd.h>

#include <cstdlib>
#include <span>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const auto doc = media::readPsdDocument(bytes);
    if (!doc) {
        return 0;
    }
    if (doc->width > 2048 || doc->height > 2048) {
        return 0; // a valid header of a huge canvas: compositing it is only slow
    }
    for (const bool documentSize : {true, false}) {
        const std::vector<int> first{0};
        const auto picture = media::compositePsd(bytes, documentSize ? std::span<const int>{} : std::span<const int>(first),
                                                 documentSize);
        if (picture && picture->rgba.size() != static_cast<std::size_t>(picture->width) * picture->height * 4) {
            std::abort();
        }
    }
    return 0;
}
