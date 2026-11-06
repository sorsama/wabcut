#!/usr/bin/env python3
"""A platform's test counts for the job summary: what ran, what failed and, for every test that did not run, why
(docs/building.md, "Platforms and their test status").

  ci-test-report.py --junit ctest-linux-gcc.xml --title "Linux x64 (linux-gcc)" [--gaps tests/platform-gaps.json
                    --platform linux] [--excluded-labels gpu,audio-device]

--junit is the file `ctest --output-junit` wrote. The report is Markdown, printed and appended to $GITHUB_STEP_SUMMARY:
the totals (passed, failed, skipped), the failed tests, and a table of the skipped ones with the reason the test itself
gave (a Catch2 SKIP(...) message, or the exit code 77 convention of the tests that need media, a GPU or an audio device).
--gaps names the known-gap table (tests/platform-gaps.json: tests that fail on a platform for a reason that is known,
and the reason), which the job excludes from the gate and which are listed here as "known gaps" so none disappears
quietly; --excluded-labels names the ctest labels the preset leaves out. Exits 0 always: the job's own test step decides
the verdict, this only makes the numbers visible.
"""

import argparse
import json
import os
import re
import sys
import xml.etree.ElementTree as ET


def reason_of(case):
    """Why a skipped test did not run: the skipped element's message, else the line Catch2 printed after SKIPPED."""
    skipped = case.find("skipped")
    message = (skipped.get("message") if skipped is not None else "") or ""
    message = message.strip()
    if message and message.lower() not in ("skipped", "not run", "notrun", "disabled", "skip_return_code=4", "skip_return_code=77"):
        return message
    output = (case.findtext("system-out") or "") + (case.findtext("system-err") or "")
    found = re.search(r"SKIPPED:?[ \t]*\r?\n?(?:[ \t]*\r?\n)?(?P<text>[^\r\n]+(?:\r?\n[ \t]+[^\r\n]+)*)", output)
    if found:
        text = " ".join(part.strip() for part in found.group("text").splitlines())
        text = re.sub(r"^explicit skip\s*", "", text, flags=re.I).strip().strip('"').strip()
        if text:
            return text
    for line in output.splitlines():
        low = line.lower()
        if "skip" in low and "skip_return_code" not in low and "skipped:" not in low and len(line.strip()) > 8:
            return line.strip()[:240]
    return "no reason in the test output (it exited with its skip code)"


def read_cases(path):
    root = ET.parse(path).getroot()
    suites = [root] if root.tag == "testsuite" else list(root.iter("testsuite"))
    cases = []
    for suite in suites:
        cases.extend(suite.iter("testcase"))
    return cases


def md(text):
    return text.replace("|", "\\|").replace("\n", " ")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--junit", required=True)
    parser.add_argument("--title", required=True)
    parser.add_argument("--gaps")
    parser.add_argument("--platform")
    parser.add_argument("--excluded-labels", default="")
    args = parser.parse_args()

    lines = [f"### {args.title}: test report", ""]
    try:
        cases = read_cases(args.junit)
    except (OSError, ET.ParseError) as error:
        lines.append(f"No test results ({error}): the build did not get as far as running tests.")
        report(lines)
        return 0

    failed, skipped, passed = [], [], 0
    for case in cases:
        name = case.get("name", "?")
        if case.find("failure") is not None or case.find("error") is not None:
            failed.append(name)
        elif case.find("skipped") is not None or case.get("status") in ("notrun", "disabled"):
            skipped.append((name, reason_of(case)))
        else:
            passed += 1
    lines += [
        f"| Tests | Passed | Failed | Skipped |",
        f"|---:|---:|---:|---:|",
        f"| {len(cases)} | {passed} | {len(failed)} | {len(skipped)} |",
        "",
    ]
    if failed:
        lines += ["**Failed:**", ""] + [f"- {md(name)}" for name in sorted(failed)] + [""]
    if skipped:
        lines += ["**Skipped, with the reason each test gave:**", "", "| Test | Reason |", "|---|---|"]
        lines += [f"| {md(name)} | {md(reason)} |" for name, reason in sorted(skipped)]
        lines.append("")
    labels = [label for label in args.excluded_labels.split(",") if label]
    if labels:
        lines += [f"Not run here at all, by label: {', '.join('`' + label + '`' for label in labels)} "
                  f"(a GitHub runner has no GPU and no audio device, and no quiet machine for timings: "
                  f"docs/building.md, \"Platforms and their test status\").", ""]
    if args.gaps and args.platform and os.path.exists(args.gaps):
        with open(args.gaps, encoding="utf-8") as f:
            gaps = json.load(f).get(args.platform, [])
        if gaps:
            lines += ["**Known gaps** (excluded from the gate, run apart and reported as information):", "",
                      "| Test | Why it is excluded |", "|---|---|"]
            lines += [f"| {md(gap['test'])} | {md(gap['reason'])} |" for gap in gaps]
            lines.append("")
    report(lines)
    return 0


def report(lines):
    text = "\n".join(lines) + "\n"
    print(text)
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
