// Export, step one (Exporters.h): a project's sequence and the sequences it plays as an IrDocument.

#include "Exporters.h"

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/TitleOps.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace wabcut::interchange::detail {

namespace {

std::string trackLabel(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
}

/// The IR kind and the name other editors know for a model transition effect.
std::pair<TransitionKind, std::string> transitionOf(const std::string& effect, TrackKind kind) {
    if (kind == TrackKind::Audio) {
        return {TransitionKind::AudioCrossfade, effect == "constantGain" ? "Constant Gain" : "Constant Power"};
    }
    if (effect == "crossDissolve") {
        return {TransitionKind::Dissolve, "Cross Dissolve"};
    }
    if (effect == "dipToBlack") {
        return {TransitionKind::DipToBlack, "Dip to Black"};
    }
    if (effect == "dipToWhite") {
        return {TransitionKind::DipToWhite, "Dip to White"};
    }
    if (effect == "wipe") {
        return {TransitionKind::Wipe, "Wipe"};
    }
    return {TransitionKind::Other, effect};
}

class Describer {
public:
    Describer(const ProjectState& state, const ExportContext& context) : m_state(state), m_ctx(context) {}

    std::expected<ExportDocument, Error> run(SequenceId root) {
        if (!m_state.findSequence(root)) {
            return std::unexpected(Error{ErrorCode::NotFound, "sequence " + std::to_string(root.value) + " does not exist"});
        }
        slotOf(root);
        // Describing a sequence may add the sequences it plays; they are described in turn.
        for (std::size_t i = 0; i < m_out.sequenceIds.size(); ++i) {
            describeSequence(i);
        }
        return std::move(m_out);
    }

private:
    std::size_t slotOf(SequenceId id) {
        const auto [it, inserted] = m_sequenceSlots.emplace(id.value, m_out.sequenceIds.size());
        if (inserted) {
            m_out.sequenceIds.push_back(id);
            m_out.ir.sequences.emplace_back();
        }
        return it->second;
    }

    std::size_t mediaSlot(const MediaItem& item) {
        if (const auto it = m_mediaSlots.find(item.id.value); it != m_mediaSlots.end()) {
            return it->second;
        }
        IrMedia media;
        media.key = "media-" + std::to_string(item.id.value);
        media.name = item.name;
        std::string path = item.locator.path;
        if (path.empty() && !item.locator.relativePath.empty() && m_ctx.options && !m_ctx.options->projectFile.empty()) {
            const std::filesystem::path resolved =
                (m_ctx.options->projectFile.parent_path() / pathFromUtf8(item.locator.relativePath)).lexically_normal();
            path = pathToUtf8(resolved);
        }
        std::replace(path.begin(), path.end(), '\\', '/');
        media.reference.name = item.name;
        media.reference.path = path;
        media.reference.original = path;
        media.hasVideo = item.info.hasVideo();
        media.stillImage = item.info.isStillImage;
        if (!item.info.video.empty()) {
            const VideoStreamInfo& v = item.info.video.front();
            media.width = v.width;
            media.height = v.height;
            if (v.frameRate.isValid()) {
                media.frameRate = v.frameRate.normalized();
            }
        }
        for (const AudioStreamInfo& a : item.info.audio) {
            media.audioChannels.push_back(std::max(a.channels, 1));
            media.sampleRate = a.sampleRate;
        }
        if (item.info.duration.value > 0) {
            media.duration = item.info.duration;
        }
        media.startTimecode = item.info.startTimecode;
        for (const Marker& m : item.markers) { // X.1: the source's markers (Premiere's clip markers)
            IrMarker marker;
            marker.marker = m;
            marker.location = SourceLocation{m_ctx.fileLabel, "media '" + item.name + "' marker '" + m.name + "'", 0};
            media.markers.push_back(std::move(marker));
        }
        media.location = SourceLocation{m_ctx.fileLabel, "media '" + item.name + "'", 0};
        if (path.empty()) {
            m_ctx.add(ReportOutcome::Approximated, ReportCategory::Media,
                      "media '" + item.name + "' has no file path; it is written by name only (offline in the other editor)",
                      media.location, {});
        }
        const std::size_t index = m_out.ir.media.size();
        m_out.ir.media.push_back(std::move(media));
        m_mediaSlots.emplace(item.id.value, index);
        return index;
    }

    std::int64_t linkOf(LinkGroupId group) {
        if (!group.isValid()) {
            return -1;
        }
        const auto [it, inserted] = m_links.emplace(group.value, m_nextLink);
        if (inserted) {
            ++m_nextLink;
        }
        return it->second;
    }

    void describeSequence(std::size_t slot) {
        const Sequence& s = *m_state.findSequence(m_out.sequenceIds[slot]);
        IrSequence seq;
        seq.key = "sequence-" + std::to_string(slot + 1);
        seq.name = s.name;
        seq.frameRate = s.frameRate;
        seq.dropFrame = s.dropFrame;
        seq.width = s.width;
        seq.height = s.height;
        seq.audioRate = s.audioRate;
        seq.markIn = s.markIn;
        seq.markOut = s.markOut;
        seq.duration = s.duration;
        if (s.startTimecode.value != 0) {
            seq.startTimecode = s.startTimecode;
        }
        seq.location = SourceLocation{m_ctx.fileLabel, "'" + s.name + "'", 0};
        if (s.multicam) {
            IrMulticam multicam;
            multicam.sync = s.multicam->sync;
            for (const MulticamAngle& a : s.multicam->angles) {
                IrMulticamAngle angle;
                angle.name = a.name;
                angle.offset = a.offset;
                if (const auto pos = a.video.isValid() ? s.trackPosition(a.video) : std::nullopt) {
                    angle.videoTrack = pos->index;
                }
                for (const TrackId t : a.audio) {
                    if (const auto pos = s.trackPosition(t)) {
                        angle.audioTracks.push_back(pos->index);
                    }
                }
                multicam.angles.push_back(std::move(angle));
            }
            seq.multicam = std::move(multicam);
        }
        for (const Marker& m : s.markers) {
            IrMarker marker;
            marker.marker = m;
            marker.location = SourceLocation{m_ctx.fileLabel, "'" + s.name + "' marker '" + m.name + "' at " +
                                                                  ticksToTimecode(m.start, s.frameRate, s.dropFrame),
                                             0};
            seq.markers.push_back(std::move(marker));
        }
        // The sequence's own name for messages before the IR sequence is stored.
        m_current = &seq;
        for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            const auto& tracks = s.tracks(kind);
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                (kind == TrackKind::Video ? seq.videoTracks : seq.audioTracks).push_back(describeTrack(s, *tracks[i], kind, i));
            }
        }
        m_current = nullptr;
        m_out.ir.sequences[slot] = std::move(seq);
    }

    IrTrack describeTrack(const Sequence& s, const Track& t, TrackKind kind, std::size_t index) {
        IrTrack track;
        track.kind = kind;
        track.name = t.name;
        track.enabled = t.enabled;
        track.locked = t.locked;
        track.targeted = t.targeted;
        track.muted = t.muted;
        track.role = t.role;
        track.location = SourceLocation{m_ctx.fileLabel, "'" + s.name + "' " + trackLabel(kind, index), 0};
        for (const Clip& c : t.clips) {
            if (auto clip = describeClip(s, c, kind, index)) {
                track.clips.push_back(std::move(*clip));
            }
        }
        for (const Transition& tr : t.transitions) {
            const auto cut = transitionEditPoint(t, tr);
            if (!cut) {
                continue;
            }
            IrTransition transition;
            std::tie(transition.kind, transition.name) = transitionOf(tr.effect, kind);
            transition.effect = tr.effect;
            transition.cut = *cut;
            transition.before = tr.before;
            transition.after = tr.after;
            transition.location = SourceLocation{
                m_ctx.fileLabel,
                exportLocation(*m_current, kind, index, "transition '" + transition.name + "'", *cut), 0};
            track.transitions.push_back(std::move(transition));
        }
        return track;
    }

    std::optional<IrClip> describeClip(const Sequence& s, const Clip& c, TrackKind kind, std::size_t index) {
        IrClip clip;
        clip.name = c.name();
        clip.stream = c.stream;
        clip.start = c.start();
        clip.duration = c.duration();
        clip.sourceIn = c.sourceIn;
        clip.enabled = c.enabled;
        clip.linkGroup = linkOf(c.linkGroup);
        clip.location =
            SourceLocation{m_ctx.fileLabel, exportLocation(*m_current, kind, index, "'" + c.name() + "'", c.start()), 0};
        if (!(c.properties() == ClipProperties{})) {
            clip.properties = c.properties();
        }
        clip.channels = c.content().channels;
        clip.effects = c.effects();
        if (c.hasUnknownSource()) {
            m_ctx.add(ReportOutcome::Dropped, ReportCategory::Clip,
                      "'" + c.name() + "' plays a source of a newer Wabcut; its time is left as a gap", clip.location, s.name);
            return std::nullopt;
        }
        if (c.isNested()) {
            clip.sequence = slotOf(c.sequence);
            for (std::size_t a = 0; a < kMaxMulticamAngles; ++a) {
                if (c.angles.contains(a)) {
                    clip.angles.push_back(a);
                }
            }
        } else {
            const MediaItem* media = m_state.findMedia(c.media);
            if (!media) {
                return std::nullopt; // validate() rules it out
            }
            if (media->isTitle()) {
                IrTitle title;
                title.text = media->generator->text;
                title.fontFamily = media->generator->fontFamily;
                title.fontStyle = media->generator->fontStyle;
                const auto style = std::find_if(clip.effects.begin(), clip.effects.end(), [](const EffectInstance& e) {
                    return e.effect == edit::kTitleStyleEffect;
                });
                if (style != clip.effects.end()) {
                    for (const EffectParam& p : style->params) {
                        if (p.value.isConstant()) {
                            title.style.push_back(p);
                        } else {
                            title.style.push_back(EffectParam{p.key, ParamValue(p.value.constant())});
                            title.approximation += std::string(title.approximation.empty() ? "" : "; ") + "its " + p.key +
                                                   " keyframes became the first key's value";
                        }
                    }
                    clip.effects.erase(style);
                }
                clip.title = std::move(title);
            } else if (media->isGenerator()) {
                m_ctx.add(ReportOutcome::Dropped, ReportCategory::Graphic,
                          "'" + c.name() + "' is a " + (media->isGraphic() ? std::string("motion graphic") : media->generator->kind) +
                              " clip, which interchange formats cannot describe; its time is left as a gap",
                          clip.location, s.name);
                return std::nullopt;
            } else {
                clip.media = mediaSlot(*media);
            }
        }
        if (const Curve* remap = c.timeRemap()) {
            IrRemap r;
            r.curve = *remap;
            r.curveIn = c.sourceIn;
            r.location = clip.location;
            clip.remap = std::move(r);
        } else if (c.isFreeze() || !c.speed.isUnity() || c.reversed) {
            IrSpeed speed;
            speed.ratio = c.speed;
            speed.reversed = c.reversed && !c.isFreeze();
            speed.location = clip.location;
            clip.speed = speed;
        }
        return clip;
    }

    const ProjectState& m_state;
    const ExportContext& m_ctx;
    ExportDocument m_out;
    std::unordered_map<std::uint64_t, std::size_t> m_sequenceSlots;
    std::unordered_map<std::uint64_t, std::size_t> m_mediaSlots;
    std::unordered_map<std::uint64_t, std::int64_t> m_links;
    std::int64_t m_nextLink = 0;
    const IrSequence* m_current = nullptr;
};

} // namespace

std::string exportLocation(const IrSequence& sequence, TrackKind kind, std::size_t track, const std::string& what,
                           Ticks at) {
    return "'" + sequence.name + "' " + trackLabel(kind, track) + " " + what + " at " +
           ticksToTimecode(at, sequence.frameRate, sequence.dropFrame);
}

std::expected<ExportDocument, Error> describeForExport(const ProjectState& project, SequenceId sequence,
                                                       const ExportContext& context) {
    Describer describer(project, context);
    return describer.run(sequence);
}

} // namespace wabcut::interchange::detail
