#!/usr/bin/env python3
"""Make an installed wabcut.app self-contained, sign it and check it (release workflow, macOS).

    python3 finish_bundle.py <path/to/Wabcut.app> [--identity <Developer ID Application identity> [--keychain <file>]
                             [--entitlements-app <plist>] [--entitlements-helper <plist>]]

`cmake --install` with macdeployqt (cmake/WabcutInstall.cmake) leaves two things a Mac without the build tools would
trip over:

1. Wabcut is built with Homebrew's LLVM and links its libc++ (cmake/toolchains/macos-llvm.cmake), which lives in the
   Homebrew prefix. This copies every library a Mach-O file in the bundle loads from outside the bundle and the OS
   (libc++, libc++abi, libunwind) into Contents/Frameworks, points the files at @rpath/<name>, gives them an rpath to
   Contents/Frameworks, and drops the rpaths into the build machine (Homebrew, the Qt kit, the build tree).
2. Apple silicon runs no unsigned code, and macdeployqt and install_name_tool changed the files after the linker
   signed them: every Mach-O file is signed again, the bundle last. Without --identity the signature is ad hoc (not a
   Developer ID signature: Gatekeeper still asks the user to confirm the first start; docs/release.md). With --identity
   (a "Developer ID Application" certificate in the keychain given by --keychain) every file gets that identity, a
   secure timestamp and the hardened runtime, the main executable the entitlements of --entitlements-app and the other
   executables in Contents/MacOS (wabcut-cli and the helpers) those of --entitlements-helper: the form Apple's
   notarization accepts (notarize.py then submits the bundle).

Then it checks that every load command of every Mach-O file resolves inside the bundle or the OS, that no Mach-O file
needs a newer macOS than the bundle's LSMinimumSystemVersion (Homebrew's libc++ is built for the macOS of its bottle:
0.5.2 declared macOS 13 and crashed at launch on 13 and 14), that the signature verifies and, for a Developer ID
signature, that every executable has the hardened runtime and a secure timestamp. Exits non-zero with the reasons
otherwise. LLVM_ROOT (the Homebrew LLVM prefix) is searched for libraries an outside library loads through @rpath
without an rpath of its own.
"""

import argparse
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import time

SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")
MACHO_MAGICS = {0xFEEDFACE, 0xFEEDFACF, 0xCEFAEDFE, 0xCFFAEDFE, 0xCAFEBABE, 0xBEBAFECA}
IN_BUNDLE_RPATH = "@executable_path/../Frameworks"


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed ({result.returncode}):\n{result.stdout}{result.stderr}")
    return result.stdout


def run_signing(*args):
    """run() for codesign with a secure timestamp: Apple's timestamp server is a network service that now and then does not
    answer ("The timestamp service is not available"); the same signature a moment later works."""
    for attempt in range(1, 5):
        try:
            return run(*args)
        except RuntimeError as error:
            if attempt == 4 or "timestamp" not in str(error).lower():
                raise
            print(f"timestamp service not available (attempt {attempt}), trying again", file=sys.stderr)
            time.sleep(5 * attempt)


def is_macho(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    try:
        with open(path, "rb") as f:
            head = f.read(4)
    except OSError:
        return False
    return len(head) == 4 and struct.unpack(">I", head)[0] in MACHO_MAGICS


def load_commands(path):
    """([dependencies], [rpaths]) from `otool -l`, in load-command order."""
    deps, rpaths = [], []
    cmd = None
    for line in run("otool", "-l", path).splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            cmd = line.split()[1]
        elif line.startswith("name ") and cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
                                                  "LC_LOAD_UPWARD_DYLIB"):
            name = line[len("name "):].rsplit(" (offset", 1)[0]
            if name not in deps:
                deps.append(name)
        elif line.startswith("path ") and cmd == "LC_RPATH":
            rpath = line[len("path "):].rsplit(" (offset", 1)[0]
            if rpath not in rpaths:
                rpaths.append(rpath)
    return deps, rpaths


def version_tuple(text):
    return tuple(int(part) for part in text.split("."))


def required_macos(path):
    """The newest macOS any architecture of a Mach-O file asks for (LC_BUILD_VERSION minos, LC_VERSION_MIN_MACOSX), or
    None."""
    newest = None
    cmd = None
    for line in run("otool", "-arch", "all", "-l", path).splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            cmd = line.split()[1]
        elif (cmd == "LC_BUILD_VERSION" and line.startswith("minos ")) or \
                (cmd == "LC_VERSION_MIN_MACOSX" and line.startswith("version ")):
            version = version_tuple(line.split()[1])
            if newest is None or version > newest:
                newest = version
    return newest


