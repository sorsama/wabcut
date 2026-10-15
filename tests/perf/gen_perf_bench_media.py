#!/usr/bin/env python3
"""Generate the P.1 benchmark media (tests/perf, docs/performance.md).

Camera-like clips (animated test pattern, film-grain-like temporal noise so the bitrate is close to real footage,
a hue drift) with a 440 Hz stereo tone, 12 seconds each:

    perf_h264_1080p30.mp4   1920x1080 30 fps, H.264 High 4:2:0 8-bit, GOP 30, 2 B-frames, 20 Mb/s, AAC stereo
    perf_hevc_2160p30.mp4   3840x2160 30 fps, HEVC Main 4:2:0 8-bit, GOP 30, 40 Mb/s (hevc_nvenc, else libx265), AAC
    perf_prores_1080p30.mov 1920x1080 30 fps, ProRes 422 (prores_ks profile 2) 10-bit 4:2:2, PCM s24le stereo
    perf_proxy_1080p30.mov  the 4K clip's half-resolution proxy, as the app makes it: ProRes Proxy (profile 0)
                            10-bit 4:2:2 1920x1080, PCM s24le stereo

Usage: gen_perf_bench_media.py --out DIR [--ffmpeg PATH] [--seconds N]
A clip is skipped when it exists with the same recipe (manifest.json in DIR). Exit 0 when every clip exists.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

COMMON = ["-hide_banner", "-loglevel", "error", "-nostdin", "-y"]
BT709 = ["-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv"]


def graph(size: str, seconds: int, seed: int, pix_fmt: str, scale: str | None = None) -> list[str]:
    scaled = f"scale={scale}:flags=bicubic," if scale else ""
    video = (f"testsrc2=s={size}:r=30,format=yuv420p,noise=alls=10:allf=t+u:all_seed={seed},"
             f"hue=H=0.6*sin(2*PI*t/7),trim=duration={seconds},setpts=PTS-STARTPTS,"
             f"setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709,{scaled}format={pix_fmt}[v]")
    audio = f"sine=frequency=440:sample_rate=48000,aformat=channel_layouts=stereo,atrim=duration={seconds}[a]"
    # Filter sources through -filter_complex (some FFmpeg builds have no lavfi input device).
    return ["-filter_complex", f"{video};{audio}", "-map", "[v]", "-map", "[a]"]


def clips(seconds: int, hevc_encoder: str) -> dict[str, list[str]]:
    hevc = (["-c:v", "hevc_nvenc", "-preset", "p4", "-rc", "cbr", "-b:v", "40M", "-g", "30", "-bf", "2"]
            if hevc_encoder == "hevc_nvenc" else
            ["-c:v", "libx265", "-preset", "ultrafast", "-b:v", "40M", "-x265-params", "keyint=30:min-keyint=30:bframes=2:log-level=error"])
    return {
        "perf_h264_1080p30.mp4": [*graph("1920x1080", seconds, 11, "yuv420p"),
                                  "-c:v", "libx264", "-preset", "veryfast", "-profile:v", "high", "-pix_fmt", "yuv420p",
                                  "-g", "30", "-keyint_min", "30", "-sc_threshold", "0", "-bf", "2",
                                  "-b:v", "20M", "-maxrate", "20M", "-bufsize", "20M", *BT709,
                                  "-c:a", "aac", "-b:a", "192k"],
        "perf_hevc_2160p30.mp4": [*graph("3840x2160", seconds, 23, "yuv420p"), *hevc, "-pix_fmt", "yuv420p",
                                  "-tag:v", "hvc1", *BT709, "-c:a", "aac", "-b:a", "192k"],
        "perf_prores_1080p30.mov": [*graph("1920x1080", seconds, 37, "yuv422p10le"),
                                    "-c:v", "prores_ks", "-profile:v", "2", "-vendor", "apl0", "-pix_fmt", "yuv422p10le",
                                    *BT709, "-c:a", "pcm_s24le"],
        # The proxy of perf_hevc_2160p30.mp4: the same pictures (same seed) at half resolution.
        "perf_proxy_1080p30.mov": [*graph("3840x2160", seconds, 23, "yuv422p10le", scale="1920:1080"),
                                   "-c:v", "prores_ks", "-profile:v", "0", "-vendor", "apl0", "-pix_fmt", "yuv422p10le",
                                   *BT709, "-c:a", "pcm_s24le"],
    }


def find_ffmpeg(explicit: str | None) -> str:
    if explicit:
        return explicit
    if os.environ.get("WABCUT_FFMPEG"):
        return os.environ["WABCUT_FFMPEG"]
    root = Path(__file__).resolve().parents[2]
    exe = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"
    candidates = sorted(root.glob(f"build/*/vcpkg_installed/*/tools/ffmpeg/{exe}"), key=lambda p: p.stat().st_mtime,
                        reverse=True)
    if candidates:
        return str(candidates[0])
    found = shutil.which("ffmpeg")
    if not found:
        sys.exit("gen_perf_bench_media: no FFmpeg found; pass --ffmpeg")
    return found


def has_encoder(ffmpeg: str, name: str) -> bool:
    """Whether `name` is built in and works on this machine (a hardware encoder needs its GPU)."""
    probe = subprocess.run([ffmpeg, *COMMON, "-f", "lavfi", "-i", "color=s=256x256:d=0.1", "-c:v", name, "-f", "null", "-"],
                           capture_output=True, text=True)
    if probe.returncode == 0:
        return True
    probe = subprocess.run([ffmpeg, *COMMON, "-filter_complex", "color=s=256x256:d=0.1[v]", "-map", "[v]", "-c:v", name,
                            "-f", "null", "-"], capture_output=True, text=True)
    return probe.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ffmpeg")
    parser.add_argument("--out", required=True)
    parser.add_argument("--seconds", type=int, default=12)
    args = parser.parse_args()
    ffmpeg = find_ffmpeg(args.ffmpeg)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    hevc_encoder = "hevc_nvenc" if has_encoder(ffmpeg, "hevc_nvenc") else "libx265"

    failed = 0
    for name, recipe in clips(args.seconds, hevc_encoder).items():
        key = hashlib.sha256(json.dumps(recipe).encode()).hexdigest()
        target = out_dir / name
        if target.exists() and manifest.get(name) == key:
            print(f"  up to date  {name}")
            continue
        started = time.monotonic()
        partial = target.with_name(target.stem + ".partial" + target.suffix)
        result = subprocess.run([ffmpeg, *COMMON, *recipe, str(partial)], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  FAILED      {name}: {result.stderr.strip()[:600]}")
            failed += 1
            continue
        partial.replace(target)
        manifest[name] = key
        manifest_path.write_text(json.dumps(manifest, indent=2))
        print(f"  generated   {name} ({time.monotonic() - started:.1f} s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
