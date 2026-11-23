#!/usr/bin/env python3
"""macdeployqt 뒤 앱 번들을 정리하고 검증한다 (#90). 표준 라이브러리 + otool 만 쓴다.

    python3 packaging/finish_macos_bundle.py path/to/OnPdf.app

1. 쓰지 않는 플러그인을 지운다 — Homebrew Qt 의 macdeployqt 는 다른 Homebrew 모듈(qtpdf·qtsvg·qtvirtualkeyboard)에
   의존하는 플러그인을 넣고 그 의존은 풀지 못한다. 사용자 Mac 에서는 로드되지 않을 뿐 아니라 가상 키보드는 QML·Quick
   전체를 끌고 들어온다. OnPdf 는 PDF 를 이미지로 읽지 않고(PDFium), SVG 아이콘·가상 키보드를 쓰지 않는다.
2. 실행 파일과 남은 플러그인에서 닿지 않는 Frameworks 항목을 지운다(전이 폐쇄).
3. Frameworks 안 라이브러리의 설치 이름(ID)이 절대 경로면 @rpath/… 로 고친다 (macdeployqt 가 일부를 놓친다).
   고친 뒤에는 서명이 깨지므로 이 스크립트 다음에 서명한다.
4. 모든 Mach-O 의 의존을 확인한다: @rpath·@loader_path·@executable_path 는 번들 안에서 찾아져야 하고, 시스템 경로
   (/System, /usr/lib) 밖의 절대 경로(예 /opt/homebrew)는 없어야 한다. 하나라도 어기면 종료 코드 1.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

UNUSED_PLUGINS = [
    "imageformats/libqpdf.dylib",                       # PDF 를 이미지로 (QtPdf 필요) — PDF 는 PDFium 으로 연다
    "iconengines/libqsvgicon.dylib",                    # SVG 아이콘 (QtSvg 필요) — 쓰지 않는다
    "platforminputcontexts/libqtvirtualkeyboardplugin.dylib",  # 가상 키보드 (QML·Quick) — 쓰지 않는다
]
SYSTEM_PREFIXES = ("/System/", "/usr/lib/")


def is_macho(path: Path) -> bool:
    try:
        with path.open("rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def install_id(binary: Path) -> str | None:
    """dylib 자신의 설치 이름 (LC_ID_DYLIB). 실행 파일·번들이면 None"""
    out = subprocess.run(["otool", "-D", str(binary)], capture_output=True, text=True, check=True).stdout
    lines = [line.strip() for line in out.splitlines()[1:] if line.strip()]
    return lines[0] if lines else None


def deps(binary: Path) -> list[str]:
    """의존 목록 (자기 설치 이름은 뺀다 — otool -L 은 dylib 의 ID 를 첫 줄에 섞어 보여 준다)"""
    out = subprocess.run(["otool", "-L", str(binary)], capture_output=True, text=True, check=True).stdout
    own = install_id(binary)
    result = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if line:
            name = line.split(" (")[0]
            if name != own:
                result.append(name)
    return result


def resolve(dep: str, binary: Path, contents: Path) -> Path | None:
    """번들 안에서 의존을 찾는다. 찾지 못하면 None"""
    frameworks = contents / "Frameworks"
    if dep.startswith("@rpath/"):
        return frameworks / dep[len("@rpath/"):]
    if dep.startswith("@executable_path/"):
        return contents / "MacOS" / dep[len("@executable_path/"):]
    if dep.startswith("@loader_path/"):
        return binary.parent / dep[len("@loader_path/"):]
    return None


def framework_root(path: Path, frameworks: Path) -> Path:
    """Frameworks/X.framework/... 또는 Frameworks/libY.dylib → Frameworks 바로 아래 항목"""
    rel = path.resolve().relative_to(frameworks.resolve())
    return frameworks / rel.parts[0]


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    app = Path(argv[1])
    contents = app / "Contents"
    frameworks = contents / "Frameworks"
    plugins = contents / "PlugIns"

    for rel in UNUSED_PLUGINS:
        p = plugins / rel
        if p.exists():
            p.unlink()
            print(f"removed plugin {rel}")
    for d in list(plugins.iterdir()) if plugins.exists() else []:
        if d.is_dir() and not any(d.iterdir()):
            d.rmdir()

    # 닿는 Frameworks 항목: 실행 파일·플러그인에서 시작해 전이적으로
    roots = [p for p in (contents / "MacOS").iterdir() if is_macho(p)]
    roots += [p for p in plugins.rglob("*.dylib")] if plugins.exists() else []
    reachable: set[Path] = set()
    queue = list(roots)
    seen: set[Path] = set()
    while queue:
        binary = queue.pop()
        if binary in seen:
            continue
        seen.add(binary)
        for dep in deps(binary):
            target = resolve(dep, binary, contents)
            if target is None or not target.exists():
                continue
            try:
                root = framework_root(target, frameworks)
            except ValueError:
                continue
            reachable.add(root)
            queue.append(target.resolve())
    for entry in sorted(frameworks.iterdir()):
        if entry not in reachable:
            shutil.rmtree(entry) if entry.is_dir() and not entry.is_symlink() else entry.unlink()
            print(f"removed unused {entry.name}")

    # 설치 이름 정리: Frameworks 안 Mach-O 의 ID 가 절대 경로면 @rpath/<Frameworks 기준 경로>
    for path in sorted(frameworks.rglob("*")):
        if path.is_symlink() or not path.is_file() or not is_macho(path):
            continue
        own = install_id(path)
        if own and not own.startswith("@"):
            new_id = "@rpath/" + str(path.relative_to(frameworks))
            subprocess.run(["install_name_tool", "-id", new_id, str(path)], check=True, capture_output=True)
            print(f"fixed id {path.relative_to(frameworks)}: {own} -> {new_id}")

    # 검증
    problems = []
    for path in sorted(contents.rglob("*")):
        if path.is_symlink() or not path.is_file() or not is_macho(path):
            continue
        for dep in deps(path):
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            target = resolve(dep, path, contents)
            rel = path.relative_to(app)
            if target is None:
                problems.append(f"{rel}: absolute dependency outside the bundle: {dep}")
            elif not target.exists():
                problems.append(f"{rel}: unresolved {dep}")
    for p in problems:
        print(p, file=sys.stderr)
    size = sum(f.stat().st_size for f in app.rglob("*") if f.is_file() and not f.is_symlink())
    print(f"bundle {app.name}: {size / 1e6:.1f} MB, {'OK' if not problems else f'{len(problems)} problem(s)'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
