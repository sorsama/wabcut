#!/usr/bin/env python3
"""Have Apple notarize a signed Wabcut.app or disk image, wait for the answer and staple the ticket (release workflow,
macOS; docs/release.md, "macOS signing and notarization").

    python3 notarize.py <Wabcut.app | image.dmg | archive.zip>

Needs the notary credentials the release workflow stores in a temporary keychain (`xcrun notarytool store-credentials`):

    WABCUT_NOTARY_PROFILE   the keychain profile's name
    WABCUT_MACOS_KEYCHAIN   the keychain that holds it (optional: the default keychain list otherwise)

A .app is zipped first (`ditto -c -k --keepParent`, the form Apple documents for notarytool; the ZIP is only the vehicle
and is deleted). The submission waits until Apple has answered. Anything but "Accepted" prints Apple's log (it names every
file that failed and why: no hardened runtime, no secure timestamp, an unsigned library, a bad entitlement) and exits
non-zero. After "Accepted" the ticket is stapled to the .app or .dmg (a ZIP cannot be stapled) and validated, so the
file passes Gatekeeper on a Mac that is offline.

References: https://developer.apple.com/documentation/security/customizing-the-notarization-workflow,
https://developer.apple.com/documentation/security/resolving-common-notarization-issues
"""

import json
import os
import subprocess
import sys
import tempfile

SUBMIT_TIMEOUT = "90m"


def notary_args():
    profile = os.environ.get("WABCUT_NOTARY_PROFILE")
    if not profile:
        sys.exit("notarize: WABCUT_NOTARY_PROFILE is not set (the release workflow stores the credentials there)")
    args = ["--keychain-profile", profile]
    keychain = os.environ.get("WABCUT_MACOS_KEYCHAIN")
    if keychain:
        args += ["--keychain", keychain]
    return args


def run(*args, check=True):
    result = subprocess.run(args, capture_output=True, text=True)
    if check and result.returncode != 0:
        sys.exit(f"notarize: {' '.join(args[:3])} failed ({result.returncode}):\n{result.stdout}{result.stderr}")
    return result


def submit(path):
    """Submit `path` and wait. Returns the submission id; exits when Apple does not accept the file."""
    result = run("xcrun", "notarytool", "submit", path, *notary_args(), "--wait", "--timeout", SUBMIT_TIMEOUT,
                 "--output-format", "json", check=False)
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    try:
        answer = json.loads(result.stdout)
    except json.JSONDecodeError:
        sys.exit(f"notarize: notarytool gave no answer for {os.path.basename(path)} (exit {result.returncode})")
    submission = answer.get("id", "")
    status = answer.get("status", "")
    if status != "Accepted":
        print(f"notarize: Apple's notary service answered '{status}' for {os.path.basename(path)}", file=sys.stderr)
        if submission:
            log = run("xcrun", "notarytool", "log", submission, *notary_args(), check=False)
            print(log.stdout + log.stderr, file=sys.stderr)
        sys.exit(1)
    print(f"notarize: {os.path.basename(path)} accepted (submission {submission})")
    return submission


def main():
    if len(sys.argv) != 2 or not os.path.exists(sys.argv[1]):
        sys.exit(__doc__)
    target = os.path.abspath(sys.argv[1].rstrip("/"))
    if target.endswith(".app"):
        with tempfile.TemporaryDirectory() as scratch:
            archive = os.path.join(scratch, "Wabcut-notarize.zip")
            run("ditto", "-c", "-k", "--keepParent", target, archive)
            submit(archive)
    else:
        submit(target)
    if target.endswith((".app", ".dmg")):
        run("xcrun", "stapler", "staple", target)
        run("xcrun", "stapler", "validate", target)
        print(f"notarize: ticket stapled to {os.path.basename(target)}")


if __name__ == "__main__":
    main()
