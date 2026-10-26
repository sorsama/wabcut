#!/usr/bin/env python3
"""The CI build caches: ccache for Wabcut's own code, the vcpkg binary cache for its dependencies (docs/building.md,
"CI caches").

GitHub keeps 10 GB of Actions caches per repository, and a cache saved by a pull request or a tag can be restored only by
that pull request or tag. So the workflows save their caches from main (pull requests and releases restore main's; a run
started by hand on another branch saves its vcpkg packages for that branch), save a new vcpkg entry only when its packages
changed, and delete a branch's older entries of a kind once the new one is in.

  ci-cache.py ccache-setup --max-size 2G [--read-only]
        installs the pinned ccache (checked by SHA-256) and configures it for the later steps through $GITHUB_ENV: the
        cache in $RUNNER_TEMP/ccache (actions/cache restores it there first), the compiler checked by content, and /WX and
        -Werror left out of the hash, so the release build (warnings not fatal) reuses the objects CI compiled with them.
        CCACHE_EXE is the launcher for CMake. --read-only: use the cache, never write to it (a job that does not save).
  ci-cache.py ccache-finish [--trim]
        the hit rate into the log and the job summary. --trim (before saving): drop what this job did not use, so the
        saved cache holds the current objects only.
  ci-cache.py vcpkg-key --build-dir build/<preset> --cache <dir> --prefix vcpkg-<os>-<preset>-
        after a configure that got past vcpkg, drops the archives this build did not install; then writes
        key=<prefix><digest of the archives> to $GITHUB_OUTPUT: the restored cache's own key when nothing was built, and
        empty when the cache has no archives.
  ci-cache.py delete-older [--ref refs/heads/main] --prefix <p> --keep <key>
        deletes the branch's caches whose key starts with <p>, except <key>; does nothing when <key> was not saved. Needs
        GH_TOKEN with actions: write.
"""

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import time
import zipfile

CCACHE_VERSION = "4.14.2"
# The official release archives (github.com/ccache/ccache/releases) and their SHA-256.
CCACHE_ARCHIVES = {
    "win32": ("windows-x86_64.zip", "6993ce645e53192cec592ee23186200b574ac4c59097ecde8d9cfd681b47ae84"),
    "linux": ("linux-x86_64-glibc.tar.xz", "bd1313f5316f594443dcb4113a4191c666ac6450e0cf016ff8cb8b080f46c160"),
    "darwin": ("darwin.tar.gz", "cf240009630b605b775c7c5c7b7105a3b196c79f31af322c1557969aa28003fc"),
}


def runner_temp():
    return pathlib.Path(os.environ.get("RUNNER_TEMP") or os.environ.get("TEMP") or "/tmp")


def append(variable, lines):
    """Appends name=value lines to the file named by $GITHUB_ENV / $GITHUB_OUTPUT (printed when not on a runner)."""
    path = os.environ.get(variable)
    text = "".join(f"{line}\n" for line in lines)
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text)
    else:
        print(f"[{variable}]\n{text}", end="")


def summary(text):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text + "\n")


def ccache_setup(args):
    suffix, sha256 = CCACHE_ARCHIVES[sys.platform if sys.platform in CCACHE_ARCHIVES else "linux"]
    name = f"ccache-{CCACHE_VERSION}-{suffix}"
    home = runner_temp() / f"ccache-{CCACHE_VERSION}"
    home.mkdir(parents=True, exist_ok=True)
    archive = home / name
    url = f"https://github.com/ccache/ccache/releases/download/v{CCACHE_VERSION}/{name}"
    # curl, not urllib: every runner image has it, with the system's certificates (Python's are not set up on macOS).
    subprocess.run(["curl", "-fsSL", "--retry", "3", "-o", str(archive), url], check=True)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != sha256:
        sys.exit(f"{name}: SHA-256 {digest}, expected {sha256}")

    # Only the executable is taken out of the archive, under a name of our own.
    exe_name = "ccache.exe" if sys.platform == "win32" else "ccache"
    exe = home / exe_name
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            member = next(m for m in z.namelist() if m.rsplit("/", 1)[-1] == exe_name)
            exe.write_bytes(z.read(member))
    else:
        with tarfile.open(archive) as t:
            member = next(m for m in t.getmembers() if m.isfile() and m.name.rsplit("/", 1)[-1] == exe_name)
            exe.write_bytes(t.extractfile(member).read())
    exe.chmod(0o755)

    cache_dir = runner_temp() / "ccache"
    cache_dir.mkdir(parents=True, exist_ok=True)
    settings = {
        "CCACHE_DIR": str(cache_dir),
        "CCACHE_MAXSIZE": args.max_size,
        # The runner image's compilers keep their contents but not always their timestamps.
        "CCACHE_COMPILERCHECK": "content",
        # An object compiled without warnings is the same object with warnings as errors.
        "CCACHE_IGNOREOPTIONS": "/WX -Werror",
    }
    if args.read_only:
        settings["CCACHE_READONLY"] = "1"
    restored = sum(1 for p in cache_dir.rglob("*") if p.is_file())
    env = dict(os.environ, **settings)
    # The restored cache carries the counters of the run that saved it: start this job's from zero.
    subprocess.run([str(exe), "--zero-stats"], env=env, check=True, stdout=subprocess.DEVNULL)
    print(f"ccache {CCACHE_VERSION} at {exe}; {restored} files restored into {cache_dir}")

    launcher = exe.as_posix()
    append("GITHUB_ENV", [f"{k}={v}" for k, v in settings.items()] +
           [f"CCACHE_EXE={launcher}", f"CCACHE_JOB_START={int(time.time())}"])


