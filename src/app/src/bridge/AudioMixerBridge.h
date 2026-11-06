#pragma once

// AudioMixerBridge: the Audio workspace (M5g), the QML singleton `AudioMixer`.
//
//   * Audio Track Mixer strips, one per audio track of the current sequence: name, role (Dialogue / Music / Effects,
//     track.setRole), mute and solo, the track's effect stack (audio.addEffect / setEffect / removeEffect /
//     moveEffect with the track as target) and a live meter. The master strip: the master effect stack
//     (audio.masterLimiter for the limiter preset) and the loudness of the mix against a target (YouTube -14,
//     Podcast -16, Broadcast -23 LUFS, or custom), from the last "Analyze loudness".
//   * Fader and pan (M5l, on A.3's Track::volume / Track::pan and Sequence::masterVolume): a drag is one undo step
//     (it merges until endFaderGesture()); on an automated parameter it sets the key at the playhead; a double-click
//     resets to 0 dB / centre. Automation modes per strip (Premiere's Read / Touch / Latch / Write, MixerAutomation.h):
//     while playing, the faders of Touch / Latch / Write strips are recorded and written into their curves when
//     playback stops, one undo step per pass; Read follows the automation.
//   * Live loudness (A.3): momentary, short-term and integrated LUFS and true peak of what leaves the player
//     (TimelineAudioPlayer::loudness), polled with the meters; resetLoudness() starts a new integration.
//   * Meters: peak / RMS / peak hold / clip per strip, polled at ~30 Hz while `metersActive` (the mixer is on
//     screen) from TimelineAudioPlayer's level meters (read-only, through Playback's session), or from a test feed
//     (setMeterSource). MeterBar items read meter() directly in C++; nothing per tick crosses into QML values.
//   * Audio tools: analyze loudness (mix, tracks or the selected clips: a report table), normalize to the target
//     (audio.normalize), auto-duck music under dialogue (audio.autoDuck; its keys show as ghost curves on the
//     timeline through TimelineView's external preview) and reduce noise on the selected clips (the denoise
//     effect). Measuring commands run as dry runs on a worker thread over a copy of the project; the preview is
//     applied as one undo step (when the project has not changed since), or dropped. 0.4: auto-duck has two modes
//     (`duckMode`): "default" (music under dialogue, by role) and "gameUnderVoice" (audio.autoDuck's gaming preset:
//     the game, and music with duckIncludeMusic, under the mic, found by track name); each mode keeps its own timing,
//     and the mode is suggested per sequence (game mode when a game track and a voice track are there) until picked.
//
// Keys cross into QML as opaque strings ("t12", "e5", "c7", "master"); levels are dB; no ticks or 64-bit ids
// (principle 9).

#include "MixerAutomation.h"

#include <wabcut/audio/LoudnessTap.h>
#include <wabcut/audio/edit/MatchCurve.h>
#include <wabcut/command/Command.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class PlaybackBridge;
class ProjectController;
class TranscriptBridge;
class VoiceOverBridge;

