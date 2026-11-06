#!/usr/bin/env python3
"""Guards for the platform work (Windows on ARM64, Linux, macOS): the CI helper scripts, the known-gap table, the
vcpkg platform qualifiers and the macOS / Linux packaging files. Run by ctest as `tools.ci_scripts` (label `tools`); the
repository root is the only argument. These are static checks: nothing here builds or signs anything.

The workflows themselves (.github/workflows/*.yml) are linted with actionlint when they change; Python has no YAML
parser in its standard library, so they are not read here.
"""

import json
import os
import plistlib
import py_compile
import re
import subprocess
import sys
import tempfile
import unittest
import xml.dom.minidom

ROOT = os.path.abspath(sys.argv.pop(1)) if len(sys.argv) > 1 and os.path.isdir(sys.argv[1]) else os.getcwd()


def path(*parts):
    return os.path.join(ROOT, *parts)


def run(script, *args):
    # The release notes quote docs with non-ASCII text; -X utf8 keeps a Windows console code page out of it (-I would
    # ignore PYTHONIOENCODING).
    result = subprocess.run([sys.executable, "-I", "-X", "utf8", path(script), *args], capture_output=True, text=True,
                            encoding="utf-8")
    return result.returncode, result.stdout, result.stderr


def test_case_names():
    names = []
    pattern = re.compile(r'TEST_CASE\(\s*"((?:[^"\\]|\\.)*)"')
    for folder, _dirs, files in os.walk(path("tests")):
        for name in files:
            if name.endswith(".cpp"):
                with open(os.path.join(folder, name), encoding="utf-8", errors="replace") as f:
                    names += pattern.findall(f.read())
    return names


class KnownGaps(unittest.TestCase):
    def setUp(self):
        with open(path("tests", "platform-gaps.json"), encoding="utf-8") as f:
            self.table = json.load(f)

    def test_regex_is_the_names_joined(self):
        for platform in ("linux", "macos"):
            code, out, err = run("tools/ci-known-gaps.py", platform)
            self.assertEqual(code, 0, err)
            self.assertEqual(out.strip(), "|".join(gap["test"] for gap in self.table[platform]))
        code, out, _ = run("tools/ci-known-gaps.py", "windows")
        self.assertEqual((code, out.strip()), (0, ""))  # Windows has no known gaps

    def test_every_entry_has_a_reason_and_names_a_real_test(self):
        names = test_case_names()
        self.assertGreater(len(names), 100)
        for platform in ("linux", "macos"):
            for gap in self.table[platform]:
                self.assertTrue(gap["reason"].strip(), f"{platform}: {gap['test']} has no reason")
                self.assertTrue(any(re.search(gap["test"], name) for name in names),
                                f"{platform}: no TEST_CASE matches '{gap['test']}': the table lists a test that is gone")

    def test_macos_gaps_are_linux_gaps(self):
        linux = {gap["test"] for gap in self.table["linux"]}
        self.assertTrue({gap["test"] for gap in self.table["macos"]} <= linux)


JUNIT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="(empty)" tests="4" failures="1" skipped="2">
  <testcase name="passes" classname="passes" status="run"><system-out>ok</system-out></testcase>
  <testcase name="fails | here" classname="x" status="run"><failure message="Failed"/></testcase>
  <testcase name="skips with a reason" classname="x" status="run"><skipped message="SKIP_RETURN_CODE=4"/>
    <system-out>f.cpp:14: SKIPPED:
explicit skip
	"no GPU on this machine"
</system-out></testcase>
  <testcase name="skips silently" classname="x" status="notrun"><skipped message="Disabled"/></testcase>
