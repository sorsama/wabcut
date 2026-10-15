#pragma once

// The dialogue-first audio commands of the catalog (registered into editingCommands(), so wabcut-cli, the MCP
// server and the app serve them):
//
//   track.setRole                    a track's role: none, dialogue, music, effects
//   audio.effects                    the effect stack of an audio track or of the master bus (query)
//   audio.addEffect / setEffect / removeEffect / moveEffect, audio.addKeyframe / removeKeyframe
//                                    edit those stacks (clip stacks use clip.addEffect & co.)
//   audio.loudness                   integrated LUFS, LRA and true peak of clips, tracks or the mix (analysis)
//   audio.normalize                  gain to a delivery target (YouTube -14, podcast -16, EBU R128 -23, custom...),
//                                    measured and refined until within tolerance; one undo step
//   audio.masterLimiter              the master true-peak limiter preset
//   audio.autoDuck                   music volume keyframes under the speech of dialogue tracks; one undo step,
//                                    previewable with dryRun
//   audio.essentialSound             Essential Sound (A.7): a clip type and a preset as ordinary, marked effects
//   audio.remix                      Music Remix (A.7): music retimed to a length by rearranging its bars
//
// The analysis commands need the host's AudioServices (AudioAnalysis.h); without them they fail with
// InvalidRequest. The effect-stack and role commands need nothing.

#include <wabcut/command/Registry.h>

namespace wabcut::audio::edit {

void registerAudioCommands(command::CommandRegistry& registry);

} // namespace wabcut::audio::edit
