#include "ScaleProject.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ReviewOps.h>

#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace wabcut::test {

namespace {

constexpr Rate kScaleRate = kRate25;
constexpr std::int64_t kClipFrames = 10;
constexpr std::int64_t kMediaFrames = 1'000'000;

constexpr Ticks frames(std::int64_t n) { return framesToTicks(n, kScaleRate); }

[[noreturn]] void fail(const std::string& what, const Error& error) {
    throw std::runtime_error("makeScaleProject: " + what + ": " + error.message);
}

template <class T>
T take(std::expected<T, Error> outcome, const std::string& what) {
    if (!outcome) {
        fail(what, outcome.error());
    }
    return std::move(*outcome);
}

MediaItem makeMedia(int index) {
    char name[48];
    std::snprintf(name, sizeof name, "A%03d_C%03d_0101XY.mov", index / 10, index);
    MediaItem item;
    item.name = name;
    item.locator.path = std::string("D:/footage/") + name;
    item.info.path = item.locator.path;
    item.info.container = "mov,mp4,m4a,3gp,3g2,mj2";
    item.info.duration = frames(kMediaFrames);
    VideoStreamInfo video;
    video.index = 0;
    video.codec = "h264";
    video.width = 1920;
    video.height = 1080;
    video.frameRate = kScaleRate;
    video.duration = item.info.duration;
    video.frameCount = kMediaFrames;
    item.info.video.push_back(video);
    for (int i = 0; i < 2; ++i) {
        AudioStreamInfo audio;
        audio.index = 1 + i;
        audio.codec = "aac";
        audio.channels = 2;
        audio.channelLayout = "stereo";
        audio.duration = item.info.duration;
        item.info.audio.push_back(audio);
    }
    return item;
}

/// Sequence `seq` with `pairs` adjacent linked V1 / A1 clips (built directly: an edit per clip would be quadratic).
void fillSequence(ProjectState& state, SequenceId seqId, int pairs, int firstMedia) {
    auto seq = std::make_shared<Sequence>(*state.findSequence(seqId));
    auto v1 = std::make_shared<Track>(*seq->videoTracks.at(0));
    auto a1 = std::make_shared<Track>(*seq->audioTracks.at(0));
    for (int i = 0; i < pairs; ++i) {
        const MediaPtr& media = state.media[static_cast<std::size_t>(firstMedia + i) % state.media.size()];
        const LinkGroupId group = state.ids.allocate<LinkGroupId>();
        for (Track* track : {v1.get(), a1.get()}) {
            Clip c;
            c.id = state.ids.allocate<ClipId>();
            c.media = media->id;
            c.stream = {track == v1.get() ? StreamKind::Video : StreamKind::Audio, 0};
            c.timelineRange = {frames(i * kClipFrames), frames(kClipFrames)};
            c.sourceIn = frames(1000 + (i % 97) * kClipFrames);
            c.linkGroup = group;
            c.setContent(media->defaultClipContent());
            track->clips.push_back(std::move(c));
        }
    }
    seq->videoTracks[0] = std::move(v1);
    seq->audioTracks[0] = std::move(a1);
    seq->duration = frames(pairs * kClipFrames);
    state = state.withSequence(seq);
}

/// Edits sequence `source` (nested or multicam) into `target` on V2 / A2 at `at`.
void placeSequence(ProjectState& state, SequenceId target, SequenceId source, Ticks at, std::int64_t length) {
    const Sequence& seq = *state.findSequence(target);
    edit::SourceClip clip;
    clip.sequence = source;
    clip.sourceRange = {Ticks{}, frames(length)};
    clip.patches = {{{StreamKind::Video, 0}, seq.videoTracks.at(1)->id}, {{StreamKind::Audio, 0}, seq.audioTracks.at(1)->id}};
    state = take(edit::overwriteEdit(state, target, clip, at), "place a sequence").state;
}

/// A brand kit, a lower-third template that uses it, and an instance of it with presets on V2 of `seqId`.
void addGraphics(ProjectState& state, SequenceId seqId) {
    BrandKit kit;
    kit.name = "Studio";
    kit.colors = {BrandColor{"Primary", {0.9, 0.1, 0.4, 1.0}}, BrandColor{"Accent", {1.0, 0.8, 0.0, 1.0}}};
    kit.fonts = {BrandFont{"heading", "Noto Sans Thai", "Bold", 700}};
    kit.logos = {BrandLogo{"Main", state.media.front()->id}};
    state = take(edit::setBrandKit(state, kit), "set the brand kit").state;

    Graphic g;
    g.name = "Lower third";
    GraphicLayer name;
    name.id = 2;
    name.name = "Name";
    name.kind = LayerKind::Text;
    TextStyle style;
    style.brandFont = "heading";
    style.fontSize = 64.0;
    name.text.runs = {TextRun{"Name", style}};
    name.transform.x = ParamValue(120.0);
    name.transform.y = ParamValue(860.0);
    GraphicLayer box;
    box.id = 1;
    box.name = "Box";
    box.kind = LayerKind::Shape;
    box.shape.cornerRadius = ParamValue(12.0);
    box.shape.fill.color = GraphicColor{{0.1, 0.1, 0.1, 0.8}, "Primary"};
    box.pin = LayerPin{2, LayerPin::kLeft | LayerPin::kTop | LayerPin::kRight | LayerPin::kBottom, {-24.0, -12.0, 24.0, 12.0}};
    GraphicLayer logo;
    logo.id = 3;
    logo.name = "Logo";
    logo.kind = LayerKind::Image;
    logo.image.media = state.media.front()->id;
    logo.image.brandLogo = "Main";
    logo.transform.x = ParamValue(1700.0);
    logo.transform.y = ParamValue(120.0);
    g.layers = {box, name, logo};
    g.responsive = ResponsiveTime{frames(50), frames(10), frames(10)};
    const GraphicTemplate tmpl =
        take(edit::makeTemplate(g, "lower-third", "Lower third",
                                {TemplateField{"name", "Name", FieldKind::Text, 2, "text", {}},
                                 TemplateField{"color", "Box colour", FieldKind::Color, 1, "fill", {}},
                                 TemplateField{"logo", "Show logo", FieldKind::Checkbox, 3, "visible", {}}},
                                frames(50), "A name on a box"),
             "make a template");
    state = take(edit::saveProjectTemplate(state, tmpl), "save a template").state;

    FieldValue speaker;
    speaker.text = "Somchai";
    const TrackId v2 = state.findSequence(seqId)->videoTracks.at(1)->id;
    auto placed = take(edit::instantiateTemplate(state, seqId, v2, Ticks{}, frames(60), tmpl, {{"name", speaker}}), "place a graphic");
    state = std::move(placed.state);
    const ClipId clip = placed.placed.at(0);
    edit::PresetOptions typewriter;
    typewriter.kind = edit::PresetKind::Typewriter;
    state = take(edit::applyGraphicPreset(state, seqId, clip, 2, typewriter), "a typewriter").state;
    edit::PresetOptions fadeOut;
    fadeOut.phase = edit::PresetPhase::Out;
    state = take(edit::applyGraphicPreset(state, seqId, clip, 1, fadeOut), "a fade").state;
}

} // namespace

