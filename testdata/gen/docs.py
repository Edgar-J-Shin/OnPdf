"""테스트 문서 생성 함수들. 각 함수는 PDF 바이트를 돌려준다. 결정적(날짜·난수 없음)."""

from __future__ import annotations

import json
from pathlib import Path

import pdfgen as g
from pdfgen import PdfWriter, content, fmt, text_string

ROOT = Path(__file__).resolve().parents[1]
FONT_JSON = ROOT / "fonts" / "NotoSansKR-subset.json"
FONT_TTF = ROOT / "fonts" / "NotoSansKR-subset.ttf"


def _helv_resources(font: int) -> bytes:
    return b"<< /Font << /F1 %d 0 R >> >>" % font


def _label(x: float, y: float, size: int, text: str) -> str:
    return f"BT /F1 {size} Tf {fmt(x)} {fmt(y)} Td {g.lit(text).decode('latin-1')} Tj ET"


def _border(w: float, h: float, inset: float = 18) -> str:
    return f"0.5 G 1 w {fmt(inset)} {fmt(inset)} {fmt(w - 2 * inset)} {fmt(h - 2 * inset)} re S"


# ----------------------------------------------------------------- 1. simple


def make_simple(*, xref_stream: bool = False, encrypt: g.Encryption | None = None,
                title: str = "OnPdf simple test", file_id_name: str = "simple.pdf") -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    font = g.simple_font(w)
    c1 = w.stream(b"", content(
        "BT /F1 24 Tf 72 720 Td (OnPdf M0 test page) Tj ET",
        "BT /F1 12 Tf 72 690 Td (Letter portrait, Helvetica, Apache-2.0) Tj ET",
        "0 0 1 rg 72 500 200 120 re f",
        "1 0 0 RG 4 w 320 500 m 540 620 l S",
        "0.5 g 72 100 468 40 re f",
    ), compress=False)
    p1 = g.page(w, pages, g.LETTER, c1, _helv_resources(font))
    c2 = w.stream(b"", content(
        "BT /F1 24 Tf 72 540 Td (Page 2: landscape) Tj ET",
        "0 0.6 0 rg 72 72 300 300 re f",
    ), compress=False)
    p2 = g.page(w, pages, (792, 612), c2, _helv_resources(font))
    g.pages_tree(w, pages, [p1, p2])
    root = g.catalog(w, pages)
    info = w.reserve()
    if encrypt is not None:
        # 암호 문서의 문자열은 객체별 키로 암호화해야 한다
        w.encryptor = encrypt
        encrypt.prepare(g.file_id_for(file_id_name))
        w.set(info, b"<< /Title " + w.enc_text(info, title) + b" /Producer " + w.enc_text(info, "onpdf pdfgen")
              + b" /CreationDate " + w.enc_text(info, "D:20260101000000Z") + b" >>")
    else:
        g.info_dict(w, title, num=info)
    return w.write(root=root, info=info, file_id=g.file_id_for(file_id_name), xref_stream=xref_stream,
                   encrypt=encrypt)


# ----------------------------------------------------------------- 2. korean-embedded


def _load_font_meta() -> dict:
    if not FONT_JSON.exists():
        raise SystemExit(f"{FONT_JSON} 이 없습니다. python3 testdata/gen/fetch_font.py 를 먼저 실행하세요")
    return json.loads(FONT_JSON.read_text(encoding="utf-8"))


def _cid_text(meta: dict, s: str) -> tuple[str, set[int]]:
    """문자열 → Identity-H 용 GID hex 문자열. 사용한 GID 집합도 돌려준다."""
    gids = []
    for ch in s:
        if ch not in meta["glyphs"]:
            raise SystemExit(f"폰트 서브셋에 없는 글자: {ch!r} — fetch_font.py 의 TEXT 에 추가")
        gids.append(meta["glyphs"][ch]["gid"])
    return "<" + "".join("%04X" % gid for gid in gids) + ">", set(gids)


