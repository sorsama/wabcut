#!/usr/bin/env python3
"""Wabcut's update signing: the key pair and the signed version manifest (docs/release.md, "Updates").

Wabcut checks a release's wabcut-update.json against a public key built into the app (CMake cache variable
WABCUT_UPDATE_PUBLIC_KEY, the base64 of a P-256 SubjectPublicKeyInfo). This script makes the key pair, prints the public
half for CMake, and writes and signs the manifest in the release workflow. It needs the `cryptography` package
(pip install cryptography).

  update-keys.py generate --out <private.pem>     a new P-256 key pair: the private key to <private.pem> (never into a
                                                  repository), the public key printed for WABCUT_UPDATE_PUBLIC_KEY
  update-keys.py public (--key <pem> | --key-env NAME)
                                                  the public key of a private key, for CMake
  update-keys.py manifest --version 0.5.0 --msi <file> --url <msi url> --page <release page> \\
                 [--notes-file <md>] [--released YYYY-MM-DD] [--authenticode] [--publisher NAME] \\
                 (--key <pem> | --key-env NAME) --out wabcut-update.json
                                                  the signed manifest of a release (size and SHA-256 of the MSI)
  update-keys.py verify --manifest wabcut-update.json --public-key <base64>
                                                  checks a manifest the way Wabcut does (exit 1 when it fails)

The private key is never printed or logged. In the release workflow it comes from the repository secret
WABCUT_UPDATE_SIGNING_KEY (--key-env), so it is never written to the runner's disk.
"""

import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import sys

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature
except ImportError:  # pragma: no cover - a clear message instead of a traceback
    sys.exit("update-keys.py needs the 'cryptography' package: pip install cryptography")

VERSION_RE = re.compile(r"^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$")
NOTES_LIMIT = 16000


def public_key_b64(public_key):
    der = public_key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return base64.b64encode(der).decode("ascii"), hashlib.sha256(der).hexdigest()[:16]


def load_private_key(args):
    if args.key_env:
        text = os.environ.get(args.key_env, "")
        if not text.strip():
            sys.exit(f"the environment variable {args.key_env} holds no key")
        data = text.encode("utf-8")
    elif args.key:
        with open(args.key, "rb") as f:
            data = f.read()
    else:
        sys.exit("give --key <pem> or --key-env NAME")
    key = serialization.load_pem_private_key(data, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit("the key is not a P-256 (secp256r1) private key")
    return key


def cmd_generate(args):
    if os.path.exists(args.out):
        sys.exit(f"{args.out} exists: keep the old key or delete it first (a new key needs a new app build)")
    folder = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(folder, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    # Created for this user only where the platform supports modes; Windows keeps the profile's own permissions.
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    b64, key_id = public_key_b64(key.public_key())
    print(f"Private key written to {args.out} (keep it secret; store it as the WABCUT_UPDATE_SIGNING_KEY secret).")
    print(f"Key id: {key_id}")
    print("Public key (WABCUT_UPDATE_PUBLIC_KEY):")
    print(b64)


def cmd_public(args):
    b64, key_id = public_key_b64(load_private_key(args).public_key())
    print(b64)
    print(f"key id {key_id}", file=sys.stderr)


def sha256_of(path):
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def cmd_manifest(args):
    if not VERSION_RE.match(args.version):
        sys.exit(f"the version {args.version!r} is not x.y.z")
    for url in (args.url, args.page):
        if not url.startswith("https://"):
            sys.exit(f"{url} is not an https:// address")
    key = load_private_key(args)
    size, digest = sha256_of(args.msi)
    notes = ""
    if args.notes_file:
        with open(args.notes_file, encoding="utf-8") as f:
            notes = f.read().strip()
        if len(notes) > NOTES_LIMIT:
            notes = notes[:NOTES_LIMIT].rsplit("\n", 1)[0] + "\n\n…"
    payload = {
        "product": "wabcut",
        "version": args.version,
        "released": args.released or datetime.date.today().isoformat(),
        "notes": notes,
        "page": args.page,
        "windows": {
            "url": args.url,
            "size": size,
            "sha256": digest,
            "authenticode": bool(args.authenticode),
            "publisher": args.publisher or "",
        },
    }
    payload_bytes = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    r, s = decode_dss_signature(key.sign(payload_bytes, ec.ECDSA(hashes.SHA256())))
    signature = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    _, key_id = public_key_b64(key.public_key())
    manifest = {
        "format": "wabcut-update",
        "version": 1,
        "key": key_id,
        "payload": base64.b64encode(payload_bytes).decode("ascii"),
        "signature": base64.b64encode(signature).decode("ascii"),
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"{args.out}: Wabcut {args.version}, {os.path.basename(args.msi)} ({size} bytes, sha256 {digest}), "
          f"authenticode {'yes' if args.authenticode else 'no'}, key {key_id}")


def cmd_verify(args):
    with open(args.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    if manifest.get("format") != "wabcut-update" or manifest.get("version") != 1:
        sys.exit("not a version 1 Wabcut update manifest")
    payload = base64.b64decode(manifest["payload"], validate=True)
    signature = base64.b64decode(manifest["signature"], validate=True)
    if len(signature) != 64:
        sys.exit("the signature is not 64 bytes (r || s)")
    der_signature = encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big"))
    ok = False
    for b64 in args.public_key.split(";"):
        public_key = serialization.load_der_public_key(base64.b64decode(b64.strip(), validate=True))
        try:
            public_key.verify(der_signature, payload, ec.ECDSA(hashes.SHA256()))
            ok = True
            break
        except InvalidSignature:
            continue
    if not ok:
        sys.exit("the signature does not verify with the given public key")
    release = json.loads(payload)
    print(f"verified: Wabcut {release['version']}, {release['windows']['url']} ({release['windows']['size']} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("generate", help="make a new key pair")
    p.add_argument("--out", required=True, help="where the private key (PEM) goes; never inside a repository")
    p.set_defaults(func=cmd_generate)

    for name, func in (("public", cmd_public), ("manifest", cmd_manifest)):
        p = sub.add_parser(name)
        p.add_argument("--key", help="the private key (PEM file)")
        p.add_argument("--key-env", help="an environment variable that holds the private key (PEM text)")
        p.set_defaults(func=func)
        if name == "manifest":
            p.add_argument("--version", required=True)
            p.add_argument("--msi", required=True, help="the release's MSI")
            p.add_argument("--url", required=True, help="where the MSI is downloaded from")
            p.add_argument("--page", required=True, help="the release page")
            p.add_argument("--notes-file", help="what is new (Markdown)")
            p.add_argument("--released", help="the release date, YYYY-MM-DD (default: today)")
            p.add_argument("--authenticode", action="store_true", help="the MSI is Authenticode-signed")
            p.add_argument("--publisher", help="the name the MSI is signed with")
            p.add_argument("--out", required=True)

    p = sub.add_parser("verify")
    p.add_argument("--manifest", required=True)
    p.add_argument("--public-key", required=True, help="base64 SubjectPublicKeyInfo (several: separated by ';')")
    p.set_defaults(func=cmd_verify)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
