#!/usr/bin/env python3
"""CHANGELOG.md 에서 한 버전의 절을 꺼내 출력한다 (릴리스 노트용, #90).

    python3 scripts/changelog_section.py 0.5.0 [CHANGELOG.md]

`## [0.5.0] - 날짜` 다음 줄부터 다음 `## [` 머리글 전까지. 그 버전이 없거나 비어 있으면 종료 코드 1
(빈 본문으로 릴리스를 내지 않는다). 표준 라이브러리만 쓴다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


def section(text: str, version: str) -> str | None:
    header = re.compile(r"^## \[" + re.escape(version) + r"\](\s|$)")
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if header.match(line)), None)
    if start is None:
        return None
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## [")), len(lines))
    body = "\n".join(lines[start + 1:end]).strip()
    return body or None


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    version = argv[1].removeprefix("v")
    path = Path(argv[2]) if len(argv) == 3 else Path(__file__).resolve().parents[1] / "CHANGELOG.md"
    body = section(path.read_text(encoding="utf-8"), version)
    if body is None:
        print(f"CHANGELOG 에 [{version}] 절이 없습니다: {path}", file=sys.stderr)
        return 1
    sys.stdout.write(body + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
