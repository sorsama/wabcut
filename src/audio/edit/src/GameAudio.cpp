#include <wabcut/audio/edit/GameAudio.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::audio::edit {

namespace {

// Lower case. OBS names its default sources "Mic/Aux" and "Desktop Audio"; the recording import names the tracks after
// the streams' titles ("Mic", "Game", "Discord", "Music").
constexpr std::array<std::string_view, 4> kVoiceNames{"mic", "microphone", "voice", "commentary"};
constexpr std::array<std::string_view, 4> kGameNames{"game", "game audio", "desktop audio", "gameplay"};
constexpr std::array<std::string_view, 1> kMusicNames{"music"};

bool isAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// The name trimmed and in lower case (ASCII; other bytes are kept).
std::string folded(std::string_view name) {
    const auto space = [](char c) { return c == ' ' || c == '\t'; };
    while (!name.empty() && space(name.front())) {
        name.remove_prefix(1);
    }
    while (!name.empty() && space(name.back())) {
        name.remove_suffix(1);
    }
    std::string out(name);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/// Whether `name` is one of `words`, alone or followed by something that is not a letter ("mic 2", "mic/aux").
bool named(std::string_view name, std::span<const std::string_view> words) {
    const std::string n = folded(name);
    return std::ranges::any_of(words, [&](std::string_view w) {
        return n == w || (n.size() > w.size() && n.starts_with(w) && !isAsciiLetter(n[w.size()]));
    });
}

} // namespace

GameVoiceTracks chooseGameVoiceTracks(const Sequence& sequence, bool includeMusic) {
    GameVoiceTracks out;
    const auto& tracks = sequence.audioTracks;
    for (const TrackPtr& t : tracks) {
        if (t && named(t->name, kVoiceNames)) {
            out.voice.push_back(t->id);
        }
    }
    if (out.voice.empty()) {
        for (const TrackPtr& t : tracks) {
            if (t && t->role == TrackRole::Dialogue) {
                out.voice.push_back(t->id);
            }
        }
    }
    const auto isVoice = [&](const Track& t) { return std::ranges::find(out.voice, t.id) != out.voice.end(); };
    // The game by name when any track has a game name (the others with the effects role are then sound effects the
    // editor added, which keep their level), else by role.
    const bool byName =
        std::ranges::any_of(tracks, [&](const TrackPtr& t) { return t && !isVoice(*t) && named(t->name, kGameNames); });
    for (const TrackPtr& t : tracks) {
        if (!t || isVoice(*t)) {
            continue;
        }
        const bool game = byName ? named(t->name, kGameNames) : t->role == TrackRole::Effects;
        const bool music = includeMusic && (t->role == TrackRole::Music || named(t->name, kMusicNames));
        if (game || music) {
            out.game.push_back(t->id);
        }
    }
    return out;
}

} // namespace wabcut::audio::edit
