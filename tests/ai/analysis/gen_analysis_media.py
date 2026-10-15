#!/usr/bin/env python3
"""Generates the shot-detection fixtures with FFmpeg's test-source filters (C.5).

Every file is 25 fps H.264 (yuv420p). Sources are filters inside -filter_complex (the vcpkg FFmpeg has no lavfi
input device). Expected cuts (the frame that starts the new shot):

  cuts_1080p.mp4   1920x1080, 10 s: hard cuts at 50, 100, 150, 200 (testsrc2, SMPTE bars, mandelbrot, testsrc,
                   rgbtestsrc) - frame-exact; also the speed fixture
  dissolve.mp4     640x360, 5 s: testsrc2 dissolving into SMPTE bars over frames 50..75 - one dissolve near 62
  fade.mp4         640x360, 4.4 s: testsrc2 fading to black (frames 38..50), black 50..60, SMPTE bars fading in
                   (60..72) - one fade cut near 55
  flash.mp4        640x360, 5 s: testsrc2 with white flashes at frames 30-31 and 60, a hard cut to SMPTE bars at
                   frame 90 - only the cut at 90

Usage: gen_analysis_media.py --out DIR [--ffmpeg PATH]. Files that exist are kept. When all four exist, DIR/.complete
is written; the tests check that marker.

Safe to run in several processes at once (parallel ctest, or a test run outside ctest next to one): each process
encodes into its own temporary file and moves it into place atomically; a process that finds the file already there
(another one won) drops its copy. CTest runs it once as the ai suite's fixture (tests/ai/CMakeLists.txt).
"""

import argparse
import os
import shutil
import subprocess
import sys
import uuid


def ffmpeg_path(given):
    if given:
        return given
    found = shutil.which("ffmpeg")
    if not found:
        sys.exit("ffmpeg not found (pass --ffmpeg)")
    return found


def unique_tmp(out, suffix):
    return f"{out}.{os.getpid()}.{uuid.uuid4().hex}.tmp{suffix}"


def publish(tmp, out):
    """Moves `tmp` to `out` atomically, unless another process already put `out` there."""
    try:
        if not os.path.exists(out):
            os.replace(tmp, out)
            return
    except OSError:
        # Windows refuses to replace a file another process has open: that process's copy is in place.
        if not os.path.exists(out):
            raise
    os.remove(tmp)


def run(ffmpeg, graph, out):
    if os.path.exists(out):
        return
    tmp = unique_tmp(out, ".mp4")
    cmd = [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-filter_complex", graph, "-map", "[v]",
           "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p", "-g", "25", tmp]
    subprocess.run(cmd, check=True)
    publish(tmp, out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--ffmpeg")
    args = parser.parse_args()
    ffmpeg = ffmpeg_path(args.ffmpeg)
    os.makedirs(args.out, exist_ok=True)

    big = "s=1920x1080:r=25"
    run(ffmpeg,
        f"testsrc2={big}:d=2[a];smptehdbars={big}:d=2[b];mandelbrot={big},trim=duration=2,setpts=PTS-STARTPTS[c];"
        f"testsrc={big}:d=2[d];rgbtestsrc={big}:d=2[e];"
        "[a][b][c][d][e]concat=n=5:v=1:a=0,format=yuv420p[v]",
        os.path.join(args.out, "cuts_1080p.mp4"))

    small = "s=640x360:r=25"
    run(ffmpeg,
        f"testsrc2={small}:d=3,format=yuv420p[a];smptehdbars={small}:d=3,format=yuv420p[b];"
        "[a][b]xfade=transition=fade:duration=1:offset=2[v]",
        os.path.join(args.out, "dissolve.mp4"))

    run(ffmpeg,
        f"testsrc2={small}:d=2,fade=t=out:st=1.5:d=0.5[a];color=c=black:{small}:d=0.4[b];"
        f"smptehdbars={small}:d=2,fade=t=in:st=0:d=0.5[c];[a][b][c]concat=n=3:v=1:a=0,format=yuv420p[v]",
        os.path.join(args.out, "fade.mp4"))

    run(ffmpeg,
        f"testsrc2={small}:d=3.6,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:"
        "enable='between(n\\,30\\,31)+eq(n\\,60)'[a];"
        f"smptehdbars={small}:d=1.4[b];[a][b]concat=n=2:v=1:a=0,format=yuv420p[v]",
        os.path.join(args.out, "flash.mp4"))

    marker = os.path.join(args.out, ".complete")
    if not os.path.exists(marker):
        tmp = unique_tmp(marker, "")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("cuts_1080p.mp4 dissolve.mp4 fade.mp4 flash.mp4\n")
        publish(tmp, marker)


if __name__ == "__main__":
    main()
