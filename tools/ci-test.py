#!/usr/bin/env python3
"""Runs ctest for CI: in parallel, then the failed tests once more one at a time (docs/building.md, "Continuous
integration").

  ci-test.py --build-dir build/<preset> [--jobs N] [--max-rerun 25] -- <ctest arguments>

The first run uses one job per processor (--jobs; a [perf] test still runs alone: RUN_SERIAL). A test that fails there
runs again on its own (ctest --rerun-failed -j 1); the job fails only for tests that fail both times. A test that passes
alone is flaky: it is listed as a warning on the run and in the job summary, so it is still seen and fixed, but it no
longer costs a full rerun of the workflow. When more than --max-rerun tests fail, nothing is rerun (that is a broken
build, not a flaky test).
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import time


def escape(text, prop=False):
    """A value for a GitHub workflow command (::warning title=<prop>::<text>)."""
    text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    if prop:
        text = text.replace(":", "%3A").replace(",", "%2C")
    return text


def summary(lines):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")


def failed_tests(build_dir):
    """The names in ctest's LastTestsFailed.log ("<number>:<name>" lines) of the last run with failures."""
    names = []
    for log in sorted((build_dir / "Testing" / "Temporary").glob("LastTestsFailed*.log")):
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            number, sep, name = line.partition(":")
            if sep and number.strip().isdigit():
                names.append(name.strip())
    return names


def without_parallel(args):
    """The ctest arguments without -j/--parallel (and their value) and --output-junit <file>."""
    out = []
    skip = False
    for i, a in enumerate(args):
        if skip:
            skip = False
            continue
        if a in ("-j", "--parallel", "--output-junit"):
            # -j / --parallel may stand alone (CMake 3.29+); a following number is theirs.
            nxt = args[i + 1] if i + 1 < len(args) else ""
            skip = a == "--output-junit" or nxt.isdigit()
            continue
        if a.startswith("-j") and a[2:].isdigit():
            continue
        out.append(a)
    return out


def run(cmd):
    print("+ " + subprocess.list2cmdline(cmd), flush=True)
    start = time.monotonic()
    code = subprocess.run(cmd).returncode
    return code, time.monotonic() - start


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--max-rerun", type=int, default=25)
    parser.add_argument("ctest_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    ctest_args = args.ctest_args[1:] if args.ctest_args[:1] == ["--"] else args.ctest_args
    ctest = shutil.which("ctest") or "ctest"

    # A stale list from an earlier run must not be read as this run's failures.
    for log in (args.build_dir / "Testing" / "Temporary").glob("LastTestsFailed*.log"):
        log.unlink()

    code, seconds = run([ctest, *without_parallel(ctest_args), "-j", str(args.jobs)])
    if code == 0:
        summary([f"Tests: all passed at -j {args.jobs} in {seconds / 60:.1f} min"])
        return 0
    first = failed_tests(args.build_dir)
    if not first:
        print(f"ctest exited with {code} without a failed test to rerun")
        return code
    if len(first) > args.max_rerun:
        print(f"{len(first)} tests failed (more than {args.max_rerun}): not rerunning them")
        summary([f"Tests: {len(first)} failed at -j {args.jobs}; not rerun"])
        return code

    print(f"\n==== {len(first)} failed test(s): running them again, one at a time ====\n", flush=True)
    code2, seconds2 = run([ctest, *without_parallel(ctest_args), "--rerun-failed", "-j", "1", "--output-on-failure"])
    second = failed_tests(args.build_dir) if code2 else []
    flaky = [name for name in first if name not in second]
    lines = [f"Tests: {len(first)} failed at -j {args.jobs} ({seconds / 60:.1f} min); "
             f"rerun alone in {seconds2 / 60:.1f} min: {len(flaky)} passed (flaky), {len(second)} failed again"]
    for name in flaky:
        print(f"::warning title={escape('Flaky test', True)}::{escape(name + ' failed in the parallel run and passed alone')}")
        lines.append(f"- flaky: {name}")
    for name in second:
        print(f"::error title={escape('Test failed twice', True)}::{escape(name)}")
        lines.append(f"- **failed twice**: {name}")
    summary(lines)
    if code2 and not second:
        print(f"ctest exited with {code2} on the rerun without a failed test")
    return code2


if __name__ == "__main__":
    sys.exit(main())
