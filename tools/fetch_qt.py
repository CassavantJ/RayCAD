#!/usr/bin/env python3
"""Download prebuilt open-source Qt from download.qt.io and unpack it.

Used by CI instead of aqtinstall / install-qt-action, which cannot find Qt 6.12 for Windows
(the repository layout changed: Windows packages live under qt6_<ver>/qt6_<ver>_<arch>/, Linux and
macOS packages under qt6_<ver>/qt6_<ver>/). Standard library only; needs bsdtar (libarchive) or a `7z` executable on PATH.

Every archive is verified against the .sha1 file published next to it.

Usage:  fetch_qt.py --out <dir> [--version 6.12.0] [--os windows|linux|mac] [--module qtshadertools ...]
Prints the Qt prefix on the last line, and appends QT_ROOT_DIR=<prefix> to $GITHUB_ENV when that is set.
"""
import argparse
import hashlib
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET

BASE = "https://download.qt.io/online/qtsdkrepository/"
TARGETS = {
    # os: (repository folder, sub-folder of qt6_<ver>, package arch suffix, install folder name)
    "windows": ("windows_x86", "qt6_{v}_msvc2022_64", "win64_msvc2022_64", "msvc2022_64"),
    "linux": ("linux_x64", "qt6_{v}", "linux_gcc_64", "gcc_64"),
    "mac": ("mac_x64", "qt6_{v}", "clang_64", "macos"),
}


def get(url: str) -> bytes:
    last = None
    for _ in range(4):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                return r.read()
        except Exception as e:  # network hiccup: retry
            last = e
    raise SystemExit(f"download failed: {url}: {last}")


def sha1(path: pathlib.Path) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_extractors() -> list[list[str]]:
    """Command prefixes that can unpack a .7z, best first. `{src}` and `{dst}` are filled in per archive.

    bsdtar (libarchive) comes first: the Linux Qt archives hold chains of symlinks (libX.so -> libX.so.6 ->
    libX.so.6.12.0) that current 7-Zip refuses as "dangerous links" and then exits with an error.
    """
    found: list[list[str]] = []
    for name in ("bsdtar", "tar"):
        exe = shutil.which(name)
        if not exe:
            continue
        try:
            banner = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=30).stdout
        except Exception:
            continue
        if "bsdtar" in banner or "libarchive" in banner:
            found.append([exe, "-xf", "{src}", "-C", "{dst}"])
            break
    sevenz = next((p for p in (shutil.which(n) for n in ("7z", "7zz", "7za", "7zr")) if p), None)
    if sevenz:
        for extra in (["-snld20"], ["-snld"], []):
            found.append([sevenz, "x", "-y", "-bso0", "-bsp0", *extra, "-o{dst}", "{src}"])
    return found


def extract(extractors: list[list[str]], src: pathlib.Path, dst: pathlib.Path) -> None:
    errors = []
    for cmd in extractors:
        argv = [a.format(src=str(src), dst=str(dst)) for a in cmd]
        r = subprocess.run(argv, capture_output=True, text=True)
        if r.returncode == 0:
            return
        errors.append(f"{' '.join(argv[:3])}: exit {r.returncode}: {(r.stderr or r.stdout).strip()[-300:]}")
    raise SystemExit("could not extract " + src.name + "\n  " + "\n  ".join(errors))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default="6.12.0")
    ap.add_argument("--os", choices=sorted(TARGETS), default=None)
    ap.add_argument("--module", action="append", default=[])
    a = ap.parse_args()

    osname = a.os or {"Windows": "windows", "Linux": "linux", "Darwin": "mac"}[platform.system()]
    repo, sub, arch, folder = TARGETS[osname]
    v = a.version.replace(".", "")
    base = f"{BASE}{repo}/desktop/qt6_{v}/{sub.format(v=v)}/"
    prefix = pathlib.Path(a.out).resolve() / a.version / folder
    marker = prefix / ".fetch_qt_complete"
    wanted = {f"qt.qt6.{v}.{arch}"} | {f"qt.qt6.{v}.addons.{m}.{arch}" for m in a.module}

    if not marker.exists():
        extractors = find_extractors()
        if not extractors:
            raise SystemExit("no bsdtar or 7z executable found")
        if prefix.exists():
            shutil.rmtree(prefix)
        prefix.mkdir(parents=True)
        dl = prefix.parent / "_downloads"
        dl.mkdir(exist_ok=True)
        root = ET.fromstring(get(base + "Updates.xml"))
        found = set()
        for pkg in root.iter("PackageUpdate"):
            name = pkg.findtext("Name")
            if name not in wanted:
                continue
            found.add(name)
            ver = pkg.findtext("Version")
            archives = [x.strip() for x in (pkg.findtext("DownloadableArchives") or "").split(",") if x.strip()]
            for arc in archives:
                if arc.startswith(("qtdoc-", "qttranslations-")):
                    continue  # not needed to build or test
                url = f"{base}{name}/{ver}{arc}"
                want = get(url + ".sha1").decode().split()[0]
                dst = dl / arc
                print("download", arc, flush=True)
                dst.write_bytes(get(url))
                if sha1(dst) != want:
                    raise SystemExit(f"SHA-1 mismatch: {arc}")
                # The ICU archive is flat (libicu*.so at its root); Qt looks for it next to its own libraries.
                target = prefix / "lib" if arc.startswith("icu-") else prefix
                target.mkdir(parents=True, exist_ok=True)
                extract(extractors, dst, target)
                dst.unlink()
        missing = wanted - found
        if missing:
            raise SystemExit(f"packages not found in {base}Updates.xml: {sorted(missing)}")
        shutil.rmtree(dl, ignore_errors=True)
        # Older archives carry their own "<version>/<arch>/" prefix; newer ones are flat. Accept both.
        if not (prefix / "lib").exists():
            inner = next((p.parent for p in prefix.rglob("lib/cmake/Qt6/Qt6Config.cmake")), None)
            if inner is None:
                raise SystemExit("Qt6Config.cmake not found after extraction")
            inner = inner.parent.parent.parent
            tmp = prefix.parent / "_moved"
            inner.rename(tmp)
            shutil.rmtree(prefix)
            tmp.rename(prefix)
        if not (prefix / "lib" / "cmake" / "Qt6" / "Qt6Config.cmake").exists():
            raise SystemExit(f"Qt6Config.cmake missing under {prefix}")
        marker.write_text(a.version + "\n")

    env = os.environ.get("GITHUB_ENV")
    if env:
        with open(env, "a", encoding="utf-8") as f:
            f.write(f"QT_ROOT_DIR={prefix}\n")
    print(prefix)
    return 0


if __name__ == "__main__":
    sys.exit(main())