std::size_t countClips(const ProjectState& state) {
    std::size_t n = 0;
    for (const SequencePtr& s : state.sequences) {
        s->forEachTrack([&](const Track& t) { n += t.clips.size(); });
    }
    return n;
}

ScaleProject makeScaleProject(const ScaleSpec& spec) {
    if (spec.media < 4 || spec.sequences < 1 || spec.clipPairsPerSequence < 4) {
        throw std::invalid_argument("makeScaleProject: at least 4 media, 1 sequence and 4 clip pairs");
    }
    ScaleProject out;
    ProjectState& state = out.state;
    for (int m = 0; m < spec.media; ++m) {
        state = take(edit::addMedia(state, makeMedia(m)), "add media").state;
    }
    for (int s = 0; s < spec.sequences; ++s) {
        edit::SequenceSettings settings;
        settings.name = "Sequence " + std::to_string(s + 1);
        settings.frameRate = kScaleRate;
        settings.videoTracks = 2;
        settings.audioTracks = 2;
        auto created = take(edit::addSequence(state, settings), "add a sequence");
        state = std::move(created.state);
        out.sequences.push_back(created.id);
        fillSequence(state, created.id, spec.clipPairsPerSequence, s * 7);
    }
    const std::int64_t length = std::int64_t{spec.clipPairsPerSequence} * kClipFrames;

    // A chain of nested sequences: sequence k plays sequence k-1 (depth `nestingDepth`).
    for (int k = 1; k <= spec.nestingDepth && k < spec.sequences; ++k) {
        placeSequence(state, out.sequences[static_cast<std::size_t>(k)], out.sequences[static_cast<std::size_t>(k - 1)],
                      Ticks{}, std::min<std::int64_t>(length, 40));
    }
    // Multicam sources of four angles, each edited into sequences from the end of the list.
    std::size_t next = out.sequences.size();
    for (int m = 0; m < spec.multicamSources; ++m) {
        edit::MulticamSpec mc;
        mc.name = "Multicam " + std::to_string(m + 1);
        mc.sync = MulticamSync::Manual;
        for (int a = 0; a < 4; ++a) {
            mc.angles.push_back(state.media[static_cast<std::size_t>(m * 4 + a) % state.media.size()]->id);
            mc.offsets.push_back(frames(a * 3));
        }
        auto created = take(edit::createMulticam(state, mc), "create a multicam source");
        state = std::move(created.state);
        out.multicams.push_back(created.id);
        for (int c = 0; c < spec.multicamClipsPerSource && next > 0; ++c) {
            --next;
            placeSequence(state, out.sequences[next], created.id, frames(length / 2), std::min<std::int64_t>(length / 4, 50));
        }
    }
    for (std::size_t s = 0; s < out.sequences.size(); ++s) {
        const SequenceId seqId = out.sequences[s];
        // Effects and keyframes on the first clips of V1, then cross dissolves further along.
        for (int e = 0; e < spec.effectsPerSequence && e < spec.clipPairsPerSequence; ++e) {
            const Clip& clip = state.findSequence(seqId)->videoTracks[0]->clips[static_cast<std::size_t>(e)];
            const ClipId id = clip.id;
            const Ticks keyTime = clip.sourceIn + frames(2);
            state = take(edit::addEffect(state, seqId, std::span(&id, 1), "wabcut.blur.gaussian"), "add an effect").state;
            state = take(edit::addKeyframe(state, seqId, id, edit::ParamRef::of(ParamId::Opacity), Keyframe{keyTime, 0.5}),
                         "add a keyframe")
                        .state;
        }
        for (int t = 0; t < spec.transitionsPerSequence; ++t) {
            const std::size_t i = static_cast<std::size_t>(spec.effectsPerSequence + 2 + t * 3);
            const Sequence& seq = *state.findSequence(seqId);
            const Track& v1 = *seq.videoTracks[0];
            if (i + 1 >= v1.clips.size() || v1.clips[i].end() != v1.clips[i + 1].start()) {
                break;
            }
            edit::TransitionSpec transition;
            transition.outgoing = v1.clips[i].id;
            transition.incoming = v1.clips[i + 1].id;
            transition.before = frames(2);
            transition.after = frames(2);
            state = take(edit::addTransition(state, seqId, v1.id, transition), "add a transition").state;
        }
        for (int k = 0; k < spec.markersPerSequence; ++k) {
            Marker marker;
            marker.start = frames((k * 7) % length);
            marker.duration = k % 3 == 0 ? frames(5) : Ticks{};
            marker.name = "Marker " + std::to_string(k + 1);
            marker.comment = k % 2 == 0 ? "Check the sound here" : "";
            marker.colorIndex = k % 8;
            state = take(edit::addMarker(state, seqId, marker), "add a marker").state;
        }
        for (int n = 0; n < spec.notesPerSequence; ++n) {
            ReviewNote note;
            note.sequence = seqId;
            note.start = frames((n * 13) % length);
            note.duration = n % 2 == 0 ? frames(4) : Ticks{};
            if (n % 3 == 0) {
                note.mark = ReviewMark{0.1 * (n % 5), 0.2, 0.25, 0.1};
            }
            note.author = n % 2 == 0 ? "Client" : "Producer";
            note.text = "Note " + std::to_string(n + 1) + ": tighten this";
            note.createdAt = "2026-10-12T09:00:00Z";
            if (n % 2 == 1) {
                note.status = ReviewStatus::Resolved;
                note.replies.push_back(ReviewReply{"Editor", "Done in v2", "2026-10-12T10:00:00Z"});
            }
            state = take(edit::addReviewNote(state, std::move(note)), "add a review note").state;
        }
    }
    if (spec.graphics) {
        addGraphics(state, out.sequences.front());
    }
    if (auto valid = validate(state, Validation::Strict); !valid) {
        fail("validate", valid.error());
    }
    out.clips = countClips(state);
    return out;
}

} // namespace wabcut::test
