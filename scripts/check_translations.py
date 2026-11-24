#!/usr/bin/env python3
"""번역 파일 누락 검사 (#8). 표준 라이브러리만 쓴다.

    python3 scripts/check_translations.py --lupdate <lupdate> --source-dir app \
        --ts app/i18n/onpdf_ko.ts --plurals app/i18n/onpdf_en.ts

1. lupdate 로 소스에서 임시 .ts 를 만들어 (문맥, 원문) 집합을 구하고, 커밋된 .ts 의 활성 메시지
   집합과 비교한다. 다르면 새 문자열이 반영되지 않았거나 지운 문자열이 남은 것이다
   → `cmake --build --preset dev-app --target update_translations` 뒤 번역을 채운다.
2. 커밋된 .ts 에 미완료(type="unfinished") 또는 빈 번역이 있으면 실패한다.
--plurals 는 같은 검사를 복수형 전용(-pluralonly)으로 한다. 종료 코드: 0 통과, 1 실패, 2 사용법.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


def messages(ts: Path) -> dict[tuple[str, str], ET.Element]:
    """활성 메시지 (vanished·obsolete 제외): (문맥, 원문) → message 요소"""
    result = {}
    for context in ET.parse(ts).getroot().iter("context"):
        name = context.findtext("name") or ""
        for message in context.iter("message"):
            translation = message.find("translation")
            kind = translation.get("type") if translation is not None else None
            if kind in ("vanished", "obsolete"):
                continue
            result[(name, message.findtext("source") or "")] = message
    return result


def unfinished(message: ET.Element) -> bool:
    translation = message.find("translation")
    if translation is None or translation.get("type") == "unfinished":
        return True
    if message.get("numerus") == "yes":
        forms = translation.findall("numerusform")
        return not forms or any(not (f.text or "").strip() for f in forms)
    return not (translation.text or "").strip()


def check(lupdate: str, source_dir: Path, ts: Path, plural_only: bool) -> list[str]:
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        fresh = Path(tmp) / "fresh.ts"
        cmd = [lupdate, "-no-obsolete", "-locations", "none", "-silent", "-extensions", "cpp,h"]
        if plural_only:
            cmd.append("-pluralonly")
        cmd += [str(source_dir), "-ts", str(fresh)]
        run = subprocess.run(cmd, capture_output=True, text=True)
        if run.returncode != 0 or not fresh.exists():
            return [f"lupdate failed: {run.stderr.strip()}"]
        expected = set(messages(fresh))
    committed = messages(ts)
    for key in sorted(expected - set(committed)):
        problems.append(f"{ts.name}: missing (run update_translations): [{key[0]}] {key[1]!r}")
    for key in sorted(set(committed) - expected):
        problems.append(f"{ts.name}: stale (run update_translations): [{key[0]}] {key[1]!r}")
    for key, message in sorted(committed.items()):
        if key in expected and unfinished(message):
            problems.append(f"{ts.name}: untranslated: [{key[0]}] {key[1]!r}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lupdate", required=True)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--ts", required=True, type=Path)
    parser.add_argument("--plurals", type=Path)
    args = parser.parse_args()
    problems = check(args.lupdate, args.source_dir, args.ts, plural_only=False)
    if args.plurals is not None:
        problems += check(args.lupdate, args.source_dir, args.plurals, plural_only=True)
    for p in problems:
        print(p, file=sys.stderr)
    print(f"translations: {'OK' if not problems else f'{len(problems)} problem(s)'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
