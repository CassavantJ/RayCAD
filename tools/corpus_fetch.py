#!/usr/bin/env python3
"""Downloads the fetch-only part of the test corpus into tests/corpus/cache/ (PLAN.md section 11).

Fetch-only entries are the manifest rows whose path starts with cache/: files we may use for testing but do not
commit, because of their size or their licence. Each file is verified against the SHA-256 in the manifest.
A file already in the cache with the right hash is not downloaded again, and an interrupted download resumes
from where it stopped (HTTP Range). Files that come out of an archive (a zip) download the archive once into
cache/_archives/, verify it, and take the member out of it without unpacking anything else.

Standard library only. Requests go one at a time, with a short pause between them, a generic User-Agent, and no
cookies or credentials.

Usage:
  corpus_fetch.py                     fetch everything missing
  corpus_fetch.py --source NAME ...   only these sources
  corpus_fetch.py --id ID ...         only these entries
  corpus_fetch.py --limit N           at most N entries (after the filters)
  corpus_fetch.py --dry-run           list what would be fetched
  corpus_fetch.py --ca-file PEM       verify TLS against this CA bundle instead of the system store
                                      (also taken from $SSL_CERT_FILE); for "certificate has expired" errors
  corpus_fetch.py --self-test         fetch from a local test server, resume, re-run, detect a bad hash
"""
import argparse
import hashlib
import http.server
import io
import json
import os
import shutil
import ssl
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zipfile

import ray_modules as rm

CORPUS_DIR = os.path.join(rm.REPO_ROOT, "tests", "corpus")
USER_AGENT = "RayCAD-corpus-fetch/1.0 (open-source CAD test corpus; Python urllib)"
CHUNK = 1 << 16
PAUSE_SECONDS = 0.3
RETRIES = 4
# TLS settings: None is Python's default (the system store). --ca-file or SSL_CERT_FILE replaces the store with one
# CA bundle, for machines whose store makes OpenSSL pick an expired root. Certificates are always verified.
TLS_CONTEXT = None


class FetchError(Exception):
    pass


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_good(path, size, sha256):
    return os.path.isfile(path) and os.path.getsize(path) == size and sha256_of(path) == sha256


def local_path(corpus_dir, relpath):
    parts = relpath.split("/")
    if not relpath or relpath.startswith("/") or ".." in parts or ":" in relpath:
        raise FetchError("unsafe path in manifest: %r" % relpath)
    return os.path.join(corpus_dir, *parts)


def download(url, target, size, sha256, pause):
    """Downloads url to target through target.part, resuming a partial file, and verifies size and hash."""
    part = target + ".part"
    os.makedirs(os.path.dirname(target), exist_ok=True)
    last_error = None
    for attempt in range(RETRIES):
        if attempt:
            time.sleep(pause * (2 ** attempt) + 1.0)
        have = os.path.getsize(part) if os.path.isfile(part) else 0
        if have > size:
            os.remove(part)
            have = 0
        if have == size and size > 0:
            break
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "*/*"})
        if have:
            request.add_header("Range", "bytes=%d-" % have)
        try:
            with urllib.request.urlopen(request, timeout=60, context=TLS_CONTEXT) as response:
                status = getattr(response, "status", 200)
                mode = "ab" if have and status == 206 else "wb"
                with open(part, mode) as handle:
                    shutil.copyfileobj(response, handle, CHUNK)
            break
        except urllib.error.HTTPError as error:
            last_error = error
            if error.code == 416:  # range not satisfiable: the partial file is wrong, start over
                os.remove(part)
            elif error.code in (401, 403, 404, 410):
                raise FetchError("%s: HTTP %d" % (url, error.code))
        except (urllib.error.URLError, OSError) as error:
            last_error = error
            if "CERTIFICATE_VERIFY_FAILED" in str(error):
                raise FetchError("%s: %s; if the site is fine in a browser, the local certificate store is the problem: "
                                 "pass --ca-file with a current CA bundle" % (url, error))
    else:
        raise FetchError("%s: %s" % (url, last_error))
    if not os.path.isfile(part) and size == 0:
        open(part, "wb").close()
    got = os.path.getsize(part)
    if got != size:
        raise FetchError("%s: got %d bytes, manifest says %d (partial file kept for resume)" % (url, got, size))
    if sha256_of(part) != sha256:
        os.remove(part)
        raise FetchError("%s: SHA-256 differs from the manifest; the file changed upstream" % url)
    os.replace(part, target)