def make_korean_embedded() -> bytes:
    meta = _load_font_meta()
    lines = [
        (24, "안녕하세요, OnPdf 입니다."),
        (14, "한글 CID 폰트 임베딩 테스트 문서입니다."),
        (14, "가나다라마바사아자차카타파하 0123456789"),
        (14, "대한민국 서울특별시 - Mixed with English text."),
    ]
    used: set[int] = set()
    ops = []
    y = 720
    for size, text in lines:
        hexs, gids = _cid_text(meta, text)
        used |= gids
        ops.append(f"BT /F0 {size} Tf 72 {y} Td {hexs} Tj ET")
        y -= size * 2
    ops.append("0.2 0.4 0.8 rg 72 420 468 6 re f")

    w = PdfWriter()
    pages = w.reserve()
    ttf = FONT_TTF.read_bytes()
    font_file = w.stream(b"/Length1 %d" % len(ttf), ttf)
    gid_width = {v["gid"]: v["width"] for v in meta["glyphs"].values()}
    widths = b" ".join(b"%d [%d]" % (gid, gid_width[gid]) for gid in sorted(used))
    bbox = b" ".join(b"%d" % v for v in meta["bbox"])
    # 서브셋 폰트 이름은 6글자 태그 + '+' 접두 (PDF 32000 9.6.4)
    font_name = b"ABCDEF+" + meta["family"].encode()
    descriptor = w.add(
        b"<< /Type /FontDescriptor /FontName /%s /Flags 4 /FontBBox [%s] /ItalicAngle 0 /Ascent %d "
        b"/Descent %d /CapHeight %d /StemV 80 /FontFile2 %d 0 R >>"
        % (font_name, bbox, meta["ascent"], meta["descent"], meta["cap_height"], font_file))
    cid_font = w.add(
        b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s "
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
        b"/FontDescriptor %d 0 R /DW 1000 /W [%s] /CIDToGIDMap /Identity >>"
        % (font_name, descriptor, widths))
    # ToUnicode: GID(=CID) → 유니코드. 없으면 텍스트 추출·검색·복사가 GID 를 문자로 돌려준다
    gid_char = sorted((v["gid"], ch) for ch, v in meta["glyphs"].items() if v["gid"] in used)
    bfchars = "\n".join("<%04X> <%s>" % (gid, ch.encode("utf-16-be").hex().upper()) for gid, ch in gid_char)
    tounicode = w.stream(b"", (
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
        f"{len(gid_char)} beginbfchar\n{bfchars}\nendbfchar\n"
        "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n").encode("latin-1"))
    type0 = w.add(b"<< /Type /Font /Subtype /Type0 /BaseFont /%s /Encoding /Identity-H /DescendantFonts [%d 0 R] "
                  b"/ToUnicode %d 0 R >>" % (font_name, cid_font, tounicode))
    c = w.stream(b"", content(*ops))
    p = g.page(w, pages, g.A4, c, b"<< /Font << /F0 %d 0 R >> >>" % type0)
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "한글 임베딩 폰트 테스트")
    return w.write(root=root, info=info, file_id=g.file_id_for("korean-embedded.pdf"))


# ----------------------------------------------------------------- 3. korean-system-font


def make_korean_system_font() -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    descriptor = w.add(
        b"<< /Type /FontDescriptor /FontName /HYSMyeongJo-Medium /Flags 6 /FontBBox [-92 -250 1010 898] "
        b"/ItalicAngle 0 /Ascent 880 /Descent -120 /CapHeight 720 /StemV 80 >>")
    cid_font = w.add(
        b"<< /Type /Font /Subtype /CIDFontType0 /BaseFont /HYSMyeongJo-Medium "
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Korea1) /Supplement 2 >> "
        b"/FontDescriptor %d 0 R /DW 1000 >>" % descriptor)
    type0 = w.add(b"<< /Type /Font /Subtype /Type0 /BaseFont /HYSMyeongJo-Medium /Encoding /UniKS-UCS2-H "
                  b"/DescendantFonts [%d 0 R] >>" % cid_font)
    helv = g.simple_font(w)
    c = w.stream(b"", content(
        f"BT /F0 24 Tf 72 720 Td {g.ucs2('안녕하세요, 시스템 폰트입니다.').decode()} Tj ET",
        f"BT /F0 14 Tf 72 680 Td {g.ucs2('비임베딩 한글 CID 폰트 (Adobe-Korea1, UniKS-UCS2-H)').decode()} Tj ET",
        _label(72, 640, 12, "Non-embedded Korean font: rendering depends on system fonts."),
    ))
    p = g.page(w, pages, g.A4, c, b"<< /Font << /F0 %d 0 R /F1 %d 0 R >> >>" % (type0, helv))
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "한글 시스템 폰트 테스트")
    return w.write(root=root, info=info, file_id=g.file_id_for("korean-system-font.pdf"))


