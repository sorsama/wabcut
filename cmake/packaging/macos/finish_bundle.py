#!/usr/bin/env python3
"""Make an installed wabcut.app self-contained, sign it ad hoc and check it (release workflow, macOS).

    python3 finish_bundle.py <path/to/Wabcut.app>

`cmake --install` with macdeployqt (cmake/WabcutInstall.cmake) leaves two things a Mac without the build tools would
trip over:

1. Wabcut is built with Homebrew's LLVM and links its libc++ (cmake/toolchains/macos-llvm.cmake), which lives in the
   Homebrew prefix. This copies every library a Mach-O file in the bundle loads from outside the bundle and the OS
   (libc++, libc++abi, libunwind) into Contents/Frameworks, points the files at @rpath/<name>, gives them an rpath to
   Contents/Frameworks, and drops the rpaths into the build machine (Homebrew, the Qt kit, the build tree).
2. Apple silicon runs no unsigned code, and macdeployqt and install_name_tool changed the files after the linker
   signed them: every Mach-O file is signed again ad hoc, the bundle last. (Not a Developer ID signature: Gatekeeper
   still asks the user to confirm the first start; docs/release.md.)

Then it checks that every load command of every Mach-O file resolves inside the bundle or the OS, and that the
signature verifies. Exits non-zero with the reasons otherwise. LLVM_ROOT (the Homebrew LLVM prefix) is searched for
libraries an outside library loads through @rpath without an rpath of its own.
"""

import os
import shutil
import struct
import subprocess
import sys

SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")
MACHO_MAGICS = {0xFEEDFACE, 0xFEEDFACF, 0xCEFAEDFE, 0xCFFAEDFE, 0xCAFEBABE, 0xBEBAFECA}
IN_BUNDLE_RPATH = "@executable_path/../Frameworks"


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed ({result.returncode}):\n{result.stdout}{result.stderr}")
    return result.stdout


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


class Bundle:
    def __init__(self, app):
        self.app = os.path.realpath(app)
        self.macos = os.path.join(self.app, "Contents", "MacOS")
        self.frameworks = os.path.join(self.app, "Contents", "Frameworks")
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

    def sign(self):
        # Inside out: every Mach-O file, then each framework, then the app.
        for macho in self.machos():
            run("codesign", "--force", "--sign", "-", macho)
        if os.path.isdir(self.frameworks):
            for name in sorted(os.listdir(self.frameworks)):
                if name.endswith(".framework"):
                    run("codesign", "--force", "--sign", "-", os.path.join(self.frameworks, name))
        run("codesign", "--force", "--sign", "-", self.app)

    def check(self):
        for macho in self.machos():
            deps, rpaths = load_commands(macho)
            rel = os.path.relpath(macho, self.app)
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


def main():
    if len(sys.argv) != 2 or not sys.argv[1].rstrip("/").endswith(".app") or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__)
    bundle = Bundle(sys.argv[1])
    for _ in range(10):
        if not bundle.localize():
            break
    bundle.drop_build_rpaths()
    bundle.sign()
    bundle.check()
    for line in bundle.copied:
        print(f"copied {line}")
    print(f"{len(bundle.machos())} Mach-O files signed ad hoc")
    if bundle.problems:
        print("\n".join(["The bundle is not self-contained:"] + bundle.problems), file=sys.stderr)
        sys.exit(1)
    print(f"{bundle.app}: self-contained, signature verifies")


if __name__ == "__main__":
    main()
