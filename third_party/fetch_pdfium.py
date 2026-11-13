#!/usr/bin/env python3
"""pdfium-binaries 사전 빌드를 받아 third_party/pdfium/<platform>/ 에 푼다.

버전은 third_party/pdfium-version.txt 의 태그(예: chromium/8152)로 고정한다.
같은 버전이 이미 있으면 아무것도 하지 않는다.

사용법:
    python3 third_party/fetch_pdfium.py            # 현재 머신 플랫폼
    python3 third_party/fetch_pdfium.py --platform win-x64
    python3 third_party/fetch_pdfium.py --force
"""

from __future__ import annotations

import argparse
import io
import platform
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VERSION_FILE = ROOT / "pdfium-version.txt"
BASE_URL = "https://github.com/bblanchon/pdfium-binaries/releases/download"


def host_platform() -> str:
    system = platform.system()
    machine = platform.machine().lower()
    arch = {"x86_64": "x64", "amd64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(machine)
    if arch is None:
        sys.exit(f"지원하지 않는 아키텍처: {machine}")
    os_name = {"Windows": "win", "Darwin": "mac", "Linux": "linux"}.get(system)
    if os_name is None:
        sys.exit(f"지원하지 않는 OS: {system}")
    return f"{os_name}-{arch}"


def read_version() -> str:
    return VERSION_FILE.read_text(encoding="utf-8").strip()


def fetch(target: str, force: bool) -> Path:
    version = read_version()
    dest = ROOT / "pdfium" / target
    stamp = dest / ".version"
    if not force and stamp.exists() and stamp.read_text(encoding="utf-8").strip() == version:
        print(f"[pdfium] {target} {version} 이미 있음: {dest}")
        return dest

    url = f"{BASE_URL}/{version}/pdfium-{target}.tgz"
    print(f"[pdfium] 다운로드 {url}")
    with urllib.request.urlopen(url, timeout=120) as resp:
        data = resp.read()

    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(dest, filter="data")
        else:
            tar.extractall(dest)
    stamp.write_text(version + "\n", encoding="utf-8")
    print(f"[pdfium] 완료: {dest}")
    return dest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", default=host_platform(), help="win-x64, win-arm64, mac-arm64, mac-x64, linux-x64, linux-arm64")
    parser.add_argument("--force", action="store_true", help="이미 있어도 다시 받는다")
    args = parser.parse_args()
    fetch(args.platform, args.force)


if __name__ == "__main__":
    main()
