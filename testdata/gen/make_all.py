#!/usr/bin/env python3
"""테스트 PDF 를 전부 생성하고 README 표를 갱신한다.

  python3 testdata/gen/make_all.py          # 생성 (표준 라이브러리만. qpdf 가 있으면 AES-256 파일도)
  python3 testdata/gen/make_all.py --check  # 디스크와 비교만 (CI 결정성 검사). 다르면 exit 1

파생 파일(절단·xref 훼손·쓰레기 헤더)과 한글 파일명 복사본도 여기서 만든다.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import docs  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 1_000_000

GENERATED = {
    "simple.pdf": docs.make_simple,
    "korean-embedded.pdf": docs.make_korean_embedded,
    "korean-system-font.pdf": docs.make_korean_system_font,
    "encrypted-rc4-user.pdf": docs.make_encrypted_rc4_user,
    "encrypted-owner-only.pdf": docs.make_encrypted_owner_only,
    "scanned.pdf": docs.make_scanned,
    "form-acro.pdf": docs.make_form_acro,
    "bookmarks.pdf": docs.make_bookmarks,
    "large-1000.pdf": docs.make_large,
    "mixed-sizes.pdf": docs.make_mixed_sizes,
    "xref-stream.pdf": docs.make_xref_stream,
    "incremental-update.pdf": docs.make_incremental_update,
    "annotations.pdf": docs.make_annotations,
    "empty-page.pdf": docs.make_empty_page,
    "unicode-metadata.pdf": docs.make_unicode_metadata,
    "transparency.pdf": docs.make_transparency,
    "text-layout.pdf": docs.make_text_layout,
}


def derived(files: dict[str, bytes]) -> dict[str, bytes]:
    simple = files["simple.pdf"]
    bookmarks = files["bookmarks.pdf"]
    out = {}
    # 뒤 40% 절단
    out["truncated.pdf"] = bookmarks[: len(bookmarks) * 6 // 10]
    # xref 오프셋 전부 틀림 (복구 파서 검증)
    head, _, tail = simple.partition(b"xref\n")
    lines = tail.split(b"\n")
    lines = [b"0000000999 00000 n " if ln.endswith(b" n ") else ln for ln in lines]
    out["bad-xref.pdf"] = head + b"xref\n" + b"\n".join(lines)
    # %PDF 앞에 1KB 쓰레기
    junk = bytes((i * 37 + 11) & 0xFF for i in range(1024))
    out["garbage-header.pdf"] = junk + simple
    # 한글 파일명 복사본 (경로 UTF-8 처리)
    out["한글 파일명.pdf"] = files["unicode-metadata.pdf"]
    return out


def make_aes(simple_path: Path, out: Path) -> bool:
    qpdf = shutil.which("qpdf")
    if qpdf is None:
        print("[skip] qpdf 없음 → encrypted-aes256.pdf 는 커밋본 유지 (brew install qpdf)")
        return False
    subprocess.run([qpdf, "--encrypt", "user", "owner", "256", "--", str(simple_path), str(out)],
                   check=True)
    return True


def update_readme() -> None:
    manifest = ROOT / "manifest.tsv"
    readme = ROOT / "README.md"
    rows = []
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if cols[0] == "file":
            continue
        file, expect, pw, opw, pages, w, h, render, source, license_, note = (cols + [""] * 11)[:11]
        pw_text = f"user `{pw}` / owner `{opw}`" if pw or opw else ""
        rows.append(f"| `{file}` | {expect} | {pw_text} | {pages} | {source} | {license_} | {note} |")
    table = "\n".join([
        "| 파일 | 열기 | 암호 | 쪽 | 출처 | 라이선스 | 용도 |",
        "|---|---|---|---|---|---|---|",
        *rows,
    ])
    text = readme.read_text(encoding="utf-8")
    start = text.index("<!-- manifest:start -->") + len("<!-- manifest:start -->")
    end = text.index("<!-- manifest:end -->")
    readme.write_text(text[:start] + "\n" + table + "\n" + text[end:], encoding="utf-8")


def main() -> int:
    check = "--check" in sys.argv
    files = {name: fn() for name, fn in GENERATED.items()}
    files.update(derived(files))

    bad = []
    for name, data in files.items():
        if len(data) > MAX_BYTES:
            bad.append(f"{name}: {len(data)} bytes > {MAX_BYTES}")
        path = ROOT / name
        if check:
            if not path.exists() or path.read_bytes() != data:
                bad.append(f"{name}: 디스크와 다름 (make_all.py 를 다시 실행해 커밋하세요)")
        else:
            path.write_bytes(data)
            print(f"{name}: {len(data)} bytes")

    if not check:
        make_aes(ROOT / "simple.pdf", ROOT / "encrypted-aes256.pdf")
        update_readme()

    for b in bad:
        print("ERROR", b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
