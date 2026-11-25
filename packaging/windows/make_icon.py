#!/usr/bin/env python3
"""OnPdf 앱 아이콘 생성기 (#99). 표준 라이브러리만 쓰고 결정적이다(난수 없음).

    python3 packaging/windows/make_icon.py app/onpdf.ico [--png out-dir]

디자인: 둥근 파란 사각형 위 흰 문서(접힌 모서리), 회색 글줄 3개 + 빨간 글줄 1개(줄 수정을 상징).
768px 로 그린 뒤 박스 평균으로 16·24·32·48·64·128·256 을 만들고(안티앨리어싱), ICO 에는 PNG 항목으로
담는다(Windows Vista+). --png 를 주면 크기별 PNG 도 쓴다(문서·macOS 아이콘 재료).
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

BASE = 768
SIZES = (16, 24, 32, 48, 64, 128, 256)

BLUE = (30, 91, 184)
WHITE = (255, 255, 255)
FOLD = (214, 222, 235)
GRAY = (150, 158, 170)
RED = (220, 50, 47)


def rounded_rect(x: float, y: float, x0: float, y0: float, x1: float, y1: float, r: float) -> bool:
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def render_base() -> list[list[tuple[int, int, int, int]]]:
    """BASE×BASE RGBA. 좌표는 0..1 비율로 정의한 도형을 픽셀 중심에서 평가한다"""
    s = BASE
    # 도형 (비율)
    bg = (0.04, 0.04, 0.96, 0.96, 0.19)  # 배경 둥근 사각형 + 모서리 반지름
    doc_x0, doc_y0, doc_x1, doc_y1 = 0.27, 0.16, 0.73, 0.84
    fold = 0.15  # 접힌 모서리 크기
    line_x0, line_x1 = 0.35, 0.65
    lines = [(0.47, GRAY), (0.56, GRAY), (0.65, GRAY), (0.74, RED)]
    line_h = 0.045
    red_x1 = 0.57
    rows = []
    for py in range(s):
        y = (py + 0.5) / s
        row = []
        for px in range(s):
            x = (px + 0.5) / s
            if not rounded_rect(x, y, bg[0], bg[1], bg[2], bg[3], bg[4]):
                row.append((0, 0, 0, 0))
                continue
            color = BLUE
            in_doc = doc_x0 <= x <= doc_x1 and doc_y0 <= y <= doc_y1
            # 오른쪽 위 모서리는 잘라 낸다 (x - doc_x1 + fold > doc_y0 + fold - y → 바깥)
            cut = (x - (doc_x1 - fold)) + (doc_y0 + fold - y) > fold
            if in_doc and not cut:
                color = WHITE
                # 접힌 모서리: 잘린 변 안쪽의 모서리 사각형 아래쪽 반 (접힌 종이)
                if x > doc_x1 - fold and y < doc_y0 + fold:
                    color = FOLD
                for ly, lc in lines:
                    x1 = red_x1 if lc is RED else line_x1
                    if line_x0 <= x <= x1 and abs(y - ly) <= line_h / 2:
                        # 둥근 끝
                        if x < line_x0 + line_h / 2 and (x - line_x0 - line_h / 2) ** 2 + (y - ly) ** 2 > (line_h / 2) ** 2:
                            continue
                        if x > x1 - line_h / 2 and (x - x1 + line_h / 2) ** 2 + (y - ly) ** 2 > (line_h / 2) ** 2:
                            continue
                        color = lc
            row.append((color[0], color[1], color[2], 255))
        rows.append(row)
    return rows


def downsample(src: list[list[tuple[int, int, int, int]]], size: int) -> bytes:
    """박스 평균(프리멀티플라이 알파) → RGBA 바이트"""
    f = BASE // size
    assert f * size == BASE, size
    out = bytearray()
    for oy in range(size):
        for ox in range(size):
            r = g = b = a = 0
            for yy in range(oy * f, oy * f + f):
                row = src[yy]
                for xx in range(ox * f, ox * f + f):
                    pr, pg, pb, pa = row[xx]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            n = f * f
            if a == 0:
                out += b"\0\0\0\0"
            else:
                out += bytes((round(r / a), round(g / a), round(b / a), round(a / n)))
    return bytes(out)


def png(size: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + rgba[y * size * 4 : (y + 1) * size * 4] for y in range(size))
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def ico(images: list[tuple[int, bytes]]) -> bytes:
    header = struct.pack("<HHH", 0, 1, len(images))
    entries = b""
    data = b""
    offset = 6 + 16 * len(images)
    for size, blob in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    return header + entries + data


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    out = Path(argv[1])
    png_dir = Path(argv[argv.index("--png") + 1]) if "--png" in argv else None
    base = render_base()
    images = []
    for size in SIZES:
        blob = png(size, downsample(base, size))
        images.append((size, blob))
        if png_dir is not None:
            png_dir.mkdir(parents=True, exist_ok=True)
            (png_dir / f"onpdf-{size}.png").write_bytes(blob)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(ico(images))
    print(f"{out}: {len(images)} images, {out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
