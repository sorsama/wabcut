#pragma once

// Game audio under the streamer's voice (A.10, docs/ai.md "Streamer voice and Game under voice"): which audio tracks of
// a gaming recording are the voice and which are the game. OBS, ShadowPlay and Game Bar recordings arrive with their
// audio streams on named tracks ("Mic", "Game", "Discord", "Music", "Track 3"), and the names say more than the roles,
// so they are read first. The ducking itself is audio.autoDuck with "preset": "gameUnderVoice" (gameUnderVoiceDucking()
// in Ducking.h). Pure.

#include <wabcut/audio/edit/Ducking.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Sequence.h>

#include <vector>

namespace wabcut::audio::edit {

struct GameVoiceTracks {
    std::vector<TrackId> voice; ///< the speech the game ducks under, in track order
    std::vector<TrackId> game;  ///< the tracks that duck, in track order: the game, and the music unless left out
};

/// The voice: the audio tracks named for a microphone ("Mic", "Microphone", "Voice", "Commentary"), else the
/// dialogue-role tracks. The game: the tracks named for the game ("Game", "Game Audio", "Desktop Audio", "Gameplay"),
/// else the effects-role tracks; with `includeMusic` also the music-role tracks and tracks named "Music" (music must
/// duck under the voice too). A voice track is never on the game side. Names match case-insensitively, also with a
/// number or a note after the word ("Mic 2", "Mic/Aux", "Game (Track 1)"), not inside another word ("Gamer").
///
/// A team chat track ("Discord", dialogue role) next to a mic track is neither: it is not ducked, and the game does not
/// duck under it (give audio.autoDuck "dialogue" to have it). Without a mic-named track it is a dialogue-role track like
/// any other, so the game ducks under it too.
GameVoiceTracks chooseGameVoiceTracks(const Sequence& sequence, bool includeMusic = true);

} // namespace wabcut::audio::edit