</testsuite>
"""


class TestReport(unittest.TestCase):
    def test_counts_failures_and_every_skip_reason(self):
        with tempfile.TemporaryDirectory() as folder:
            junit = os.path.join(folder, "ctest.xml")
            with open(junit, "w", encoding="utf-8") as f:
                f.write(JUNIT)
            code, out, err = run("tools/ci-test-report.py", "--junit", junit, "--title", "Linux x64", "--gaps",
                                 path("tests", "platform-gaps.json"), "--platform", "linux", "--excluded-labels",
                                 "gpu,audio-device")
        self.assertEqual(code, 0, err)
        self.assertIn("| 4 | 1 | 1 | 2 |", out)
        self.assertIn("fails \\| here", out)  # a pipe in a name does not break the table
        self.assertIn("| skips with a reason | no GPU on this machine |", out)
        self.assertIn("| skips silently | no reason in the test output", out)
        self.assertIn("`gpu`, `audio-device`", out)
        self.assertIn("Managed local helper lifecycle", out)  # the known gaps are listed with their reasons

    def test_missing_results_are_reported_not_fatal(self):
        code, out, _ = run("tools/ci-test-report.py", "--junit", path("no-such-ctest.xml"), "--title", "Linux")
        self.assertEqual(code, 0)
        self.assertIn("No test results", out)


class ReleaseNotes(unittest.TestCase):
    def notes(self, *args):
        code, out, err = run("cmake/packaging/release_notes.py", "0.6.0", *args)
        self.assertEqual(code, 0, err)
        return out

    def test_previews_are_listed_when_they_passed(self):
        out = self.notes("success", "success", "--windows-arm64", "success", "--macos-notarized", "true")
        self.assertIn("wabcut-0.6.0-windows-arm64.msi", out)
        self.assertIn("wabcut-0.6.0-windows-arm64-portable.zip", out)
        self.assertIn("Preview, signed and notarized", out)
        self.assertIn("notarized by Apple", out)
        self.assertNotIn("Open Anyway", out)

    def test_unsigned_macos_keeps_the_gatekeeper_instructions(self):
        out = self.notes("success", "success")
        self.assertIn("Open Anyway", out)
        self.assertNotIn("windows-arm64", out)  # a platform that was not built has no row

    def test_failed_and_skipped_platforms(self):
        out = self.notes("skipped", "failure", "--windows-arm64", "failure")
        self.assertIn("No Windows ARM64 or macOS build in this release", out)
        self.assertNotIn("Linux build", out)  # skipped is not failed
        self.assertNotIn("notarized by Apple", out)


class Packaging(unittest.TestCase):
    def test_python_tools_compile(self):
        with tempfile.TemporaryDirectory() as folder:
            for script in ("cmake/packaging/macos/finish_bundle.py", "cmake/packaging/macos/notarize.py",
                           "cmake/packaging/release_notes.py", "tools/ci-known-gaps.py", "tools/ci-test-report.py"):
                py_compile.compile(path(script), cfile=os.path.join(folder, os.path.basename(script) + "c"), doraise=True)

    def test_entitlements(self):
        with open(path("cmake", "packaging", "macos", "entitlements-app.plist"), "rb") as f:
            app = plistlib.load(f)
        with open(path("cmake", "packaging", "macos", "entitlements-helper.plist"), "rb") as f:
            helper = plistlib.load(f)
        self.assertTrue(app["com.apple.security.cs.allow-jit"])
        self.assertTrue(app["com.apple.security.device.audio-input"])  # voice-over recording
        self.assertTrue(helper["com.apple.security.cs.allow-jit"])
        self.assertNotIn("com.apple.security.device.audio-input", helper)
        for entitlements in (app, helper):  # nothing broader than what notarization needs
            self.assertNotIn("com.apple.security.cs.disable-library-validation", entitlements)
            self.assertNotIn("com.apple.security.cs.allow-unsigned-executable-memory", entitlements)

    def test_info_plist_declares_the_project_type_and_the_microphone(self):
        with open(path("cmake", "packaging", "macos", "Info.plist.in"), "rb") as f:
            info = plistlib.load(f)
        self.assertIn("NSMicrophoneUsageDescription", info)
        exported = info["UTExportedTypeDeclarations"][0]
        self.assertEqual(exported["UTTypeIdentifier"], "io.github.sorsama.wabcut.project")
        self.assertEqual(exported["UTTypeTagSpecification"]["public.filename-extension"], ["wabcut"])
        owner = info["CFBundleDocumentTypes"][0]
        self.assertEqual(owner["LSItemContentTypes"], ["io.github.sorsama.wabcut.project"])

    def test_linux_launcher_and_mime_type_agree(self):
        mime = xml.dom.minidom.parse(path("cmake", "packaging", "linux", "wabcut-mime.xml"))
        types = [node.getAttribute("type") for node in mime.getElementsByTagName("mime-type")]
        with open(path("cmake", "packaging", "linux", "wabcut.desktop"), encoding="utf-8") as f:
            desktop = f.read()
        self.assertEqual(types, ["application/x-wabcut-project"])
        self.assertIn("MimeType=application/x-wabcut-project;", desktop)
        self.assertEqual(mime.getElementsByTagName("glob")[0].getAttribute("pattern"), "*.wabcut")


class Manifest(unittest.TestCase):
    def test_hardware_codec_features_stay_off_arm64_and_x64_keeps_them(self):
        with open(path("vcpkg.json"), encoding="utf-8") as f:
            manifest = json.load(f)
        ffmpeg = next(d for d in manifest["dependencies"] if isinstance(d, dict) and d["name"] == "ffmpeg")
        platform = {feature["name"]: feature["platform"] for feature in ffmpeg["features"]
                    if isinstance(feature, dict) and "platform" in feature}
        # NVENC / NVDEC, AMF and Quick Sync have no ARM64 Windows libraries (libvpl's port supports x64 and x86 only).
        # The x64 Windows set is the one the vcpkg binary caches hold: these three must still apply to it.
        self.assertEqual(platform["nvcodec"], "(windows | linux) & x64")
        self.assertEqual(platform["amf"], "(windows | linux) & !arm64")
        self.assertEqual(platform["qsv"], "(windows | linux) & !arm64")

    def test_arm64_triplet_and_preset(self):
        with open(path("cmake", "triplets", "arm64-windows-release.cmake"), encoding="utf-8") as f:
            triplet = f.read()
        self.assertIn("set(VCPKG_TARGET_ARCHITECTURE arm64)", triplet)
        self.assertIn("set(VCPKG_BUILD_TYPE release)", triplet)
        with open(path("CMakePresets.json"), encoding="utf-8") as f:
            presets = json.load(f)
        preset = next(p for p in presets["configurePresets"] if p["name"] == "win-msvc-arm64")
        self.assertEqual(preset["cacheVariables"]["VCPKG_TARGET_TRIPLET"], "arm64-windows-release")
        self.assertTrue(any(p["name"] == "win-msvc-arm64" for p in presets["testPresets"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