def fetch(corpus_dir, manifest, entries, dry_run=False, pause=PAUSE_SECONDS, log=print):
    """Returns (present, fetched, failed, bytes_downloaded)."""
    archives = manifest.get("archives", {})
    archive_dir = os.path.join(corpus_dir, "cache", "_archives")
    present = fetched = downloaded = 0
    failed = []
    first_request = True
    for entry in entries:
        try:
            target = local_path(corpus_dir, entry["path"])
            if is_good(target, entry["size"], entry["sha256"]):
                present += 1
                continue
            if dry_run:
                log("would fetch %s (%d bytes)" % (entry["id"], entry["size"]))
                continue
            if "archive" in entry:
                archive = archives[entry["archive"]]
                name = entry["archive"] + os.path.splitext(archive["url"].split("?")[0])[1]
                archive_path = os.path.join(archive_dir, name)
                if not is_good(archive_path, archive["size"], archive["sha256"]):
                    if not first_request:
                        time.sleep(pause)
                    first_request = False
                    log("downloading archive %s (%d bytes)" % (entry["archive"], archive["size"]))
                    download(archive["url"], archive_path, archive["size"], archive["sha256"], pause)
                    downloaded += archive["size"]
                with zipfile.ZipFile(archive_path) as bundle:
                    data = bundle.read(entry["member"])
                if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                    raise FetchError("member %s of %s differs from the manifest" % (entry["member"], entry["archive"]))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target + ".part", "wb") as handle:
                    handle.write(data)
                os.replace(target + ".part", target)
            else:
                if not first_request:
                    time.sleep(pause)
                first_request = False
                download(entry["url"], target, entry["size"], entry["sha256"], pause)
                downloaded += entry["size"]
            fetched += 1
            log("fetched %s" % entry["id"])
        except (FetchError, KeyError, zipfile.BadZipFile, OSError) as error:
            failed.append("%s: %s" % (entry.get("id", "?"), error))
    return present, fetched, failed, downloaded


def select(manifest, sources=None, ids=None, limit=None):
    entries = [e for e in manifest["files"] if e["path"].startswith("cache/")]
    if sources:
        entries = [e for e in entries if e["source"] in sources]
    if ids:
        entries = [e for e in entries if e["id"] in ids]
    return entries[:limit] if limit else entries


# ---------- self-test ----------

class _RangeHandler(http.server.SimpleHTTPRequestHandler):
    """Serves a folder, honouring "Range: bytes=N-" and counting the requests it answers."""

    hits = []

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            self.send_error(404)
            return
        with open(path, "rb") as handle:
            data = handle.read()
        _RangeHandler.hits.append((self.path, self.headers.get("Range"), self.headers.get("User-Agent")))
        start = 0
        header = self.headers.get("Range")
        if header and header.startswith("bytes="):
            start = int(header[6:].split("-")[0])
            if start >= len(data):
                self.send_error(416)
                return
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(data) - 1, len(data)))
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(len(data) - start))
        self.end_headers()
        self.wfile.write(data[start:])


