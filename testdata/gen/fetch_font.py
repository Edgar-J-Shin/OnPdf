#!/usr/bin/env python3
"""Noto Sans KR 서브셋 폰트와 글리프 매핑 JSON 을 만든다 (가끔만 실행).

  pip install "fonttools==4.60.*" brotli      # 버전이 다르면 글리프 순서가 바뀔 수 있다
  python3 testdata/gen/fetch_font.py

출력:
  testdata/fonts/NotoSansKR-subset.ttf   — wght 400 인스턴스, 필요한 글자만 (OFL 1.1, family 이름 변경)
  testdata/fonts/NotoSansKR-subset.json  — {"upem":1000, "ascent":.., "descent":.., "bbox":[..],
                                            "glyphs": {"가": {"gid": 12, "width": 920}, ...}}
  testdata/licenses/OFL.txt
docs.py 는 JSON 만 읽으므로 fontTools 는 이 스크립트에만 필요하다.
"""

from __future__ import annotations

import io
import json
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_URL = "https://github.com/google/fonts/raw/main/ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf"
OFL_URL = "https://github.com/google/fonts/raw/main/ofl/notosanskr/OFL.txt"
FAMILY = "NotoSansKR-subset"

# docs.py 가 쓰는 모든 글자. 여기에 없는 글자는 임베딩 문서에 쓸 수 없다.
TEXT = (
    "안녕하세요, OnPdf 입니다. 한글 CID 폰트 임베딩 테스트 문서입니다. "
    "가나다라마바사아자차카타파하 "
    "대한민국 서울특별시 한글 제목 작성자 메모 북마크 첫째 둘째 셋째 장 절 항 "
    "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz .,:;!?()[]-+/%"
)


def main() -> None:
    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
    except ImportError:
        sys.exit("fontTools 가 필요합니다: pip install fonttools brotli")

    print("downloading", FONT_URL)
    data = urllib.request.urlopen(FONT_URL, timeout=120).read()
    ofl = urllib.request.urlopen(OFL_URL, timeout=60).read()
    (ROOT / "licenses").mkdir(exist_ok=True)
    (ROOT / "licenses" / "OFL.txt").write_bytes(ofl)

    font = TTFont(io.BytesIO(data), recalcTimestamp=False)  # head.modified 고정 → 재생성 결정적
    font = instancer.instantiateVariableFont(font, {"wght": 400})

    chars = sorted(set(TEXT) - {" "}) + [" "]
    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.recalc_bounds = True
    options.glyph_names = False
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=[ord(c) for c in chars])
    subsetter.subset(font)

    # OFL: Reserved Font Name 을 쓰지 않도록 family 이름 변경
    for rec in font["name"].names:
        if rec.nameID in (1, 3, 4, 6, 16):
            rec.string = FAMILY
    font["OS/2"].achVendID = "ONPD"

    out_ttf = ROOT / "fonts" / f"{FAMILY}.ttf"
    out_ttf.parent.mkdir(exist_ok=True)
    font.save(out_ttf)

    font = TTFont(out_ttf, recalcTimestamp=False)
    upem = font["head"].unitsPerEm
    cmap = font.getBestCmap()
    hmtx = font["hmtx"]
    order = font.getGlyphOrder()
    gid_of = {g: i for i, g in enumerate(order)}
    glyphs = {}
    for c in chars:
        gname = cmap.get(ord(c))
        if gname is None:
            continue
        glyphs[c] = {"gid": gid_of[gname], "width": round(hmtx[gname][0] * 1000 / upem)}
    head = font["head"]
    os2 = font["OS/2"]
    meta = {
        "family": FAMILY,
        "upem": upem,
        "ascent": round(os2.sTypoAscender * 1000 / upem),
        "descent": round(os2.sTypoDescender * 1000 / upem),
        "cap_height": round(getattr(os2, "sCapHeight", 700) * 1000 / upem),
        "bbox": [round(v * 1000 / upem) for v in (head.xMin, head.yMin, head.xMax, head.yMax)],
        "num_glyphs": len(order),
        "glyphs": glyphs,
    }
    (ROOT / "fonts" / f"{FAMILY}.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_ttf} ({out_ttf.stat().st_size} bytes, {len(order)} glyphs, {len(glyphs)} chars)")


if __name__ == "__main__":
    main()
