# wabcut-name: Loudness check
# wabcut-description: Measures the mix against a target (default -14 LUFS, true peak under -1 dBTP) and reports pass or fail.
# wabcut-permissions: Read, MediaAnalysis
"""Check the programme loudness before delivery (EBU R128 / ITU-R BS.1770), for one person or a render farm.

It measures the sequence as it renders, changes nothing, and ends with exit code 1 when the mix is off target, so a
build script can stop on it.

    python loudness_qc.py film.wabcut [--target -14] [--tolerance 1] [--peak -1] [--sequence "Final cut"]

Inside Wabcut:        File > Run Python Script...  (the defaults are used)
"""

import argparse
import sys

import wabcut


def check(session, target=-14.0, tolerance=1.0, ceiling=-1.0):
    """Returns (passed, report lines)."""
    answer = session.audio.loudness(oversAbove=ceiling)
    mix = answer["results"][0]
    loudness, peak = mix["integratedLufs"], mix["truePeakDbtp"]
    lines = [f"Integrated loudness {loudness:.1f} LUFS (target {target:g} +/- {tolerance:g})",
             f"True peak {peak:.1f} dBTP (ceiling {ceiling:g})"]
    passed = abs(loudness - target) <= tolerance and peak <= ceiling
    lines.append("PASS" if passed else "FAIL")
    return passed, lines


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project", nargs="?", help="the .wabcut file (leave out inside Wabcut)")
    parser.add_argument("--target", type=float, default=-14.0, help="target integrated loudness in LUFS")
    parser.add_argument("--tolerance", type=float, default=1.0, help="allowed distance from the target in LU")
    parser.add_argument("--peak", type=float, default=-1.0, help="true-peak ceiling in dBTP")
    parser.add_argument("--sequence", help="the sequence's name or id (needed when the project has several)")
    args = parser.parse_args(argv)
    with wabcut.connect(args.project, analysis=True, sequence=args.sequence) as session:  # analysis decodes the media: it can take a while
        passed, lines = check(session, args.target, args.tolerance, args.peak)
    print("\n".join(lines))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