class AudioMixerBridge : public QObject {
    Q_OBJECT
    /// [{ key ("t12"), name, shortName, colorIndex, role, roleName, muted, solo, gainDb, pan, meter (its index),
    ///    effects: [{ key ("e5"), effect, name, enabled, summary, details (r07: the main values, for the hover tip) }],
    ///    clipEffects (r07: clip effects on the track's clips that the "+ Effect" menu adds, Enhance Speech:
    ///    [{ effect, name, enabled, count, total, clips, summary, details }]) }], in track order (A1 first).
    Q_PROPERTY(QVariantList strips READ strips NOTIFY stripsChanged FINAL)
    /// { effects: [...as above], hasLimiter, gainDb }
    Q_PROPERTY(QVariantMap master READ master NOTIFY stripsChanged FINAL)
    /// [{ id, name }]: none, dialogue, music, effects.
    Q_PROPERTY(QVariantList roles READ roles CONSTANT FINAL)
    /// The audio effects a stack can hold: [{ id, name, category }].
    Q_PROPERTY(QVariantList effectTypes READ effectTypes CONSTANT FINAL)
    /// Track faders and pan (A.3): always available now; faderNote stays for older QML.
    Q_PROPERTY(bool faderAvailable READ faderAvailable CONSTANT FINAL)
    Q_PROPERTY(QString faderNote READ faderNote CONSTANT FINAL)
    /// The faders now, per strip key ("t12", "master"): { gainDb, pan, automated (a curve), panAutomated, writing
    /// (this pass records it) }. Changes while playing (Read follows the curves), so the strips bind it, not `strips`.
    Q_PROPERTY(QVariantMap faderValues READ faderValues NOTIFY faderValuesChanged FINAL)
    /// Automation pass running: playing with a Touch, Latch or Write strip.
    Q_PROPERTY(bool automationActive READ automationActive NOTIFY faderValuesChanged FINAL)
    /// [{ id ("read" | "touch" | "latch" | "write"), name }]
    Q_PROPERTY(QVariantList automationModes READ automationModes CONSTANT FINAL)
    /// Live loudness: { valid, momentary, shortTerm, integrated, truePeak (numbers or null), momentaryText,
    /// shortTermText, integratedText, truePeakText, deltaText (integrated against the target), over (true peak above
    /// the target's ceiling), seconds (measured) }.
    Q_PROPERTY(QVariantMap liveLoudness READ liveLoudness NOTIFY liveLoudnessChanged FINAL)
    /// Poll the meters (~30 Hz) while true: someone shows them. 0.5: a count of viewers (setMeterViewer: the mixer and the
    /// Audio Meters panel each count while they are on screen); setting it is one more viewer of its own (tests).
    Q_PROPERTY(bool metersActive READ metersActive WRITE setMetersActive NOTIFY metersActiveChanged FINAL)
    /// 0.5: the items that show meters now (setMeterViewer).
    Q_PROPERTY(int meterViewers READ meterViewers NOTIFY metersActiveChanged FINAL)
    /// p02 audio (AudioMixerRouting.cpp): the submix strips, in submix order: [{ key ("t15"), name, shortName ("S1"),
    /// format, muted, solo, gainDb, pan, automated, automation, effects, output, outputName, meter }]. Every strip (tracks
    /// too) also has `output` ("mix" or a submix key), `outputName` and `sends`: [{ index (1..5), submix, submixName,
    /// preFader, muted, levelDb, pan }].
    Q_PROPERTY(QVariantList submixes READ submixes NOTIFY stripsChanged FINAL)
    /// Where a track or submix can output to: [{ id ("mix" or a submix key), name }], the Mix first.
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY stripsChanged FINAL)
    /// Premiere's "Create ... Submix" choices of the output and send menus: [{ id ("new:stereo"), name }].
    Q_PROPERTY(QVariantList submixFormats READ submixFormats CONSTANT FINAL)

    /// Loudness targets: [{ id, name, lufs, truePeak }]; "custom" takes `customLufs`.
    Q_PROPERTY(QVariantList targets READ targets CONSTANT FINAL)
    Q_PROPERTY(QString target READ target WRITE setTarget NOTIFY targetChanged FINAL)
    Q_PROPERTY(double customLufs READ customLufs WRITE setCustomLufs NOTIFY targetChanged FINAL)
    Q_PROPERTY(double targetLufs READ targetLufs NOTIFY targetChanged FINAL)
    Q_PROPERTY(double targetTruePeak READ targetTruePeak NOTIFY targetChanged FINAL)
    /// The mix's last analysis: { valid, stale (the project changed since), integrated, range, truePeak, samplePeak,
    /// momentary, shortTerm (maxima), seconds, deltaText ("2.3 LU below the target"), whenText }. 0.6 (P1 round, #54):
    /// also where - momentaryAt, shortTermAt, truePeakAt ("00:01:02:03" timecodes, "" when not known) - and the
    /// true-peak overs above targetTruePeak: overs [{ start, end (timecodes), truePeak }] (at most 200), overCount.
    Q_PROPERTY(QVariantMap loudness READ loudness NOTIFY loudnessChanged FINAL)
    /// Rows of the last analysis: [{ target, kind ("mix" | "track" | "clip"), integrated, range, truePeak,
    /// samplePeak, momentary, shortTerm }] (numbers, or null for silence); 0.6 (#54): with momentaryAt, shortTermAt,
    /// truePeakAt, overs and overCount as `loudness` has them.
    Q_PROPERTY(QVariantList report READ report NOTIFY reportChanged FINAL)
    /// A running job: "" | "analyze" | "normalize" | "autoDuck".
    Q_PROPERTY(QString busy READ busy NOTIFY jobChanged FINAL)
    /// A dry run waiting to be applied: { kind ("normalize" | "autoDuck"), title, info, rows: [...], warnings }, or
    /// an empty map.
    Q_PROPERTY(QVariantMap pending READ pending NOTIFY pendingChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

    // Auto-duck and noise settings (the tools panel).
    Q_PROPERTY(double duckAmount READ duckAmount WRITE setDuckAmount NOTIFY toolSettingsChanged FINAL)
    Q_PROPERTY(double duckAttack READ duckAttack WRITE setDuckAttack NOTIFY toolSettingsChanged FINAL)
    Q_PROPERTY(double duckRelease READ duckRelease WRITE setDuckRelease NOTIFY toolSettingsChanged FINAL)
    Q_PROPERTY(double duckHold READ duckHold WRITE setDuckHold NOTIFY toolSettingsChanged FINAL)
    Q_PROPERTY(double denoiseAmount READ denoiseAmount WRITE setDenoiseAmount NOTIFY toolSettingsChanged FINAL)
    /// 0.4: "default" (music under dialogue, by role) or "gameUnderVoice" (audio.autoDuck's gaming preset, by name).
    /// card 137: or "autoMix" (audio.autoMix: the microphones of the voices that are not speaking go down while
    /// another voice speaks; the voices are the dialogue-role tracks).
    Q_PROPERTY(QString duckMode READ duckMode WRITE setDuckMode NOTIFY duckChanged FINAL)
    /// card 137, autoMix: also lower a microphone while nobody speaks (a gate on each voice).
    Q_PROPERTY(bool duckGate READ duckGate WRITE setDuckGate NOTIFY duckChanged FINAL)
    /// 0.4, gameUnderVoice: the music tracks duck with the game (default true).
    Q_PROPERTY(bool duckIncludeMusic READ duckIncludeMusic WRITE setDuckIncludeMusic NOTIFY duckChanged FINAL)
    /// 0.4: what gameUnderVoice would use on the current sequence: { voice: [names], game: [names] } (the ducked
    /// tracks: game, then music with duckIncludeMusic; audio::edit::chooseGameVoiceTracks); 0.7: `mix`: the names of the
    /// dialogue-role tracks autoMix mixes.
    Q_PROPERTY(QVariantMap gameTracks READ gameTracks NOTIFY duckChanged FINAL)

    /// 0.7 (card 032, Premiere's Audio Track Mixer menu > Meter Input(s) Only): while on and the microphone is open
    /// (VoiceOver.inputOpen), the meter of a record-armed track shows the input's level and the other tracks' meters read
    /// the floor; the master keeps metering the mix.
    Q_PROPERTY(bool meterInputsOnly READ meterInputsOnly WRITE setMeterInputsOnly NOTIFY meterInputsOnlyChanged FINAL)
    /// 0.7 (card 032, Show/Hide Tracks): strips hidden in the current sequence (every strip and submix map has `hidden`).
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY stripsChanged FINAL)

    /// 0.7 (cards 104 and 141, the Loudness panel): the Loudness Radar, one revolution of the short-term loudness:
    /// { valid, mode ("live" | "programme"), lap (seconds of one revolution, live), slots, values ([LUFS or null] per slot, in
    /// time order from the top, clockwise), secondsPerSlot, head (live: the slot being written; -1), seconds (measured),
    /// target (LUFS), lra (LU or null), lowLufs / highLufs (the range band, null when unknown), stale (programme: the
    /// project changed since the analysis), text (a one-line summary) }.
    Q_PROPERTY(QVariantMap radar READ radar NOTIFY radarChanged FINAL)
    /// Seconds of one revolution of the live radar: 60, 300, 600, 1800 or 3600.
    Q_PROPERTY(int radarLap READ radarLap WRITE setRadarLap NOTIFY radarChanged FINAL)
    /// "live" (what is playing since the play started or Reset) or "programme" (the last analysis of the mix, one revolution).
    Q_PROPERTY(QString radarMode READ radarMode WRITE setRadarMode NOTIFY radarChanged FINAL)
    /// The radar is on screen: the history is folded for it (about once a second) only while true.
    Q_PROPERTY(bool radarShown READ radarShown WRITE setRadarShown NOTIFY radarChanged FINAL)

    /// 0.7 (card 045, the Audio Tools panel's Match tab): the state of EQ Match and Level Match: { reference: { set, label },
    /// target: { count, label }, options: { amount, smoothing (percent), mode ("loudness" | "peak"), headroom, limit },
    /// busy, learned (false until Learn ran on these), bands: [{ hz, referenceDb, targetDb, gainDb }] (the curve with the
    /// options, refitted whenever they change), levelOffsetDb, referenceStats / targetStats: { integrated, truePeak, rms,
    /// crest, seconds }, levelGainDb (the gain Level Match would apply), applied (clips that have an EQ Match), bypassed
    /// (of them, switched off), message, report: [{ target, gainDb, before, after, ... }] of the last Level Match }.
    Q_PROPERTY(QVariantMap match READ match NOTIFY matchChanged FINAL)

