#!/usr/bin/env python3
"""Checks, or refreshes, the vendored PlaneGCS sources against FreeCAD upstream (see README.md).

  sync_upstream.py            compare src/ and LICENSE with the git blob ids of the pinned upstream commit
                              (recorded below; no network needed)
  sync_upstream.py --fetch DIR
                              sparse-clone FreeCAD at TAG into the empty folder DIR, check that the tag resolves to
                              COMMIT, and print which vendored files differ from it. Nothing in this folder is changed.
  sync_upstream.py --fetch DIR --copy
                              the same, then copy the upstream files over src/ and LICENSE. To move to a new tag,
                              change TAG, COMMIT and BLOBS below in the same change, rebuild, and rerun the sketch tests.

Standard library only. Exit code 1 when a file differs.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

TAG = "1.1.4"
COMMIT = "4fd3bf320d9566a27e60069fc8387448aaa3a094"
UPSTREAM_DIR = "src/Mod/Sketcher/App/planegcs"
# git blob ids at COMMIT (git ls-tree -r COMMIT), byte for byte as vendored.
BLOBS = {
    "LICENSE": "bc0390e98c98f2ad3ab7decdb82b4d57d7f850d4",
    "src/Constraints.cpp": "ed5640619fa363e83c699fa31ae7abdbb7ed6af8",
    "src/Constraints.h": "d696daedebd72a71a85e9c23334509dac162ce3f",
    "src/GCS.cpp": "533a8c66029504f59206018861494dcb9d8761c3",
    "src/GCS.h": "26b6f912e62e1b9b12ee75e9bd9c694d8764fb15",
    "src/Geo.cpp": "2ff8399ea769416ca7b08a784458744ee0892fce",
    "src/Geo.h": "567dac64cb3906484c7fb5b992653b01562db882",
    "src/SubSystem.cpp": "1ccafbb448847e6517b842b59440404f0b41e79e",
    "src/SubSystem.h": "6c4062d2426f48c4f2eb2c32b184300eb439435c",
    "src/Util.h": "87b0892775c77b8c504d85978cc3f406632d02dd",
    "src/qp_eq.cpp": "ac7fce9a0df359366a268d27c23efab5fc187e8a",
    "src/qp_eq.h": "965d3bc65a207309d88713b2f521c05734b530d8",
}
HERE = os.path.dirname(os.path.abspath(__file__))


def blob_id(path):
    with open(path, "rb") as handle:
        data = handle.read()
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def upstream_path(root, name):
    if name == "LICENSE":
        return os.path.join(root, "LICENSE")
    return os.path.join(root, UPSTREAM_DIR, os.path.basename(name))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fetch", metavar="DIR")
    parser.add_argument("--copy", action="store_true")
    args = parser.parse_args()

    differ = []
    if args.fetch:
        root = os.path.abspath(args.fetch)
        if os.path.exists(root) and os.listdir(root):
            print("sync_upstream: %s is not empty" % root, file=sys.stderr)
            return 2
        url = "https://github.com/FreeCAD/FreeCAD.git"
        subprocess.check_call(["git", "clone", "--filter=blob:none", "--sparse", "--depth", "1", "--branch", TAG,
                               url, root])
        subprocess.check_call(["git", "-C", root, "sparse-checkout", "set", "--no-cone", "/" + UPSTREAM_DIR + "/",
                               "/LICENSE"])
        head = subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"], text=True).strip()
        if head != COMMIT:
            print("sync_upstream: tag %s is %s, expected %s" % (TAG, head, COMMIT), file=sys.stderr)
            return 1
        for name in sorted(BLOBS):
            src = upstream_path(root, name)
            dst = os.path.join(HERE, name)
            if not os.path.isfile(dst) or blob_id(src) != blob_id(dst):
                differ.append(name)
                if args.copy:
                    shutil.copyfile(src, dst)
    else:
        for name, expected in sorted(BLOBS.items()):
            path = os.path.join(HERE, name)
            if not os.path.isfile(path) or blob_id(path) != expected:
                differ.append(name)
    for name in differ:
        print("differs from FreeCAD %s: %s" % (TAG, name))
    if differ and not args.copy:
        return 1
    print("sync_upstream: %d files checked against FreeCAD %s (%s)" % (len(BLOBS), TAG, COMMIT[:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