# ----------------------------------------------------------------- 4~6. 암호


def make_encrypted_rc4_user() -> bytes:
    return make_simple(encrypt=g.Encryption("user", "owner"), title="Encrypted RC4 user",
                       file_id_name="encrypted-rc4-user.pdf")


def make_encrypted_owner_only() -> bytes:
    # 사용자 암호 없음, 소유자 암호만. 인쇄·수정·복사 금지 (P = -3904)
    return make_simple(encrypt=g.Encryption("", "owner", permissions=-3904), title="Encrypted owner only",
                       file_id_name="encrypted-owner-only.pdf")


# ----------------------------------------------------------------- 7. scanned


def make_scanned() -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    width, height = 306, 396
    kids = []
    for page_no in range(3):
        rnd = g.lcg(1000 + page_no)
        px = bytearray(width * height)
        for y in range(height):
            # 텍스트 줄처럼 보이는 어두운 띠: 24px 간격, 위아래 여백
            in_band = 40 <= y <= height - 40 and (y % 24) < 9
            for x in range(width):
                r = next(rnd)
                base = 236 + (r % 20)  # 종이 톤 노이즈
                if in_band and 30 <= x <= width - 30 and (r % 7) < 5:
                    base = 40 + (r % 60)  # 글자 조각
                px[y * width + x] = min(255, base)
        img = w.stream(b"/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8"
                       % (width, height), bytes(px))
        c = w.stream(b"", content("q 612 0 0 792 0 0 cm /Im1 Do Q"))
        kids.append(g.page(w, pages, g.LETTER, c, b"<< /XObject << /Im1 %d 0 R >> >>" % img))
    g.pages_tree(w, pages, kids)
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Scanned document (image only)")
    return w.write(root=root, info=info, file_id=g.file_id_for("scanned.pdf"))


# ----------------------------------------------------------------- 8. form-acro


