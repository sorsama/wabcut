#include "TimelineFixture.h"

#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <vector>

namespace wabcut::test {

namespace {

constexpr std::int64_t kDefaultMediaFrames = 1000;

struct Line {
    std::string label;
    std::vector<std::string> flags;
    std::string cells;
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::vector<Line> parsePicture(std::string_view picture) {
    std::vector<Line> lines;
    while (!picture.empty()) {
        const auto newline = picture.find('\n');
        const std::string_view raw = trim(picture.substr(0, newline));
        picture = newline == std::string_view::npos ? std::string_view{} : picture.substr(newline + 1);
        if (raw.empty()) {
            continue;
        }
        const auto open = raw.find('|');
        const auto close = raw.rfind('|');
        INFO("picture line: " << raw);
        REQUIRE(open != std::string_view::npos);
        REQUIRE(close > open);
        Line line;
        std::string_view head = raw.substr(0, open);
        while (!(head = trim(head)).empty()) {
            const auto space = head.find_first_of(" \t");
            const std::string_view word = head.substr(0, space);
            if (line.label.empty()) {
                line.label = std::string(word);
            } else {
                line.flags.emplace_back(word);
            }
            head = space == std::string_view::npos ? std::string_view{} : head.substr(space);
        }
        line.cells = std::string(raw.substr(open + 1, close - open - 1));
        lines.push_back(std::move(line));
    }
    return lines;
}

struct TrackLabel {
    TrackKind kind;
    std::size_t index;
};

TrackLabel parseLabel(std::string_view label) {
    INFO("track label: " << label);
    REQUIRE(label.size() >= 2);
    REQUIRE((label[0] == 'V' || label[0] == 'A'));
    const int number = std::stoi(std::string(label.substr(1)));
    REQUIRE(number >= 1);
    return {label[0] == 'V' ? TrackKind::Video : TrackKind::Audio, static_cast<std::size_t>(number - 1)};
}

std::string labelOf(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
}

std::int64_t frameOf(Ticks t) { return t.value / kRate.ticksPerUnit(); }
bool onFrame(Ticks t) { return t.value % kRate.ticksPerUnit() == 0; }

MediaItem makeMedia(char letter, std::int64_t frames) {
    MediaItem item;
    item.name = std::string(1, letter);
    item.locator.path = "D:/footage/" + item.name + ".mov";
    item.info.path = item.locator.path;
    item.info.container = "mov,mp4,m4a,3gp,3g2,mj2";
    item.info.duration = f(frames);
    VideoStreamInfo video;
    video.index = 0;
    video.codec = "h264";
    video.width = 1920;
    video.height = 1080;
    video.frameRate = kRate;
    video.duration = f(frames);
    video.frameCount = frames;
    item.info.video.push_back(video);
    for (int i = 0; i < 2; ++i) {
        AudioStreamInfo audio;
        audio.index = 1 + i;
        audio.codec = "aac";
        audio.channels = 2;
        audio.channelLayout = "stereo";
        audio.duration = f(frames);
        item.info.audio.push_back(audio);
    }
    return item;
}

} // namespace

Timeline::Timeline(std::string_view picture, const std::map<char, std::int64_t>& mediaFrames) {
    const std::vector<Line> lines = parsePicture(picture);
    std::size_t videoTracks = 0;
    std::size_t audioTracks = 0;
    std::set<char> letters;
    for (const Line& line : lines) {
        const TrackLabel label = parseLabel(line.label);
        auto& count = label.kind == TrackKind::Video ? videoTracks : audioTracks;
        count = std::max(count, label.index + 1);
        for (const char c : line.cells) {
            if (std::isupper(static_cast<unsigned char>(c))) {
                letters.insert(c);
            }
        }
    }
    REQUIRE(videoTracks + audioTracks == lines.size()); // every track listed exactly once
    // Every letter is available as a source, whether or not the picture uses it.
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        letters.insert(letter);
    }

    for (const char letter : letters) {
        const auto it = mediaFrames.find(letter);
        auto added = edit::addMedia(state, makeMedia(letter, it == mediaFrames.end() ? kDefaultMediaFrames : it->second));
        REQUIRE(added);
        state = std::move(added->state);
    }
    edit::SequenceSettings settings;
    settings.name = "Fixture";
    settings.frameRate = kRate;
    settings.videoTracks = static_cast<std::int32_t>(videoTracks);
    settings.audioTracks = static_cast<std::int32_t>(audioTracks);
    auto created = edit::addSequence(state, settings);
    REQUIRE(created);
    state = std::move(created->state);
    seq = created->id;

