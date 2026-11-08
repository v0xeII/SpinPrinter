#!/usr/bin/env python3
"""One version for the whole printer: the firmware, SpinPi and the flasher.

    python3 dev/version.py              # print the version; exit 1 if the places disagree
    python3 dev/version.py 0.8.2        # set it everywhere (then commit, then tag v0.8.2)
    python3 dev/version.py --tag v0.8.2 # CI on a tag: every place must say 0.8.2

A release is a tag vX.Y.Z on a commit of main whose files all say X.Y.Z.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_RE = r"\d+\.\d+\.\d+"

# file -> pattern; group 1 is the version. Every match in the file is checked and replaced.
PLACES = {
    "SpinScreen_v0.7/SpinScreen/config.h": rf'#define FW_VERSION "({VERSION_RE})"',
    "SpinPi/spinpi.py": rf'^VERSION = "({VERSION_RE})"',
    "flasher/manifest.json": rf'"version": "({VERSION_RE})"',
    "SpinScreen_v0.7/README.md": rf"(?:PONG|READY) SpinScreen ({VERSION_RE})",
    "SpinScreen_v0.7/README.en.md": rf"(?:PONG|READY) SpinScreen ({VERSION_RE})",
}


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", newline="") as f:
        return f.read()


def found():
    """{file: [versions found]}"""
    out = {}
    for rel, pat in PLACES.items():
        out[rel] = re.findall(pat, read(rel), flags=re.M)
    return out


def check(want=None):
    seen = found()
    versions = {v for vs in seen.values() for v in vs}
    missing = [rel for rel, vs in seen.items() if not vs]
    ok = not missing and len(versions) == 1 and (want is None or versions == {want})
    if not ok:
        for rel, vs in seen.items():
            print(f"  {rel}: {', '.join(vs) or 'NO VERSION FOUND'}")
        if want:
            print(f"expected {want} everywhere")
        else:
            print("the versions disagree: python3 dev/version.py X.Y.Z")
        return 1
    print(versions.pop())
    return 0


def set_version(new):
    for rel, pat in PLACES.items():
        text = read(rel)
        out, n = re.subn(pat, lambda m: m.group(0).replace(m.group(1), new), text, flags=re.M)
        if not n:
            sys.exit(f"{rel}: no version found")
        with open(os.path.join(ROOT, rel), "w", encoding="utf-8", newline="") as f:
            f.write(out)
    json.loads(read("flasher/manifest.json"))
    return check(new)


def main(argv):
    if not argv:
        return check()
    if argv[0] == "--tag" and len(argv) == 2:
        tag = argv[1].rsplit("/", 1)[-1]
        if not re.fullmatch(rf"v{VERSION_RE}", tag):
            print(f"{tag}: a release tag is vX.Y.Z")
            return 1
        return check(tag[1:])
    if len(argv) == 1 and re.fullmatch(VERSION_RE, argv[0]):
        return set_version(argv[0])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
