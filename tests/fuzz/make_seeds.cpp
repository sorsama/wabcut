// wabcut_fuzz_seeds <folder>: writes the generated seed corpora of the harnesses whose inputs are Wabcut's own formats,
// from the small every-shape project (ScaleProject.h), so the seeds follow the formats as they change and nothing
// large is checked in:
//   <folder>/project_io/      .wabcut files (the project, an empty one)
//   <folder>/export/          the same, for timeline export (X.1)
//   <folder>/history/         '0' + index.json, '1' + an object file, '2' + a revision's chunks ('\0'-separated)
//   <folder>/review_package/  notes.json of review packages
//   <folder>/clipboard/       clipboard documents (C.11)
//   <folder>/graphic/         '0' + a clip's graphic, '1' + its layers, '2' + the brand kit, '3' + a template file (C.13)
//   <folder>/command/         a request per catalog command (its documented example, and with real clip ids)
//   <folder>/mcp/             the same as MCP sessions (initialize, initialized, tools/call)
// ctest runs it before the fuzz tests (fixture fuzz_seeds); run it by hand before a libFuzzer run.

#include "FuzzRegistry.h"
#include "HistoryFormat.h"
#include "ProjectChunks.h"
#include "ScaleProject.h"

#include <wabcut/command/ReviewPackage.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/History.h>
#include <wabcut/model/Layouts.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Synthetic.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <string>

namespace fs = std::filesystem;
using namespace wabcut;

namespace {

class MemorySink final : public io::detail::ChunkSink {
public:
    std::optional<std::string> known(const std::shared_ptr<const void>&) override { return std::nullopt; }
    std::string put(const std::shared_ptr<const void>&, std::string text) override {
        std::string key = std::to_string(chunks.size());
        chunks.emplace_back(std::move(text));
        return key;
    }
    std::vector<std::string> chunks;
};

void write(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error("cannot write " + file.string());
    }
}

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

