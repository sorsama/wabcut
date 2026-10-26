#!/usr/bin/env python3
"""Generate Wabcut's deterministic media test clips with the FFmpeg from the vcpkg dependency tree.

Every video clip carries its frame index as a barcode in the top-left corner: 16 cells of 32x32 luma pixels on one
row (cell b at x = 32*b, bit b of the frame number, white = 1, black = 0), drawn before encoding. Decoders are
verified frame-exactly by sampling the centre of each cell, which survives near-lossless coding and chroma
subsampling. Audio test signals are pure functions of the sample index, so sample accuracy is checkable too.

Usage:
    gen_test_media.py [--ffmpeg PATH] [--out DIR] [--force] [--list]

FFmpeg is taken from --ffmpeg, else $WABCUT_FFMPEG, else the newest
build/*/vcpkg_installed/*/tools/ffmpeg/ffmpeg[.exe] under the repository. Generation is skipped when the output
directory already holds the clips of this exact script version and FFmpeg build (see manifest.json there).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCHEMA = 1  # bump to force regeneration when clip definitions change (the script hash does that as well)

BARCODE_BITS = 16
BARCODE_CELL = 32

COMMON = ["-hide_banner", "-loglevel", "error", "-nostdin", "-y"]
# Deterministic containers: no encoder version strings or creation times.
BITEXACT = ["-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact", "-map_metadata", "-1"]
BT709 = ["-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv"]


def barcode() -> str:
    """drawbox chain that writes the frame number (input frame n) as a 16-bit barcode."""
    width = BARCODE_BITS * BARCODE_CELL
    parts = [f"drawbox=x=0:y=0:w={width}:h={BARCODE_CELL}:color=black:t=fill"]
    for bit in range(BARCODE_BITS):
        parts.append(
            f"drawbox=x={bit * BARCODE_CELL}:y=0:w={BARCODE_CELL}:h={BARCODE_CELL}:color=white:t=fill"
            f":enable='mod(floor(n/{1 << bit}),2)'"
        )
    return ",".join(parts)


SETPARAMS_BT709 = "setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709"
SETPARAMS_HDR10 = "setparams=range=tv:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc"


def video_source(size: str, rate: str, duration: str, pix_fmt: str, extra: str = "",
                 tags: str = SETPARAMS_BT709) -> str:
    """testsrc2 pattern + barcode, colour-tagged on the frames, converted to the encoder's pixel format."""
    chain = f"testsrc2=s={size}:r={rate}:d={duration},format=yuv444p,{barcode()}"
    if extra:
        chain += "," + extra
    return f"{chain},{tags},format={pix_fmt}[v]"


# ---- Clip definitions --------------------------------------------------------------------------------------------
# Each clip is a list of candidate recipes; the first one FFmpeg accepts wins (encoder availability differs between
# builds and machines). A recipe is a list of steps; a step is an argument list for ffmpeg ({out} = output file,
# {tmp} = scratch file).

HEVC_MAIN10_SOURCE = video_source("1280x720", "30", "2", "p010le", tags=SETPARAMS_HDR10)
HEVC_TAGS = ["-colorspace", "bt2020nc", "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-color_range", "tv"]


def hevc_recipe(encoder_args: list[str]) -> list[list[str]]:
    return [["-filter_complex", HEVC_MAIN10_SOURCE, "-map", "[v]", *encoder_args, *HEVC_TAGS, "-tag:v", "hvc1",
             *BITEXACT, "{out}"]]