class Bundle:
    def __init__(self, app, identity=None, keychain=None, entitlements_app=None, entitlements_helper=None):
        self.app = os.path.realpath(app)
        self.macos = os.path.join(self.app, "Contents", "MacOS")
        self.frameworks = os.path.join(self.app, "Contents", "Frameworks")
        self.identity = identity
        self.keychain = keychain
        self.entitlements_app = entitlements_app
        self.entitlements_helper = entitlements_helper
        self.search = []  # outside folders that held a copied library: where its own @rpath dependencies are
        llvm = os.environ.get("LLVM_ROOT")
        if llvm:
            self.search += [os.path.join(llvm, "lib", "c++"), os.path.join(llvm, "lib", "unwind"),
                            os.path.join(llvm, "lib")]
        self.copied = []
        self.problems = []

    def inside(self, path):
        return os.path.realpath(path).startswith(self.app + os.sep)

    def machos(self):
        found = []
        for root, _dirs, files in os.walk(self.app):
            found += [os.path.join(root, name) for name in files if is_macho(os.path.join(root, name))]
        return sorted(found)

    def expand(self, path, macho):
        return path.replace("@executable_path", self.macos).replace("@loader_path", os.path.dirname(macho))

    def resolve(self, dep, macho, rpaths, outside_too):
        """The file dyld loads for `dep` from `macho`, or None. Bundle locations first; with `outside_too` also the
        build machine's rpaths and the folders copied libraries came from."""
        if not dep.startswith("@rpath/"):
            candidate = self.expand(dep, macho)
            return os.path.realpath(candidate) if os.path.exists(candidate) else None
        tail = dep[len("@rpath/"):]
        # A library's @rpath also searches the rpaths of the image that loaded it: every executable in Contents/MacOS
        # has Contents/Frameworks.
        places = [self.expand(r, macho) for r in rpaths if r.startswith("@")]
        places.append(self.expand(IN_BUNDLE_RPATH, macho))
        if outside_too:
            places += [r for r in rpaths if not r.startswith("@")] + self.search
        for place in places:
            candidate = os.path.join(place, tail)
            if os.path.exists(candidate):
                return os.path.realpath(candidate)
        return None

    def localize(self):
        """One pass: copy every outside library into Contents/Frameworks. True when something changed."""
        changed = False
        os.makedirs(self.frameworks, exist_ok=True)
        for macho in self.machos():
            deps, rpaths = load_commands(macho)
            uses_frameworks = False
            for dep in deps:
                if dep.startswith(SYSTEM_PREFIXES):
                    continue
                found = self.resolve(dep, macho, rpaths, outside_too=True)
                if found is None or self.inside(found):
                    continue
                name = os.path.basename(dep)
                copy = os.path.join(self.frameworks, name)
                if not os.path.exists(copy):
                    shutil.copy2(found, copy)
                    os.chmod(copy, 0o755)
                    run("install_name_tool", "-id", "@rpath/" + name, copy)
                    self.search.append(os.path.dirname(found))
                    self.copied.append(f"{found} -> Contents/Frameworks/{name}")
                if dep != "@rpath/" + name:
                    run("install_name_tool", "-change", dep, "@rpath/" + name, macho)
                uses_frameworks = True
                changed = True
            # Libraries find each other through the rpath of the executable that loads them.
            if uses_frameworks and os.path.dirname(macho) == self.macos and IN_BUNDLE_RPATH not in rpaths:
                run("install_name_tool", "-add_rpath", IN_BUNDLE_RPATH, macho)
        return changed

    def drop_build_rpaths(self):
        for macho in self.machos():
            for rpath in load_commands(macho)[1]:
                if not rpath.startswith("@"):
                    run("install_name_tool", "-delete_rpath", rpath, macho)
                    print(f"dropped rpath {rpath} from {os.path.relpath(macho, self.app)}")

    def main_executable(self):
        with open(os.path.join(self.app, "Contents", "Info.plist"), "rb") as f:
            return os.path.join(self.macos, plistlib.load(f)["CFBundleExecutable"])

    def codesign(self, entitlements=None):
        """The codesign command line (without the path): ad hoc, or the Developer ID with a secure timestamp and the
        hardened runtime that notarization demands."""
        if not self.identity:
            return ["codesign", "--force", "--sign", "-"]
        args = ["codesign", "--force", "--timestamp", "--options", "runtime", "--sign", self.identity]
        if self.keychain:
            args += ["--keychain", self.keychain]
        if entitlements:
            args += ["--entitlements", entitlements]
        return args

    def sign(self):
        # Inside out: every Mach-O file, then each framework, then the app. Entitlements belong to executables: the
        # main one gets the app's, the other executables in Contents/MacOS (wabcut-cli, the helpers) the helpers'.
        main = self.main_executable()
        for macho in self.machos():
            entitlements = None
            if os.path.dirname(macho) == self.macos:
                entitlements = self.entitlements_app if macho == main else self.entitlements_helper
            run_signing(*self.codesign(entitlements), macho)
        if os.path.isdir(self.frameworks):
            for name in sorted(os.listdir(self.frameworks)):
                if name.endswith(".framework"):
                    run_signing(*self.codesign(), os.path.join(self.frameworks, name))
        run_signing(*self.codesign(self.entitlements_app), self.app)

    def check_developer_id(self):
        """Every Mach-O file must carry the hardened runtime and a secure timestamp, or Apple's notary service rejects
        the bundle (and says so only after minutes: this finds it at once)."""
        for macho in self.machos():
            result = subprocess.run(["codesign", "-d", "--verbose=4", macho], capture_output=True, text=True)
            report = result.stdout + result.stderr
            rel = os.path.relpath(macho, self.app)
            if "(runtime)" not in report:
                self.problems.append(f"{rel}: signed without the hardened runtime")
            if "Timestamp=" not in report:
                self.problems.append(f"{rel}: signed without a secure timestamp")
            if "Authority=Developer ID Application" not in report:
                self.problems.append(f"{rel}: not signed with a Developer ID Application certificate")

    def check(self):
        with open(os.path.join(self.app, "Contents", "Info.plist"), "rb") as f:
            minimum = f"{plistlib.load(f)['LSMinimumSystemVersion']}"
        for macho in self.machos():
            deps, rpaths = load_commands(macho)
            rel = os.path.relpath(macho, self.app)
            needs = required_macos(macho)
            if needs and needs > version_tuple(minimum):
                self.problems.append(f"{rel}: needs macOS {'.'.join(map(str, needs))}, newer than the bundle's "
                                     f"LSMinimumSystemVersion {minimum} (cmake/packaging/macos/Info.plist.in)")
            for rpath in rpaths:
                if not rpath.startswith("@"):
                    self.problems.append(f"{rel}: rpath into the build machine: {rpath}")
            for dep in deps:
                if dep.startswith(SYSTEM_PREFIXES):
                    continue
                found = self.resolve(dep, macho, rpaths, outside_too=False)
                if found is None:
                    self.problems.append(f"{rel}: {dep} is not in the bundle")
                elif not self.inside(found):
                    self.problems.append(f"{rel}: {dep} loads from outside the bundle ({found})")
        try:
            run("codesign", "--verify", "--deep", "--strict", self.app)
        except RuntimeError as error:
            self.problems.append(str(error))
        if self.identity:
            self.check_developer_id()