def ccache_stats(exe):
    out = subprocess.run([exe, "--print-stats"], capture_output=True, text=True, check=True).stdout
    stats = {}
    for line in out.splitlines():
        key, _, value = line.partition("\t")
        if value.strip().isdigit():
            stats[key] = int(value)
    return stats


def ccache_finish(args):
    exe = os.environ.get("CCACHE_EXE")
    if not exe:
        print("ccache was not set up in this job")
        return
    subprocess.run([exe, "--show-stats", "--verbose"], check=False)
    stats = ccache_stats(exe)
    hits = stats.get("direct_cache_hit", 0) + stats.get("preprocessed_cache_hit", 0)
    misses = stats.get("cache_miss", 0)
    rate = 100.0 * hits / (hits + misses) if hits + misses else 0.0
    line = f"Compiler cache: {hits} hits, {misses} misses ({rate:.0f} % hit)"
    append("GITHUB_OUTPUT", [f"compiled={hits + misses}"])
    # A job that compiled nothing (a failed configure) used nothing: trimming it would empty the cache.
    if args.trim and hits + misses:
        start = int(os.environ.get("CCACHE_JOB_START", "0"))
        if start:
            age = max(60, int(time.time()) - start)
            subprocess.run([exe, f"--evict-older-than={age}s"], check=True)
        stats = ccache_stats(exe)
    size_mb = stats.get("cache_size_kibibyte", 0) / 1024
    line += f"; {stats.get('files_in_cache', 0)} files, {size_mb:.0f} MB{' after trimming' if args.trim and hits + misses else ''}"
    print(line)
    summary(line)


def vcpkg_key(args):
    cache = pathlib.Path(args.cache)
    archives = sorted(cache.rglob("*.zip")) if cache.is_dir() else []
    status = pathlib.Path(args.build_dir) / "vcpkg_installed" / "vcpkg" / "status"
    configured = (pathlib.Path(args.build_dir) / "build.ninja").is_file()
    if configured and status.is_file():
        # vcpkg's status database names the ABI of every installed package; the files provider stores each package as
        # <abi[:2]>/<abi>.zip. A configure that stopped inside vcpkg leaves everything in place.
        installed = set()
        for paragraph in status.read_text(encoding="utf-8").split("\n\n"):
            fields = dict(line.split(": ", 1) for line in paragraph.splitlines() if ": " in line)
            if fields.get("Status", "").endswith(" installed") and fields.get("Abi"):
                installed.add(fields["Abi"].strip())
        unused = [a for a in archives if a.stem not in installed]
        for a in unused:
            a.unlink()
            if not any(a.parent.iterdir()):
                a.parent.rmdir()
        archives = [a for a in archives if a.stem in installed]
        print(f"vcpkg cache: {len(archives)} packages this build installed, {len(unused)} unused removed")
    else:
        print(f"vcpkg cache: {len(archives)} packages (configure did not finish: nothing removed)")
    if not archives:
        append("GITHUB_OUTPUT", ["key="])
        return
    digest = hashlib.sha256("\n".join(a.stem for a in archives).encode()).hexdigest()[:16]
    key = f"{args.prefix}{digest}"
    print(f"key {key}")
    append("GITHUB_OUTPUT", [f"key={key}"])


def gh_api(*args):
    out = subprocess.run(["gh", "api", *args], capture_output=True, text=True, check=True).stdout
    return json.loads(out) if out.strip() else None


def delete_older(args):
    repo = os.environ["GITHUB_REPOSITORY"]
    caches = []
    page = 1
    while True:
        result = gh_api("-X", "GET", f"repos/{repo}/actions/caches", "-f", f"key={args.prefix}", "-f",
                        f"ref={args.ref}", "-f", "per_page=100", "-f", f"page={page}")
        batch = result.get("actions_caches", [])
        caches += [c for c in batch if c["key"].startswith(args.prefix)]
        if len(batch) < 100:
            break
        page += 1
    if not any(c["key"] == args.keep for c in caches):
        print(f"{args.keep} is not saved: keeping the other {args.prefix}* caches of {args.ref}")
        return
    for c in caches:
        if c["key"] != args.keep:
            print(f"deleting {c['key']} ({c['size_in_bytes'] / 1e6:.0f} MB, {c['created_at']})")
            gh_api("-X", "DELETE", f"repos/{repo}/actions/caches/{c['id']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("ccache-setup")
    p.add_argument("--max-size", required=True)
    p.add_argument("--read-only", action="store_true")
    p.set_defaults(run=ccache_setup)
    p = sub.add_parser("ccache-finish")
    p.add_argument("--trim", action="store_true")
    p.set_defaults(run=ccache_finish)
    p = sub.add_parser("vcpkg-key")
    p.add_argument("--build-dir", required=True)
    p.add_argument("--cache", required=True)
    p.add_argument("--prefix", required=True)
    p.set_defaults(run=vcpkg_key)
    p = sub.add_parser("delete-older")
    p.add_argument("--ref", default="refs/heads/main")
    p.add_argument("--prefix", required=True)
    p.add_argument("--keep", required=True)
    p.set_defaults(run=delete_older)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
