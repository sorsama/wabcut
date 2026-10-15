#include <wabcut/ai/Provenance.h>
#include <wabcut/ai/Sha256.h>
#include <fstream>
#include <wabcut/model/ProjectIO.h>
#include <algorithm>
namespace wabcut::ai {
namespace {
std::string xml(std::string_view text) {
    std::string out;
    for (char c : text) switch (c) {
        case '&': out += "&amp;"; break; case '<': out += "&lt;"; break; case '>': out += "&gt;"; break;
        case '\"': out += "&quot;"; break; case '\'': out += "&apos;"; break; default: out += c;
    }
    return out;
}
Json fields(const GeneratedProvenance& p) {
    return {{"aiGenerated", true}, {"provider", p.provider}, {"model", p.model}, {"promptHash", p.promptHash},
        {"createdAt", p.createdAt}, {"kind", p.kind}, {"operation", p.operation}, {"sourceMedia", p.source.value}};
}
std::expected<void, Error> write(const std::filesystem::path& path, std::string_view text) {
    return io::writeFileAtomically(path, text);
}
}
Json provenanceReport(const ProjectState& state, SequenceId id, std::optional<TimeRange> exportRange) {
    Json segments = Json::array();
    if (const auto* sequence = state.findSequence(id)) {
        for (auto kind : {TrackKind::Video, TrackKind::Audio}) for (const auto& track : sequence->tracks(kind)) {
            if (!track->enabled || track->muted) continue;
            for (const auto& clip : track->clips) {
                const auto* media = state.findMedia(clip.media);
                if (!clip.enabled || !media || !media->generated) continue;
                const Ticks start = exportRange ? std::max(clip.start(), exportRange->start) : clip.start();
                const Ticks end = exportRange ? std::min(clip.end(), exportRange->end()) : clip.end();
                if (end <= start) continue;
                auto segment = fields(*media->generated);
                segment["clip"] = clip.id.value; segment["media"] = media->id.value;
                segment["startTicks"] = (start - (exportRange ? exportRange->start : Ticks{})).value; segment["durationTicks"] = (end - start).value;
                segments.push_back(std::move(segment));
            }
        }
    }
    Json actions = Json::array({{{"action", "c2pa.edited"}, {"softwareAgent", "Wabcut"}}});
    if (!segments.empty()) actions.push_back({{"action", "c2pa.created"},
        {"digitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia"}});
    return {{"claim_generator", "Wabcut"}, {"title", "Edited with Wabcut"}, {"format", "video/mp4"},
        {"wabcut:unsigned", true}, {"wabcut:sequence", id.value}, {"wabcut:ticksPerSecond", kTicksPerSecond},
        {"assertions", Json::array({{{"label", "c2pa.actions"}, {"data", {{"actions", actions}}}},
                                    {{"label", "org.wabcut.ai-segments"}, {"data", segments}}})}};
}
std::string provenanceXmp(const GeneratedProvenance& p) {
    return "<?xpacket begin=''?>\n<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n"
        "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
        "<rdf:Description rdf:about='' xmlns:Iptc4xmpExt='http://iptc.org/std/Iptc4xmpExt/2008-02-29/' "
        "xmlns:wabcut='https://wabcut.app/ns/ai/1.0/' "
        "Iptc4xmpExt:DigitalSourceType='http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia' "
        "wabcut:AIGenerated='true' wabcut:Provider='" + xml(p.provider) + "' wabcut:Model='" + xml(p.model) +
        "' wabcut:PromptSHA256='" + xml(p.promptHash) + "' wabcut:CreatedAt='" + xml(p.createdAt) +
        "' wabcut:Operation='" + xml(p.operation) + "'/></rdf:RDF></x:xmpmeta>\n<?xpacket end='w'?>\n";
}
std::expected<void, Error> writeProvenanceSidecars(const std::filesystem::path& asset, const GeneratedProvenance& p) {
    auto path = asset; path += ".xmp"; return write(path, provenanceXmp(p));
}
std::expected<void, Error> writeExportProvenance(const std::filesystem::path& output,
    const ProjectState& state, SequenceId sequence) {
    if (!state.findSequence(sequence)) return std::unexpected(Error{ErrorCode::NotFound, "sequence not found"});
    auto report = provenanceReport(state, sequence);
    auto path = output; path += ".provenance.json";
    return write(path, report.dump(2) + "\n");
}
}