def make_form_acro() -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    zadb = w.add(b"<< /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >>")
    page_num = w.reserve()
    # AP 스트림은 /DR 을 보지 않으므로 체크 표시용 ZaDb 도 AP 리소스에 넣는다
    res_font = b"<< /Font << /Helv %d 0 R /ZaDb %d 0 R >> >>" % (helv, zadb)

    def ap(width: float, height: float, *ops: str) -> int:
        return w.stream(b"/Type /XObject /Subtype /Form /BBox [0 0 %s %s] /Resources %s"
                        % (fmt(width).encode(), fmt(height).encode(), res_font), content(*ops))

    fields = []
    # 텍스트 필드
    ap_tx = ap(200, 24, "0.9 0.9 1 rg 0 0 200 24 re f", "0 G 0.5 w 0.25 0.25 199.5 23.5 re S",
               "/Tx BMC BT /Helv 12 Tf 0 g 4 7 Td (Hong Gil-dong) Tj ET EMC")
    fields.append(w.add(
        b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (Hong Gil-dong) /DA (/Helv 12 Tf 0 g) "
        b"/Rect [150 680 350 704] /F 4 /P %d 0 R /AP << /N %d 0 R >> >>" % (page_num, ap_tx)))
    # 체크박스
    ap_on = ap(18, 18, "0 G 1 w 0.5 0.5 17 17 re S", "0 g BT /ZaDb 14 Tf 3 3 Td (4) Tj ET")
    ap_off = ap(18, 18, "0 G 1 w 0.5 0.5 17 17 re S")
    fields.append(w.add(
        b"<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Yes /AS /Yes /DA (/ZaDb 0 Tf 0 g) "
        b"/Rect [150 640 168 658] /F 4 /P %d 0 R /AP << /N << /Yes %d 0 R /Off %d 0 R >> >> >>"
        % (page_num, ap_on, ap_off)))
    # 라디오 (부모 필드 + 위젯 2개)
    radio = w.reserve()
    ap_ron = ap(16, 16, "0 G 1 w 8 1 m 11.9 1 15 4.1 15 8 c 15 11.9 11.9 15 8 15 c 4.1 15 1 11.9 1 8 c 1 4.1 4.1 1 8 1 c s",
                "0 g 8 4 m 10.2 4 12 5.8 12 8 c 12 10.2 10.2 12 8 12 c 5.8 12 4 10.2 4 8 c 4 5.8 5.8 4 8 4 c f")
    ap_roff = ap(16, 16, "0 G 1 w 8 1 m 11.9 1 15 4.1 15 8 c 15 11.9 11.9 15 8 15 c 4.1 15 1 11.9 1 8 c 1 4.1 4.1 1 8 1 c s")
    r1 = w.add(b"<< /Type /Annot /Subtype /Widget /Parent %d 0 R /AS /A /Rect [150 600 166 616] /F 4 /P %d 0 R "
               b"/AP << /N << /A %d 0 R /Off %d 0 R >> >> >>" % (radio, page_num, ap_ron, ap_roff))
    r2 = w.add(b"<< /Type /Annot /Subtype /Widget /Parent %d 0 R /AS /Off /Rect [250 600 266 616] /F 4 /P %d 0 R "
               b"/AP << /N << /B %d 0 R /Off %d 0 R >> >> >>" % (radio, page_num, ap_ron, ap_roff))
    w.set(radio, b"<< /FT /Btn /Ff 49152 /T (choice) /V /A /Kids [%d 0 R %d 0 R] /DA (/ZaDb 0 Tf 0 g) >>" % (r1, r2))
    fields.append(radio)
    # 콤보
    ap_ch = ap(160, 24, "1 g 0 0 160 24 re f", "0 G 0.5 w 0.25 0.25 159.5 23.5 re S",
               "BT /Helv 12 Tf 0 g 4 7 Td (Seoul) Tj ET")
    fields.append(w.add(
        b"<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (city) /V (Seoul) /Opt [(Seoul) (Busan) (Daegu)] "
        b"/DA (/Helv 12 Tf 0 g) /Rect [150 550 310 574] /F 4 /P %d 0 R /AP << /N %d 0 R >> >>" % (page_num, ap_ch)))

    c = w.stream(b"", content(
        _label(72, 740, 18, "AcroForm test"),
        _label(72, 686, 12, "Name:"), _label(72, 645, 12, "Agree:"),
        _label(72, 604, 12, "Choice:"), _label(176, 604, 12, "A"), _label(276, 604, 12, "B"),
        _label(72, 557, 12, "City:"),
    ))
    annots = [fields[0], fields[1], r1, r2, fields[3]]
    g.page(w, pages, g.LETTER, c, b"<< /Font << /F1 %d 0 R >> >>" % helv, annots=annots, num=page_num)
    g.pages_tree(w, pages, [page_num])
    acro = (b" /AcroForm << /Fields [" + b" ".join(b"%d 0 R" % f for f in fields)
            + b"] /DR << /Font << /Helv %d 0 R /ZaDb %d 0 R >> >> /DA (/Helv 0 Tf 0 g) /NeedAppearances false >>"
            % (helv, zadb))
    root = g.catalog(w, pages, acro)
    info = g.info_dict(w, "AcroForm fields")
    return w.write(root=root, info=info, file_id=g.file_id_for("form-acro.pdf"))


# ----------------------------------------------------------------- 9. bookmarks