def main():
    parser = argparse.ArgumentParser(usage="finish_bundle.py <Wabcut.app> [--identity ...]", add_help=False)
    parser.add_argument("app")
    parser.add_argument("--identity")
    parser.add_argument("--keychain")
    parser.add_argument("--entitlements-app")
    parser.add_argument("--entitlements-helper")
    args = parser.parse_args()
    if not args.app.rstrip("/").endswith(".app") or not os.path.isdir(args.app):
        sys.exit(__doc__)
    if args.identity and not (args.entitlements_app and args.entitlements_helper):
        sys.exit("finish_bundle: a Developer ID signature needs --entitlements-app and --entitlements-helper "
                 "(cmake/packaging/macos/entitlements-*.plist): the hardened runtime stops the app without them")
    bundle = Bundle(args.app, args.identity, args.keychain, args.entitlements_app, args.entitlements_helper)
    for _ in range(10):
        if not bundle.localize():
            break
    bundle.drop_build_rpaths()
    bundle.sign()
    bundle.check()
    for line in bundle.copied:
        print(f"copied {line}")
    print(f"{len(bundle.machos())} Mach-O files signed " + ("with the Developer ID" if args.identity else "ad hoc"))
    if bundle.problems:
        print("\n".join(["The bundle is not self-contained:"] + bundle.problems), file=sys.stderr)
        sys.exit(1)
    print(f"{bundle.app}: self-contained, signature verifies")


if __name__ == "__main__":
    main()
