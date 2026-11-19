#!/usr/bin/env python3
"""앱에 번들할 글꼴(Noto Sans KR, SIL OFL 1.1)을 받아 third_party/fonts/ 에 둔다.

버전은 third_party/fonts-version.txt 의 google/fonts 커밋 해시로 고정한다. 같은 파일이 이미 있으면 아무것도 하지 않는다.
앱 빌드가 이 폴더의 파일을 build/<preset>/bin/fonts/ 로 복사한다 (저장소에는 커밋하지 않는다).

사용법:
    python3 third_party/fetch_fonts.py
    python3 third_party/fetch_fonts.py --force
"""

from __future__ import annotations

import argparse
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VERSION_FILE = ROOT / "fonts-version.txt"
OUT_DIR = ROOT / "fonts"
FILES = {
    "NotoSansKR[wght].ttf": "ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf",
    "OFL-NotoSansKR.txt": "ofl/notosanskr/OFL.txt",
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    commit = VERSION_FILE.read_text(encoding="utf-8").strip()
    OUT_DIR.mkdir(exist_ok=True)
    for name, rel in FILES.items():
        target = OUT_DIR / name
        if target.exists() and not args.force:
            print(f"already present: {target}")
            continue
        url = f"https://raw.githubusercontent.com/google/fonts/{commit}/{rel}"
        print(f"downloading {url}")
        data = urllib.request.urlopen(url, timeout=120).read()
        if len(data) < 1024:
            sys.exit(f"unexpected small file: {name}")
        target.write_bytes(data)
        print(f"  → {target} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
