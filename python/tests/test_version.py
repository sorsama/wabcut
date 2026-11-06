"""The package's version follows the release it ships with."""

import re
import unittest
from pathlib import Path

import wabcut


def as_tuple(version):
    return tuple(int(part) for part in re.match(r"(\d+)\.(\d+)\.(\d+)", version).groups())


class VersionTests(unittest.TestCase):
    def test_the_version_looks_like_a_release(self):
        self.assertRegex(wabcut.__version__, r"^\d+\.\d+\.\d+$")

    def test_the_package_is_not_behind_the_release_being_built(self):
        cmake = Path(__file__).resolve().parents[2] / "CMakeLists.txt"
        if not cmake.is_file():
            self.skipTest("run from the source tree to compare with CMakeLists.txt")
        match = re.search(r"project\(Wabcut\s+VERSION\s+([0-9.]+)", cmake.read_text(encoding="utf-8"))
        self.assertIsNotNone(match)
        self.assertGreaterEqual(as_tuple(wabcut.__version__), as_tuple(match.group(1)),
                                "bump python/wabcut/_version.py with the root CMakeLists.txt (docs/release.md)")

    def test_pyproject_reads_the_version_from_the_package(self):
        text = (Path(__file__).resolve().parents[1] / "pyproject.toml").read_text(encoding="utf-8")
        self.assertIn('version = { attr = "wabcut._version.__version__" }', text)


if __name__ == "__main__":
    unittest.main()
