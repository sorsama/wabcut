"""Wabcut's icon set: Boxicons (https://boxicons.com, @boxicons/core, MIT) under Wabcut's logical icon names.

QML names icons by a logical name (Icon { name: "scissors" }, IconButton.iconName); the file behind each one is
src/app/resources/icons/<name>.svg. tools/icons/boxicons-map.json says which Boxicons glyph each name is ("bx-cut",
or "filled/bx-play" from the filled pack); the names under "own" are Wabcut's drawings in the same folder.

    python -I tools/icons/import_boxicons.py fetch                 download @boxicons/core (npm, integrity-checked)
    python -I tools/icons/import_boxicons.py sync                  (re)write every mapped icon from the package
    python -I tools/icons/import_boxicons.py add <bx-name> <name> [--filled]
                                                                   map a new logical name, write its file and list
                                                                   it in src/app/CMakeLists.txt (the module's icons)
    python -I tools/icons/import_boxicons.py check                 every file matches the package, every file is
                                                                   mapped or own, every file is in CMakeLists.txt

The package is read from --source <dir with basic/ filled/>, else $WABCUT_BOXICONS_DIR, else the copy `fetch` keeps
in build/boxicons/<version>/package/svg. Written files are normalised for Icon.qml: a 24x24 viewBox, the glyph's
paths filled black (Icon.qml tints them through a colorize pass), a provenance comment, LF line endings.
"""
import argparse
import base64
import hashlib
import io
import json
import os
import re
import sys
import tarfile
import urllib.request

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MAP_PATH = os.path.join(REPO, "tools", "icons", "boxicons-map.json")
ICON_DIR = os.path.join(REPO, "src", "app", "resources", "icons")
CMAKE_PATH = os.path.join(REPO, "src", "app", "CMakeLists.txt")
LOGICAL_NAME = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
# An icon in a RESOURCES list of src/app/CMakeLists.txt (a block's last entry may close it: "...svg)").
ICON_LINE = re.compile(r"resources/icons/([a-z0-9-]+)\.svg\)?\s*$")


def load_map():
    with open(MAP_PATH, encoding="utf-8") as f:
        return json.load(f)