def make_bookmarks(page_count: int = 10) -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    kids = []
    for i in range(page_count):
        c = w.stream(b"", content(_label(72, 720, 20, f"Bookmarks test - page {i + 1} of {page_count}"),
                                  _border(*g.LETTER)))
        kids.append(g.page(w, pages, g.LETTER, c, _helv_resources(helv)))
    g.pages_tree(w, pages, kids)

    # 3단계 트리: 1장(1쪽) > 1.1절(2쪽) > 1.1.1항(3쪽) …  총 3장 × 2절 × 1항
    outlines = w.reserve()
    items: list[dict] = []  # 각 항목: num, parent, page, title, children

    def dest(page_index: int) -> bytes:
        return b"[%d 0 R /XYZ 0 792 0]" % kids[page_index]

    chapter_nums = []
    page_index = 0
    for ch in range(3):
        ch_num = w.reserve()
        sec_nums = []
        ch_page = page_index
        page_index += 1
        for sec in range(2):
            sec_num = w.reserve()
            sec_page = page_index
            page_index += 1
            sub_num = w.reserve()
            w.set(sub_num, b"<< /Title " + text_string(f"{ch + 1}.{sec + 1}.1 항 (page {min(sec_page + 1, page_count - 1) + 1})")
                  + b" /Parent %d 0 R /Dest %s >>" % (sec_num, dest(min(sec_page + 1, page_count - 1))))
            w.set(sec_num, b"<< /Title " + text_string(f"{ch + 1}.{sec + 1} 절 (page {sec_page + 1})")
                  + b" /Parent %d 0 R /First %d 0 R /Last %d 0 R /Count 1 /Dest %s >>"
                  % (ch_num, sub_num, sub_num, dest(sec_page)))
            sec_nums.append(sec_num)
        # 형제 연결
        for a, b in zip(sec_nums, sec_nums[1:]):
            w.set(a, w.objects[a - 1][:-2] + b" /Next %d 0 R >>" % b)
            w.set(b, w.objects[b - 1][:-2] + b" /Prev %d 0 R >>" % a)
        w.set(ch_num, b"<< /Title " + text_string(f"제 {ch + 1} 장 (page {ch_page + 1})")
              + b" /Parent %d 0 R /First %d 0 R /Last %d 0 R /Count 4 /Dest %s >>"  # 절 2 + 항 2
              % (outlines, sec_nums[0], sec_nums[-1], dest(ch_page)))
        chapter_nums.append(ch_num)
    for a, b in zip(chapter_nums, chapter_nums[1:]):
        w.set(a, w.objects[a - 1][:-2] + b" /Next %d 0 R >>" % b)
        w.set(b, w.objects[b - 1][:-2] + b" /Prev %d 0 R >>" % a)
    # /Count = 열린 상태로 보이는 자손 총수: 장 3 + 절 6 + 항 6 = 15
    w.set(outlines, b"<< /Type /Outlines /First %d 0 R /Last %d 0 R /Count 15 >>" % (chapter_nums[0], chapter_nums[-1]))
    root = g.catalog(w, pages, b" /Outlines %d 0 R /PageMode /UseOutlines" % outlines)
    info = g.info_dict(w, "북마크 테스트")
    return w.write(root=root, info=info, file_id=g.file_id_for("bookmarks.pdf"))


# ----------------------------------------------------------------- 10. large-1000


def make_large(page_count: int = 1000) -> bytes:
    w = PdfWriter(version="1.5")
    pages = w.reserve()
    helv = g.simple_font(w)
    kids = []
    for i in range(page_count):
        c = w.stream(b"", content(_label(72, 720, 18, f"Page {i + 1} / {page_count}"),
                                  f"0.85 g 72 {700 - (i % 50) * 12} 300 8 re f"))
        kids.append(g.page(w, pages, g.LETTER, c, _helv_resources(helv)))
    g.pages_tree(w, pages, kids)
    root = g.catalog(w, pages)
    info = g.info_dict(w, f"Large document ({page_count} pages)")
    return w.write(root=root, info=info, file_id=g.file_id_for("large-1000.pdf"), xref_stream=True)


# ----------------------------------------------------------------- 11. mixed-sizes


