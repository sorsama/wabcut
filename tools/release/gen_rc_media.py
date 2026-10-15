#!/usr/bin/env python3
"""Generate the media of the 0.1 release workflows (docs/internal/release-checklist.md, tools/release/rc_workflows.py).

    gaming/obs_1440p60.mkv      12 s OBS-style recording: 2560x1440 60 fps H.264 (h264_nvenc, else libx264), 3 AAC
                                tracks titled Game / Mic / Discord (tests/perf/gen_longvod_media.py's recipe, 12 s)
    essay/voiceover.wav         a narration (Windows text to speech, 48 kHz mono; a tone-and-noise stand-in elsewhere)
    essay/still_1.png ..3       1920x1080 B-roll stills (and the same as JPEG)
    essay/broll_1.mp4, _2       6 s 1080p30 B-roll clips with room tone
    essay/music.wav             a 60 s music bed (chords, bass, a beat)
    doc/interview_a.mp4         1080p30 angle A with the interview audio (on-camera mic)
    doc/interview_b.mp4         angle B, starting 0.4 s later, its own camera mic
    doc/lav.wav                 the lavalier recording of the same interview, starting 0.2 s before angle A

Usage: gen_rc_media.py --out DIR [--ffmpeg PATH]. Skips files that exist.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

COMMON = ["-hide_banner", "-loglevel", "error", "-nostdin", "-y"]

NARRATION = (
    "Every great comeback starts with a mistake. In this video we look at the moment the match turned, frame by frame. "
    "Watch the left side of the map. The defenders rotate early, and that one decision opens the whole site. "
    "Here is the replay at half speed. Notice the timing of the flash, and the second player holding the angle. "
    "By the end of the round, the team that was down eight points had taken back control of the game."
)
COMMENTARY = (
    "Okay, push left now. Two enemies on the bridge, watch the sniper. I have the flank. Nice shot! "
    "That is a clean triple kill, let's go!"
)
INTERVIEW = (
    "So tell me how you started streaming. Honestly, it began as a joke with friends. We recorded our late night matches "
    "and somebody said we should put them online. The first stream had four viewers, and two of them were my parents. "
    "What changed? Consistency. I streamed every evening for a year, and slowly people started to come back."
)


def run(ffmpeg: str, args: list[str]) -> None:
    subprocess.run([ffmpeg, *COMMON, *args], check=True)


def tts(text: str, out: Path) -> bool:
    """Windows text to speech (System.Speech through Windows PowerShell): 48 kHz 16-bit mono."""
    if os.name != "nt":
        return False
    script = (
        "Add-Type -AssemblyName System.Speech;"
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
        "$f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000,"
        "[System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,[System.Speech.AudioFormat.AudioChannel]::Mono);"
        f"$s.SetOutputToWaveFile('{out}', $f); $s.Speak([Console]::In.ReadToEnd()); $s.Dispose()"
    )
    result = subprocess.run(["powershell.exe", "-NoProfile", "-Command", script], input=text.encode("utf-8"),
                            capture_output=True)
    return result.returncode == 0 and out.exists() and out.stat().st_size > 10000


def has_encoder(ffmpeg: str, name: str) -> bool:
    probe = subprocess.run([ffmpeg, *COMMON, "-f", "lavfi", "-i", "color=s=256x256:d=0.1", "-c:v", name, "-f", "null", "-"],
                           capture_output=True)
    return probe.returncode == 0


def find_ffmpeg(explicit: str | None) -> str:
    if explicit:
        return explicit
    root = Path(__file__).resolve().parents[2]
    exe = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"
    for c in sorted(root.glob(f"build/*/vcpkg_installed/*/tools/ffmpeg/{exe}"), key=lambda p: p.stat().st_mtime, reverse=True):
        return str(c)
    found = shutil.which("ffmpeg")
    if not found:
        sys.exit("gen_rc_media: no FFmpeg found; pass --ffmpeg")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True)
    parser.add_argument("--ffmpeg")
    args = parser.parse_args()
    ffmpeg = find_ffmpeg(args.ffmpeg)
    out = Path(args.out)
    for sub in ("gaming", "essay", "doc"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    # Gaming: the long-VOD generator's OBS recording, 12 s.
    obs = out / "gaming" / "obs_1440p60.mkv"
    if not obs.exists():
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([sys.executable, "-I", str(root / "tests" / "perf" / "gen_longvod_media.py"), "--out", tmp,
                            "--ffmpeg", ffmpeg, "--minutes", "0.2", "--chunk-seconds", "4", "--only",
                            "longvod_h264_1440p60_2h.mkv"], check=True)
            recording = Path(tmp) / "longvod_h264_1440p60_2h.mkv"
            # The Mic track speaks (commentary for the captions and the transcript): the narration replaces the
            # generator's speech-like bursts, under the same title, padded with silence to the recording's 12 s.
            # (Not apad + -shortest: with the video stream-copied, -shortest cut the video at its first 2 s GOP.)
            commentary = Path(tmp) / "commentary.wav"
            if tts(COMMENTARY, commentary):
                run(ffmpeg, ["-i", str(recording), "-i", str(commentary), "-map", "0:v", "-map", "0:a:0", "-map", "1:a",
                             "-map", "0:a:2", "-c:v", "copy", "-c:a", "aac", "-b:a", "160k", "-ar", "48000", "-ac", "2",
                             "-filter:a:1", "apad=whole_dur=12,atrim=end=12", "-metadata:s:a:0", "title=Game",
                             "-metadata:s:a:1", "title=Mic", "-metadata:s:a:2", "title=Discord", str(obs)])
            else:
                shutil.move(str(recording), obs)

    # Essay: narration, stills, B-roll, music.
    vo = out / "essay" / "voiceover.wav"
    if not vo.exists() and not tts(NARRATION, vo):
        run(ffmpeg, ["-f", "lavfi", "-i", "sine=frequency=220:sample_rate=48000:duration=30", "-af",
                     "volume='0.4*(0.5+0.5*sin(2*PI*4*t))':eval=frame", "-ac", "1", str(vo)])
    for i, (source, label) in enumerate([("testsrc2=s=1920x1080", "1"), ("smptehdbars=s=1920x1080", "2"),
                                         ("mandelbrot=s=1920x1080", "3")], start=1):
        still = out / "essay" / f"still_{label}.png"
        if not still.exists():
            run(ffmpeg, ["-f", "lavfi", "-i", source, "-frames:v", "1", str(still)])
        jpeg = still.with_suffix(".jpg")
        if not jpeg.exists():
            run(ffmpeg, ["-i", str(still), "-q:v", "2", str(jpeg)])
    for i, source in enumerate(["testsrc2=s=1920x1080:r=30", "rgbtestsrc=s=1920x1080:r=30"], start=1):
        clip = out / "essay" / f"broll_{i}.mp4"
        if not clip.exists():
            run(ffmpeg, ["-f", "lavfi", "-i", f"{source},hue=H=0.5*t,format=yuv420p", "-f", "lavfi", "-i",
                         "anoisesrc=color=pink:amplitude=0.02:sample_rate=48000", "-t", "6", "-c:v", "libx264", "-preset",
                         "veryfast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2", str(clip)])
    music = out / "essay" / "music.wav"
    if not music.exists():
        chord = ("0.08*sin(2*PI*261.6*t)+0.06*sin(2*PI*329.6*t)+0.06*sin(2*PI*392*t)"
                 "+0.10*sin(2*PI*65.4*t)*(0.6+0.4*sin(2*PI*2*t))"
                 "+0.25*sin(2*PI*55*t)*exp(-12*mod(t,0.5))")
        run(ffmpeg, ["-f", "lavfi", "-i", f"aevalsrc='{chord}|{chord}':s=48000:d=60", "-c:a", "pcm_s16le", str(music)])

    # Documentary: one interview, two cameras and a lavalier at different starts.
    speech = out / "doc" / "interview_speech.wav"
    if not speech.exists() and not tts(INTERVIEW, speech):
        run(ffmpeg, ["-f", "lavfi", "-i", "sine=frequency=180:sample_rate=48000:duration=25", "-af",
                     "volume='0.4*(0.5+0.5*sin(2*PI*3*t))':eval=frame", "-ac", "1", str(speech)])
    angles = [("interview_a.mp4", "testsrc2=s=1920x1080:r=30", 0.0, 0.7), ("interview_b.mp4",
              "smptehdbars=s=1920x1080:r=30,hue=H=0.3*t", 0.4, 0.5)]
    for name, picture, late, gain in angles:
        clip = out / "doc" / name
        if not clip.exists():
            # The camera starts `late` seconds into the interview: its sound is the interview from there, plus its own
            # room noise.
            run(ffmpeg, ["-f", "lavfi", "-i", f"{picture},format=yuv420p", "-ss", f"{late}", "-i", str(speech), "-f", "lavfi",
                         "-i", "anoisesrc=color=brown:amplitude=0.01:sample_rate=48000", "-filter_complex",
                         f"[1:a]volume={gain},aformat=sample_rates=48000:channel_layouts=mono[s];[s][2:a]amix=inputs=2:"
                         "duration=first:normalize=0,aformat=channel_layouts=stereo[a]",
                         "-map", "0:v", "-map", "[a]", "-shortest", "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt",
                         "yuv420p", "-c:a", "aac", str(clip)])
    lav = out / "doc" / "lav.wav"
    if not lav.exists():
        run(ffmpeg, ["-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono", "-i", str(speech), "-filter_complex",
                     "[0:a]atrim=duration=0.2[pad];[pad][1:a]concat=n=2:v=0:a=1[a]", "-map", "[a]", "-c:a", "pcm_s24le",
                     str(lav)])
    print("rc media ready in", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