def save_map(data):
    data["icons"] = dict(sorted(data["icons"].items()))
    data["own"] = sorted(set(data.get("own", [])))
    with open(MAP_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")


def cache_dir(data):
    return os.path.join(REPO, "build", "boxicons", data["version"])


def source_dir(args, data):
    for candidate in (getattr(args, "source", None), os.environ.get("WABCUT_BOXICONS_DIR"),
                      os.path.join(cache_dir(data), "package", "svg")):
        if candidate and os.path.isdir(os.path.join(candidate, "basic")):
            return candidate
    sys.exit("import_boxicons: no Boxicons package found; run `fetch`, or pass --source <.../package/svg>")


def fetch(args, data):
    """Downloads the pinned @boxicons/core tarball and checks it against the registry's sha512 integrity."""
    meta_url = "https://registry.npmjs.org/" + data["package"].replace("/", "%2f")
    with urllib.request.urlopen(meta_url, timeout=60) as r:
        meta = json.load(r)
    dist = meta["versions"][data["version"]]["dist"]
    with urllib.request.urlopen(dist["tarball"], timeout=120) as r:
        blob = r.read()
    algo, _, expected = dist["integrity"].partition("-")
    if algo != "sha512" or base64.b64decode(expected) != hashlib.sha512(blob).digest():
        sys.exit("import_boxicons: the downloaded tarball does not match the registry's integrity")
    target = cache_dir(data)
    os.makedirs(target, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as tar:
        members = [m for m in tar.getmembers()
                   if m.isfile() and (m.name.startswith("package/svg/") or m.name == "package/LICENSE")]
        tar.extractall(target, members=members, filter="data")
    print(f"fetched {data['package']} {data['version']} into {target}")


def glyph_path(src, glyph):
    pack, _, name = glyph.rpartition("/")
    path = os.path.join(src, pack or "basic", name + ".svg")
    if not os.path.isfile(path):
        sys.exit(f"import_boxicons: {glyph} is not in the package ({path})")
    return path


def normalised(src, glyph):
    """The glyph as Wabcut stores it: viewBox 0 0 24 24, filled black, nothing else on the root."""
    with open(glyph_path(src, glyph), encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"<svg\b[^>]*>(.*)</svg>", text, re.S)
    if not m:
        sys.exit(f"import_boxicons: {glyph} is not an SVG")
    root = text[:m.start(1)]
    for attr, value in (("width", "24"), ("height", "24")):
        found = re.search(rf'\b{attr}="([^"]*)"', root)
        if found and found.group(1) != value:
            sys.exit(f"import_boxicons: {glyph} is not a 24x24 glyph ({attr}={found.group(1)})")
    body = m.group(1).strip()
    if re.search(r'\b(stroke|opacity|fill-opacity|style)=', body):
        print(f"warning: {glyph} carries stroke/opacity/style attributes; check how Icon.qml tints it", file=sys.stderr)
    pack, _, name = glyph.rpartition("/")
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="#000">'
            f"<!-- Boxicons {name} ({pack or 'basic'}), MIT -->{body}</svg>\n")


def write_icon(src, logical, glyph):
    with open(os.path.join(ICON_DIR, logical + ".svg"), "w", encoding="utf-8", newline="\n") as f:
        f.write(normalised(src, glyph))


def cmake_lines():
    with open(CMAKE_PATH, "rb") as f:
        raw = f.read().decode("utf-8")
    eol = "\r\n" if "\r\n" in raw else "\n"
    return raw.split(eol), eol


def listed_in_cmake():
    lines, _ = cmake_lines()
    return {m.group(1) for line in lines for m in [ICON_LINE.search(line)] if m}


def list_in_cmake(logical):
    """Lists resources/icons/<logical>.svg at the end of the module's main icon list, the first run of icon lines
    (no-op when any RESOURCES list already has it)."""
    lines, eol = cmake_lines()
    entry = f"resources/icons/{logical}.svg"
    if logical in listed_in_cmake():
        return False
    first = next((i for i, line in enumerate(lines) if ICON_LINE.search(line)), -1)
    if first < 0:
        sys.exit("import_boxicons: no icon list found in src/app/CMakeLists.txt")
    last = first
    while last + 1 < len(lines) and (ICON_LINE.search(lines[last + 1]) or lines[last + 1].strip().startswith("#")):
        last += 1
    while lines[last].strip().startswith("#"):
        last -= 1
    if lines[last].rstrip().endswith(")"):
        sys.exit("import_boxicons: the main icon list closes on its last icon; add the entry by hand")
    indent = re.match(r"\s*", lines[last]).group(0)
    lines.insert(last + 1, indent + entry)
    with open(CMAKE_PATH, "wb") as f:
        f.write(eol.join(lines).encode("utf-8"))
    return True


def sync(args, data):
    src = source_dir(args, data)
    for logical, glyph in data["icons"].items():
        write_icon(src, logical, glyph)
    print(f"wrote {len(data['icons'])} icons from {src}")


def add(args, data):
    logical = args.name
    if not LOGICAL_NAME.match(logical):
        sys.exit("import_boxicons: a logical name is lower-case words joined by '-' (e.g. 'folder-plus')")
    glyph = args.glyph if args.glyph.startswith("bx-") else "bx-" + args.glyph
    if args.filled:
        glyph = "filled/" + glyph
    if logical in data.get("own", []):
        sys.exit(f"import_boxicons: '{logical}' is one of Wabcut's own drawings; pick another name")
    previous = data["icons"].get(logical)
    if previous and previous != glyph and not args.replace:
        sys.exit(f"import_boxicons: '{logical}' is already {previous} (pass --replace to change it everywhere)")
    src = source_dir(args, data)
    write_icon(src, logical, glyph)
    data["icons"][logical] = glyph
    save_map(data)
    listed = list_in_cmake(logical)
    print(f"{logical} = {glyph}" + (" (listed in src/app/CMakeLists.txt)" if listed else ""))


def check(args, data):
    src = source_dir(args, data)
    problems = []
    files = {f[:-4] for f in os.listdir(ICON_DIR) if f.endswith(".svg")}
    own = set(data.get("own", []))
    for logical, glyph in data["icons"].items():
        path = os.path.join(ICON_DIR, logical + ".svg")
        if not os.path.isfile(path):
            problems.append(f"{logical}: missing file")
            continue
        with open(path, encoding="utf-8") as f:
            if f.read() != normalised(src, glyph):
                problems.append(f"{logical}: differs from {glyph} (run sync)")
    for name in sorted(files - set(data["icons"]) - own):
        problems.append(f"{name}.svg: neither mapped nor own")
    for name in sorted(own - files):
        problems.append(f"{name}: listed as own but missing")
    for name in sorted(files - listed_in_cmake()):
        problems.append(f"{name}.svg: not listed in src/app/CMakeLists.txt")
    for p in problems:
        print(p)
    print("ok" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", help="the package's svg directory (holding basic/, filled/)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("fetch")
    sub.add_parser("sync")
    sub.add_parser("check")
    a = sub.add_parser("add")
    a.add_argument("glyph", help="Boxicons name, e.g. bx-cut")
    a.add_argument("name", help="Wabcut's logical name, e.g. scissors")
    a.add_argument("--filled", action="store_true", help="from the filled pack")
    a.add_argument("--replace", action="store_true", help="re-map an existing logical name")
    args = parser.parse_args()
    data = load_map()
    if args.command == "fetch":
        fetch(args, data)
    elif args.command == "sync":
        sync(args, data)
    elif args.command == "add":
        add(args, data)
    elif args.command == "check":
        sys.exit(check(args, data))


if __name__ == "__main__":
    main()