def make_mixed_sizes() -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    specs = [
        ("A4 portrait", g.A4, 0), ("A4 landscape", (g.A4[1], g.A4[0]), 0), ("Letter", g.LETTER, 0),
        ("A5", g.A5, 0), ("Business card 252x144", (252, 144), 0),
        ("A4 /Rotate 90", g.A4, 90), ("Letter /Rotate 270", g.LETTER, 270),
    ]
    kids = []
    for label, size, rotate in specs:
        c = w.stream(b"", content(_label(24, size[1] - 40, 14, f"{label}: {fmt(size[0])} x {fmt(size[1])} pt"),
                                  _border(*size, inset=12), "0.9 0.3 0.1 rg 24 24 40 40 re f"))
        kids.append(g.page(w, pages, size, c, _helv_resources(helv), rotate=rotate))
    g.pages_tree(w, pages, kids)
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Mixed page sizes and rotations")
    return w.write(root=root, info=info, file_id=g.file_id_for("mixed-sizes.pdf"))


# ----------------------------------------------------------------- 12. xref-stream


def make_xref_stream() -> bytes:
    return make_simple(xref_stream=True, title="xref stream test", file_id_name="xref-stream.pdf")


# ----------------------------------------------------------------- 13. incremental-update


def make_incremental_update() -> bytes:
    base = make_simple(title="Incremental update base", file_id_name="incremental-update.pdf")
    # make_simple 의 객체 배치: 1 pages, 2 font, 3 c1, 4 p1, 5 c2, 6 p2, 7 catalog, 8 info
    new_content = 9
    new_page = 10
    c = content("BT /F1 24 Tf 72 700 Td (Page 3 added by incremental update) Tj ET", "0.8 0 0.8 rg 72 400 200 200 re f")
    objects = {
        new_content: b"<< /Length %d >>\nstream\n" % len(c) + c + b"\nendstream",
        new_page: b"<< /Type /Page /Parent 1 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 2 0 R >> >> >>" % new_content,
        1: b"<< /Type /Pages /Kids [4 0 R 6 0 R %d 0 R] /Count 3 >>" % new_page,
    }
    return PdfWriter.append_update(base, objects, root=7, info=8, size=11,
                                   file_id=g.file_id_for("incremental-update.pdf"))


# ----------------------------------------------------------------- 14. annotations


def make_annotations() -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    page_num = w.reserve()
    page2_num = w.reserve()
    # 하이라이트는 Multiply 블렌드로 글자가 비치게 한다 (Acrobat 과 같은 방식)
    gs = w.add(b"<< /Type /ExtGState /BM /Multiply >>")
    hl_ap = w.stream(b"/Type /XObject /Subtype /Form /BBox [70 696 330 722] /Resources << /ExtGState << /GS0 %d 0 R >> >>" % gs,
                     content("/GS0 gs 1 1 0 rg 70 696 260 26 re f"))
    annots = [
        w.add(b"<< /Type /Annot /Subtype /Highlight /Rect [70 696 330 722] /QuadPoints [70 722 330 722 70 696 330 696] "
              b"/C [1 1 0] /CA 1 /F 4 /P %d 0 R /Contents " % page_num + text_string("하이라이트 메모")
              + b" /AP << /N %d 0 R >> >>" % hl_ap),
        w.add(b"<< /Type /Annot /Subtype /Square /Rect [72 480 300 620] /C [1 0 0] /IC [1 0.9 0.9] /CA 0.8 "
              b"/BS << /W 3 >> /F 4 /P %d 0 R /Contents (red square) >>" % page_num),
        w.add(b"<< /Type /Annot /Subtype /Text /Rect [340 580 360 600] /C [1 0.8 0] /Name /Comment /Open false "
              b"/F 28 /P %d 0 R /T " % page_num + text_string("작성자") + b" /Contents " + text_string("메모 주석입니다") + b" >>"),
        w.add(b"<< /Type /Annot /Subtype /Link /Rect [72 400 300 420] /Border [0 0 1] /C [0 0 1] "
              b"/A << /S /URI /URI (https://github.com/Edgar-J-Shin/OnPdf) >> /F 4 /P %d 0 R >>" % page_num),
        # 내부 링크: 2쪽의 (72, 700) 으로 (/XYZ 위치 포함)
        w.add(b"<< /Type /Annot /Subtype /Link /Rect [72 360 300 380] /Border [0 0 1] /C [0 0.5 0] "
              b"/Dest [%d 0 R /XYZ 72 700 0] /F 4 /P %d 0 R >>" % (page2_num, page_num)),
        # 차단되어야 하는 스킴 (보안 테스트)
        w.add(b"<< /Type /Annot /Subtype /Link /Rect [72 320 300 340] /Border [0 0 0] "
              b"/A << /S /URI /URI (javascript:alert(1)) >> /F 4 /P %d 0 R >>" % page_num),
    ]
    c = w.stream(b"", content(
        _label(72, 740, 18, "Annotations test"),
        _label(72, 702, 14, "This sentence is highlighted in yellow."),
        _label(72, 460, 11, "Square annotation above, text (note) annotation to the right."),
        "0 0 1 rg " + _label(72, 406, 12, "Link annotation: github.com/Edgar-J-Shin/OnPdf"),
        "0 0.5 0 rg " + _label(72, 366, 12, "Internal link: go to page 2"),
        "0.5 g " + _label(72, 326, 12, "Blocked link (javascript: scheme)"),
    ))
    g.page(w, pages, g.LETTER, c, _helv_resources(helv), annots=annots, num=page_num)
    c2 = w.stream(b"", content(_label(72, 700, 18, "Link target: page 2"), _label(72, 670, 12, "The internal link lands here.")))
    g.page(w, pages, g.LETTER, c2, _helv_resources(helv), num=page2_num)
    g.pages_tree(w, pages, [page_num, page2_num])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Annotations")
    return w.write(root=root, info=info, file_id=g.file_id_for("annotations.pdf"))


