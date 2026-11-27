#!/usr/bin/env python3
"""왕복 테스트 코퍼스(#119)를 testdata/corpus.tsv 목록대로 내려받아 testdata/corpus/<source>/<name> 에 둔다.

파일은 저장소에 커밋하지 않는다(gitignore). 목록이 출처 저장소의 커밋 SHA 와 파일별 sha256 으로 고정하므로
언제 받아도 같은 바이트다. 표준 라이브러리만 쓴다.

사용법:
    python3 testdata/gen/fetch_corpus.py                 # 빠진·다른 파일만 내려받는다 (스레드 8)
    python3 testdata/gen/fetch_corpus.py --check         # 내려받지 않고 빠짐·불일치만 보고 (있으면 exit 1)
    python3 testdata/gen/fetch_corpus.py --source qpdf   # 일부 출처만 (반복 가능)
    python3 testdata/gen/fetch_corpus.py --list          # 출처·태그별 개수·크기 요약
    python3 testdata/gen/fetch_corpus.py --probe build/dev/bin/onpdf_cli
        # 각 파일을 onpdf_cli info 로 열어 관찰값(ok/password/invalid)을 expect 열과 비교. PDFium 판을 올릴 때

출처(SOURCES): 저장소·커밋 SHA·라이선스. 올릴 때는 SHA 를 바꾸고 목록의 sha256 을 다시 잰다
(바뀐 파일은 --check 가 알려 준다). 라이선스 원문은 testdata/licenses/ 에 커밋돼 있다.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIST = ROOT / "corpus.tsv"
DEST = ROOT / "corpus"

COLUMNS = ["name", "source", "path", "sha256", "size", "expect", "password", "note"]
EXPECTS = {"ok", "password", "invalid"}

# 2026-11-27 고정. kind: github → raw.githubusercontent.com, gitiles → ?format=TEXT (base64)
SOURCES = {
    "pdfium": dict(kind="gitiles", base="https://pdfium.googlesource.com/pdfium",
                   ref="6688c576fd851f4159c6fae39396da50939de110",  # chromium/8152 (third_party/pdfium-version.txt)
                   license="BSD-3-Clause", license_file="PDFium-LICENSE.txt"),
    "qpdf": dict(kind="github", repo="qpdf/qpdf", ref="47c4d79f0e02587c25d00c852b395336334d59e4",
                 license="Apache-2.0", license_file="qpdf-LICENSE.txt"),
    "pdfjs": dict(kind="github", repo="mozilla/pdf.js", ref="e473982703af9037218726158a7b0ca2e9c86f13",
                  license="Apache-2.0", license_file="pdf.js-LICENSE.txt"),
    "pdfbox": dict(kind="github", repo="apache/pdfbox", ref="28fe61f4ad64ea2c474de775113028b4cfea981e",
                   license="Apache-2.0", license_file="PDFBox-LICENSE.txt"),
}

THREADS = 8  # github raw. gitiles(PDFium)는 동시 요청에 429 를 돌려주므로 직렬로 받는다
RETRIES = 5  # 대기 3·6·12·24·48초 — 429 가 풀릴 때까지
TIMEOUT = 60


class Entry:
    __slots__ = COLUMNS + ["line"]

    def __init__(self, cols: list[str], line: int) -> None:
        for key, value in zip(COLUMNS, cols):
            setattr(self, key, value)
        self.size = int(self.size)
        self.line = line

    @property
    def tags(self) -> list[str]:
        return [t for t in self.note.split(" ") if t and not t.startswith("(")]

    @property
    def dest(self) -> Path:
        return DEST / self.source / self.name

    @property
    def url(self) -> str:
        src = SOURCES[self.source]
        quoted = urllib.parse.quote(self.path)
        if src["kind"] == "github":
            return f"https://raw.githubusercontent.com/{src['repo']}/{src['ref']}/{quoted}"
        return f"{src['base']}/+/{src['ref']}/{quoted}?format=TEXT"


def load_list() -> list[Entry]:
    """목록을 읽고 무결성을 검사한다. 위반이 있으면 exit 2."""
    entries: list[Entry] = []
    problems: list[str] = []
    seen: set[tuple[str, str]] = set()
    for line_no, line in enumerate(LIST.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if cols[0] == "name":
            continue
        if len(cols) != len(COLUMNS):
            problems.append(f"line {line_no}: {len(cols)} columns (expected {len(COLUMNS)})")
            continue
        try:
            e = Entry(cols, line_no)
        except ValueError:
            problems.append(f"line {line_no}: size is not a number")
            continue
        if e.source not in SOURCES:
            problems.append(f"line {line_no}: unknown source {e.source!r}")
        if len(e.sha256) != 64 or any(c not in "0123456789abcdef" for c in e.sha256):
            problems.append(f"line {line_no}: sha256 is not 64 lowercase hex digits")
        if e.expect not in EXPECTS:
            problems.append(f"line {line_no}: expect {e.expect!r} not in {sorted(EXPECTS)}")
        if e.expect == "password" and not e.password:
            problems.append(f"line {line_no}: expect=password needs a password")
        if any(c in e.name for c in "/\\") or e.name in (".", "..") or not e.name:
            problems.append(f"line {line_no}: bad name {e.name!r}")
        if (e.source, e.name) in seen:
            problems.append(f"line {line_no}: duplicate name {e.source}/{e.name}")
        seen.add((e.source, e.name))
        entries.append(e)
    for p in problems:
        print("corpus.tsv:", p, file=sys.stderr)
    if problems:
        sys.exit(2)
    return entries


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def status(e: Entry) -> str:
    """present | missing | mismatch"""
    if not e.dest.is_file():
        return "missing"
    if e.dest.stat().st_size != e.size or sha256_of(e.dest) != e.sha256:
        return "mismatch"
    return "present"


def download(e: Entry) -> bytes:
    last: Exception | None = None
    for attempt in range(RETRIES):
        try:
            req = urllib.request.Request(e.url, headers={"User-Agent": "onpdf-fetch-corpus"})
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                data = resp.read()
            if SOURCES[e.source]["kind"] == "gitiles":
                data = base64.b64decode(data)
            return data
        except (urllib.error.URLError, OSError, ValueError) as ex:  # ValueError: base64
            last = ex
            time.sleep(3 * 2**attempt)
    raise RuntimeError(f"{e.url}: {last}")


def fetch_one(e: Entry) -> str:
    data = download(e)
    digest = hashlib.sha256(data).hexdigest()
    if digest != e.sha256:
        raise RuntimeError(f"{e.source}/{e.name}: sha256 {digest} != {e.sha256} (size {len(data)})")
    e.dest.parent.mkdir(parents=True, exist_ok=True)
    part = e.dest.with_name(e.dest.name + ".part")
    part.write_bytes(data)
    os.replace(part, e.dest)
    return f"{e.source}/{e.name}: {len(data)} bytes"


def cmd_fetch(entries: list[Entry], check_only: bool) -> int:
    todo = [e for e in entries if status(e) != "present"]
    if check_only:
        for e in todo:
            print(f"{status(e)}: {e.source}/{e.name}")
        print(f"[corpus] {len(entries) - len(todo)} present, {len(todo)} missing/mismatched")
        return 1 if todo else 0
    if not todo:
        print(f"[corpus] all {len(entries)} files present")
        return 0
    print(f"[corpus] downloading {len(todo)} of {len(entries)} files")
    failures: list[str] = []

    def done(e: Entry, result: str | None, error: Exception | None) -> None:
        if error is None:
            print(" ", result)
            return
        failures.append(str(error))
        part = e.dest.with_name(e.dest.name + ".part")
        if part.exists():
            part.unlink()

    serial = [e for e in todo if SOURCES[e.source]["kind"] == "gitiles"]
    parallel = [e for e in todo if SOURCES[e.source]["kind"] != "gitiles"]
    with ThreadPoolExecutor(max_workers=THREADS) as pool:
        futures = {pool.submit(fetch_one, e): e for e in parallel}
        for e in serial:  # 풀이 github 파일을 받는 동안 메인 스레드가 gitiles 를 하나씩
            try:
                done(e, fetch_one(e), None)
            except Exception as ex:  # noqa: BLE001 — 어떤 실패든 모아서 보고
                done(e, None, ex)
        for fut in as_completed(futures):
            try:
                done(futures[fut], fut.result(), None)
            except Exception as ex:  # noqa: BLE001
                done(futures[fut], None, ex)
    for f in failures:
        print("FAILED", f, file=sys.stderr)
    print(f"[corpus] {len(todo) - len(failures)} downloaded, {len(failures)} failed")
    return 1 if failures else 0


def cmd_list(entries: list[Entry]) -> int:
    by_source: dict[str, list[Entry]] = defaultdict(list)
    for e in entries:
        by_source[e.source].append(e)
    total = 0
    print(f"{'source':8} {'files':>5} {'bytes':>11}  ok/password/invalid")
    for source in SOURCES:
        rows = by_source.get(source, [])
        if not rows:
            continue
        size = sum(e.size for e in rows)
        total += size
        c = Counter(e.expect for e in rows)
        print(f"{source:8} {len(rows):5} {size:11}  {c['ok']}/{c['password']}/{c['invalid']}")
    print(f"{'total':8} {len(entries):5} {total:11}")
    tags = Counter(t for e in entries for t in e.tags)
    print("\ntags:")
    for tag, n in sorted(tags.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"  {tag:16} {n}")
    return 0


def observe(cli: str, e: Entry) -> str:
    """onpdf_cli info 로 관찰한 열기 결과: ok | password | invalid | broken-page | password?

    broken-page: 문서는 열리지만 어떤 쪽을 못 연다(저장 검증이 거절하므로 코퍼스에 넣지 않는다).
    password?: 암호가 필요하지만 목록의 암호로는 열리지 않는다."""
    def run(password: str | None) -> subprocess.CompletedProcess:
        cmd = [cli, "info", str(e.dest)]
        if password is not None:
            cmd += ["--password", password]
        return subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=120)

    def classify(r: subprocess.CompletedProcess) -> str:
        if r.returncode == 0:
            return "ok"
        if "pages:" in r.stdout:  # 열린 뒤 쪽 크기에서 실패
            return "broken-page"
        if "password required" in r.stderr:
            return "password"
        return "invalid"

    first = classify(run(None))
    if first != "password":
        return first
    if not e.password:
        return "password?"
    with_password = classify(run(e.password))
    return "password" if with_password == "ok" else "password?"


def cmd_probe(entries: list[Entry], cli: str) -> int:
    mismatches = 0
    for e in entries:
        if status(e) == "missing":
            print(f"missing: {e.source}/{e.name}")
            mismatches += 1
            continue
        seen = observe(cli, e)
        if seen != e.expect:
            mismatches += 1
            print(f"line {e.line}: {e.source}/{e.name} expect={e.expect} observed={seen}")
    print(f"[corpus] probe: {len(entries) - mismatches} match, {mismatches} differ")
    return 1 if mismatches else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="내려받지 않고 빠짐·불일치만 보고")
    ap.add_argument("--source", action="append", choices=sorted(SOURCES), help="일부 출처만")
    ap.add_argument("--list", action="store_true", help="목록 요약")
    ap.add_argument("--probe", metavar="ONPDF_CLI", help="onpdf_cli 로 expect 를 실측 비교")
    args = ap.parse_args()

    entries = load_list()
    if args.source:
        entries = [e for e in entries if e.source in args.source]
    if args.list:
        return cmd_list(entries)
    if args.probe:
        return cmd_probe(entries, args.probe)
    return cmd_fetch(entries, args.check)


if __name__ == "__main__":
    sys.exit(main())
