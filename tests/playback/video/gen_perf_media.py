#!/usr/bin/env python3
"""Generate the playback benchmark clips (Graphics/playback; see bench_playback.cpp).

Distinct H.264 streams (different animated sources, noise seeds and hue drifts), so no two decoders share work:

    perf_1080p60_{a..f}.mp4   1920x1080 60 fps, H.264 High 4:2:0 8-bit, GOP 60, 2 B-frames, ~25 Mb/s
    perf_2160p30.mp4          3840x2160 30 fps, H.264 High 4:2:0 8-bit, GOP 30, ~60 Mb/s
    perf_1440p144.mp4         2560x1440 144 fps, H.264 High 4:2:0 8-bit, GOP 144, ~40 Mb/s (I.9, gaming)
    perf_1080p240.mp4         1920x1080 240 fps, H.264 High 4:2:0 8-bit, GOP 240, ~50 Mb/s
    perf_1080p60_pq_{a..c}.mp4 1920x1080 60 fps, HEVC Main 10 (NVENC), BT.2020 PQ: HDR game captures as OBS records them

Film-grain-like temporal noise keeps the bitrate close to camera footage (flat synthetic patterns would decode
unrealistically fast). The smoke set (--set smoke) makes three 4-second 1080p60 clips for the CTest smoke run.

Usage: gen_perf_media.py --ffmpeg PATH --out DIR [--set smoke|full] [--seconds N]
Generation is skipped for clips that already exist with the same recipe (manifest.json in DIR).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

SOURCES_1080 = {
    "a": "testsrc2=s=1920x1080:r=60",
    "b": "gradients=s=1920x1080:r=60:speed=0.02:seed=7",
    "c": "life=s=480x270:r=60:mold=10:ratio=0.2:seed=11:life_color=#ff9040:death_color=#102040,scale=1920:1080:flags=neighbor",
    "d": "zoneplate=s=1920x1080:r=60:kt2=8:kx2=32:ky2=24:kxt=4",
    "e": "cellauto=s=640x360:r=60:rule=110:seed=5,scale=1920:1080:flags=neighbor,format=yuv420p,hue=s=0.4",
    "f": "testsrc=s=1920x1080:r=60",
}
SOURCE_2160 = "testsrc2=s=3840x2160:r=30"

COMMON = ["-hide_banner", "-loglevel", "error", "-nostdin", "-y"]
BITEXACT = ["-fflags", "+bitexact", "-flags:v", "+bitexact", "-map_metadata", "-1"]
BT709 = ["-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv"]


def recipe(source: str, seconds: int, seed: int, rate: int, bitrate: str) -> list[str]:
    chain = (f"{source},format=yuv420p,noise=alls=10:allf=t+u:all_seed={seed},"
             f"hue=H=0.6*sin(2*PI*t/{7 + seed % 5}),trim=duration={seconds},setpts=PTS-STARTPTS,"
             "setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709,format=yuv420p[v]")
    # Filter sources through -filter_complex (this FFmpeg build has no lavfi input device).
    return ["-filter_complex", chain, "-map", "[v]", "-c:v", "libx264", "-preset", "veryfast", "-profile:v", "high",
            "-pix_fmt", "yuv420p", "-g", str(rate), "-keyint_min", str(rate), "-sc_threshold", "0", "-bf", "2",
            "-b:v", bitrate, "-maxrate", bitrate, "-bufsize", bitrate, *BT709, *BITEXACT]


def pq_recipe(source: str, seconds: int, seed: int) -> list[str]:
    """An HDR game capture: the source's code values tagged BT.2020 PQ, HEVC Main 10 by NVENC (OBS's HDR recording)."""
    chain = (f"{source},format=yuv420p10le,noise=alls=10:allf=t+u:all_seed={seed},trim=duration={seconds},"
             "setpts=PTS-STARTPTS,setparams=range=tv:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc,"
             "format=p010le[v]")
    return ["-filter_complex", chain, "-map", "[v]", "-c:v", "hevc_nvenc", "-profile:v", "main10", "-preset", "p4",
            "-rc", "vbr", "-b:v", "25M", "-maxrate", "25M", "-g", "60", "-bf", "0", "-colorspace", "bt2020nc",
            "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-color_range", "tv", "-tag:v", "hvc1", *BITEXACT]


def clips(which: str, seconds: int) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    if which == "smoke":
        for i, key in enumerate("abc"):
            out[f"perf_smoke_{key}.mp4"] = recipe(SOURCES_1080[key], seconds, 100 + i, 60, "25M")
        return out
    for i, (key, source) in enumerate(SOURCES_1080.items()):
        out[f"perf_1080p60_{key}.mp4"] = recipe(source, seconds, 1 + i, 60, "25M")
    out["perf_2160p30.mp4"] = recipe(SOURCE_2160, seconds, 42, 30, "60M")
    # I.9 (gaming): high frame rates and HDR game captures.
    out["perf_1440p144.mp4"] = recipe("testsrc2=s=2560x1440:r=144", seconds, 51, 144, "40M")
    out["perf_1080p240.mp4"] = recipe("testsrc2=s=1920x1080:r=240", seconds, 52, 240, "50M")
    for i, key in enumerate("abc"):
        out[f"perf_1080p60_pq_{key}.mp4"] = pq_recipe(SOURCES_1080[key], seconds, 61 + i)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--set", choices=["smoke", "full"], default="full")
    parser.add_argument("--seconds", type=int, default=0, help="clip length (default: 4 smoke, 20 full)")
    args = parser.parse_args()
    seconds = args.seconds or (4 if args.set == "smoke" else 20)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}

    failed = 0
    for name, args_ in clips(args.set, seconds).items():
        key = hashlib.sha256(json.dumps(args_).encode()).hexdigest()
        target = out_dir / name
        if target.exists() and manifest.get(name) == key:
            print(f"  up to date  {name}")
            continue
        started = time.monotonic()
        partial = target.with_suffix(".partial.mp4")
        result = subprocess.run([args.ffmpeg, *COMMON, *args_, str(partial)], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  FAILED      {name}: {result.stderr.strip()[:400]}")
            failed += 1
            continue
        partial.replace(target)
        manifest[name] = key
        manifest_path.write_text(json.dumps(manifest, indent=2))
        print(f"  generated   {name} ({time.monotonic() - started:.1f} s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