# ----------------------------------------------------------------- 18~20


def make_empty_page() -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    p = g.page(w, pages, g.LETTER, None)
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Empty page")
    return w.write(root=root, info=info, file_id=g.file_id_for("empty-page.pdf"))


def make_unicode_metadata() -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    c = w.stream(b"", content(_label(72, 720, 16, "Unicode metadata test (see document properties)")))
    p = g.page(w, pages, g.LETTER, c, _helv_resources(helv))
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = w.add(b"<< /Title " + text_string("한글 제목 — Unicode 메타데이터") + b" /Author " + text_string("홍길동")
                 + b" /Subject " + text_string("주제: 유니코드") + b" /Keywords " + text_string("한글, PDF, 테스트")
                 + b" /Producer (onpdf pdfgen) /CreationDate (D:20260101000000Z) >>")
    return w.write(root=root, info=info, file_id=g.file_id_for("unicode-metadata.pdf"))


def make_transparency() -> bytes:
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    gs = w.add(b"<< /Type /ExtGState /CA 0.5 /ca 0.5 >>")
    k = 0.5523

    def circle(cx: float, cy: float, r: float) -> str:
        return (f"{fmt(cx + r)} {fmt(cy)} m "
                f"{fmt(cx + r)} {fmt(cy + k * r)} {fmt(cx + k * r)} {fmt(cy + r)} {fmt(cx)} {fmt(cy + r)} c "
                f"{fmt(cx - k * r)} {fmt(cy + r)} {fmt(cx - r)} {fmt(cy + k * r)} {fmt(cx - r)} {fmt(cy)} c "
                f"{fmt(cx - r)} {fmt(cy - k * r)} {fmt(cx - k * r)} {fmt(cy - r)} {fmt(cx)} {fmt(cy - r)} c "
                f"{fmt(cx + k * r)} {fmt(cy - r)} {fmt(cx + r)} {fmt(cy - k * r)} {fmt(cx + r)} {fmt(cy)} c f")

    c = w.stream(b"", content(
        _label(72, 740, 18, "Transparency test (ca = 0.5)"),
        "q /GS0 gs",
        "1 0 0 rg " + circle(240, 520, 110),
        "0 0.7 0 rg " + circle(340, 520, 110),
        "0 0 1 rg " + circle(290, 430, 110),
        "Q",
    ))
    p = g.page(w, pages, g.LETTER, c, b"<< /Font << /F1 %d 0 R >> /ExtGState << /GS0 %d 0 R >> >>" % (helv, gs))
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Transparency")
    return w.write(root=root, info=info, file_id=g.file_id_for("transparency.pdf"))
