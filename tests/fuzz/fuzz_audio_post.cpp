// Fuzz harness: the readers of AAF export's compound files (MS-CFB, AafReader.h) and OMF export's Bento containers
// (Bento.h), P.2. They read Wabcut's own output in tests, but take any file. A read either fails with a ParseError or
// gives a structure whose values stay inside the input.

#include "FuzzSupport.h"

#include "AafReader.h"
#include "Bento.h"

using namespace wabcut;
namespace aaf = wabcut::interchange::detail::aaf;
namespace bento = wabcut::interchange::detail::bento;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view body = fuzz::text(data, size);
    if (const auto container = bento::read(body, 1u << 20); container) {
        for (const auto& [object, properties] : container->objects) {
            for (const auto& [property, values] : properties) {
                for (const bento::ReadValue& v : values) {
                    WABCUT_FUZZ_CHECK(v.immediate || v.offset <= body.size(), "a value outside the file");
                    WABCUT_FUZZ_CHECK(v.data.size() <= v.size, "more data than the value's size");
                    if (v.data.size() >= 4) {
                        static_cast<void>(container->resolve(v, 0));
                    }
                }
            }
        }
    } else {
        WABCUT_FUZZ_CHECK(container.error().code == ErrorCode::ParseError, container.error().message);
    }
    if (const auto file = aaf::readAaf(std::string(body)); !file) {
        WABCUT_FUZZ_CHECK(file.error().code == ErrorCode::ParseError, file.error().message);
    }
    return 0;
}
