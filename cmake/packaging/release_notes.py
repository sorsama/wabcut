#!/usr/bin/env python3
"""The text of a GitHub release (release workflow, publish job).

    python3 release_notes.py <version> <linux job result> <macos job result>

Prints Markdown: the downloads table (Linux and macOS only when their jobs succeeded), the version's section of
docs/release-notes-<major>.<minor>.md ("## <version> ..."), first-start notes for each platform, and where the
checksums and the sources are.
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
URL = "https://github.com/" + os.environ.get("GITHUB_REPOSITORY", "sorsama/wabcut")


def section(version):
    major_minor = ".".join(version.split(".")[:2])
    path = os.path.join(REPO, "docs", f"release-notes-{major_minor}.md")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    match = re.search(rf"^## {re.escape(version)}\b.*?$\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    if not match:
        sys.exit(f"{path} has no '## {version}' section: add the release's notes there first")
    return match.group(1).strip(), os.path.relpath(path, REPO).replace(os.sep, "/")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    version, linux, macos = sys.argv[1:]
    notes, notes_path = section(version)
    # Links to headings of the notes file point into the file at this release's tag.
    notes = notes.replace("](#", f"]({URL}/blob/v{version}/{notes_path}#")
    base = f"wabcut-{version}"

    rows = [
        f"| **Windows** 10/11 x64 | [`{base}-windows-x64.msi`]({URL}/releases/download/v{version}/{base}-windows-x64.msi) "
        f"(installer, no administrator rights) or [`{base}-windows-x64-portable.zip`]"
        f"({URL}/releases/download/v{version}/{base}-windows-x64-portable.zip) | Tested |",
    ]
    if linux == "success":
        rows.append(
            f"| **Linux** x64 | [`{base}-linux-x64.AppImage`]({URL}/releases/download/v{version}/{base}-linux-x64.AppImage)"
            f" or [`{base}-linux-x64.tar.gz`]({URL}/releases/download/v{version}/{base}-linux-x64.tar.gz) | Preview |")
    if macos == "success":
        rows.append(
            f"| **macOS** 15+, Apple silicon | [`{base}-macos-arm64.dmg`]"
            f"({URL}/releases/download/v{version}/{base}-macos-arm64.dmg) | Preview |")

    out = [
        "A free, open-source video editor for gaming videos, video essays and documentaries, with Premiere Pro's "
        "shortcuts and edit rules and AI that runs on your own machine. **Alpha:** keep backups of your projects.",
        "",
        "## Download",
        "",
        "| Platform | File | Status |",
        "|---|---|---|",
        *rows,
        "",
        "**Tested** means the developer uses it day to day. **Preview** means it is built and checked automatically on "
        "GitHub's machines (installed, the app started, a project rendered), but nobody has edited with it on a real "
        "machine yet: please report what you find.",
    ]
    missing = [name for name, result in (("Linux", linux), ("macOS", macos)) if result != "success"]
    if missing:
        out += ["", f"No {' or '.join(missing)} build in this release: the preview build did not pass its checks."]
    out += [
        "",
        f"## What's new in {version}",
        "",
        notes,
        "",
        "## First start",
        "",
        "- **Windows:** the packages are not code-signed yet, so SmartScreen may stop the first run: choose **More "
        "info**, then **Run anyway**.",
    ]
    if linux == "success":
        out.append("- **Linux:** make the AppImage executable (`chmod +x`) and run it, or unpack the tar.gz and run "
                   "`bin/wabcut`. Built on Ubuntu 24.04; needs a distribution of that age or newer.")
    if macos == "success":
        out.append("- **macOS:** drag Wabcut to Applications. It is not notarized yet, so the first start is refused: "
                   "open **System Settings > Privacy & Security** and choose **Open Anyway** (or run "
                   "`xattr -dr com.apple.quarantine /Applications/Wabcut.app`).")
    if linux == "success" or macos == "success":
        out.append("- **Linux and macOS** do not have local transcription or audio plug-ins yet; cloud features with "
                   "your own key, smart search and everything else in the editor are there.")
    out += [
        "",
        "`SHA256SUMS.txt` lists the checksum of every file. Wabcut is GPL-3.0-or-later: this release's tag and "
        "`vcpkg.json`'s baseline pin every source that went into it (FFmpeg, x264 and x265 included); "
        "`THIRD-PARTY-NOTICES.txt` in each package lists the components and their licenses.",
        "",
        f"Full notes: [{notes_path}]({URL}/blob/v{version}/{notes_path}).",
    ]
    print("\n".join(out))


if __name__ == "__main__":
    main()
