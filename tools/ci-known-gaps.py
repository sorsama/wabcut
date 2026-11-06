#!/usr/bin/env python3
"""The tests a platform's CI leaves out of the gate because they fail there for a known, unfixed cause
(tests/platform-gaps.json; docs/building.md, "Platforms and their test status").

  ci-known-gaps.py <platform>

Prints the ctest -R regex that matches exactly those tests ("a|b|c"), or nothing when the platform has none; the
workflows use it as `ctest -E "$(python3 tools/ci-known-gaps.py linux)"` for the gate and `ctest -R` for the separate,
informational run of the same tests.
"""

import json
import os
import sys

GAPS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests", "platform-gaps.json")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(GAPS, encoding="utf-8") as f:
        gaps = json.load(f).get(sys.argv[1], [])
    print("|".join(gap["test"] for gap in gaps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
