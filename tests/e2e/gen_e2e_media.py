#!/usr/bin/env python3
"""Generates the e2e clips: three 4-second 1280x720 30 fps H.264 + AAC files with distinct pictures and tones, and
one 2-second 3840x2160 60 fps H.264 High 4:2:2 10-bit clip (no consumer GPU before NVIDIA RTX 50 decodes it: the
automatic proxy case), and a 4-second 1920x1080 60 fps H.264 clip (I.5: graded multi-layer playback).

    python tests/e2e/gen_e2e_media.py --ffmpeg <ffmpeg> --out build/<preset>/test-output/e2e/media

Skips files that already exist with the same recipe (a manifest records it), so CTest can run it as a fixture.
"""
import argparse
import json
import pathlib
import shutil
import subprocess
import sys

DURATION = 4
CLIPS = {
    # name: (video source filter, tone Hz). Sources run inside -filter_complex (the vcpkg FFmpeg has no lavfi
    # input device).
    "e2e_a.mp4": ("testsrc2=s=1280x720:r=30:d={d}", 440),
    "e2e_b.mp4": ("smptehdbars=s=1280x720:r=30:d={d}", 660),
    "e2e_c.mp4": ("testsrc=s=1280x720:r=30:d={d},hue=h=120", 880),
}


# name: (seconds, video source, pixel format, tone Hz, x264 arguments)
HEAVY_CLIPS = {
    "e2e_422_10bit.mp4": (2, "testsrc2=s=3840x2160:r=60:d={d}", "yuv422p10le", 550,
                          ["-preset", "ultrafast", "-profile:v", "high422", "-g", "60", "-bf", "2"]),
    # I.5: three graded layers of 1080p60 for the playback / scopes measurement.
    "e2e_1080p60.mp4": (4, "testsrc2=s=1920x1080:r=60:d={d}", "yuv420p", 330,
                        ["-preset", "veryfast", "-g", "60", "-bf", "2"]),
}


def recipe(name):
    if name in HEAVY_CLIPS:
        seconds, video, pix_fmt, tone, x264 = HEAVY_CLIPS[name]
        graph = (f"{video.format(d=seconds)},format={pix_fmt}[v];"
                 f"sine=f={tone}:r=48000:d={seconds},aformat=channel_layouts=stereo[a]")
        return [
            "-filter_complex", graph, "-map", "[v]", "-map", "[a]",
            "-c:v", "libx264", *x264, "-pix_fmt", pix_fmt,
            "-c:a", "aac", "-b:a", "128k",
            "-movflags", "+faststart",
        ]
    video, tone = CLIPS[name]
    graph = (f"{video.format(d=DURATION)},format=yuv420p[v];"
             f"sine=f={tone}:r=48000:d={DURATION},aformat=channel_layouts=stereo[a]")
    return [
        "-filter_complex", graph, "-map", "[v]", "-map", "[a]",
        "-c:v", "libx264", "-preset", "veryfast", "-g", "30", "-bf", "2",
        "-c:a", "aac", "-b:a", "128k",
        "-movflags", "+faststart",
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    if not args.ffmpeg:
        print("ffmpeg not found", file=sys.stderr)
        return 1
    ffmpeg = pathlib.Path(args.ffmpeg)
    if ffmpeg.exists():
        args.ffmpeg = str(ffmpeg.resolve())  # CreateProcess wants a native path
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest_path = out / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    for name in [*CLIPS, *HEAVY_CLIPS]:
        target = out / name
        args_list = recipe(name)
        if target.exists() and manifest.get(name) == args_list:
            continue
        cmd = [args.ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-nostdin", *args_list, str(target)]
        subprocess.run(cmd, check=True)
        manifest[name] = args_list
        print(f"generated {target}")
    manifest_path.write_text(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