public:
    /// Channels a meter shows: stereo strips 2; a 5.1 master 6 (M5n, A.9: L, R, C, LFE, Ls, Rs); a 7.1 master 8 (0.7).
    static constexpr int kMeterChannels = 8;
    /// One meter reading, linear (1.0 = 0 dBFS), as TimelineAudioPlayer's LevelMeter gives it.
    struct MeterReading {
        int channels = 0;
        std::array<float, kMeterChannels> peak{};
        std::array<float, kMeterChannels> rms{};
        bool clipped = false;
    };
    /// The levels of one strip, in dBFS (-inf below the scale), with the peak hold.
    struct MeterLevels {
        int channels = 0;
        std::array<double, kMeterChannels> peakDb{};
        std::array<double, kMeterChannels> rmsDb{};
        std::array<double, kMeterChannels> holdDb{};
        bool clipped = false;
    };
    /// Where readings come from: `index` 0 = A1 ..., -1 = master, -2 = S1, -3 = S2 ... (p02 submixes); nullopt when
    /// there is nothing to read.
    using MeterSource = std::function<std::optional<MeterReading>(int index)>;
    static constexpr double kMeterFloorDb = -90.0;

    explicit AudioMixerBridge(QObject* parent = nullptr);
    ~AudioMixerBridge() override;

    static AudioMixerBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setPlayback(PlaybackBridge* playback);
    void setTranscripts(TranscriptBridge* transcripts);
    /// 0.7 (card 032): the microphone input Meter Input(s) Only shows.
    void setVoiceOver(VoiceOverBridge* voiceOver);
    /// The analysis services of the audio commands (tests inject fakes; by default media-backed ones).
    void setServices(std::shared_ptr<command::CommandServices> services);
    /// The meter feed (tests); by default TimelineAudioPlayer's meters.
    void setMeterSource(MeterSource source);
    /// One meter tick now (the timer calls it ~30 times a second).
    void pollMeters();
    /// Runs measuring jobs on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    /// The live loudness feed (tests); by default TimelineAudioPlayer::loudness().
    using LoudnessSource = std::function<std::optional<audio::LoudnessReading>()>;
    void setLoudnessSource(LoudnessSource source) { m_loudnessSource = std::move(source); }
    /// The microphone's levels for Meter Input(s) Only (tests): nullopt while the input is closed. By default the
    /// VoiceOver bridge's input.
    using InputSource = std::function<std::optional<MeterLevels>()>;
    void setInputSource(InputSource source) { m_inputSource = std::move(source); }
    /// The short-term loudness history of the live radar (tests); by default TimelineAudioPlayer::loudnessHistory().
    using LoudnessHistorySource = std::function<std::vector<float>()>;
    void setLoudnessHistorySource(LoudnessHistorySource source) { m_loudnessHistorySource = std::move(source); }
    /// M5m: what an automation pass sounds like while it records. Called with a strip's live fader / pan while a
    /// Touch, Latch or Write pass writes it, and with (nullopt, nullopt) when it stops writing (release, stop). Without
    /// one set (the app), the player's RT-safe override does it: TimelineAudioPlayer::setLiveTrackMix (A.8). Returns
    /// whether the value was applied: one that was not (the player's slot not published yet) is sent again.
    using LiveMixSink = std::function<bool(const QString& target, std::optional<double> gainDb, std::optional<double> pan)>;
    void setLiveMixSink(LiveMixSink sink) { m_liveMixSink = std::move(sink); }
    /// Tests: playing or not without a transport (the automation pass follows it as it follows Playback).
    void setPlayingForTests(bool playing) { onPlayingChanged(playing); }
    /// One automation tick now (the timer calls it ~30 times a second while a pass runs).
    void pollAutomation();

    QVariantList strips() const { return m_strips; }
    QVariantMap master() const { return m_master; }
    QVariantList roles() const;
    QVariantList effectTypes() const;
    bool faderAvailable() const { return true; }
    QString faderNote() const;
    bool metersActive() const { return m_metersActive; }
    void setMetersActive(bool active);
    int meterViewers() const { return static_cast<int>(m_meterViewers.size()); }
    /// Whether `viewer` holds the meters on (tests).
    bool isMeterViewer(const QObject* viewer) const;
    QVariantList targets() const;
    QString target() const { return m_target; }
    void setTarget(const QString& target);
    double customLufs() const { return m_customLufs; }
    void setCustomLufs(double lufs);
    double targetLufs() const;
    double targetTruePeak() const;
    QVariantMap loudness() const;
    QVariantList report() const { return m_report; }
    QString busy() const { return m_busy; }
    QVariantMap pending() const;
    QString message() const { return m_message; }
    /// The duck timing of the current duckMode (each mode keeps its own).
    double duckAmount() const { return m_duckTiming[static_cast<std::size_t>(m_duckIndex)].amount; }
    void setDuckAmount(double v);
    double duckAttack() const { return m_duckTiming[static_cast<std::size_t>(m_duckIndex)].attack; }
    void setDuckAttack(double v);
    double duckRelease() const { return m_duckTiming[static_cast<std::size_t>(m_duckIndex)].release; }
    void setDuckRelease(double v);
    double duckHold() const { return m_duckTiming[static_cast<std::size_t>(m_duckIndex)].hold; }
    void setDuckHold(double v);
    double denoiseAmount() const { return m_denoiseAmount; }
    void setDenoiseAmount(double v);
    QString duckMode() const;
    /// "default" or "gameUnderVoice" (anything else is ignored); a pick sticks until the current sequence changes.
    void setDuckMode(const QString& mode);
    bool duckIncludeMusic() const { return m_duckIncludeMusic; }
    void setDuckIncludeMusic(bool on);
    bool duckGate() const { return m_duckGate; }
    void setDuckGate(bool on);
    QVariantMap gameTracks() const { return m_gameTracks; }

    /// The levels of a strip ("t12" or "master"); channels 0 when there is nothing to show.
    MeterLevels meter(const QString& key) const;
    QVariantMap faderValues() const { return m_faderValues; }
    bool automationActive() const { return m_pass.has_value(); }
    QVariantList automationModes() const;
    QVariantMap liveLoudness() const { return m_liveLoudness; }

    // ---- 0.7 (card 032): Show/Hide Tracks, Meter Input(s) Only ----
    bool meterInputsOnly() const { return m_meterInputsOnly; }
    void setMeterInputsOnly(bool on);
    int hiddenCount() const;
    /// Hides (or shows again) a strip of the current sequence, a track ("t12") or a submix, for this session. A hidden strip
    /// stays in the mix: hiding is only about the mixer's room (mute and solo are separate).
    Q_INVOKABLE void setTrackHidden(const QString& key, bool hidden);
    Q_INVOKABLE bool isTrackHidden(const QString& key) const;
    Q_INVOKABLE void showAllTracks();

    // ---- 0.7 (cards 104, 141): the Loudness Radar ----
    QVariantMap radar() const { return m_radar; }
    int radarLap() const { return m_radarLap; }
    void setRadarLap(int seconds);
    QString radarMode() const { return m_radarMode; }
    void setRadarMode(const QString& mode);
    bool radarShown() const { return m_radarShown; }
    void setRadarShown(bool shown);

    // ---- 0.7 (card 045): EQ Match and Level Match (AudioMixerMatch.cpp) ----
    QVariantMap match() const;
    /// The reference: the first selected audio clip (as it renders), the mix between the sequence's In and Out points, or an
    /// audio file of the project (`matchMedia`: [{ id, name, seconds }]). Each returns "" or why not.
    Q_INVOKABLE QString matchUseSelectedClip();
    Q_INVOKABLE QString matchUseRange();
    Q_INVOKABLE QString matchUseMedia(const QString& mediaId);
    Q_INVOKABLE QVariantList matchMedia() const;
    /// The target: the audio clips selected on the timeline now.
    Q_INVOKABLE QString matchUseSelectedTargets();
    /// amount / smoothing (percent), headroom (dB), mode ("loudness" | "peak"), limit (bool).
    Q_INVOKABLE void setMatchOption(const QString& name, const QVariant& value);
    /// Learns: measures the reference and the targets in the background (Cancel stops it). The result fills `match`.
    Q_INVOKABLE QString matchLearn();
    /// Adds the EQ Match to the target clips (one undo step) / matches the level (one undo step) / takes the EQ Match off.
    Q_INVOKABLE QString matchApplyEq();
    Q_INVOKABLE QString matchApplyLevel();
    Q_INVOKABLE QString matchReset();
    /// Preview: switches the EQ Match of the target clips on or off (one undo step) to compare by ear while playing.
    Q_INVOKABLE QString matchTogglePreview();

    // ---- Strips ----
    /// Sets a track's role ("none", "dialogue", "music", "effects"). Returns "" or the error.
    Q_INVOKABLE QString setRole(const QString& track, const QString& role);
    Q_INVOKABLE void setMuted(const QString& track, bool muted);
    Q_INVOKABLE void setSolo(const QString& track, bool solo);
    /// The fader (a track "t12" or "master", dB) and a track's pan (-1..1). Paused (or in Read): the value, or the key at
    /// the playhead of an automated parameter; a drag merges into one undo step until endFaderGesture(). Playing in
    /// Touch / Latch / Write: recorded into the pass instead.
    Q_INVOKABLE QString setGainDb(const QString& target, double gainDb);
    Q_INVOKABLE QString setPan(const QString& track, double pan);
    Q_INVOKABLE void endFaderGesture();
    /// Double-click: 0 dB / centre (one undo step).
    Q_INVOKABLE QString resetGain(const QString& target);
    Q_INVOKABLE QString resetPan(const QString& track);
    /// The fader or pan knob is held (Touch and Latch write while it is). `param`: "volume" or "pan".
    Q_INVOKABLE void touch(const QString& target, const QString& param, bool held);
    /// A strip's automation mode: "read" (default), "touch", "latch" or "write".
    Q_INVOKABLE QString automationMode(const QString& target) const;
    Q_INVOKABLE void setAutomationMode(const QString& target, const QString& mode);
    /// Live loudness: a new integration from now.
    Q_INVOKABLE void resetLoudness();
    /// Effect stacks of a track ("t12") or the master ("master").
    Q_INVOKABLE QString addEffect(const QString& target, const QString& effectId);
    /// The audio clips of a track ("c…" keys, in time order).
    Q_INVOKABLE QStringList trackClips(const QString& track) const;
    Q_INVOKABLE QString removeEffect(const QString& target, const QString& effect);
    Q_INVOKABLE QString setEffectEnabled(const QString& target, const QString& effect, bool enabled);
    Q_INVOKABLE QString moveEffect(const QString& target, const QString& effect, int index);
    /// The master limiter preset (-1 dBTP ceiling), or its update when the master already ends with a limiter.
    Q_INVOKABLE QString addMasterLimiter();

    // ---- r07: effect settings (AudioMixerEffects.cpp; the strips' effect settings popup) ----
    /// A track, submix or master effect ("e5") of `target`: { key, target, effect, name, description, enabled, known
    /// (the registry can run it), plugin, details, params: [{ key, name, kind ("slider" | "toggle" | "choice"), value,
    /// defaultValue, from, to, step, decimals, origin, unit, animated, animatable, valueText, options ([{ id, name }]) and
    /// current (choices) }] }, values in UI units (ParameterUi.h) at the playhead. An empty map when there is no such effect.
    Q_INVOKABLE QVariantMap effectSettings(const QString& target, const QString& effect) const;
    /// Sets parameter `param` of an effect (UI units, clamped): the constant, or on an animated parameter the key at the
    /// playhead. A `gesture` (a slider drag, a scrub) merges into one undo step until endEffectGesture().
    /// 0.7 (card 044): the Dynamics Processing graph in an effect's settings: the live levels of the effect `effect` ("e5") and
    /// a band's static curve (EffectControlsModel::dynamicsMeters / dynamicsCurve have the shapes).
    Q_INVOKABLE QVariantMap dynamicsMeters(const QString& effect);
    Q_INVOKABLE QVariantList dynamicsCurve(int mode, double threshold, double ratio, double range, double knee, double makeup,
                                           double fromDb, double toDb, int points) const;
    Q_INVOKABLE QString setEffectParam(const QString& target, const QString& effect, const QString& param, double value,
                                       bool gesture = false);
    Q_INVOKABLE void endEffectGesture();
    /// Every parameter of the effect back to its default (keys too), one undo step.
    Q_INVOKABLE QString resetEffect(const QString& target, const QString& effect);
    /// A clip effect the "+ Effect" menu put on a track's clips (Enhance Speech; a strip's `clipEffects`): on or bypassed
    /// on every clip of the track that has it, or removed from all of them; one undo step each.
    Q_INVOKABLE QString setClipEffectEnabled(const QString& track, const QString& effectId, bool enabled);
    Q_INVOKABLE QString removeClipEffect(const QString& track, const QString& effectId);
    /// A strip asks for an effect's settings: the mixer panel opens its one popup (effectSettingsRequested). The strips
    /// are made again on every change, so the popup cannot live in one.
    Q_INVOKABLE void openEffectSettings(const QString& target, const QString& effect, const QString& stripName) {
        emit effectSettingsRequested(target, effect, stripName);
    }
    /// 0.7 (card 101): a strip asks to save its effect chain as a preset: the mixer panel opens its Save Preset dialog
    /// (savePresetRequested), for the same reason as openEffectSettings.
    Q_INVOKABLE void openSavePreset(const QString& target, const QString& suggestedName) {
        emit savePresetRequested(target, suggestedName);
    }
    /// Clears a strip's clip indicator.
    Q_INVOKABLE void clearClip(const QString& key);
    /// 0.5: `viewer` (the mixer, the Audio Meters panel) shows meters now, or no longer: the meters poll while any viewer
    /// does. A viewer that goes away stops counting by itself.
    Q_INVOKABLE void setMeterViewer(QObject* viewer, bool showing);

    // ---- Routing (p02 audio, AudioMixerRouting.cpp): submixes, output assignment, sends ----
    QVariantList submixes() const { return m_submixes; }
    QVariantList outputs() const { return m_outputs; }
    QVariantList submixFormats() const;
    /// Adds a submix track ("mono", "stereo", "5.1", "adaptive"). Returns "" or the error.
    Q_INVOKABLE QString addSubmix(const QString& format);
    /// Deletes a submix (its tracks go to the Mix).
    Q_INVOKABLE QString removeSubmix(const QString& submix);
    /// A track's or submix's output: "mix", a submix key, or "new:<format>" (a new submix, assigned in one undo step).
    Q_INVOKABLE QString setOutput(const QString& target, const QString& output);
    /// A send from a track to a submix key or "new:<format>".
    Q_INVOKABLE QString addSend(const QString& track, const QString& submix);
    /// Send `send` (1..5) of a track: its level (dB) and pan; a drag is one undo step until endSendGesture().
    Q_INVOKABLE QString setSendLevel(const QString& track, int send, double db);
    Q_INVOKABLE QString setSendPan(const QString& track, int send, double pan);
    Q_INVOKABLE void endSendGesture();
    /// "preFader" or "muted".
    Q_INVOKABLE QString setSendOption(const QString& track, int send, const QString& option, bool value);
    Q_INVOKABLE QString setSendSubmix(const QString& track, int send, const QString& submix);
    Q_INVOKABLE QString removeSend(const QString& track, int send);

    // ---- Audio tools ----
    /// Measures "mix", "tracks" (every audio track and the mix) or "clips" (the selected audio clips).
    Q_INVOKABLE QString analyze(const QString& scope);
    /// 0.6 (P1 round, #54, loudness QC): a red range marker over every true-peak over of the mix's last analysis (above
    /// targetTruePeak), named with its peak, in one undo step "Add True-Peak Over Markers". "" or why not.
    Q_INVOKABLE QString addOverMarkers();
    /// Dry run of normalizing "mix", "tracks" or "clips" to the target; applyPending() applies it.
    Q_INVOKABLE QString previewNormalize(const QString& scope);
    /// Dry run of ducking the music tracks under the dialogue tracks (roles); its keys show on the timeline. 0.7: in
    /// autoMix mode the dialogue tracks against each other (audio.autoMix). 0.4: in
    /// gameUnderVoice mode the game (and music, duckIncludeMusic) under the voice tracks, by track name.
    Q_INVOKABLE QString previewAutoDuck();
    /// Adds the Reduce Noise effect to the selected audio clips, or sets its amount where it is: one undo step.
    Q_INVOKABLE QString reduceNoise();
    Q_INVOKABLE QString applyPending();
    Q_INVOKABLE void cancelPending();
    Q_INVOKABLE void cancelJob();

