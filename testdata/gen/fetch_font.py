#!/usr/bin/env python3
"""Noto Sans KR 서브셋 폰트와 글리프 매핑 JSON 을 만든다 (가끔만 실행).

  pip install "fonttools==4.60.*" brotli      # 버전이 다르면 글리프 순서가 바뀔 수 있다
  python3 testdata/gen/fetch_font.py

출력:
  testdata/fonts/NotoSansKR-subset.ttf   — wght 400 인스턴스, 필요한 글자만 (OFL 1.1, family 이름 변경)
  testdata/fonts/NotoSansCJKkr-subset.otf — Noto Sans CJK KR(OTF, CID-keyed CFF) 같은 글자 서브셋 (#73: CFF 후처리·charset 비항등)
  testdata/fonts/NotoSansKR-subset.ttc   — 위 TTF 서브셋 2벌(면 1 은 family "NotoSansKR-subset-Face1") 모음 (#73: TTC 면 선택)
  testdata/fonts/SourceSans3-subset.otf  — Source Sans 3(OTF, 비CID CFF) 라틴 서브셋 (#73 W4: charset 을 손대지 않는 변환 경로)
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
CFF_URL = "https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Korean/NotoSansCJKkr-Regular.otf"
CFF_FAMILY = "NotoSansCJKkr-subset"
# 비CID CFF (이름 기반 charset): 저장 후처리가 charset 을 손대지 않고 사전만 바꾸는 경로 (#73 리뷰 W4). Adobe Source Sans 3, OFL
NONCID_URL = "https://github.com/adobe-fonts/source-sans/raw/release/OTF/SourceSans3-Regular.otf"
NONCID_FAMILY = "SourceSans3-subset"
TTC_FACE1 = "NotoSansKR-subset-Face1"

# docs.py 가 쓰는 모든 글자. 여기에 없는 글자는 임베딩 문서에 쓸 수 없다.
TEXT = (
    "안녕하세요, OnPdf 입니다. 한글 CID 폰트 임베딩 테스트 문서입니다. "
    "가나다라마바사아자차카타파하 "
    "대한민국 서울특별시 한글 제목 작성자 메모 북마크 첫째 둘째 셋째 장 절 항 "
    "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz .,:;!?()[]-+/%"
    # #45 텍스트 추가 테스트 문장 (한글·영문·숫자·기호·NBSP)
    " 텍스트 추가 검증 임베딩 왕복 객체 편집 년 둘째 줄 입력 저장 열기 회전 쪽 빨강 파랑 —"  # NBSP 는 넣지 않는다: 공백과 글리프를 공유해 생성기의 ToUnicode 가 바뀐다
)


def subset_and_rename(font, chars, family: str) -> None:
    from fontTools import subset

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
            rec.string = family
    font["OS/2"].achVendID = "ONPD"


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
    subset_and_rename(font, chars, FAMILY)

    out_ttf = ROOT / "fonts" / f"{FAMILY}.ttf"
    out_ttf.parent.mkdir(exist_ok=True)
    font.save(out_ttf)

    # #73: CID-keyed CFF 서브셋 (charset 은 원래 CID 유지 — 후처리 테스트 대상), TTC 2면
    print("downloading", CFF_URL)
    cff = TTFont(io.BytesIO(urllib.request.urlopen(CFF_URL, timeout=300).read()), recalcTimestamp=False)
    subset_and_rename(cff, chars, CFF_FAMILY)
    out_otf = ROOT / "fonts" / f"{CFF_FAMILY}.otf"
    cff.save(out_otf)
    print(f"wrote {out_otf} ({out_otf.stat().st_size} bytes, {len(cff.getGlyphOrder())} glyphs)")
    print("downloading", NONCID_URL)
    latin = TTFont(io.BytesIO(urllib.request.urlopen(NONCID_URL, timeout=300).read()), recalcTimestamp=False)
    subset_and_rename(latin, [c for c in chars if ord(c) < 0x3000], NONCID_FAMILY)
    out_noncid = ROOT / "fonts" / f"{NONCID_FAMILY}.otf"
    latin.save(out_noncid)
    print(f"wrote {out_noncid} ({out_noncid.stat().st_size} bytes, {len(latin.getGlyphOrder())} glyphs, non-CID CFF)")
    from fontTools.ttLib import TTCollection
    face1 = TTFont(out_ttf, recalcTimestamp=False)
    for rec in face1["name"].names:
        if rec.nameID in (1, 3, 4, 6, 16):
            rec.string = TTC_FACE1
    ttc = TTCollection()
    ttc.fonts = [TTFont(out_ttf, recalcTimestamp=False), face1]
    out_ttc = ROOT / "fonts" / f"{FAMILY}.ttc"
    ttc.save(out_ttc)
    print(f"wrote {out_ttc} ({out_ttc.stat().st_size} bytes, 2 faces)")

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