def multitrack_recipe(frequencies: list[int], tags: list[dict[str, str]]) -> list[list[str]]:
    """A small game recording (A.10 multi-track ingest): H.264 320x180 10 fps, 2 s, plus one AAC stereo 48 kHz stream
    per frequency (stream k: 0.5 sin(2 pi f_k t) on both channels, so tests can tell the streams apart). tags[k] are
    the metadata of audio stream k: "title" (Matroska TrackName, what OBS writes to MKV) or "handler_name" (the MP4
    hdlr name, where OBS's hybrid MP4 keeps the track name); streams without tags are untitled (MP4: FFmpeg's default
    handler "SoundHandler")."""
    graph = [video_source("320x180", "10", "2", "yuv420p")]
    maps = ["-map", "[v]"]
    for k, frequency in enumerate(frequencies):
        graph.append(f"aevalsrc=exprs='0.5*sin(2*PI*{frequency}*t)|0.5*sin(2*PI*{frequency}*t)':s=48000:d=2[a{k}]")
        maps += ["-map", f"[a{k}]"]
    metadata = []
    for k, stream_tags in enumerate(tags):
        for key, value in stream_tags.items():
            metadata += [f"-metadata:s:a:{k}", f"{key}={value}"]
    return [[
        "-filter_complex", ";".join(graph), *maps,
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "23", "-g", "10", "-bf", "0", *BT709,
        "-c:a", "aac", "-b:a", "96k", *BITEXACT, *metadata, "{out}",
    ]]


def chapters_recipe(seconds: int) -> list[list[str]]:
    """A recording with chapter markers (P0 round: OBS "Add Chapter Marker" -> clip markers on import): H.264 320x180
    10 fps + one AAC stereo stream (440 Hz), `seconds` long, and the chapters of the clip's "ffmetadata" (an FFMETADATA
    file the driver writes to {meta}; FFmpeg here has no lavfi input, so the sources stay in -filter_complex). Global
    metadata is dropped for determinism; the chapters keep their titles."""
    return [[
        "-f", "ffmetadata", "-i", "{meta}",
        "-filter_complex",
        video_source("320x180", "10", str(seconds), "yuv420p") +
        f";aevalsrc=exprs='0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t)':s=48000:d={seconds}[a]",
        "-map", "[v]", "-map", "[a]", "-map_chapters", "0",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "23", "-g", "10", "-bf", "0", *BT709,
        "-c:a", "aac", "-b:a", "96k",
        "-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact", "-map_metadata:g", "-1", "{out}",
    ]]


def ffmetadata_chapters(chapters: list[tuple[int, int, bytes | None]]) -> bytes:
    """FFMETADATA text of chapters (start ms, end ms, raw title bytes or None for an untitled chapter)."""
    out = b";FFMETADATA1\n"
    for start, end, title in chapters:
        out += b"[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\n" % (start, end)
        if title is not None:
            out += b"title=" + title + b"\n"
    return out


