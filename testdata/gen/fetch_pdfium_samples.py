#!/usr/bin/env python3
"""PDFium testing/resources 에서 샘플 PDF 를 받아 testdata/pdfium-<name> 으로 저장한다.

라이선스: BSD-3-Clause (testdata/licenses/PDFium-LICENSE.txt). 버전은 third_party/pdfium-version.txt 와 동일.
표준 라이브러리만 사용. 결과물은 커밋하므로 평소에는 실행할 필요가 없다.
"""

from __future__ import annotations

import base64
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT.parent / "third_party" / "pdfium-version.txt").read_text().strip()
BASE = f"https://pdfium.googlesource.com/pdfium/+/refs/heads/{VERSION}"

SAMPLES = [
    "hello_world.pdf",          # 가장 단순한 텍스트 문서
    "encrypted.pdf",            # 암호 문서 (PDFium 테스트: 사용자 암호 "1234")
    "bookmarks.pdf",            # 북마크 트리
    "text_form.pdf",            # AcroForm 텍스트 필드
    "rectangles.pdf",           # 도형 렌더
    "trailer_unterminated.pdf", # 손상: trailer 미종료
    "bug_481363.pdf",           # 실제 버그 리포트 손상 파일
]


def fetch(path: str) -> bytes:
    url = f"{BASE}/{path}?format=TEXT"
    print("downloading", url)
    return base64.b64decode(urllib.request.urlopen(url, timeout=60).read())


def main() -> None:
    (ROOT / "licenses").mkdir(exist_ok=True)
    (ROOT / "licenses" / "PDFium-LICENSE.txt").write_bytes(fetch("LICENSE"))
    for name in SAMPLES:
        data = fetch(f"testing/resources/{name}")
        out = ROOT / f"pdfium-{name}"
        out.write_bytes(data)
        print(f"  {out.name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