signals:
    void stripsChanged();
    void metersActiveChanged();
    void metersUpdated();
    /// Enhance Speech picked on a track: Main.qml opens the Enhance Speech dialog for these clips (ClipAudio).
    void enhanceClipsRequested(const QStringList& clips);
    /// r07: openEffectSettings().
    void effectSettingsRequested(const QString& target, const QString& effect, const QString& stripName);
    /// 0.7 (card 101): openSavePreset().
    void savePresetRequested(const QString& target, const QString& suggestedName);
    void targetChanged();
    void loudnessChanged();
    void reportChanged();
    void jobChanged();
    void pendingChanged();
    void messageChanged();
    void toolSettingsChanged();
    void faderValuesChanged();
    void liveLoudnessChanged();
    void meterInputsOnlyChanged();
    void radarChanged();
    void matchChanged();
    /// 0.4: duckMode, duckIncludeMusic or gameTracks changed.
    void duckChanged();

private:
    struct Job;
    struct Pending {
        QString kind, title, info;
        QVariantList rows;
        QStringList warnings;
        std::shared_ptr<const ProjectState> preview;
        std::uint64_t baseRevision = 0;
        /// 0.4: the undo step applyPending() names ("" = the command's own label).
        std::string undoLabel;
    };
    /// 0.4: one duck mode's timing (amount dB; attack, release, hold seconds).
    struct DuckTiming {
        double amount, attack, release, hold;
    };
    /// Recomputes gameTracks and, unless the user picked a mode for this sequence, the suggested mode.
    void updateDuckTracks();

    void rebuild();
    /// p02 audio (AudioMixerRouting.cpp): a strip's routing fields (output, outputName, sends), the submix strips and the
    /// output choices of `seq`.
    void addRouting(QVariantMap& strip, const Track& track, const Sequence& seq) const;
    QVariantList buildSubmixes(const Sequence& seq) const;
    QVariantList buildOutputs(const Sequence& seq) const;
    QString editSend(const QString& track, int send, const char* label, const std::string& merge,
                     const std::function<void(TrackSend&)>& change);
    /// One automation pass (playing with a writing strip): the stretches each (target, param) wrote so far.
    struct Pass {
        struct Lane {
            std::vector<AutomationSegment> segments;
            bool open = false;   ///< a segment is being written
            bool latched = false; ///< Latch: touched once, writes until stop
        };
        std::map<std::pair<QString, QString>, Lane> lanes; ///< (target, "volume" | "pan")
        /// The sequence the pass writes: the one current when it started (review wave 5, A5-P2-22).
        SequenceId sequence;
    };
    void onPlayingChanged(bool playing);
    /// A pass on the current sequence (Touch / Latch / Write strips while the program plays).
    void startPass();
    void commitPass();
    void updateFaderValues();
    void pollLoudness();
    QString setFader(const QString& target, const QString& param, double value, bool gesture);
    /// The parameter's value in the model at the playhead (target "master" or a track key).
    std::optional<double> modelValue(const QString& target, const QString& param) const;
    const ParamValue* modelParam(const QString& target, const QString& param) const;
    command::CommandResult run(const std::string& name, command::Json params);
    command::CommandResult runBatch(std::vector<command::CommandRequest> requests, const std::string& label);
    std::shared_ptr<command::CommandServices> services();
    std::optional<command::Json> targetParam(const QString& key) const; // "master" or the track's id (a number)
    QString startJob(const QString& kind, std::vector<command::CommandRequest> requests);
    void finishJob(const QString& kind, std::vector<command::CommandResult> results, std::uint64_t baseRevision);
    void showDuckPreview(const ProjectState& preview);
    void clearDuckPreview();
    void setMessage(const QString& message);
    void setPending(std::optional<Pending> pending);

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_playback;
    QPointer<TranscriptBridge> m_transcripts;
    QPointer<VoiceOverBridge> m_voiceOver;
    std::shared_ptr<command::CommandServices> m_services;
    MeterSource m_meterSource;
    LoudnessSource m_loudnessSource;
    QVariantMap m_liveLoudness;
    std::uint64_t m_loudnessResets = 0;
    QVariantMap m_faderValues;
    std::map<QString, AutomationMode> m_modes;
    std::map<std::pair<QString, QString>, bool> m_held;    ///< (target, param) held now
    std::map<std::pair<QString, QString>, double> m_live;  ///< values the pass records (target, param)
    LiveMixSink m_liveMixSink;
    /// What the sink was last told, per target (gain, pan); cleared targets are not kept.
    std::map<QString, std::pair<std::optional<double>, std::optional<double>>> m_liveSent;
    void pushLiveMix();
    /// The sink, or the player's live override; false when it was not applied.
    bool sendLiveMix(const QString& target, std::optional<double> gainDb, std::optional<double> pan);
    std::optional<Pass> m_pass;
    QTimer m_automationTimer;
    bool m_playing = false;
    QTimer m_meterTimer;
    bool m_metersActive = false;
    /// 0.5: the viewers showing meters (setMeterViewer), and setMetersActive's own hold.
    std::vector<QObject*> m_meterViewers;
    bool m_metersHeld = false;
    void updateMetersActive();
    bool m_synchronous = false;
    QVariantList m_strips;
    QVariantMap m_master;
    QVariantList m_submixes; ///< p02 audio
    QVariantList m_outputs;
    std::vector<TrackId> m_trackOrder; ///< audio tracks, A1 first (meter index = position)
    std::vector<TrackId> m_submixOrder; ///< p02 audio: submix tracks, S1 first (meter index -2 - position)
    std::map<QString, MeterLevels> m_levels;
    std::map<QString, std::array<qint64, kMeterChannels>> m_holdSince; ///< ms timestamps of the peak holds
    /// 0.7 (card 044): each Dynamics Processing instance's meter block counter at the last look (is audio going through it?).
    std::map<std::uint64_t, std::uint64_t> m_dynamicsBlocks;
    QString m_target = QStringLiteral("youtube");
    double m_customLufs = -14.0;
    std::optional<std::pair<QVariantMap, std::uint64_t>> m_mixLoudness; ///< row + the revision it measured
    QVariantList m_report;
    QString m_busy;
    std::optional<Pending> m_pending;
    QString m_message;
    /// 0.4: per duck mode (0 "default", 1 "gameUnderVoice"; audio::edit's music and gaming ducking defaults); 0.7: 2
    /// "autoMix" (AutoMixOptions' defaults).
    std::array<DuckTiming, 3> m_duckTiming{{{-12.0, 0.25, 0.75, 0.25}, {-10.0, 0.12, 0.8, 0.35}, {-12.0, 0.1, 0.5, 0.4}}};
    bool m_duckGate = false;
    int m_duckIndex = 0;
    bool m_duckModeChosen = false; ///< the user picked the mode for m_duckSequence
    bool m_duckIncludeMusic = true;
    QVariantMap m_gameTracks;
    SequenceId m_duckSequence; ///< the sequence the mode was suggested (or picked) for
    double m_denoiseAmount = 1.0;
    std::unique_ptr<Job> m_job;
    bool m_duckPreviewShown = false;

    // ---- 0.7 (card 032) ----
    bool m_meterInputsOnly = false;
    std::map<std::uint64_t, std::set<QString>> m_hiddenStrips; ///< by sequence id: the strip keys hidden (this session)
    bool isArmedStrip(const QString& key) const;
    MeterLevels inputLevels() const;
    /// The input's levels while it is open (Meter Input(s) Only reads them), else nullopt.
    std::optional<MeterLevels> inputNow() const;
    InputSource m_inputSource;

    // ---- 0.7 (cards 104, 141) ----
    LoudnessHistorySource m_loudnessHistorySource;
    QVariantMap m_radar;
    int m_radarLap = 300;
    QString m_radarMode = QStringLiteral("live");
    bool m_radarShown = false;
    std::size_t m_radarSeen = 0;       ///< live history entries folded last
    qint64 m_radarLastMs = 0;          ///< when the live history was last looked at
    std::vector<float> m_mixSeries;    ///< the last analysis of the mix: short-term loudness once a second
    double m_mixRange = 0.0;           ///< ... and its loudness range (LU)
    std::uint64_t m_mixSeriesRevision = 0;
    double m_liveRange = 0.0;
    void updateRadar(bool force);

    // ---- 0.7 (card 045): AudioMixerMatch.cpp ----
    struct MatchState {
        bool referenceSet = false;
        QString referenceLabel;
        QString referenceKind;                       ///< "clip", "range" or "media"
        std::uint64_t referenceId = 0;               ///< the clip or the media item
        std::optional<std::int64_t> referenceFrom;   ///< ticks: the range of the mix, or of the clip / file
        std::optional<std::int64_t> referenceTo;
        std::vector<ClipId> targets;
        QString targetLabel;
        double amount = 100.0;                       ///< percent
        double smoothing = 50.0;                     ///< percent
        double headroom = 1.0;                       ///< dB under full scale
        QString mode = QStringLiteral("loudness");
        bool limit = false;
        bool learned = false;
        std::uint64_t learnedRevision = 0;
        audio::edit::SpectrumProfile reference;
        audio::edit::SpectrumProfile target;
        QVariantMap referenceStats;
        QVariantMap targetStats;
        QVariantList report;
        QString message;
    };
    MatchState m_match;
    void matchFinish(const QString& kind, const std::vector<command::CommandResult>& results, std::uint64_t baseRevision);
    void refitMatch();
    QString runMatch(const QString& kind, const char* command, bool apply);
    command::CommandRequest matchRequest(const char* command) const;
};

/// QML: the singleton `AudioMixer`.
struct AudioMixerBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::AudioMixerBridge)
    QML_NAMED_ELEMENT(AudioMixer)
    QML_SINGLETON
public:
    static AudioMixerBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return AudioMixerBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