    auto sequence = std::make_shared<Sequence>(*state.findSequence(seq));
    for (const Line& line : lines) {
        const TrackLabel label = parseLabel(line.label);
        auto& list = label.kind == TrackKind::Video ? sequence->videoTracks : sequence->audioTracks;
        auto track = std::make_shared<Track>(*list[label.index]);
        track->targeted = false;
        for (const std::string& flag : line.flags) {
            INFO("track flag: " << flag);
            if (flag == "tgt") {
                track->targeted = true;
            } else if (flag == "lock") {
                track->locked = true;
            } else if (flag == "nosync") {
                track->syncLocked = false;
            } else if (flag == "mute") {
                track->muted = true;
            } else if (flag == "solo") {
                track->solo = true;
            } else if (flag == "off") {
                track->enabled = false;
            } else if (const auto role = trackRoleFromString(flag); role && *role != TrackRole::None) {
                track->role = *role; // dialogue, music, effects
            } else {
                FAIL("unknown track flag");
            }
        }
        const std::string& cells = line.cells;
        for (std::size_t i = 0; i < cells.size();) {
            const char c = cells[i];
            if (c == '.') {
                ++i;
                continue;
            }
            INFO("track " << line.label << " cell " << i << " '" << c << "'");
            REQUIRE(std::isupper(static_cast<unsigned char>(c)));
            const std::size_t start = i++;
            std::int64_t sourceIn = 0;
            while (i < cells.size() && std::isdigit(static_cast<unsigned char>(cells[i]))) {
                sourceIn = sourceIn * 10 + (cells[i++] - '0');
            }
            const char rest = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            while (i < cells.size() && cells[i] == rest) {
                ++i;
            }
            Clip clip;
            clip.id = state.ids.allocate<ClipId>();
            clip.media = media(c);
            clip.stream = {streamKindOf(track->kind), 0};
            clip.timelineRange = {f(static_cast<std::int64_t>(start)), f(static_cast<std::int64_t>(i - start))};
            clip.sourceIn = f(sourceIn);
            clip.setContent(state.findMedia(clip.media)->defaultClipContent()); // named after the media
            track->clips.push_back(std::move(clip));
        }
        list[label.index] = std::move(track);
    }
    const IdGenerator ids = state.ids;
    sequence->duration = sequence->contentEnd();
    state = state.withSequence(std::move(sequence));
    state.ids = ids;
    requireValid(state);
}

const Sequence& Timeline::sequence() const { return *state.findSequence(seq); }

TrackId Timeline::track(std::string_view label) const {
    const TrackLabel l = parseLabel(label);
    const auto& list = sequence().tracks(l.kind);
    REQUIRE(l.index < list.size());
    return list[l.index]->id;
}

ClipId Timeline::clip(std::string_view ref) const { return clipAt(state, seq, ref); }

MediaId Timeline::media(char letter) const {
    for (const MediaPtr& m : state.media) {
        if (m->name == std::string(1, letter)) {
            return m->id;
        }
    }
    FAIL("no media " << letter);
    return {};
}

Timeline& Timeline::link(std::string_view refs) {
    std::vector<ClipId> ids;
    while (!(refs = trim(refs)).empty()) {
        const auto space = refs.find_first_of(" \t");
        ids.push_back(clip(refs.substr(0, space)));
        refs = space == std::string_view::npos ? std::string_view{} : refs.substr(space);
    }
    state = ok(edit::linkClips(state, seq, ids)).state;
    return *this;
}

Timeline& Timeline::linkMatching() {
    std::vector<std::vector<ClipId>> groups;
    for (const TrackPtr& v : sequence().videoTracks) {
        for (const Clip& vc : v->clips) {
            std::vector<ClipId> group{vc.id};
            for (const TrackPtr& a : sequence().audioTracks) {
                for (const Clip& ac : a->clips) {
                    if (ac.media == vc.media && ac.timelineRange == vc.timelineRange) {
                        group.push_back(ac.id);
                    }
                }
            }
            if (group.size() > 1) {
                groups.push_back(std::move(group));
            }
        }
    }
    for (const auto& group : groups) {
        state = ok(edit::linkClips(state, seq, group)).state;
    }
    return *this;
}

ClipId clipAt(const ProjectState& state, SequenceId seqId, std::string_view ref) {
    INFO("clip ref " << ref);
    const auto at = ref.find('@');
    REQUIRE(at != std::string_view::npos);
    const TrackLabel label = parseLabel(ref.substr(0, at));
    const std::int64_t frame = std::stoll(std::string(ref.substr(at + 1)));
    const Sequence* s = state.findSequence(seqId);
    REQUIRE(s);
    const auto& list = s->tracks(label.kind);
    REQUIRE(label.index < list.size());
    for (const Clip& c : list[label.index]->clips) {
        if (c.start() == f(frame)) {
            return c.id;
        }
    }
    FAIL("no clip starts at that frame");
    return {};
}

std::string normalize(std::string_view picture) {
    const std::vector<Line> lines = parsePicture(picture);
    std::size_t width = 0;
    for (const Line& line : lines) {
        width = std::max(width, line.cells.size());
    }
    std::string out;
    for (const Line& line : lines) {
        if (!out.empty()) {
            out += '\n';
        }
        // Short lines are padded with gaps, so "A2 |M|" may stand for a mostly empty track.
        out += line.label + " |" + line.cells + std::string(width - line.cells.size(), '.') + "|";
    }
    return out;
}

std::string render(const ProjectState& state, SequenceId seqId, std::string_view widthFrom) {
    const Sequence* s = state.findSequence(seqId);
    REQUIRE(s);
    std::size_t width = 0;
    for (const Line& line : parsePicture(widthFrom)) {
        width = std::max(width, line.cells.size());
    }
    width = std::max(width, static_cast<std::size_t>((s->contentEnd().value + kRate.ticksPerUnit() - 1) /
                                                     kRate.ticksPerUnit()));

    const auto renderTrack = [&](const Track& t, const std::string& label) {
        std::string cells(width, '.');
        for (const Clip& c : t.clips) {
            // A clip of a sequence (C.8) draws with the first letter of that sequence's name.
            const MediaItem* media = state.findMedia(c.media);
            const Sequence* nested = c.isNested() ? state.findSequence(c.sequence) : nullptr;
            const std::string& source = nested ? nested->name : media ? media->name : std::string();
            const char letter = !source.empty() ? source[0] : '?';
            const auto begin = static_cast<std::size_t>(frameOf(c.start()));
            const auto length = static_cast<std::size_t>(frameOf(c.duration()));
            std::string text(1, letter);
            if (c.sourceIn.value != 0) {
                text += std::to_string(frameOf(c.sourceIn));
            }
            const bool aligned = onFrame(c.start()) && onFrame(c.duration()) && onFrame(c.sourceIn);
            for (std::size_t k = 0; k < length && begin + k < cells.size(); ++k) {
                char cell = k < text.size() ? text[k] : static_cast<char>(std::tolower(letter));
                if (!aligned) {
                    cell = '~'; // off-frame clip: cannot be drawn
                } else if (text.size() > length) {
                    cell = '?'; // source In does not fit in the clip's width
                }
                cells[begin + k] = cells[begin + k] == '.' ? cell : '*';
            }
        }
        return label + " |" + cells + "|";
    };

    std::vector<std::string> lines;
    for (std::size_t i = s->videoTracks.size(); i-- > 0;) {
        lines.push_back(renderTrack(*s->videoTracks[i], labelOf(TrackKind::Video, i)));
    }
    for (std::size_t i = 0; i < s->audioTracks.size(); ++i) {
        lines.push_back(renderTrack(*s->audioTracks[i], labelOf(TrackKind::Audio, i)));
    }
    std::string out;
    for (const auto& line : lines) {
        if (!out.empty()) {
            out += '\n';
        }
        out += line;
    }
    return out;
}

std::string links(const ProjectState& state, SequenceId seqId) {
    const Sequence* s = state.findSequence(seqId);
    REQUIRE(s);
    std::map<LinkGroupId, std::vector<std::string>> groups;
    const auto collect = [&](const Track& t, const std::string& label) {
        for (const Clip& c : t.clips) {
            if (c.linkGroup.isValid()) {
                groups[c.linkGroup].push_back(label + "@" + std::to_string(frameOf(c.start())));
            }
        }
    };
    for (std::size_t i = 0; i < s->videoTracks.size(); ++i) {
        collect(*s->videoTracks[i], labelOf(TrackKind::Video, i));
    }
    for (std::size_t i = 0; i < s->audioTracks.size(); ++i) {
        collect(*s->audioTracks[i], labelOf(TrackKind::Audio, i));
    }
    std::vector<std::string> rendered;
    for (const auto& [group, members] : groups) {
        std::string joined;
        for (const auto& m : members) {
            joined += (joined.empty() ? "" : "+") + m;
        }
        rendered.push_back(std::move(joined));
    }
    std::sort(rendered.begin(), rendered.end());
    std::string out;
    for (const auto& g : rendered) {
        out += (out.empty() ? "" : " ") + g;
    }
    return out;
}

void requireValid(const ProjectState& state) {
    const auto valid = validate(state, Validation::Strict);
    INFO((valid ? std::string() : valid.error().message));
    REQUIRE(valid.has_value());
}

edit::EditResult ok(edit::EditOutcome outcome) {
    INFO((outcome ? std::string() : "edit failed: " + outcome.error().message));
    REQUIRE(outcome.has_value());
    requireValid(outcome->state);
    return std::move(*outcome);
}

} // namespace wabcut::test