template <class T>
T take(std::expected<T, Error> value, const char* what) {
    if (!value) {
        throw std::runtime_error(std::string(what) + ": " + value.error().message);
    }
    return std::move(*value);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: wabcut_fuzz_seeds <folder>\n");
        return 2;
    }
    try {
        const fs::path out = argv[1];
        const test::ScaleProject small = test::makeScaleProject(test::smallScaleSpec());
        const ProjectState& state = small.state;

        // .wabcut files.
        write(out / "project_io" / "every-shape.wabcut", take(io::toJson(state), "save"));
        write(out / "project_io" / "empty.wabcut", take(io::toJson(ProjectState{}), "save an empty project"));
        write(out / "export" / "every-shape.wabcut", take(io::toJson(state), "save"));
        {
            // I.9: colour management and a clip's input colour space.
            ProjectState managed = state;
            managed.colorManagement.mode = ColorManagementMode::HdrPq;
            managed.colorManagement.peakNits = 4000.0;
            bool done = false;
            for (SequencePtr& s : managed.sequences) {
                for (std::size_t i = 0; i < s->videoTracks.size() && !done; ++i) {
                    Track track = *s->videoTracks[i];
                    for (std::size_t k = 0; k < track.clips.size(); ++k) {
                        Clip& c = track.clips.mutableAt(k);
                        if (!c.isNested() && !c.hasUnknownSource()) {
                            ClipContent content = c.content();
                            content.inputColorSpace = InputColorSpace::SLog3SGamut3Cine;
                            c.setContent(std::move(content));
                            done = true;
                            break;
                        }
                    }
                    if (done) {
                        Sequence copy = *s;
                        copy.videoTracks[i] = std::make_shared<const Track>(std::move(track));
                        s = std::make_shared<const Sequence>(std::move(copy));
                    }
                }
            }
            write(out / "project_io" / "colour-managed.wabcut", take(io::toJson(managed), "save a colour-managed project"));
        }

        // A history of two revisions: its index, an object, and each revision's chunks.
        const fs::path work = out / "work" / "history";
        fs::remove_all(work);
        ProjectHistory history = take(ProjectHistory::open(work), "open a history");
        take(history.checkpoint(state, HistoryKind::Save, "First cut", "2026-10-12T09:00:00Z"), "checkpoint");
        const SequenceId seq = small.sequences.front();
        const ClipId first = state.findSequence(seq)->videoTracks[0]->clips[0].id;
        const ProjectState edited =
            take(edit::trimEdge(state, seq, first, edit::Edge::Tail, -framesToTicks(2, kRate25)), "trim").state;
        take(history.checkpoint(edited, HistoryKind::Auto, {}, "2026-10-12T09:05:00Z"), "checkpoint");
        write(out / "history" / "index.json", "0" + read(work / "index.json"));
        for (const auto& entry : fs::recursive_directory_iterator(work / "objects")) {
            if (entry.is_regular_file()) {
                write(out / "history" / "object", "1" + read(entry.path()));
                break;
            }
        }
        int n = 0;
        for (const ProjectState* revision : {&state, &edited}) {
            MemorySink sink;
            take(io::detail::writeChunks(*revision, sink), "write chunks");
            std::string joined = "2";
            for (std::size_t i = 0; i < sink.chunks.size(); ++i) {
                joined += (i == 0 ? "" : std::string(1, '\0')) + sink.chunks[i];
            }
            write(out / "history" / ("chunks-" + std::to_string(++n)), joined);
        }

        // Review packages: all notes, and the open ones only.
        std::vector<ReviewNote> open;
        for (const ReviewNote& note : state.reviewNotes) {
            if (note.status == ReviewStatus::Open) {
                open.push_back(note);
            }
        }
        const command::PlaceholderFrames frames;
        int k = 0;
        const std::vector<ReviewNote>* lists[] = {&state.reviewNotes.get(), &open};
        for (const std::vector<ReviewNote>* notes : lists) {
            const fs::path package = out / "work" / ("package-" + std::to_string(++k));
            take(command::writeReviewPackage(state, package, *notes, &frames, "Client cut", "2026-10-12T09:00:00Z"),
                 "write a review package");
            write(out / "review_package" / ("notes-" + std::to_string(k) + ".json"), read(package / "notes.json"));
        }
        // Graphics (C.13): the template instance on V2 of the first sequence, its layers, the brand kit, the template.
        {
            const Graphic* graphic = nullptr;
            state.findSequence(small.sequences.front())->forEachTrack([&](const Track& t) {
                for (const Clip& c : t.clips) {
                    if (c.graphic()) {
                        graphic = c.graphic();
                    }
                }
            });
            if (!graphic || !state.brandKit || state.templates.size() == 0) {
                throw std::runtime_error("the small project has no graphics");
            }
            write(out / "graphic" / "graphic.json", "0" + io::graphicToJson(*graphic));
            int layers = 0;
            for (const GraphicLayer& l : graphic->layers) {
                write(out / "graphic" / ("layer-" + std::to_string(++layers) + ".json"), "1" + io::layerToJson(l));
            }
            write(out / "graphic" / "brand-kit.json", "2" + io::brandKitToJson(*state.brandKit));
            write(out / "graphic" / "template.json", "3" + io::templateFileToJson(state.templates[0]));
        }
        // Clipboard documents (C.11): two clips with their partners, a whole sequence (nested and multicam clips,
        // effects, transitions, markers), and the same with its captions.
        {
            int copies = 0;
            for (std::size_t s = 0; s < 2 && s < small.sequences.size(); ++s) {
                const SequenceId id = small.sequences[s];
                std::vector<ClipId> clips;
                state.findSequence(id)->forEachTrack([&](const Track& t) {
                    for (const Clip& c : t.clips) {
                        clips.push_back(c.id);
                    }
                });
                if (s == 0) {
                    clips.resize(std::min<std::size_t>(clips.size(), 2));
                }
                for (const bool captions : {false, true}) {
                    CopyOptions options;
                    options.captions = captions;
                    const ClipboardDocument doc = take(copyClips(state, id, clips, options), "copy clips");
                    write(out / "clipboard" / ("clipboard-" + std::to_string(++copies) + ".json"), take(clipboardToJson(doc), "clipboard JSON"));
                }
            }
        }

        // Command requests and MCP sessions: every catalog command's documented example, as is and with its clip ids
        // replaced by real ones, on the project's first sequence.
        const Sequence& firstSeq = *state.findSequence(seq);
        const command::Json clipIds = command::Json::array({firstSeq.videoTracks[0]->clips[1].id.value,
                                                            firstSeq.videoTracks[0]->clips[2].id.value});
        for (const command::CommandSpec& spec : fuzz::safeCommands().all()) {
            command::Json params = spec.example.is_object() ? spec.example : command::Json::object();
            params["sequence"] = seq.value;
            command::Json real = params;
            if (real.contains("clip")) {
                real["clip"] = clipIds[0];
            }
            if (real.contains("clips")) {
                real["clips"] = clipIds;
            }
            int variant = 0;
            for (const command::Json& p : {params, real}) {
                const std::string stem = spec.name + "-" + std::to_string(++variant);
                write(out / "command" / (stem + ".json"), command::Json{{"command", spec.name}, {"params", p}}.dump());
                const std::string session =
                    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"fuzz","version":"1"}}})"
                    "\n"
                    R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})"
                    "\n" +
                    command::Json{{"jsonrpc", "2.0"},
                                  {"id", 2},
                                  {"method", "tools/call"},
                                  {"params", {{"name", command::toolNameFor(spec.name)}, {"arguments", p}}}}
                        .dump();
                write(out / "mcp" / (stem + ".jsonl"), session);
            }
        }
        // C.15: blend modes, keyers, a track matte, every synthetic kind and an adjustment layer (project_io and the
        // preview fuzzer), and a preview cache index.
        {
            ProjectState parity = state;
            const ClipId video = parity.findSequence(seq)->videoTracks[0]->clips[0].id;
            edit::ClipAttributeChange blend;
            blend.blendMode = BlendMode::Screen;
            parity = take(edit::setClipAttributes(parity, seq, std::span(&video, 1), blend), "set a blend mode").state;
            parity = take(edit::addEffect(parity, seq, std::span(&video, 1), std::string(kUltraKeyEffect)), "key").state;
            parity = take(edit::addEffect(parity, seq, std::span(&video, 1), std::string(kTrackMatteEffect),
                                          {EffectParam{"matteTrack", 2.0}}),
                          "track matte")
                         .state;
            for (const SyntheticDescriptor& d : syntheticKinds()) {
                parity = take(edit::addSyntheticMedia(parity, d.kind, {}, seq), "a synthetic item").state;
            }
            LayoutRequest pip; // a facecam cut from the capture: a copy with crop, a rounded mask, a border, a shadow
            pip.gameplay = video;
            pip.options.facecamRegion = NormalizedRect{0.7, 0.7, 0.3, 0.3};
            parity = take(edit::applyLayout(parity, seq, pip), "a layout").edit.state;
            const auto adjustmentTrack = take(edit::addTrack(parity, seq, TrackKind::Video), "a track");
            parity = adjustmentTrack.state;
            parity = take(edit::addAdjustmentLayer(parity, seq, adjustmentTrack.id, framesToTicks(1, kRate25),
                                                   framesToTicks(10, kRate25)),
                          "an adjustment layer")
                         .state;
            Marker clutch{framesToTicks(2, kRate25), framesToTicks(4, kRate25), "Clutch"}; // a highlight finder's marker
            clutch.highlight = MarkerHighlight{0.87, HighlightConfidence::High, {"kill streak", "mic peak"},
                                               framesToTicks(3, kRate25), "highlights"};
            parity = take(edit::addMarker(parity, seq, clutch), "a highlight marker").state;
            const std::string json = take(io::toJson(parity), "save");
            write(out / "project_io" / "parity.wabcut", json);
            write(out / "preview" / "project.wabcut", "1" + json);
            const std::vector<PreviewEntry> entries{
                PreviewEntry{seq, TimeRange{Ticks{}, framesToTicks(10, kRate25)}, "0123456789abcdef", "1-0-0123456789abcdef.wpv", 4096, 3},
                PreviewEntry{seq, TimeRange{framesToTicks(10, kRate25), framesToTicks(5, kRate25)}, "fedcba9876543210",
                             "1-10-fedcba9876543210.wpv", 2048, 4}};
            write(out / "preview" / "index.json", "0" + PreviewCache::indexJson(entries));
        }
        fs::remove_all(out / "work");
        std::printf("fuzz seeds written to %s\n", out.string().c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wabcut_fuzz_seeds: %s\n", e.what());
        return 1;
    }
}