def self_test():
    failures = []
    expect = rm.expect
    with tempfile.TemporaryDirectory() as root:
        served = os.path.join(root, "served")
        corpus = os.path.join(root, "corpus")
        os.makedirs(served)
        payload = bytes(range(256)) * 400
        with open(os.path.join(served, "part.stp"), "wb") as handle:
            handle.write(payload)
        member = b"ISO-10303-21;\nmember\n"
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as bundle:
            bundle.writestr("set/inner.stp", member)
            bundle.writestr("set/other.stp", b"not wanted")
        with open(os.path.join(served, "set.zip"), "wb") as handle:
            handle.write(buffer.getvalue())

        handler = lambda *a, **k: _RangeHandler(*a, directory=served, **k)  # noqa: E731
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = "http://127.0.0.1:%d/" % server.server_address[1]
        try:
            sha = lambda data: hashlib.sha256(data).hexdigest()  # noqa: E731
            manifest = {
                "archives": {"set": {"url": base + "set.zip", "size": len(buffer.getvalue()),
                                     "sha256": sha(buffer.getvalue())}},
                "files": [
                    {"id": "t/part", "path": "cache/t/part.stp", "source": "t", "url": base + "part.stp",
                     "size": len(payload), "sha256": sha(payload)},
                    {"id": "t/inner", "path": "cache/t/inner.stp", "source": "t", "url": base + "set.zip",
                     "archive": "set", "member": "set/inner.stp", "size": len(member), "sha256": sha(member)},
                    {"id": "t/committed", "path": "files/t/c.stp", "source": "t", "url": base + "none",
                     "size": 1, "sha256": "0" * 64},
                ],
            }
            quiet = lambda *_: None  # noqa: E731
            entries = select(manifest)
            expect(failures, [e["id"] for e in entries] == ["t/part", "t/inner"], "select: %r" % entries)

            # A partial download from an earlier, interrupted run is resumed with a Range request.
            os.makedirs(os.path.join(corpus, "cache", "t"))
            with open(os.path.join(corpus, "cache", "t", "part.stp.part"), "wb") as handle:
                handle.write(payload[:1000])
            present, fetched, failed, _ = fetch(corpus, manifest, entries, pause=0, log=quiet)
            expect(failures, (present, fetched, failed) == (0, 2, []), "first fetch: %r" % ((present, fetched, failed),))
            expect(failures, any(r == "bytes=1000-" for _, r, _ in _RangeHandler.hits), "no resume: %r" % _RangeHandler.hits)
            expect(failures, all(ua == USER_AGENT for _, _, ua in _RangeHandler.hits), "user agent not generic")
            with open(os.path.join(corpus, "cache", "t", "part.stp"), "rb") as handle:
                expect(failures, handle.read() == payload, "resumed file differs from the original")
            expect(failures, not os.path.exists(os.path.join(corpus, "cache", "t", "other.stp")),
                   "archive member that was not asked for was unpacked")

            # A second run downloads nothing.
            hits = len(_RangeHandler.hits)
            present, fetched, failed, downloaded = fetch(corpus, manifest, entries, pause=0, log=quiet)
            expect(failures, (present, fetched, downloaded) == (2, 0, 0) and len(_RangeHandler.hits) == hits,
                   "second fetch was not a no-op: %r" % ((present, fetched, downloaded),))

            # A file that changed upstream is reported and not kept.
            os.remove(os.path.join(corpus, "cache", "t", "part.stp"))
            bad = json.loads(json.dumps(manifest))
            bad["files"][0]["sha256"] = "1" * 64
            present, fetched, failed, _ = fetch(corpus, bad, select(bad)[:1], pause=0, log=quiet)
            expect(failures, len(failed) == 1 and "SHA-256" in failed[0], "bad hash not caught: %r" % failed)
            expect(failures, not os.path.exists(os.path.join(corpus, "cache", "t", "part.stp")), "bad file kept")

            # A missing upstream file and an unsafe path are failures, not crashes.
            gone = dict(manifest["files"][0], url=base + "gone.stp")
            _, _, failed, _ = fetch(corpus, manifest, [gone], pause=0, log=quiet)
            expect(failures, len(failed) == 1 and "404" in failed[0], "404 not reported: %r" % failed)
            unsafe = dict(manifest["files"][0], path="cache/../../evil.stp")
            _, _, failed, _ = fetch(corpus, manifest, [unsafe], pause=0, log=quiet)
            expect(failures, len(failed) == 1 and "unsafe" in failed[0], "unsafe path not refused: %r" % failed)
        finally:
            server.shutdown()
            server.server_close()
    return rm.self_test_result("corpus_fetch", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", default=CORPUS_DIR)
    parser.add_argument("--source", nargs="+")
    parser.add_argument("--id", nargs="+")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--ca-file", default=os.environ.get("SSL_CERT_FILE"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.ca_file:
        global TLS_CONTEXT
        TLS_CONTEXT = ssl.create_default_context(cafile=args.ca_file)
    with open(os.path.join(args.corpus, "manifest.json"), "r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    entries = select(manifest, args.source, args.id, args.limit)
    started = time.time()
    present, fetched, failed, downloaded = fetch(args.corpus, manifest, entries, args.dry_run)
    for failure in failed:
        print("corpus_fetch: " + failure, file=sys.stderr)
    print("corpus_fetch: %d entries: %d already present, %d fetched, %d failed; %.1f MB downloaded in %.0f s" % (
        len(entries), present, fetched, len(failed), downloaded / 1e6, time.time() - started))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
