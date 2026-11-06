#include <wabcut/export/DynamicMetadata.h>

#include <mutex>

namespace wabcut::exporter {

namespace {

std::mutex g_mutex;
std::shared_ptr<DolbyVisionWriter> g_writer;

} // namespace

void DolbyVisionRegistry::set(std::shared_ptr<DolbyVisionWriter> writer) {
    std::lock_guard lock(g_mutex);
    g_writer = std::move(writer);
}

std::shared_ptr<DolbyVisionWriter> DolbyVisionRegistry::get() {
    std::lock_guard lock(g_mutex);
    return g_writer;
}

std::string DolbyVisionRegistry::unavailableReason(DynamicMetadata kind) {
    if (kind != DynamicMetadata::DolbyVision81 && kind != DynamicMetadata::DolbyVision84) {
        return {};
    }
    const std::shared_ptr<DolbyVisionWriter> writer = get();
    if (!writer) {
        return "Dolby Vision needs a Dolby licence. Dolby licenses its metadata format and certifies the files, and this build "
               "of Wabcut has no licensed Dolby Vision writer. HDR10, HDR10+ and HLG exports are not affected.";
    }
    if (auto supported = writer->supports(kind); !supported) {
        return supported.error().message;
    }
    return {};
}

} // namespace wabcut::exporter