CLIPS: list[dict] = [
    {
        "name": "h264_1080p60_aac.mp4",
        "what": "H.264 High 8-bit 4:2:0 1920x1080 60 fps, 10 s (600 frames, GOP 60, 2 B-frames) + AAC stereo 48 kHz",
        "expect": {"codec_name": "h264", "pix_fmt": "yuv420p", "color_primaries": "bt709", "nb_frames": "600"},
        "recipes": [[[
            "-filter_complex",
            video_source("1920x1080", "60", "10", "yuv420p") +
            ";aevalsrc=exprs='0.5*sin(2*PI*440*t)|0.5*sin(2*PI*880*t)':s=48000:d=10[a]",
            "-map", "[v]", "-map", "[a]",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-profile:v", "high",
            "-g", "60", "-keyint_min", "60", "-sc_threshold", "0", "-bf", "2", *BT709,
            "-c:a", "aac", "-b:a", "192k", *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "hevc_main10_420.mp4",
        "what": "HEVC Main 10 4:2:0 1280x720 30 fps, 2 s, tagged BT.2020/PQ",
        # vcpkg's x265 is 8-bit only, so this needs a 10-bit libx265 or a hardware encoder (NVENC/QSV/AMF).
        "optional": True,
        "expect": {"codec_name": "hevc", "profile": "Main 10", "pix_fmt": "yuv420p10le",
                   "color_transfer": "smpte2084"},
        "recipes": [
            hevc_recipe(["-c:v", "libx265", "-preset", "ultrafast", "-crf", "16", "-pix_fmt", "yuv420p10le",
                         "-x265-params", "log-level=error:keyint=30:min-keyint=30"]),
            hevc_recipe(["-c:v", "hevc_nvenc", "-preset", "p4", "-rc", "constqp", "-qp", "16", "-profile:v", "main10",
                         "-g", "30", "-bf", "0"]),
            hevc_recipe(["-c:v", "hevc_qsv", "-global_quality", "16", "-profile:v", "main10", "-g", "30"]),
            hevc_recipe(["-c:v", "hevc_amf", "-rc", "cqp", "-qp_i", "16", "-qp_p", "16", "-profile:v", "main10",
                         "-g", "30"]),
        ],
    },
    {
        "name": "h264_422_10bit.mp4",
        "what": "H.264 High 4:2:2 10-bit 1280x720 30 fps, 2 s (no consumer GPU before RTX 50 decodes this)",
        "expect": {"codec_name": "h264", "profile": "High 4:2:2", "pix_fmt": "yuv422p10le"},
        "recipes": [[[
            "-filter_complex", video_source("1280x720", "30", "2", "yuv422p10le"), "-map", "[v]",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-profile:v", "high422", "-g", "30", "-bf", "2",
            *BT709, *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "prores_422.mov",
        "what": "ProRes 422 (prores_ks) 10-bit 4:2:2 1280x720 23.976 fps, 48 frames + PCM s24le stereo 48 kHz",
        "expect": {"codec_name": "prores", "pix_fmt": "yuv422p10le", "nb_frames": "48"},
        "recipes": [[[
            "-filter_complex",
            video_source("1280x720", "24000/1001", "2.002", "yuv422p10le") +
            ";aevalsrc=exprs='0.5*sin(2*PI*440*t)|0.5*sin(2*PI*660*t)':s=48000:d=2.002[a]",
            "-map", "[v]", "-map", "[a]", "-frames:v", "48",
            "-c:v", "prores_ks", "-profile:v", "2", "-vendor", "apl0", *BT709,
            "-c:a", "pcm_s24le", *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "prores_4444_alpha.mov",
        "what": "ProRes 4444 (prores_ks) 4:4:4 + 16-bit alpha, 640x360 24 fps, 12 frames (FFmpeg decodes it as "
                "12-bit yuva444p12); alpha is a horizontal ramp (x * 255 / width in 8-bit terms) on every row",
        "expect": {"codec_name": "prores", "pix_fmt": "yuva444p12le"},
        "recipes": [[[
            "-filter_complex",
            "testsrc2=s=640x360:r=24:d=0.5,format=yuva444p,"
            "geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='X*255/W'," + barcode() + "," + SETPARAMS_BT709 +
            ",format=yuva444p10le[v]",
            "-map", "[v]", "-frames:v", "12",
            "-c:v", "prores_ks", "-profile:v", "4", "-alpha_bits", "16", "-vendor", "apl0", *BT709,
            *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "ffv1_12bit.mkv",
        "what": "FFV1 (lossless) 4:4:4 12-bit 640x360 25 fps, 10 frames: decoded as 10-bit in the MVP",
        "expect": {"codec_name": "ffv1", "pix_fmt": "yuv444p12le"},
        "recipes": [[[
            "-filter_complex", video_source("640x360", "25", "0.4", "yuv444p12le"), "-map", "[v]",
            "-c:v", "ffv1", "-level", "3", *BT709, *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "vfr_quadratic.mkv",
        "what": "FFV1 in Matroska, 160x90, 8 frames; frame n starts at n*n/25 s (0, .04, .16, .36, .64, 1, 1.44, "
                "1.96): VFR with growing gaps and no sample index (code-review repro)",
        "expect": {"codec_name": "ffv1"},
        "recipes": [[[
            "-filter_complex",
            video_source("160x90", "25", "0.32", "yuv420p", extra="setpts='N*N/(25*TB)'"),
            "-map", "[v]", "-frames:v", "8", "-fps_mode", "vfr",
            "-c:v", "ffv1", *BT709, *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "vfr.mp4",
        "what": "H.264 VFR 640x360: frames 0-29 at 30 fps, frames 30-59 at 15 fps (frame n starts at "
                "n/30 s for n < 30, (30 + 2(n-30))/30 s after), 2 B-frames",
        "expect": {"codec_name": "h264", "nb_frames": "60"},
        "recipes": [[[
            "-filter_complex",
            video_source("640x360", "30", "2", "yuv420p",
                         extra="setpts='if(lt(N,30),N,30+2*(N-30))/(30*TB)'"),
            "-map", "[v]", "-fps_mode", "passthrough",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-g", "30", "-bf", "2", *BT709,
            *BITEXACT, "{out}",
        ]]],
    },
    # I.9 (gaming): high-frame-rate captures and the variable frame rates of OBS (dropped frames) and phones.
    *[{
        "name": f"h264_360p{fps}.mp4",
        "what": f"H.264 640x360 {fps} fps, {seconds} s ({fps * seconds} frames): a high-frame-rate game capture",
        "expect": {"codec_name": "h264", "nb_frames": str(fps * seconds)},
        "recipes": [[[
            "-filter_complex", video_source("640x360", str(fps), str(seconds), "yuv420p"),
            "-map", "[v]", "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-g", str(fps), "-bf", "2", *BT709,
            "-video_track_timescale", "15360", *BITEXACT, "{out}",
        ]]],
    } for fps, seconds in ((120, 2), (144, 2), (240, 1))],
    {
        "name": "obs_vfr.mp4",
        "what": "H.264 VFR 640x360 as OBS writes it under load: a 60 fps grid with every seventh frame dropped (frame n "
                "starts at (n + floor(n/6))/60 s), 120 frames, 2 B-frames",
        "expect": {"codec_name": "h264", "nb_frames": "120"},
        "recipes": [[[
            "-filter_complex",
            video_source("640x360", "60", "2", "yuv420p", extra="setpts='(N+floor(N/6))/(60*TB)'"),
            "-map", "[v]", "-fps_mode", "passthrough",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-g", "60", "-bf", "2", *BT709,
            "-video_track_timescale", "15360", *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "phone_vfr.mp4",
        "what": "H.264 VFR 640x360 as phones record: frames alternately 1/30 s and 1/24 s long (frame n starts at "
                "floor(n/2)*(1/30 + 1/24) + (n mod 2)/30 s), 60 frames",
        "expect": {"codec_name": "h264", "nb_frames": "60"},
        "recipes": [[[
            "-filter_complex",
            video_source("640x360", "30", "2", "yuv420p", extra="settb=1/15360,setpts='floor(N/2)*1152+mod(N,2)*512'"),
            "-map", "[v]", "-fps_mode", "passthrough", "-enc_time_base:v", "1/15360",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-g", "30", "-bf", "2", *BT709,
            "-video_track_timescale", "15360", *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "rotated_90.mp4",
        "what": "H.264 640x360 30 fps, 1 s, display matrix = rotate 90 degrees clockwise for display",
        "expect": {"codec_name": "h264", "rotation": "-90"},
        "recipes": [[
            ["-filter_complex", video_source("640x360", "30", "1", "yuv420p"), "-map", "[v]",
             "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", *BT709, *BITEXACT, "{tmp}"],
            ["-display_rotation:v:0", "270", "-i", "{tmp}", "-c", "copy", *BITEXACT, "{out}"],
        ]],
    },
    {
        "name": "transposed.mp4",
        "what": "H.264 640x360 30 fps, 1 s, display matrix = rotate 90 clockwise + horizontal flip (a transpose: "
                "the top-left barcode is displayed as a vertical strip down the left edge)",
        "expect": {"codec_name": "h264", "rotation": "-90"},
        "recipes": [[
            ["-filter_complex", video_source("640x360", "30", "1", "yuv420p"), "-map", "[v]",
             "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", *BT709, *BITEXACT, "{tmp}"],
            ["-display_rotation:v:0", "270", "-display_hflip:v:0", "-i", "{tmp}", "-c", "copy", *BITEXACT,
             "{out}"],
        ]],
    },
    {
        "name": "sine_48k.wav",
        "what": "PCM s16le 48 kHz stereo, 5 s: L = 0.5 sin(2 pi 1000 t), R = 0.25 sin(2 pi 1500 t)",
        "expect": {"codec_name": "pcm_s16le", "sample_rate": "48000", "channels": "2"},
        "recipes": [[[
            "-filter_complex", "aevalsrc=exprs='0.5*sin(2*PI*1000*t)|0.25*sin(2*PI*1500*t)':s=48000:d=5[a]",
            "-map", "[a]", "-c:a", "pcm_s16le", *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "ramp_48k.wav",
        "what": "PCM s16le 48 kHz stereo, 5 s: sample n is L = ((n mod 65536) - 32768) / 32768, "
                "R = (((n + 12345) mod 65536) - 32768) / 32768",
        "expect": {"codec_name": "pcm_s16le", "sample_rate": "48000", "channels": "2"},
        "recipes": [[[
            "-filter_complex",
            "aevalsrc=exprs='(mod(n,65536)-32768)/32768|(mod(n+12345,65536)-32768)/32768':s=48000:d=5[a]",
            "-map", "[a]", "-c:a", "pcm_s16le", *BITEXACT, "{out}",
        ]]],
    },
    # ---- A.10 multi-track game recordings (OBS, ShadowPlay, Game Bar): one named or untitled stream per source.
    {
        "name": "obs_multitrack_titled.mkv",
        "what": "OBS-style MKV: H.264 320x180 10 fps, 2 s + 4 AAC stereo streams titled Game, Mic, Discord, Music "
                "(sines of 220, 440, 660, 880 Hz)",
        "expect": {"codec_name": "h264"},
        "recipes": [multitrack_recipe([220, 440, 660, 880], [{"title": "Game"}, {"title": "Mic"},
                                                             {"title": "Discord"}, {"title": "Music"}])],
    },
    {
        "name": "obs_multitrack_tracknames.mkv",
        "what": "OBS-style MKV with OBS's default track names: H.264 320x180 10 fps, 2 s + 3 AAC stereo streams titled "
                "Track1, Track2, Track3 (220, 440, 660 Hz)",
        "expect": {"codec_name": "h264"},
        "recipes": [multitrack_recipe([220, 440, 660],
                                      [{"title": "Track1"}, {"title": "Track2"}, {"title": "Track3"}])],
    },
    {
        "name": "obs_hybrid_titled.mp4",
        "what": "OBS hybrid-MP4-style: H.264 320x180 10 fps, 2 s + 3 AAC stereo streams whose handler names are "
                "Desktop Audio, Mic/Aux, Discord (220, 440, 660 Hz)",
        "expect": {"codec_name": "h264"},
        "recipes": [multitrack_recipe([220, 440, 660], [{"handler_name": "Desktop Audio"}, {"handler_name": "Mic/Aux"},
                                                        {"handler_name": "Discord"}])],
    },
    {
        "name": "shadowplay_two_track.mp4",
        "what": "ShadowPlay / Game Bar style: H.264 320x180 10 fps, 2 s + 2 untitled AAC stereo streams (game 220 Hz, "
                "mic 440 Hz; FFmpeg's default handler name SoundHandler)",
        "expect": {"codec_name": "h264"},
        "recipes": [multitrack_recipe([220, 440], [])],
    },
    {
        # P0 round: OBS chapter markers on import (media::readChapters, editor::chapterMarkers).
        "name": "obs_chapters.mkv",
        "what": "OBS-style MKV with chapter markers: H.264 320x180 10 fps, 4 s + AAC stereo; chapters at 1 s \"Boss "
                "fight\", 2 s untitled, 2.5 s a title with an invalid UTF-8 byte, 3 s a 216-byte Thai title, 5 s (past the "
                "end) \"After the end\"",
        "expect": {"codec_name": "h264"},
        "ffmetadata": ffmetadata_chapters([
            (1000, 2000, b"Boss fight"),
            (2000, 2500, None),
            (2500, 3000, b"Clutch \xff win"),
            (3000, 4000, ("\u0e44\u0e2e\u0e44\u0e25\u0e15\u0e4c" * 12).encode("utf-8")),
            (5000, 6000, b"After the end"),
        ]),
        "recipes": [chapters_recipe(4)],
    },
    {
        "name": "obs_chapter_zero.mkv",
        "what": "MKV with one chapter at 00:00 covering the file (what muxers write by themselves): H.264 320x180 10 fps, "
                "2 s + AAC stereo",
        "expect": {"codec_name": "h264"},
        "ffmetadata": ffmetadata_chapters([(0, 2000, b"Chapter 1")]),
        "recipes": [chapters_recipe(2)],
    },
    {
        "name": "untitled_three_track.mkv",
        "what": "MKV: H.264 320x180 10 fps, 2 s + 3 untitled AAC stereo streams (220, 440, 660 Hz)",
        "expect": {"codec_name": "h264"},
        "recipes": [multitrack_recipe([220, 440, 660], [])],
    },
    # ---- A.10 high-frame-rate game captures in miniature: played in a 60 fps sequence, every fourth frame is shown.
    {
        "name": "h264_240fps.mp4",
        "what": "H.264 High 8-bit 4:2:0 640x360 240 fps, 2 s (480 frames in one GOP of 480, no B-frames)",
        "expect": {"codec_name": "h264", "pix_fmt": "yuv420p", "nb_frames": "480"},
        "recipes": [[[
            "-filter_complex", video_source("640x360", "240", "2", "yuv420p"), "-map", "[v]",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-g", "480", "-keyint_min", "480",
            "-sc_threshold", "0", "-bf", "0", *BT709, *BITEXACT, "{out}",
        ]]],
    },
    {
        "name": "h264_240fps_bframes.mp4",
        "what": "H.264 High 8-bit 4:2:0 640x360 240 fps, 2 s (480 frames in one GOP of 480, 2 B-frames)",
        "expect": {"codec_name": "h264", "pix_fmt": "yuv420p", "nb_frames": "480"},
        "recipes": [[[
            "-filter_complex", video_source("640x360", "240", "2", "yuv420p"), "-map", "[v]",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-g", "480", "-keyint_min", "480",
            "-sc_threshold", "0", "-bf", "2", *BT709, *BITEXACT, "{out}",
        ]]],
    },
    # ---- 0.2 media formats: interlaced footage and camera chapters (spanned clips).
    {
        "name": "mpeg2_interlaced_tff.mpg",
        "what": "MPEG-2 interlaced 640x480 29.97 fps in MPEG-PS (Matroska mislabels the field order), upper field "
                "first, 30 frames: the upper field (even lines) of frame n is frame 2n of a 59.94 fps pattern and the "
                "lower field frame 2n + 1, each with its own barcode",
        "expect": {"codec_name": "mpeg2video", "field_order": "tt"},
        "recipes": [[[
            "-filter_complex",
            video_source("640x480", "60000/1001", "1.001", "yuv420p", extra="tinterlace=mode=interleave_top,setfield=tff"),
            "-map", "[v]", "-frames:v", "30",
            "-c:v", "mpeg2video", "-q:v", "2", "-g", "15", "-bf", "0", *BT709,
            "-fflags", "+bitexact", "-flags:v", "+ilme+ildct+bitexact", "-map_metadata", "-1", "{out}",
        ]]],
    },
    *[{
        "name": name,
        "what": f"GoPro-style chapter {chapter} of one recording: H.264 640x360 30 fps, frames {first}..{first + 29} "
                f"(barcodes {first}..{first + 29}) + AAC stereo 48 kHz (a 440 Hz sine continuing from {first / 30:g} s)",
        "expect": {"codec_name": "h264", "nb_frames": "30"},
        "recipes": [[[
            "-filter_complex",
            video_source("640x360", "30", "2", "yuv420p", extra=f"trim=start_frame={first}:end_frame={first + 30},"
                                                                 "setpts=PTS-STARTPTS") +
            f";aevalsrc=exprs='0.5*sin(2*PI*440*(t+{first / 30}))|0.5*sin(2*PI*440*(t+{first / 30}))':s=48000:d=1[a]",
            "-map", "[v]", "-map", "[a]",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-g", "30", "-bf", "0", *BT709,
            "-c:a", "aac", "-b:a", "128k", *BITEXACT, "{out}",
        ]]],
    } for name, chapter, first in (("GX010042.MP4", 1, 0), ("GX020042.MP4", 2, 30))],
    *[{
        "name": name,
        "what": f"AVCHD-style stream file: H.264 640x360 30 fps in MPEG-TS, frames {first}..{first + 29} (barcodes "
                f"{first}..{first + 29}) + AAC stereo 48 kHz; two of them are one spanned recording",
        "expect": {"codec_name": "h264"},
        "recipes": [[[
            "-filter_complex",
            video_source("640x360", "30", "2", "yuv420p", extra=f"trim=start_frame={first}:end_frame={first + 30},"
                                                                 "setpts=PTS-STARTPTS") +
            f";aevalsrc=exprs='0.5*sin(2*PI*660*(t+{first / 30}))|0.5*sin(2*PI*660*(t+{first / 30}))':s=48000:d=1[a]",
            "-map", "[v]", "-map", "[a]",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-g", "30", "-bf", "0", *BT709,
            "-c:a", "aac", "-b:a", "128k", "-f", "mpegts", *BITEXACT, "{out}",
        ]]],
    } for name, first in (("avchd_00000.MTS", 0), ("avchd_00001.MTS", 30))],
    # ---- RC.1 stills: a PNG with alpha (FFmpeg's png codec needs its zlib feature) and a JPEG.
    {
        "name": "still_alpha.png",
        "what": "PNG still 640x360 RGBA: the left half opaque red, the right half fully transparent",
        "expect": {"codec_name": "png", "pix_fmt": "rgba"},
        "recipes": [[[
            "-filter_complex", "color=c=red:s=640x360:d=1,format=rgba,geq=r='255':g='0':b='0':a='if(lt(X,W/2),255,0)'[v]",
            "-map", "[v]", "-frames:v", "1", "-c:v", "png", "-pix_fmt", "rgba", "{out}",
        ]]],
    },
    {
        "name": "still.jpg",
        "what": "JPEG still 640x360 (a testsrc2 frame)",
        "expect": {"codec_name": "mjpeg"},
        "recipes": [[[
            "-filter_complex", "testsrc2=s=640x360:r=1:d=1,format=yuvj420p[v]",
            "-map", "[v]", "-frames:v", "1", "-c:v", "mjpeg", "-q:v", "3", "{out}",
        ]]],
    },
]


# ---- Driver ------------------------------------------------------------------------------------------------------

def find_ffmpeg(explicit: str | None) -> Path:
    exe = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"
    if explicit:
        path = Path(explicit)
        if path.is_dir():
            path = path / exe
        if not path.is_file():
            sys.exit(f"gen_test_media: --ffmpeg {explicit} does not exist")
        return path
    if os.environ.get("WABCUT_FFMPEG"):
        return find_ffmpeg(os.environ["WABCUT_FFMPEG"])
    candidates = sorted(REPO_ROOT.glob(f"build/*/vcpkg_installed/*/tools/ffmpeg/{exe}"),
                        key=lambda p: p.stat().st_mtime, reverse=True)
    if not candidates:
        sys.exit("gen_test_media: no FFmpeg found; configure a preset first or pass --ffmpeg")
    return candidates[0]


def ffmpeg_version(ffmpeg: Path) -> str:
    out = subprocess.run([str(ffmpeg), "-hide_banner", "-version"], capture_output=True, text=True, check=True)
    return out.stdout.splitlines()[0].strip()


def script_hash() -> str:
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def probe_first_stream(ffmpeg: Path, file: Path) -> dict[str, str]:
    """Key/value view of the first stream (plus side-data rotation) as ffprobe reports it."""
    ffprobe = ffmpeg.with_name(ffmpeg.name.replace("ffmpeg", "ffprobe"))
    proc = subprocess.run([str(ffprobe), "-v", "error", "-select_streams", "0", "-show_entries",
                           "stream:stream_side_data=rotation", "-of", "flat=s=_", str(file)],
                          capture_output=True, text=True, timeout=120)
    fields: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        key, _, value = line.partition("=")
        fields[key.split("_", 3)[-1] if key.startswith("streams_stream_0_") else key] = value.strip('"')
        if key.endswith("_rotation"):
            fields["rotation"] = value.strip('"')
    return fields


def run_recipe(ffmpeg: Path, recipe: list[list[str]], out: Path, tmp: Path,
               expect: dict[str, str], meta: Path | None = None) -> tuple[bool, str]:
    for step in recipe:
        args = [str(ffmpeg), *COMMON, *[a.replace("{out}", str(out)).replace("{tmp}", str(tmp))
                                         .replace("{meta}", str(meta) if meta else "") for a in step]]
        proc = subprocess.run(args, capture_output=True, text=True, timeout=900)
        if proc.returncode != 0:
            lines = (proc.stderr or proc.stdout or "").strip().splitlines()
            return False, lines[-1] if lines else f"exit code {proc.returncode}"
    # FFmpeg silently picks another pixel format when an encoder lacks the requested one (e.g. 8-bit-only x265),
    # so check that the result is really what the tests expect.
    actual = probe_first_stream(ffmpeg, out)
    wrong = {k: actual.get(k) for k, v in expect.items() if actual.get(k) != v}
    if wrong:
        return False, f"output mismatch {wrong} (expected {expect})"
    return True, ""


def generate(ffmpeg: Path, out_dir: Path, force: bool) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out_dir / "manifest.json"
    key = {"schema": SCHEMA, "script": script_hash(), "ffmpeg": ffmpeg_version(ffmpeg)}

    previous = {}
    if manifest_path.is_file() and not force:
        try:
            previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            previous = {}
    same_build = all(previous.get(k) == v for k, v in key.items())
    if not same_build:
        previous = {}
    clips_state: dict = previous.get("clips", {}) if same_build else {}

    failures = 0
    for clip in CLIPS:
        name = clip["name"]
        target = out_dir / name
        state = clips_state.get(name)
        if state and state.get("ok") and target.is_file() and target.stat().st_size == state.get("size"):
            print(f"  up to date  {name}")
            continue
        if state and not state.get("ok") and clip.get("optional"):
            print(f"  skipped     {name} (optional; no usable encoder: {state.get('error')})")
            continue
        partial = out_dir / (target.stem + ".partial" + target.suffix)
        scratch = out_dir / (target.stem + ".step1" + target.suffix)
        started = time.monotonic()
        errors = []
        # P0 round: a clip's FFMETADATA (chapters) goes to a scratch file the recipe reads as {meta}.
        meta = out_dir / (target.stem + ".ffmeta.txt") if clip.get("ffmetadata") else None
        if meta:
            meta.write_bytes(clip["ffmetadata"])
        for recipe in clip["recipes"]:
            ok, error = run_recipe(ffmpeg, recipe, partial, scratch, clip.get("expect", {}), meta)
            encoders = [step[step.index("-c:v") + 1] for step in recipe if "-c:v" in step]
            if ok:
                os.replace(partial, target)
                clips_state[name] = {"ok": True, "size": target.stat().st_size, "what": clip["what"],
                                     "video_encoder": encoders[0] if encoders else None}
                via = f", {encoders[0]}" if encoders else ""
                print(f"  generated   {name} ({time.monotonic() - started:.1f} s{via})")
                break
            errors.append(error)
        else:
            clips_state[name] = {"ok": False, "error": " | ".join(errors), "what": clip["what"]}
            if clip.get("optional"):
                print(f"  skipped     {name} (optional; no usable encoder: {' | '.join(errors)})")
            else:
                print(f"  FAILED      {name}: {' | '.join(errors)}")
                failures += 1
        for leftover in (partial, scratch, *([meta] if meta else [])):
            if leftover.exists():
                leftover.unlink()

    manifest = {**key, "barcode": {"bits": BARCODE_BITS, "cell": BARCODE_CELL}, "clips": clips_state}
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ffmpeg", help="ffmpeg executable (or its directory)")
    parser.add_argument("--out", default=str(REPO_ROOT / "build" / "test-media"), help="output directory")
    parser.add_argument("--force", action="store_true", help="regenerate everything")
    parser.add_argument("--list", action="store_true", help="describe the clips and exit")
    args = parser.parse_args()

    if args.list:
        for clip in CLIPS:
            print(f"{clip['name']:24} {clip['what']}")
        return 0

    ffmpeg = find_ffmpeg(args.ffmpeg)
    print(f"gen_test_media: {ffmpeg} -> {args.out}")
    # Optional clips without a usable encoder are reported and the tests that need them skip themselves; a failing
    # required clip fails the fixture (and with it every media test).
    return 1 if generate(ffmpeg, Path(args.out), args.force) else 0


if __name__ == "__main__":
    sys.exit(main())
