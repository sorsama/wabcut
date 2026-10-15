#!/usr/bin/env python3
"""The 0.1 release workflows end to end (RC.1, docs/internal/release-checklist.md), through wabcut-cli.

  a  long-form gaming   OBS MKV (1440p60, Game / Mic / Discord) -> 1440p60 sequence -> named tracks -> razor + ripple
                        delete -> a callout template -> a speed ramp with sound -> Streamer voice + Game under voice ->
                        loudness for YouTube -> export 1440p60 -> ffprobe (codec, fps, size, duration, audio, loudness)
  b  short-form         the same moment -> Shorts sequence -> verticalBlurFill layout -> word-by-word captions ->
                        export 1080x1920@60 -> a Discord 10 MB file
  c  video essay        voice-over + stills + B-roll -> transcript -> captions styled -> lower third -> music ducked ->
                        loudness normalized -> export 1080p
  d  documentary        two cameras + a lavalier -> multicam synced by audio -> angle switches -> transcript edit (delete
                        words = ripple) -> auto colour -> a region redaction -> export -> FCP7 XML -> re-import report

Every step is a wabcut-cli call on a project file; the import is the editor's (the editor test case "Release workflows:
import each workflow's media list into a saved project" turns test-output/release/projects/<name>.txt into
<name>.wabcut). Results: test-output/release/results.json (each step: ok, what was checked, evidence).

Usage: rc_workflows.py --build build/win-msvc [--only a,b,c,d] [--ffmpeg PATH]

Another build's programs, read only (RC.2: the release build), with everything written elsewhere:
  rc_workflows.py --build <release build> --bin <release build>/bin --out <scratch>/release --media <media>
                  --import-tests <own build>/bin/wabcut_editor_tests.exe --import-dir <own build>/test-output/release/projects
The import case reads its lists from its own build's test-output (compiled in), so --import-dir names that folder and
the saved projects are moved from there to <out>/projects.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

TICKS_PER_SECOND = 254_016_000_000


class StepFailed(Exception):
    pass


class Runner:
    def __init__(self, build: Path, ffmpeg: str, bin_dir: Path | None = None, out: Path | None = None):
        self.build = build
        self.bin = bin_dir or build / "bin"
        self.cli = str(self.bin / "wabcut-cli.exe")
        self.ffmpeg = ffmpeg
        self.ffprobe = str(Path(ffmpeg).with_name("ffprobe.exe" if os.name == "nt" else "ffprobe"))
        self.out = out or build / "test-output" / "release"
        self.import_tests: Path | None = None
        self.import_dir: Path | None = None
        self.results: dict[str, list[dict]] = {}
        self.workflow = ""
        self.env = dict(os.environ)
        qt = Path("C:/Qt/6.11.3/msvc2022_64/bin")
        if qt.exists():
            self.env["PATH"] = str(qt) + os.pathsep + self.env["PATH"]
        self.env.pop("QT_QPA_PLATFORM", None)

    # ---- recording ------------------------------------------------------------------------------------------------

    def record(self, step: str, ok: bool, evidence: str) -> None:
        self.results.setdefault(self.workflow, []).append({"step": step, "ok": ok, "evidence": evidence})
        print(f"  [{'PASS' if ok else 'FAIL'}] {step}: {evidence}")

    def check(self, step: str, condition: bool, evidence: str) -> None:
        self.record(step, condition, evidence)

    # ---- the CLI --------------------------------------------------------------------------------------------------

    def run(self, args: list[str], timeout: int = 1800) -> subprocess.CompletedProcess:
        return subprocess.run([self.cli, *args], capture_output=True, env=self.env, timeout=timeout)

    def cmd(self, project: Path, command: str, params: dict | None = None, save: bool = True) -> dict:
        request = json.dumps({"command": command, "params": params or {}})
        req_file = project.with_suffix(".request.json")
        req_file.write_bytes(request.encode("utf-8"))
        args = ["cmd", str(project), "--json-file", str(req_file)]
        if save:
            args.append("--save")
        r = self.run(args)
        text = r.stdout.decode("utf-8", errors="replace")
        try:
            result = json.loads(text)
        except json.JSONDecodeError:
            raise StepFailed(f"{command}: exit {r.returncode}, no JSON: {text[:300]} {r.stderr.decode('utf-8', 'replace')[:300]}")
        if not result.get("ok", False):
            raise StepFailed(f"{command}: {json.dumps(result.get('error', result))[:600]}")
        return result

    def render(self, project: Path, out: Path, preset: str, sequence: str | None = None, extra: list[str] | None = None) -> float:
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists():
            out.unlink()
        args = ["render", str(project), str(out), "--preset", preset]
        if sequence:
            args += ["--sequence", sequence]
        args += extra or []
        started = time.monotonic()
        r = self.run(args, timeout=3600)
        if r.returncode != 0 or not out.exists():
            raise StepFailed(f"render {preset}: exit {r.returncode}: {r.stderr.decode('utf-8', 'replace')[-600:]}")
        return time.monotonic() - started

    # ---- checking outputs -----------------------------------------------------------------------------------------

    def probe(self, file: Path) -> dict:
        r = subprocess.run([self.ffprobe, "-v", "error", "-show_streams", "-show_format", "-of", "json", str(file)],
                           capture_output=True)
        return json.loads(r.stdout)

    def loudness(self, file: Path) -> tuple[float, float]:
        """Integrated LUFS and true peak (dBTP) of the file's first audio stream (FFmpeg's ebur128)."""
        r = subprocess.run([self.ffmpeg, "-hide_banner", "-nostats", "-i", str(file), "-map", "0:a:0", "-af",
                            "ebur128=peak=true", "-f", "null", "-"], capture_output=True)
        text = r.stderr.decode("utf-8", errors="replace")
        summary = text[text.rfind("Summary:"):]
        lufs = re.search(r"I:\s+(-?[\d.]+) LUFS", summary)
        peak = re.search(r"Peak:\s+(-?[\d.]+) dBFS", summary)
        return (float(lufs.group(1)) if lufs else float("nan"), float(peak.group(1)) if peak else float("nan"))

    def describe(self, file: Path) -> dict:
        info = self.probe(file)
        video = [s for s in info.get("streams", []) if s.get("codec_type") == "video"]
        audio = [s for s in info.get("streams", []) if s.get("codec_type") == "audio"]
        d = {"size_mb": round(file.stat().st_size / 1e6, 2), "duration": float(info["format"].get("duration", 0)),
             "video": None, "audio": len(audio)}
        if video:
            v = video[0]
            num, den = (int(x) for x in v.get("r_frame_rate", "0/1").split("/"))
            d["video"] = {"codec": v.get("codec_name"), "width": v.get("width"), "height": v.get("height"),
                          "fps": round(num / den, 3) if den else 0}
        if audio:
            d["audio_codec"] = audio[0].get("codec_name")
            d["audio_channels"] = audio[0].get("channels")
            d["lufs"], d["true_peak"] = self.loudness(file)
        return d

    # ---- projects -------------------------------------------------------------------------------------------------

    def import_projects(self, lists: dict[str, list[Path]]) -> None:
        projects = self.out / "projects"
        lists_dir = self.import_dir or projects
        for folder in {projects, lists_dir}:
            folder.mkdir(parents=True, exist_ok=True)
            for old in folder.glob("*"):
                old.unlink()
        for name, files in lists.items():
            (lists_dir / f"{name}.txt").write_bytes(("\n".join(str(f) for f in files) + "\n").encode("utf-8"))
        tests = self.import_tests or self.bin / ("wabcut_editor_tests.exe" if os.name == "nt" else "wabcut_editor_tests")
        r = subprocess.run([str(tests), "Release workflows: import each workflow's media list into a saved project"],
                           capture_output=True, env=self.env, timeout=600)
        if r.returncode != 0:
            raise StepFailed("import: " + r.stdout.decode("utf-8", "replace")[-1500:])
        if lists_dir != projects:
            for name in lists:
                shutil.move(str(lists_dir / f"{name}.wabcut"), str(projects / f"{name}.wabcut"))

    def project(self, name: str) -> Path:
        return self.out / "projects" / f"{name}.wabcut"


def seconds(t: float) -> int:
    return int(round(t * TICKS_PER_SECOND))


def tc(seconds_: float, fps: int) -> str:
    frames = int(round(seconds_ * fps))
    return f"{frames // (3600 * fps):02d}:{frames // (60 * fps) % 60:02d}:{frames // fps % 60:02d}:{frames % fps:02d}"


def summary(r: Runner, project: Path, sequence: int | None = None) -> dict:
    params = {} if sequence is None else {"sequence": sequence}
    return r.cmd(project, "sequence.summary", params, save=False)["data"]


def created(result: dict) -> list:
    return result.get("createdIds") or result.get("data", {}).get("createdIds") or []


def parse_tc(text: str, fps: float) -> float:
    h, m, s, f = (int(x) for x in re.split(r"[:;]", text))
    return h * 3600 + m * 60 + s + f / fps


def tracks_of(r: Runner, project: Path, sequence: int) -> list[dict]:
    return summary(r, project, sequence).get("tracks", [])


def clip_at(r: Runner, project: Path, sequence: int, track: str, at: float, fps: float) -> int | None:
    for t in tracks_of(r, project, sequence):
        if t.get("track") == track:
            for c in t.get("clips", []):
                if parse_tc(c[2], fps) <= at < parse_tc(c[3], fps):
                    return int(c[0])
    return None


def duration_of(r: Runner, project: Path, sequence: int, fps: float) -> float:
    """Where the content ends (the sequence's own duration only grows: a ripple delete keeps it)."""
    ends = [parse_tc(c[3], fps) for t in tracks_of(r, project, sequence) for c in t.get("clips", [])]
    return max(ends, default=0.0)


def templates(r: Runner, project: Path) -> list[dict]:
    data = r.cmd(project, "template.list", {}, save=False)["data"]
    return list(data.get("builtin", [])) + list(data.get("project", [])) + list(data.get("user", []))


def callout(r: Runner, project: Path, sequence: int, at: str, kinds: tuple[str, ...], text: str, fallback_style: dict,
            step: str) -> None:
    found = [t for t in templates(r, project) if any(k in (t.get("id", "") + t.get("name", "")).lower() for k in kinds)]
    if found:
        t = found[0]
        r.cmd(project, "template.instantiate", {"sequence": sequence, "template": t["id"], "at": at, "track": "V2",
                                               "placement": "freeTrackAtOrAbove"})
        r.record(step, True, f"template {t['id']} ({t.get('name')})")
    else:
        r.cmd(project, "title.add", {"sequence": sequence, "text": text, "at": at, "duration": "00:00:03:00", "track": "V2",
                                     "placement": "freeTrackAtOrAbove", "style": fallback_style})
        r.record(step, False, "no matching built-in template (template.list); a title.add text graphic stands in")


def check_export(r: Runner, step: str, file: Path, width: int, height: int, fps: float, duration: float,
                 lufs: float | None = None, codec: str | None = None) -> dict:
    d = r.describe(file)
    v = d.get("video") or {}
    problems = []
    if (v.get("width"), v.get("height")) != (width, height):
        problems.append(f"size {v.get('width')}x{v.get('height')}")
    if abs(v.get("fps", 0) - fps) > 0.01:
        problems.append(f"fps {v.get('fps')}")
    if codec and v.get("codec") != codec:
        problems.append(f"codec {v.get('codec')}")
    if abs(d["duration"] - duration) > 0.15:
        problems.append(f"duration {d['duration']:.2f} s, sequence {duration:.2f} s")
    if d.get("audio", 0) < 1:
        problems.append("no audio")
    if lufs is not None and not (abs(d.get("lufs", 99) - lufs) <= 1.0):
        problems.append(f"loudness {d.get('lufs')} LUFS, target {lufs}")
    if lufs is not None and d.get("true_peak", 99) > -0.5:
        problems.append(f"true peak {d.get('true_peak')} dBFS")
    r.check(step, not problems, (", ".join(problems) + " | " if problems else "") + json.dumps(d))
    return d


# ---- a: long-form gaming ------------------------------------------------------------------------------------------

def workflow_a(r: Runner, media: Path) -> None:
    p = r.project("gaming")
    obs = "obs_1440p60.mkv"
    fps = 60
    seq = created(r.cmd(p, "sequence.create", {"preset": "1440p60", "name": "Clutch round (long-form)"}))[0]
    r.record("new sequence from the 1440p60 preset", True, f"sequence {seq}")
    r.cmd(p, "clip.insert", {"media": obs, "at": "00:00:00:00", "sequence": seq})
    names = {t["track"]: t.get("name", "") for t in tracks_of(r, p, seq) if t["track"].startswith("A")}
    r.check("each OBS stream on its own named, linked track", [names.get(k) for k in ("A1", "A2", "A3")] ==
            ["Game", "Mic", "Discord"], f"audio tracks {names}")
    # Premiere ops: razor at 3 s and 5 s on every track, ripple delete the piece between.
    for at in (3, 5):
        r.cmd(p, "sequence.addEdit", {"sequence": seq, "time": tc(at, fps), "tracks": ["V1", "A1", "A2", "A3"],
                                      "linked": True})
    piece = clip_at(r, p, seq, "V1", 4.0, fps)
    r.cmd(p, "timeline.rippleDelete", {"sequence": seq, "clips": [piece], "linked": True})
    length = duration_of(r, p, seq, fps)
    r.check("razor + ripple delete (Premiere ops)", abs(length - 10.0) < 0.02, f"sequence {length:.2f} s after cutting 2 s")
    callout(r, p, seq, tc(1, fps), ("callout", "sticker", "badge"), "CLUTCH!",
            {"size": 160, "stroke.width": 8, "fill.red": 1, "fill.green": 0.85, "fill.blue": 0.1},
            "a built-in callout template")
    # A speed ramp with sound: remap the last piece, 50 % between 5 s and 7 s.
    last = clip_at(r, p, seq, "V1", 7.0, fps)
    r.cmd(p, "timeRemap.enable", {"sequence": seq, "clip": last})
    for at in (5, 7):
        r.cmd(p, "timeRemap.addKey", {"sequence": seq, "clip": last, "time": tc(at, fps)})
    r.cmd(p, "timeRemap.setSpeed", {"sequence": seq, "clip": last, "time": tc(6, fps), "speed": 50, "extendClip": True})
    length = duration_of(r, p, seq, fps)
    ramp = r.cmd(p, "audio.loudness", {"sequence": seq, "from": tc(5.2, fps), "to": tc(8.8, fps),
                                       "targets": [{"track": "A2"}]}, save=False)["data"]
    r.check("speed ramp (50 %) plays its audio", "inf" not in json.dumps(ramp).lower(),
            f"sequence {length:.2f} s; Mic loudness in the slowed span {json.dumps(ramp)[:240]}")
    voice = r.cmd(p, "audio.essentialSound", {"sequence": seq, "tracks": ["A2"], "type": "dialogue", "preset": "streamerVoice"})
    r.record("Streamer voice on the Mic track", True, json.dumps(voice.get("data", {}))[:300])
    duck = r.cmd(p, "audio.autoDuck", {"sequence": seq, "preset": "gameUnderVoice"})
    r.record("Game under voice", True, json.dumps(duck.get("data", {}))[:300])
    norm = r.cmd(p, "audio.normalize", {"sequence": seq, "preset": "youtube", "limit": True})
    r.record("loudness for YouTube (-14 LUFS, -1 dBTP)", True, json.dumps(norm.get("data", {}))[:200])
    out = r.out / "exports" / "a_youtube_1440p60.mp4"
    took = r.render(p, out, "youtube-1440p60", str(seq))
    check_export(r, f"export YouTube 1440p60 (preset youtube-1440p60, {took:.1f} s)", out, 2560, 1440, 60, length, lufs=-14.0)
    # A Discord 10 MB file of the whole long-form edit (the size target's rate control, not a short clip's luck).
    discord = r.out / "exports" / "a_discord_10mb.mp4"
    took = r.render(p, discord, "discord-10mb", str(seq))
    d = r.describe(discord)
    r.check(f"Discord 10 MB target (preset discord-10mb, {took:.1f} s)", discord.stat().st_size <= 10 * 1000 * 1000 and
            d.get("audio", 0) >= 1 and abs(d["duration"] - length) <= 0.15, json.dumps(d))


# ---- b: short-form ------------------------------------------------------------------------------------------------

def workflow_b(r: Runner, media: Path) -> None:
    p = r.project("gaming")
    obs = "obs_1440p60.mkv"
    fps = 60
    seq = created(r.cmd(p, "sequence.create", {"preset": "shorts", "name": "Clutch (Short)"}))[0]
    r.cmd(p, "clip.insert", {"media": obs, "at": "00:00:00:00", "sequence": seq, "sourceIn": tc(1, fps),
                             "sourceOut": tc(9, fps)})
    gameplay = clip_at(r, p, seq, "V1", 1.0, fps)
    r.cmd(p, "layout.apply", {"sequence": seq, "gameplay": gameplay, "preset": "verticalBlurFill",
                              "facecamRegion": {"x": 0.72, "y": 0.70, "width": 0.25, "height": 0.25}})
    r.record("9:16 layout (layout.apply verticalBlurFill + facecam region)", True, f"sequence {seq}, gameplay clip {gameplay}")
    mic = clip_at(r, p, seq, "A2", 1.0, fps)
    tx = r.cmd(p, "transcript.transcribe", {"sequence": seq, "clips": [mic], "provider": "local", "language": "en"})
    r.record("transcript of the Mic (local)", True, json.dumps(tx.get("data", {}))[:200])
    cap = r.cmd(p, "captions.generateFromTranscript", {"sequence": seq, "preset": "Shorts", "maxCharsPerLine": 12,
                                                     "maxLines": 1, "maxDuration": 1.5, "minDuration": 0.2})
    cues = r.cmd(p, "captions.get", {"sequence": seq}, save=False)["data"]
    count = len(cues.get("cues", []))
    r.check("word-by-word captions (Shorts style)", count >= 5, f"{count} cues; {json.dumps(cues)[:240]}")
    length = duration_of(r, p, seq, fps)
    out = r.out / "exports" / "b_shorts_1080x1920.mp4"
    took = r.render(p, out, "shorts-1080x1920-60", str(seq))
    check_export(r, f"export Shorts 1080x1920@60 (preset shorts-1080x1920-60, {took:.1f} s)", out, 1080, 1920, 60, length)
    discord = r.out / "exports" / "b_shorts_discord_10mb.mp4"
    took = r.render(p, discord, "discord-10mb", str(seq))
    size = discord.stat().st_size
    r.check(f"the Short as a Discord 10 MB file ({took:.1f} s)", size <= 10 * 1000 * 1000, f"{size / 1e6:.2f} MB")


# ---- c: video essay -----------------------------------------------------------------------------------------------

def workflow_c(r: Runner, media: Path) -> None:
    p = r.project("essay")
    fps = 30
    seq = created(r.cmd(p, "sequence.create", {"preset": "1080p30", "name": "The comeback (essay)"}))[0]
    r.cmd(p, "clip.insert", {"media": "voiceover.wav", "at": "00:00:00:00", "sequence": seq, "video": None, "audio": ["A1"]})
    narration = duration_of(r, p, seq, fps)
    at = 0.0
    for name, length in (("still_1.jpg", 5), ("broll_1.mp4", 6), ("still_2.jpg", 5), ("broll_2.mp4", 6), ("still_3.jpg", 8)):
        params = {"media": name, "at": tc(at, fps), "sequence": seq, "audio": []}
        if name.endswith(".mp4"):
            params["sourceOut"] = tc(length, fps)
        r.cmd(p, "clip.overwrite", params)
        at += length
    r.record("voice-over + stills + B-roll", True, f"narration {narration:.1f} s, pictures to {at:.0f} s")
    tx = r.cmd(p, "transcript.transcribe", {"sequence": seq, "media": ["voiceover.wav"], "provider": "local", "language": "en"})
    words = r.cmd(p, "transcript.words", {"sequence": seq, "limit": 1000}, save=False)["data"]
    count = len(words.get("words", []))
    r.check("transcript (local)", count >= 30, f"{count} words; {json.dumps(tx.get('data', {}))[:160]}")
    r.cmd(p, "captions.generateFromTranscript", {"sequence": seq, "preset": "Bold Box"})
    r.cmd(p, "captions.applyPreset", {"sequence": seq, "preset": "Clean"})
    cues = r.cmd(p, "captions.get", {"sequence": seq}, save=False)["data"].get("cues", [])
    r.check("captions styled (Bold Box, then Clean)", len(cues) >= 3, f"{len(cues)} cues")
    callout(r, p, seq, tc(1, fps), ("lower", "third", "name"), "A. Example\nVideo essayist",
            {"size": 60, "alignment": 0, "shadow.opacity": 0.6}, "a lower third template")
    r.cmd(p, "clip.overwrite", {"media": "music.wav", "at": "00:00:00:00", "sequence": seq, "video": None, "audio": ["A2"],
                                "sourceOut": tc(min(narration, 59), fps)})
    r.cmd(p, "track.setRole", {"sequence": seq, "track": "A1", "role": "dialogue"})
    r.cmd(p, "track.setRole", {"sequence": seq, "track": "A2", "role": "music"})
    duck = r.cmd(p, "audio.autoDuck", {"sequence": seq})
    r.record("music ducked under the narration", True, json.dumps(duck.get("data", {}))[:240])
    r.cmd(p, "audio.normalize", {"sequence": seq, "preset": "youtube", "limit": True})
    length = duration_of(r, p, seq, fps)
    out = r.out / "exports" / "c_essay_1080p.mp4"
    took = r.render(p, out, "youtube-1080p", str(seq))
    check_export(r, f"export 1080p ({took:.1f} s)", out, 1920, 1080, 30, length, lufs=-14.0)
    # PNG stills: a second sequence with one PNG, exported.
    png = created(r.cmd(p, "sequence.create", {"preset": "1080p30", "name": "PNG still"}))[0]
    r.cmd(p, "clip.insert", {"media": "still_1.png", "at": "00:00:00:00", "sequence": png, "audio": []})
    try:
        r.render(p, r.out / "exports" / "c_png_still.mp4", "youtube-1080p", str(png))
        r.record("a PNG still exports", True, "rendered")
    except StepFailed as e:
        r.record("a PNG still exports", False, str(e)[-200:].replace("\n", " "))


# ---- d: documentary -----------------------------------------------------------------------------------------------

def workflow_d(r: Runner, media: Path) -> None:
    p = r.project("doc")
    fps = 30
    mc = r.cmd(p, "multicam.create", {"media": ["interview_a.mp4", "interview_b.mp4", "lav.wav"], "sync": "audio",
                                      "name": "Interview multicam"})
    mc_id = created(mc)[0]
    r.record("multicam synced by audio", True, json.dumps(mc.get("data", {}))[:300])
    seq = created(r.cmd(p, "sequence.create", {"preset": "1080p30", "name": "Streamer story (documentary)"}))[0]
    r.cmd(p, "clip.insert", {"sourceSequence": mc_id, "at": "00:00:00:00", "sequence": seq})
    clip = clip_at(r, p, seq, "V1", 1.0, fps)
    r.cmd(p, "multicam.switchAngle", {"sequence": seq, "clip": clip, "switches": [{"angle": 2, "at": tc(6, fps)},
                                                                                {"angle": 1, "at": tc(12, fps)}]})
    r.record("angle switches", True, "angle 2 at 6 s, angle 1 at 12 s")
    r.cmd(p, "transcript.transcribe", {"sequence": seq, "media": ["lav.wav", "interview_a.mp4", "interview_b.mp4"],
                                       "provider": "local", "language": "en"})
    words = r.cmd(p, "transcript.words", {"sequence": seq, "limit": 1000}, save=False)["data"].get("words", [])
    r.check("transcript words through the multicam clip", len(words) >= 20,
            f"{len(words)} words (the multicam source sequence has its angles' words)")
    if len(words) < 20:
        # Flatten (Premiere's Flatten): the angles' clips replace the multicam clips, then the words are the clips'.
        for c in [int(c[0]) for t in tracks_of(r, p, seq) if t["track"] == "V1" for c in t.get("clips", [])]:
            r.cmd(p, "multicam.flatten", {"sequence": seq, "clip": c})
        words = r.cmd(p, "transcript.words", {"sequence": seq, "limit": 1000}, save=False)["data"].get("words", [])
        r.check("transcript words after multicam.flatten", len(words) >= 20, f"{len(words)} words")
    before = duration_of(r, p, seq, fps)
    r.cmd(p, "transcript.deleteRanges", {"sequence": seq, "words": [[3, 6]]})
    after = duration_of(r, p, seq, fps)
    r.check("transcript edit: deleting words ripples", after < before - 0.3, f"{before:.2f} s -> {after:.2f} s")
    # Auto colour: the media clips here (what a transcript deletion flattened), and the angles in the multicam source
    # (a multicam clip has no picture of its own to analyse; colouring its angles colours every use).
    v1 = [c for t in tracks_of(r, p, seq) if t["track"] == "V1" for c in t.get("clips", [])]
    media_clips = [int(c[0]) for c in v1 if c[1] != "Interview multicam"]
    multicam_clips = [int(c[0]) for c in v1 if c[1] == "Interview multicam"]
    if multicam_clips:
        try:
            r.cmd(p, "color.auto", {"sequence": seq, "clips": multicam_clips}, save=False)
            r.record("auto colour on a multicam clip", True, "accepted")
        except StepFailed as e:
            r.record("auto colour on a multicam clip", False, str(e)[:200])
    angles = [int(c[0]) for t in tracks_of(r, p, mc_id) if t["track"].startswith("V") for c in t.get("clips", [])]
    color = r.cmd(p, "color.auto", {"sequence": mc_id, "clips": angles})
    if media_clips:
        r.cmd(p, "color.auto", {"sequence": seq, "clips": media_clips})
    r.record("auto colour (the angles in the multicam source, and the flattened clips)", True,
             json.dumps(color.get("data", {}))[:200])
    red = r.cmd(p, "redaction.add", {"media": "interview_a.mp4", "source": {"start": 0, "end": seconds(20)},
                                     "region": {"x": 0.05, "y": 0.05, "width": 0.3, "height": 0.2, "shape": "rectangle"},
                                     "style": "mosaic", "name": "Name tag"})
    r.record("region redaction (mosaic)", True, json.dumps(red.get("data", {}))[:160])
    out = r.out / "exports" / "d_documentary_1080p.mp4"
    took = r.render(p, out, "youtube-1080p", str(seq))
    check_export(r, f"export 1080p ({took:.1f} s)", out, 1920, 1080, 30, after)
    xml = r.out / "exports" / "d_documentary.xml"
    report = r.cmd(p, "interchange.export", {"sequence": seq, "format": "fcp7-xml", "path": str(xml)}, save=False)
    r.check("timeline export to FCP7 XML", xml.exists(), json.dumps(report.get("data", {}))[:300])
    back = r.run(["import", str(xml), "--json"])
    text = back.stdout.decode("utf-8", "replace")
    r.check("re-import report", back.returncode == 0 and len(text) > 10, text[:400].replace("\n", " "))


WORKFLOWS = {"a": workflow_a, "b": workflow_b, "c": workflow_c, "d": workflow_d}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", required=True)
    parser.add_argument("--only", default="a,b,c,d")
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    parser.add_argument("--no-import", action="store_true")
    parser.add_argument("--bin", help="the programs (default <build>/bin): a copy, while the build dir is rebuilt")
    parser.add_argument("--out", help="projects, exports and results (default <build>/test-output/release)")
    parser.add_argument("--media", help="the media of gen_rc_media.py (default <out>/media)")
    parser.add_argument("--import-tests", help="the editor test program for the import (default <bin>/wabcut_editor_tests)")
    parser.add_argument("--import-dir", help="the folder that program's import case reads (default <out>/projects)")
    args = parser.parse_args()
    build = Path(args.build).resolve()
    r = Runner(build, args.ffmpeg, Path(args.bin).resolve() if args.bin else None,
               Path(args.out).resolve() if args.out else None)
    r.import_tests = Path(args.import_tests).resolve() if args.import_tests else None
    r.import_dir = Path(args.import_dir).resolve() if args.import_dir else None
    media = Path(args.media).resolve() if args.media else r.out / "media"
    lists = {
        "gaming": [media / "gaming" / "obs_1440p60.mkv"],
        "essay": [media / "essay" / n for n in ("voiceover.wav", "still_1.jpg", "still_2.jpg", "still_3.jpg", "broll_1.mp4",
                                               "broll_2.mp4", "music.wav", "still_1.png")],
        "doc": [media / "doc" / n for n in ("interview_a.mp4", "interview_b.mp4", "lav.wav")],
    }
    if not args.no_import:
        r.import_projects(lists)
    for key in args.only.split(","):
        r.workflow = key
        print(f"workflow {key}")
        try:
            WORKFLOWS[key](r, media)
        except StepFailed as e:
            r.record("stopped", False, str(e))
        except Exception as e:  # noqa: BLE001 - a broken step must not hide the others
            r.record("stopped", False, f"{type(e).__name__}: {e}")
    (r.out / "results.json").write_bytes(json.dumps(r.results, indent=2).encode("utf-8"))
    failed = sum(1 for steps in r.results.values() for s in steps if not s["ok"])
    print(f"{failed} failed step(s); results in {r.out / 'results.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
