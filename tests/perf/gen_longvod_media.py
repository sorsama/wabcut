#!/usr/bin/env python3
"""Generate the A.10 long-VOD and high-frame-rate benchmark recordings (tests/perf, docs/performance.md).

Two gaming recordings as the capture tools write them, with camera-like pictures (an animated test pattern,
grain-like temporal noise so the bitrate is close to real footage, a hue drift):

    longvod_h264_1440p60_2h.mkv    OBS-style (Matroska, OBS's classic default): 2560x1440 60 fps, H.264 High 4:2:0
                                   8-bit (h264_nvenc preset p5, CBR 24 Mb/s, GOP 120 = 2 s, 2 B-frames: OBS's NVENC
                                   defaults; libx264 veryfast with the same GOP and bitrate where NVENC is missing),
                                   2 hours, and three AAC-LC 48 kHz stereo 160 kb/s tracks titled "Game" (a broadband
                                   bed: noise, tones, loud bursts), "Mic" (speech-like bursts with pauses) and
                                   "Discord" (a quieter second voice). About 22 GB.
    highfps_hevc_1080p240_30m.mp4  ShadowPlay-style: 1920x1080 240 fps, HEVC Main 8-bit (hevc_nvenc, CBR 50 Mb/s,
                                   GOP 480 = 2 s, no B-frames, hvc1; libx265 ultrafast where NVENC is missing),
                                   30 minutes, two untitled AAC tracks (game, mic: the default handler names).
                                   About 11 GB.

Disk: about 33 GB for both, plus about 1 GB of intermediates while one is built (deleted when it is done). Keep
them out of git: CMake writes them to build/<preset>/test-output/perf-longvod-media.

Fast generation: the video is encoded once, as one chunk (--chunk-seconds, a multiple of the 2 s GOP; every GOP is
closed, so the chunk ends on a GOP boundary), and repeated to the full length with the concat demuxer and a stream
copy: a 2-hour file costs a minute of encoding plus a copy. Each concat entry carries the chunk's exact duration, so
the timestamps run on without a gap. The audio tracks are encoded at full length instead, one FFmpeg per track in
parallel (the chunk's AAC would repeat the encoder's priming frame at every joint): a 60 s pattern looped. The video
chunk and the audio tracks are encoded at the same time; the final mux is a stream copy.

Usage: gen_longvod_media.py --out DIR [--ffmpeg PATH] [--minutes N] [--chunk-seconds N] [--only NAME]
--minutes overrides both lengths for quick runs (the recipe changes, so the full length is generated again later;
lengths round up to whole chunks). A recording is skipped when it exists with the same recipe (manifest.json in
DIR); it is written as <name>.partial.<ext> and renamed when complete. Exit 0 when every requested recording exists.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import shutil
import subprocess
import sys
import time
from pathlib import Path

# The FFmpeg lookup and the encoder probe are gen_perf_bench_media.py's (the same folder; also under python -I).
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_perf_bench_media import BT709, COMMON, find_ffmpeg, has_encoder  # noqa: E402

SAMPLE_RATE = 48000
AAC = ["-c:a", "aac", "-b:a", "160k", "-ar", str(SAMPLE_RATE), "-ac", "2"]  # AAC-LC, the encoder's default profile
PATTERN_SECONDS = 60  # each audio track is a looped pattern this long


def video_graph(size: str, fps: int, seconds: int, seed: int) -> list[str]:
    graph = (f"testsrc2=s={size}:r={fps},format=yuv420p,noise=alls=10:allf=t+u:all_seed={seed},"
             f"hue=H=0.6*sin(2*PI*t/7),trim=duration={seconds},setpts=PTS-STARTPTS,"
             f"setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709,format=yuv420p[v]")
    # Filter sources through -filter_complex (some FFmpeg builds have no lavfi input device).
    return ["-filter_complex", graph, "-map", "[v]", "-an"]


def looped(pattern: str, seconds: int) -> str:
    """`pattern` (a mono graph ending in [p], PATTERN_SECONDS long) repeated to `seconds`, stereo, as [a]."""
    length = min(PATTERN_SECONDS, seconds)
    return (f"{pattern};[p]atrim=duration={length},aloop=loop=-1:size={length * SAMPLE_RATE},"
            f"atrim=duration={seconds},aformat=sample_fmts=fltp:channel_layouts=stereo[a]")


def game_audio(seconds: int, seed: int) -> str:
    # A broadband bed: pink noise, a low tone and a pulsing higher one, and a loud 0.4 s burst every 9.7 s.
    length = min(PATTERN_SECONDS, seconds)
    pattern = (f"anoisesrc=d={length}:c=pink:r={SAMPLE_RATE}:a=0.25:s={seed},aformat=channel_layouts=mono[n];"
               f"aevalsrc=exprs='0.12*sin(2*PI*110*t)+0.08*sin(2*PI*440*t)*(0.5+0.5*sin(2*PI*t/3))'"
               f":s={SAMPLE_RATE}:c=mono:d={length}[t];"
               f"[n][t]amix=inputs=2:duration=shortest:normalize=0,"
               f"volume='1+1.2*lt(mod(t+{seed % 7},9.7),0.4)':eval=frame[p]")
    return looped(pattern, seconds)


def mic_audio(seconds: int) -> str:
    # Speech-like: three harmonics of a 170 Hz voice with a slow pitch wobble, syllables at 4.2 Hz, talking about
    # 60 % of the time (on/off with a 5.3 s period).
    length = min(PATTERN_SECONDS, seconds)
    pattern = (f"aevalsrc=exprs='0.35*(sin(2*PI*170*t+3*sin(2*PI*0.6*t))+0.5*sin(4*PI*170*t+6*sin(2*PI*0.6*t))"
               f"+0.25*sin(6*PI*170*t+9*sin(2*PI*0.6*t)))*(0.55+0.45*sin(2*PI*4.2*t))*gt(sin(2*PI*t/5.3),-0.3)'"
               f":s={SAMPLE_RATE}:c=mono:d={length}[p]")
    return looped(pattern, seconds)


def discord_audio(seconds: int, seed: int) -> str:
    # A quieter second voice (230 Hz, other syllable and turn rates, mostly while the mic pauses) over a faint hiss.
    length = min(PATTERN_SECONDS, seconds)
    pattern = (f"aevalsrc=exprs='0.16*(sin(2*PI*230*t+2*sin(2*PI*0.9*t))+0.4*sin(4*PI*230*t+4*sin(2*PI*0.9*t)))"
               f"*(0.6+0.4*sin(2*PI*3.3*t))*gt(sin(2*PI*(t+2.6)/7.1),0.1)':s={SAMPLE_RATE}:c=mono:d={length}[v];"
               f"anoisesrc=d={length}:c=white:r={SAMPLE_RATE}:a=0.004:s={seed},aformat=channel_layouts=mono[h];"
               f"[v][h]amix=inputs=2:duration=shortest:normalize=0[p]")
    return looped(pattern, seconds)


def recordings(seconds_2h: int, seconds_30m: int, nvenc_h264: bool, nvenc_hevc: bool) -> dict[str, dict]:
    h264 = (["-c:v", "h264_nvenc", "-preset", "p5", "-profile:v", "high", "-rc", "cbr", "-b:v", "24M",
             "-maxrate", "24M", "-bufsize", "24M", "-g", "120", "-bf", "2"]
            if nvenc_h264 else
            ["-c:v", "libx264", "-preset", "veryfast", "-profile:v", "high", "-g", "120", "-keyint_min", "120",
             "-sc_threshold", "0", "-bf", "2", "-b:v", "24M", "-maxrate", "24M", "-bufsize", "24M",
             "-x264-params", "nal-hrd=cbr"])
    hevc = (["-c:v", "hevc_nvenc", "-preset", "p4", "-profile:v", "main", "-rc", "cbr", "-b:v", "50M",
             "-maxrate", "50M", "-bufsize", "50M", "-g", "480", "-bf", "0"]
            if nvenc_hevc else
            ["-c:v", "libx265", "-preset", "ultrafast", "-b:v", "50M", "-x265-params",
             "keyint=480:min-keyint=480:bframes=0:scenecut=0:open-gop=0:vbv-maxrate=50000:vbv-bufsize=50000"
             ":log-level=error"])
    return {
        "longvod_h264_1440p60_2h.mkv": {
            "seconds": seconds_2h,
            "size": "2560x1440",
            "fps": 60,
            "seed": 41,
            "encoder": "h264_nvenc" if nvenc_h264 else "libx264",
            "video": [*h264, "-pix_fmt", "yuv420p", *BT709],
            "audio": [("Game", game_audio(seconds_2h, 5)), ("Mic", mic_audio(seconds_2h)),
                      ("Discord", discord_audio(seconds_2h, 9))],
            "bitrate": 24_000_000,
            "mux": [],
        },
        "highfps_hevc_1080p240_30m.mp4": {
            "seconds": seconds_30m,
            "size": "1920x1080",
            "fps": 240,
            "seed": 43,
            "encoder": "hevc_nvenc" if nvenc_hevc else "libx265",
            "video": [*hevc, "-pix_fmt", "yuv420p", "-tag:v", "hvc1", *BT709],
            # Untitled, as ShadowPlay writes them (the MP4 muxer's default handler name).
            "audio": [(None, game_audio(seconds_30m, 13)), (None, mic_audio(seconds_30m))],
            "bitrate": 50_000_000,
            "mux": ["-tag:v", "hvc1"],
        },
    }


def run(label: str, cmd: list[str]) -> tuple[str, int, str, float]:
    started = time.monotonic()
    result = subprocess.run(cmd, capture_output=True, text=True)
    return label, result.returncode, result.stderr.strip(), time.monotonic() - started


def build(ffmpeg: str, name: str, spec: dict, chunk: int, out_dir: Path) -> tuple[bool, str]:
    """Encodes the chunk and the audio tracks, then muxes `name` in `out_dir`. Returns (ok, message)."""
    target = out_dir / name
    work = out_dir / f".work-{target.stem}"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    seconds = spec["seconds"]
    chunks = seconds // chunk

    chunk_file = work / "chunk.mp4"
    jobs = [(f"video chunk {chunk} s ({spec['encoder']})",
             [ffmpeg, *COMMON, *video_graph(spec["size"], spec["fps"], chunk, spec["seed"]), *spec["video"],
              str(chunk_file)])]
    audio_files = []
    for i, (title, graph) in enumerate(spec["audio"]):
        audio_file = work / f"audio{i}.mka"
        audio_files.append(audio_file)
        jobs.append((f"audio {title or i + 1} {seconds} s",
                     [ffmpeg, *COMMON, "-filter_complex", graph, "-map", "[a]", *AAC, str(audio_file)]))
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as pool:
        for label, code, stderr, elapsed in pool.map(lambda job: run(*job), jobs):
            if code != 0:
                return False, f"{label}: {stderr[:600]} (intermediates kept in {work})"
            print(f"    {label}: {elapsed:.1f} s", flush=True)

    # The chunk repeated to the full length: every entry with its exact duration, so the timestamps are continuous.
    playlist = work / "chunks.ffconcat"
    playlist.write_text("ffconcat version 1.0\n" + f"file 'chunk.mp4'\nduration {chunk}\n" * chunks)
    partial = target.with_name(target.stem + ".partial" + target.suffix)
    mux = [ffmpeg, *COMMON, "-f", "concat", "-safe", "0", "-i", str(playlist)]
    for audio_file in audio_files:
        mux += ["-i", str(audio_file)]
    mux += ["-map", "0:v:0"]
    for i in range(len(audio_files)):
        mux += ["-map", f"{i + 1}:a:0"]
    mux += ["-c", "copy", "-map_metadata", "-1", *spec["mux"]]
    for i, (title, _) in enumerate(spec["audio"]):
        if title:
            mux += [f"-metadata:s:a:{i}", f"title={title}"]
    label, code, stderr, elapsed = run(f"mux {chunks} x {chunk} s + {len(audio_files)} audio tracks",
                                       [*mux, str(partial)])
    if code != 0:
        partial.unlink(missing_ok=True)
        return False, f"{label}: {stderr[:600]} (intermediates kept in {work})"
    print(f"    {label}: {elapsed:.1f} s", flush=True)
    partial.replace(target)
    shutil.rmtree(work, ignore_errors=True)
    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ffmpeg")
    parser.add_argument("--out", required=True)
    parser.add_argument("--minutes", type=float, help="length of both recordings (default: 120 and 30)")
    parser.add_argument("--chunk-seconds", type=int, default=60, help="the encoded video chunk (a multiple of 2)")
    parser.add_argument("--only", choices=["longvod_h264_1440p60_2h.mkv", "highfps_hevc_1080p240_30m.mp4"])
    args = parser.parse_args()
    if args.chunk_seconds < 2 or args.chunk_seconds % 2 != 0:
        sys.exit("gen_longvod_media: --chunk-seconds must be a positive multiple of the 2 s GOP")
    ffmpeg = find_ffmpeg(args.ffmpeg)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}

    def length(default_seconds: int) -> tuple[int, int]:
        """(seconds, chunk): whole chunks of a whole number of GOPs, at least the requested length."""
        wanted = max(1, math.ceil(round(args.minutes * 60, 6))) if args.minutes is not None else default_seconds
        chunk = min(args.chunk_seconds, 2 * math.ceil(wanted / 2))
        return chunk * math.ceil(wanted / chunk), chunk

    seconds_2h, chunk_2h = length(7200)
    seconds_30m, chunk_30m = length(1800)
    specs = recordings(seconds_2h, seconds_30m, has_encoder(ffmpeg, "h264_nvenc"), has_encoder(ffmpeg, "hevc_nvenc"))
    chunks = {"longvod_h264_1440p60_2h.mkv": chunk_2h, "highfps_hevc_1080p240_30m.mp4": chunk_30m}

    failed = 0
    for name, spec in specs.items():
        if args.only and name != args.only:
            continue
        chunk = chunks[name]
        recipe = {"video": spec["video"], "size": spec["size"], "fps": spec["fps"], "seed": spec["seed"],
                  "audio": spec["audio"], "aac": AAC, "mux": spec["mux"], "seconds": spec["seconds"],
                  "chunkSeconds": chunk, "graph": video_graph(spec["size"], spec["fps"], chunk, spec["seed"])}
        key = hashlib.sha256(json.dumps(recipe).encode()).hexdigest()
        target = out_dir / name
        entry = manifest.get(name)
        if target.exists() and isinstance(entry, dict) and entry.get("recipe") == key:
            print(f"  up to date  {name}")
            continue
        estimate = (spec["bitrate"] + 160_000 * len(spec["audio"])) * spec["seconds"] / 8
        # An outdated recording is deleted before the new one is written: its space counts as free.
        free = shutil.disk_usage(out_dir).free + (target.stat().st_size if target.exists() else 0)
        if free < estimate * 1.1 + 2e9:
            print(f"  FAILED      {name}: needs about {estimate / 1e9:.1f} GB, {free / 1e9:.1f} GB free in {out_dir}")
            failed += 1
            continue
        print(f"  generating  {name}: {spec['seconds']} s, {spec['encoder']}, about {estimate / 1e9:.1f} GB",
              flush=True)
        started = time.monotonic()
        target.unlink(missing_ok=True)
        ok, message = build(ffmpeg, name, spec, chunk, out_dir)
        if not ok:
            print(f"  FAILED      {name}: {message}")
            failed += 1
            continue
        elapsed = time.monotonic() - started
        manifest[name] = {"recipe": key, "videoEncoder": spec["encoder"], "seconds": spec["seconds"],
                          "chunkSeconds": chunk, "bytes": target.stat().st_size,
                          "generatedSeconds": round(elapsed, 1)}
        manifest_path.write_text(json.dumps(manifest, indent=2))
        print(f"  generated   {name} ({elapsed:.1f} s, {target.stat().st_size / 1e9:.2f} GB)", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
