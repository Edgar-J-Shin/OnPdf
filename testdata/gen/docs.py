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


# ----------------------------------------------------------------- 8b. form-fields (#51)


def make_form_fields() -> bytes:
    """폼 채우기 테스트: 한 줄·여러 줄·읽기 전용·MaxLen 텍스트, 체크박스, 기본 선택 없는 라디오, 편집 가능 콤보,
    다중 선택 목록. 2쪽(/Rotate 90)에 이름 필드의 둘째 위젯과 서명 필드."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    zadb = w.add(b"<< /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >>")
    p1, p2 = w.reserve(), w.reserve()
    res_font = b"<< /Font << /Helv %d 0 R /ZaDb %d 0 R >> >>" % (helv, zadb)
    mk = b"/MK << /BG [0.95 0.95 1] /BC [0 0 0] >>"

    def ap(width: float, height: float, *ops: str) -> int:
        return w.stream(b"/Type /XObject /Subtype /Form /BBox [0 0 %s %s] /Resources %s"
                        % (fmt(width).encode(), fmt(height).encode(), res_font), content(*ops))

    def text_ap(width: float, height: float, value: str = "") -> int:
        ops = ["0.95 0.95 1 rg 0 0 %s %s re f" % (fmt(width), fmt(height)),
               "0 G 0.5 w 0.25 0.25 %s %s re S" % (fmt(width - 0.5), fmt(height - 0.5))]
        if value:
            ops.append("/Tx BMC BT /Helv 12 Tf 0 g 3 %s Td %s Tj ET EMC" % (fmt(height / 2 - 4), g.lit(value).decode("latin-1")))
        return ap(width, height, *ops)

    def rect(x0: float, y0: float, x1: float, y1: float) -> bytes:
        return b"/Rect [%s %s %s %s]" % tuple(fmt(v).encode() for v in (x0, y0, x1, y1))

    fields: list[int] = []
    p1_annots: list[int] = []
    p2_annots: list[int] = []
    # 이름: 부모 필드 + 위젯 2개 (1쪽, 2쪽) — 같은 값을 공유한다
    name = w.reserve()
    n1 = w.add(b"<< /Type /Annot /Subtype /Widget /Parent %d 0 R %s /F 4 /P %d 0 R %s /AP << /N %d 0 R >> >>"
               % (name, rect(150, 690, 350, 714), p1, mk, text_ap(200, 24, "Hong")))
    n2 = w.add(b"<< /Type /Annot /Subtype /Widget /Parent %d 0 R %s /F 4 /P %d 0 R %s /AP << /N %d 0 R >> >>"
               % (name, rect(100, 500, 300, 524), p2, mk, text_ap(200, 24, "Hong")))
    w.set(name, b"<< /FT /Tx /T (name) /V (Hong) /DA (/Helv 12 Tf 0 g) /Kids [%d 0 R %d 0 R] >>" % (n1, n2))
    fields.append(name)
    p1_annots.append(n1)
    p2_annots.append(n2)

    def text_field(title: str, r: tuple[float, float, float, float], extra: bytes = b"", value: str = "") -> int:
        v = b" /V " + g.lit(value) if value else b""
        num = w.add(b"<< /Type /Annot /Subtype /Widget /FT /Tx /T %s%s /DA (/Helv 12 Tf 0 g) %s /F 4 /P %d 0 R %s%s "
                    b"/AP << /N %d 0 R >> >>"
                    % (g.lit(title), v, rect(*r), p1, mk, extra, text_ap(r[2] - r[0], r[3] - r[1], value)))
        fields.append(num)
        p1_annots.append(num)
        return num

    text_field("address", (150, 610, 450, 670), b" /Ff 4096")
    text_field("empid", (150, 570, 300, 594), b" /Ff 1", "A-1024")
    text_field("zip", (150, 530, 230, 554), b" /MaxLen 5")
    # 동의 (체크박스, 꺼짐)
    on = ap(18, 18, "0 G 1 w 0.5 0.5 17 17 re S", "0 g BT /ZaDb 14 Tf 3 3 Td (4) Tj ET")
    off = ap(18, 18, "0 G 1 w 0.5 0.5 17 17 re S")
    agree = w.add(b"<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /AS /Off /DA (/ZaDb 0 Tf 0 g) %s /F 4 "
                  b"/P %d 0 R /MK << /CA (4) >> /AP << /N << /Yes %d 0 R /Off %d 0 R >> >> >>"
                  % (rect(150, 490, 168, 508), p1, on, off))
    fields.append(agree)
    p1_annots.append(agree)
    # 성별 (라디오 3개, 기본 선택 없음, NoToggleToOff)
    ring = "0 G 1 w 8 1 m 11.9 1 15 4.1 15 8 c 15 11.9 11.9 15 8 15 c 4.1 15 1 11.9 1 8 c 1 4.1 4.1 1 8 1 c s"
    dot = "0 g 8 4 m 10.2 4 12 5.8 12 8 c 12 10.2 10.2 12 8 12 c 5.8 12 4 10.2 4 8 c 4 5.8 5.8 4 8 4 c f"
    r_on, r_off = ap(16, 16, ring, dot), ap(16, 16, ring)
    gender = w.reserve()
    kids = []
    for i, state in enumerate(("M", "F", "X")):
        x = 150 + i * 80
        k = w.add(b"<< /Type /Annot /Subtype /Widget /Parent %d 0 R /AS /Off %s /F 4 /P %d 0 R /MK << /CA (l) >> "
                  b"/AP << /N << /%s %d 0 R /Off %d 0 R >> >> >>"
                  % (gender, rect(x, 450, x + 16, 466), p1, state.encode(), r_on, r_off))
        kids.append(k)
        p1_annots.append(k)
    w.set(gender, b"<< /FT /Btn /Ff 49152 /T (gender) /DA (/ZaDb 0 Tf 0 g) /Kids [%s] >>"
          % b" ".join(b"%d 0 R" % k for k in kids))
    fields.append(gender)
    # 도시 (편집 가능 콤보)
    city = w.add(b"<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 393216 /T (city) /V (Seoul) "
                 b"/Opt [(Seoul) (Busan) (Daegu)] /DA (/Helv 12 Tf 0 g) %s /F 4 /P %d 0 R %s /AP << /N %d 0 R >> >>"
                 % (rect(150, 400, 310, 424), p1, mk, text_ap(160, 24, "Seoul")))
    fields.append(city)
    p1_annots.append(city)
    # 과목 (다중 선택 목록, 선택 없음)
    subjects = w.add(b"<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 2097152 /T (subjects) "
                     b"/Opt [(Math) (Science) (Art) (Music)] /DA (/Helv 12 Tf 0 g) %s /F 4 /P %d 0 R %s "
                     b"/AP << /N %d 0 R >> >>" % (rect(150, 300, 310, 380), p1, mk, text_ap(160, 80)))
    fields.append(subjects)
    p1_annots.append(subjects)
    # 2쪽 서명 칸 (채우지 않는다)
    sig = w.add(b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (signature) %s /F 4 /P %d 0 R >>"
                % (rect(100, 400, 300, 450), p2))
    fields.append(sig)
    p2_annots.append(sig)

    c1 = w.stream(b"", content(
        _label(72, 740, 18, "Form fields test"),
        _label(72, 696, 12, "Name:"), _label(72, 650, 12, "Address:"), _label(72, 576, 12, "Employee:"),
        _label(72, 536, 12, "Zip:"), _label(72, 494, 12, "Agree:"), _label(72, 454, 12, "Gender:"),
        _label(170, 454, 12, "M"), _label(250, 454, 12, "F"), _label(330, 454, 12, "X"),
        _label(72, 406, 12, "City:"), _label(72, 366, 12, "Subjects:"),
    ))
    g.page(w, pages, g.LETTER, c1, b"<< /Font << /F1 %d 0 R >> >>" % helv, annots=p1_annots, num=p1)
    c2 = w.stream(b"", content(_label(72, 740, 18, "Page 2 (rotated)"), _label(30, 506, 12, "Name:"),
                               _label(30, 420, 12, "Sign:")))
    g.page(w, pages, g.LETTER, c2, b"<< /Font << /F1 %d 0 R >> >>" % helv, rotate=90, annots=p2_annots, num=p2)
    g.pages_tree(w, pages, [p1, p2])
    acro = (b" /AcroForm << /Fields [" + b" ".join(b"%d 0 R" % f for f in fields)
            + b"] /DR << /Font << /Helv %d 0 R /ZaDb %d 0 R >> >> /DA (/Helv 0 Tf 0 g) /NeedAppearances false >>"
            % (helv, zadb))
    root = g.catalog(w, pages, acro)
    info = g.info_dict(w, "Form fields")
    return w.write(root=root, info=info, file_id=g.file_id_for("form-fields.pdf"))


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


def make_annotation_popup() -> bytes:
    """외부 프로그램이 만든 메모처럼 Text 주석에 /Popup 이 연결된 문서. 메모를 지울 때 Popup 도 함께 지워야 한다."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    page_num = w.reserve()
    text_num = w.reserve()
    popup_num = w.reserve()
    w.set(text_num, b"<< /Type /Annot /Subtype /Text /Rect [340 580 360 600] /C [1 0.8 0] /Name /Comment /Open true "
          b"/F 28 /P %d 0 R /Popup %d 0 R /T " % (page_num, popup_num) + text_string("작성자")
          + b" /Contents " + text_string("팝업이 있는 메모") + b" >>")
    w.set(popup_num, b"<< /Type /Annot /Subtype /Popup /Rect [370 500 560 600] /Open true /F 28 /P %d 0 R /Parent %d 0 R >>"
          % (page_num, text_num))
    c = w.stream(b"", content(_label(72, 740, 18, "Annotation with popup"),
                              _label(72, 700, 12, "The note on the right has a linked Popup annotation.")))
    g.page(w, pages, g.LETTER, c, _helv_resources(helv), annots=[text_num, popup_num], num=page_num)
    g.pages_tree(w, pages, [page_num])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Annotation popup")
    return w.write(root=root, info=info, file_id=g.file_id_for("annotation-popup.pdf"))


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


def make_text_layout() -> bytes:
    """줄 묶음 휴리스틱 회귀용: 회전 텍스트 매트릭스, `1 Tf` + Tm 스케일, 위첨자, 2단 배치."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    c = w.stream(b"", content(
        "BT /F1 12 Tf 1 0 0 1 72 700 Tm (Normal line of text) Tj ET",
        # 90도 회전 텍스트 매트릭스: 글이 위로 흐른다 (세로 축 라벨)
        "BT /F1 12 Tf 0 1 -1 0 520 300 Tm (Rotated text matrix) Tj ET",
        # Cairo/Skia 식: 1 Tf + Tm 스케일 12, 위첨자는 8 스케일로 4pt 올림
        "BT /F1 1 Tf 12 0 0 12 72 600 Tm (E=mc) Tj ET",
        "BT /F1 1 Tf 8 0 0 8 103 604 Tm (2) Tj ET",
        # 같은 기준선의 두 단 (x 가 크게 뒤로 감 → 새 줄이 아니라 같은 줄로 취급되면 안 됨: 두 번째 Tj 가 앞 x 로 돌아감)
        "BT /F1 12 Tf 1 0 0 1 300 500 Tm (Right column) Tj ET",
        "BT /F1 12 Tf 1 0 0 1 72 500 Tm (Left column) Tj ET",
    ))
    p = g.page(w, pages, g.LETTER, c, _helv_resources(helv))
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Text layout heuristics")
    return w.write(root=root, info=info, file_id=g.file_id_for("text-layout.pdf"))


def make_print_flags() -> bytes:
    """인쇄 플래그 주석 + 문서 인쇄 기본값.
    녹색 상자: /F 0 (Print 비트 없음) → 화면에만 보이고 인쇄되지 않는다.
    빨간 상자: /F 36 (Print + NoView) → 화면에 안 보이고 인쇄에만 나온다.
    /ViewerPreferences: PrintScaling None, NumCopies 2, PrintPageRange [2 2](1 기준, 2쪽만), 긴 쪽 양면."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    page_nums = [w.reserve(), w.reserve()]

    def box_ap(rect: tuple[int, int, int, int], rgb: str) -> int:
        x0, y0, x1, y1 = rect
        return w.stream(b"/Type /XObject /Subtype /Form /BBox [%d %d %d %d]" % rect,
                        content(f"{rgb} rg {x0} {y0} {x1 - x0} {y1 - y0} re f"))

    screen_rect = (72, 520, 272, 640)
    print_rect = (340, 520, 540, 640)
    annots = [
        w.add(b"<< /Type /Annot /Subtype /Square /Rect [%d %d %d %d] /F 0 /P %d 0 R /Contents (screen only) "
              b"/AP << /N %d 0 R >> >>" % (*screen_rect, page_nums[0], box_ap(screen_rect, "0 0.6 0"))),
        w.add(b"<< /Type /Annot /Subtype /Square /Rect [%d %d %d %d] /F 36 /P %d 0 R /Contents (print only) "
              b"/AP << /N %d 0 R >> >>" % (*print_rect, page_nums[0], box_ap(print_rect, "0.85 0 0"))),
    ]
    c1 = w.stream(b"", content(_label(72, 720, 20, "Print flags test - page 1"),
                               _label(72, 650, 12, "Left: SCREEN ONLY (green)"),
                               _label(340, 650, 12, "Right: PRINT ONLY (red)")))
    c2 = w.stream(b"", content(_label(72, 720, 20, "Print flags test - page 2 (PrintPageRange)")))
    kids = [g.page(w, pages, g.LETTER, c1, _helv_resources(helv), annots=annots, num=page_nums[0]),
            g.page(w, pages, g.LETTER, c2, _helv_resources(helv), num=page_nums[1])]
    g.pages_tree(w, pages, kids)
    root = g.catalog(w, pages, b" /ViewerPreferences << /PrintScaling /None /NumCopies 2 /PrintPageRange [2 2] "
                               b"/Duplex /DuplexFlipLongEdge >>")
    info = g.info_dict(w, "Print flags and viewer preferences")
    return w.write(root=root, info=info, file_id=g.file_id_for("print-flags.pdf"))


def make_print_low_res() -> bytes:
    # 소유자 암호만, 인쇄는 허용하되 고품질 인쇄(비트 12) 금지 → 저해상도 인쇄만 (P = -2052)
    return make_simple(encrypt=g.Encryption("", "owner", permissions=-2052), title="Print low resolution only",
                       file_id_name="print-low-res.pdf")


def make_unicode_map_error() -> bytes:
    """ToUnicode 없는 Type3 글꼴 + 표준 글리프 이름 목록(AGL) 밖 이름(/g1 /g2) → PDFium 유니코드 매핑 오류.
    글자 코드 65·66 을 그린다. PDFium 값은 "ABAB" 지만 뷰어는 매핑 오류 글자를 U+FFFD 로 추출한다 (#12 C1)."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    g1 = w.stream(b"", content("600 0 0 0 500 700 d1", "50 0 400 700 re f"))
    g2 = w.stream(b"", content("600 0 0 0 500 700 d1", "50 0 400 350 re f"))
    t3 = w.add(b"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 600 700] /FontMatrix [0.001 0 0 0.001 0 0] "
               b"/CharProcs << /g1 %d 0 R /g2 %d 0 R >> /Encoding << /Type /Encoding /Differences [65 /g1 /g2] >> "
               b"/FirstChar 65 /LastChar 66 /Widths [600 600] /Resources << >> >>" % (g1, g2))
    c = w.stream(b"", content(_label(72, 740, 14, "Unicode map error test (Type3, no ToUnicode)"),
                              "BT /T3 24 Tf 72 680 Td (ABAB) Tj ET"))
    kids = [g.page(w, pages, g.LETTER, c, b"<< /Font << /F1 %d 0 R /T3 %d 0 R >> >>" % (helv, t3))]
    g.pages_tree(w, pages, kids)
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Unicode map error (Type3 without ToUnicode)")
    return w.write(root=root, info=info, file_id=g.file_id_for("unicode-map-error.pdf"))


def make_signed_dummy() -> bytes:
    """전자서명 필드 1개(가짜 서명값). 암호학적으로 유효하지 않지만 PDFium 의 서명 수(FPDF_GetSignatureCount)는 1.
    뷰어는 서명 문서를 증분 저장해 원본 바이트를 보존해야 한다 (#31)."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    page_num = w.reserve()
    sig_value = w.add(b"<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached "
                      b"/ByteRange [0 0 0 0] /Contents <" + b"00" * 64 + b"> /M (D:20260101000000Z) >>")
    widget = w.add(b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature1) /V %d 0 R /F 132 "
                   b"/Rect [0 0 0 0] /P %d 0 R >>" % (sig_value, page_num))
    c = w.stream(b"", content(_label(72, 720, 18, "Signed document (dummy signature)"),
                              _label(72, 690, 12, "Saving must be incremental to keep the signature bytes.")))
    kids = [g.page(w, pages, g.LETTER, c, _helv_resources(helv), annots=[widget], num=page_num)]
    g.pages_tree(w, pages, kids)
    root = g.catalog(w, pages, b" /AcroForm << /Fields [%d 0 R] /SigFlags 3 >>" % widget)
    info = g.info_dict(w, "Signed document (dummy signature)")
    return w.write(root=root, info=info, file_id=g.file_id_for("signed-dummy.pdf"))


def make_perm_modify_only() -> bytes:
    """내용 수정(비트 4)만 허용, 주석(비트 6)·조립 없음 — 텍스트 도구 게이트 테스트 (#45)."""
    return make_simple(encrypt=g.Encryption("", "owner", permissions=-3896), title="Permissions: modify contents only",
                       file_id_name="perm-modify-only.pdf")


def make_perm_assemble_only() -> bytes:
    # 소유자 암호만(R3). P=-12: 내용 수정(비트 4)만 금지, 쪽 조립(비트 11)·주석(비트 6) 허용 → 쪽 회전 가능
    return make_simple(encrypt=g.Encryption("", "owner", permissions=-12), title="Permissions: assemble only",
                       file_id_name="perm-assemble-only.pdf")


def make_perm_no_assemble() -> bytes:
    # 소유자 암호만(R3). P=-1028: 쪽 조립(비트 11)만 금지 → 쪽 회전 불가 (내용 수정 비트 4 는 허용)
    return make_simple(encrypt=g.Encryption("", "owner", permissions=-1028), title="Permissions: no assembly",
                       file_id_name="perm-no-assemble.pdf")


def make_perm_no_extract() -> bytes:
    # 소유자 암호만(R3). P=-17: 복사·추출(비트 5)만 금지 → 다른 문서로 쪽을 가져가는 삽입 소스가 될 수 없다
    return make_simple(encrypt=g.Encryption("", "owner", permissions=-17), title="Permissions: no extraction",
                       file_id_name="perm-no-extract.pdf")


def make_outline_cycle() -> bytes:
    """악성 목차: 형제 순환(B /Next A), 자식 자기 참조(A /First A), 깊이 40 체인(B 아래).
    뷰어는 무한 루프·스택 오버플로 없이 유한한 항목만 보여야 한다."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    kids = []
    for i in range(2):
        c = w.stream(b"", content(_label(72, 720, 18, f"Outline cycle test - page {i + 1}")))
        kids.append(g.page(w, pages, g.LETTER, c, _helv_resources(helv)))
    g.pages_tree(w, pages, kids)
    outlines = w.reserve()
    a = w.reserve()
    b = w.reserve()
    chain = [w.reserve() for _ in range(40)]
    w.set(a, b"<< /Title (A self child) /Parent %d 0 R /Next %d 0 R /First %d 0 R /Last %d 0 R /Count 1 "
             b"/Dest [%d 0 R /XYZ 0 792 0] >>" % (outlines, b, a, a, kids[0]))
    w.set(b, b"<< /Title (B sibling cycle) /Parent %d 0 R /Prev %d 0 R /Next %d 0 R /First %d 0 R /Last %d 0 R "
             b"/Count 1 /Dest [%d 0 R /XYZ 0 792 0] >>" % (outlines, a, a, chain[0], chain[0], kids[1]))
    for i, num in enumerate(chain):
        parent = b if i == 0 else chain[i - 1]
        child = b" /First %d 0 R /Last %d 0 R /Count 1" % (chain[i + 1], chain[i + 1]) if i + 1 < len(chain) else b""
        w.set(num, b"<< /Title (Depth %d) /Parent %d 0 R%s /Dest [%d 0 R /XYZ 0 700 0] >>"
              % (i + 1, parent, child, kids[1]))
    w.set(outlines, b"<< /Type /Outlines /First %d 0 R /Last %d 0 R /Count 2 >>" % (a, b))
    root = g.catalog(w, pages, b" /Outlines %d 0 R /PageMode /UseOutlines" % outlines)
    info = g.info_dict(w, "Malicious outline (cycles, deep chain)")
    return w.write(root=root, info=info, file_id=g.file_id_for("outline-cycle.pdf"))


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


def make_pattern_bomb() -> bytes:
    """렌더 위험 (#121): 타일링 패턴 /XStep 0.001 은 PDFium 렌더가 끝나지 않는다 (pdf.js
    tiling-pattern-tiny-steps 와 같은 구조). 4쪽 — 1 정상(라벨 + 36pt 스텝 패턴), 2 폭탄 채우기,
    3 Form XObject 안의 폭탄, 4 Square 주석 외형(/AP) 안의 폭탄. 쪽 1 만 렌더할 수 있다."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    cell = b"q 0 0 1 rg 0 0 18 18 re f Q"
    safe = w.stream(b"/PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 36 36] /XStep 36 /YStep 36"
                    b" /Resources << >>", cell)
    bomb = w.stream(b"/PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 612 792] /XStep 0.001"
                    b" /YStep 0.001 /Resources << >>", b"q 1 0 0 rg 500 700 100 80 re f Q")
    fill = "q /Pattern cs /P1 scn 72 72 468 648 re f Q"
    c1 = w.stream(b"", content(_label(72, 740, 18, "Pattern bomb test: page 1 is safe"), fill))
    p1 = g.page(w, pages, g.LETTER, c1,
                b"<< /Font << /F1 %d 0 R >> /Pattern << /P1 %d 0 R >> >>" % (helv, safe))
    c2 = w.stream(b"", content(fill))
    p2 = g.page(w, pages, g.LETTER, c2, b"<< /Pattern << /P1 %d 0 R >> >>" % bomb)
    form = w.stream(b"/Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << /Pattern << /P1 %d 0 R >> >>"
                    % bomb, content(fill))
    c3 = w.stream(b"", content("q /Fx0 Do Q"))
    p3 = g.page(w, pages, g.LETTER, c3, b"<< /XObject << /Fx0 %d 0 R >> >>" % form)
    ap = w.stream(b"/Type /XObject /Subtype /Form /BBox [0 0 200 100] /Resources << /Pattern << /P1 %d 0 R >> >>"
                  % bomb, content("q /Pattern cs /P1 scn 0 0 200 100 re f Q"))
    annot = w.add(b"<< /Type /Annot /Subtype /Square /Rect [100 600 300 700] /F 4 /C [1 0 0]"
                  b" /AP << /N %d 0 R >> >>" % ap)
    c4 = w.stream(b"", content(_border(612, 792)))
    p4 = g.page(w, pages, g.LETTER, c4, b"<< >>", annots=[annot])
    g.pages_tree(w, pages, [p1, p2, p3, p4])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Pattern bomb")
    return w.write(root=root, info=info, file_id=g.file_id_for("pattern-bomb.pdf"))

# ----------------------------------------------------------------- 기존 텍스트 줄 수정 (#65)


def make_text_lines_basic(*, encrypt: g.Encryption | None = None, file_id_name: str = "text-lines-basic.pdf") -> bytes:
    """줄 단위 수정 회귀용: 평범한 줄, 자간·어간(Tc/Tw) 줄, 굵게 섞인 줄, 같은 BT 에서 Td 없이 이어지는 두 Tj(밀림),
    지울 줄, 공백만의 객체 뒤의 줄(순번), 보존 확인용 도형."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    bold = g.simple_font(w, base="Helvetica-Bold")
    c = w.stream(b"", content(
        "BT /F1 14 Tf 72 700 Td (The quick brown fox jumps over the lazy dog) Tj ET",
        "BT /F1 12 Tf 0.5 Tc 2 Tw 72 670 Td (Spaced words with char and word spacing) Tj ET",
        "BT /F2 12 Tf 72 640 Td (Bold start) Tj ET",
        "BT /F1 12 Tf 140 640 Td (then regular end of the mixed line) Tj ET",
        "BT /F1 12 Tf 72 610 Td (Shifted one ) Tj (Shifted two) Tj ET",
        "BT /F1 12 Tf 72 580 Td (Delete me entirely) Tj ET",
        "BT /F1 12 Tf 72 550 Td (Hello world line) Tj ET",
        "BT /F1 12 Tf 72 520 Td [(Array) -500 (show line)] TJ ET",
        "BT /F1 12 Tf 14 TL 72 500 Td (Quote first) Tj (Quote second) ' 1 0.2 (Double quote third) \" ET",
        # 공백만의 객체(Quartz·Word 가 줄마다 수십 개): 추출 글은 비지만 PDFium 객체·qpdf 표시 연산자 모두에 든다 (#75 순번)
        "BT /F1 12 Tf 72 445 Td ( ) Tj ET",
        "BT /F1 12 Tf 80 445 Td (      ) Tj ET",
        "BT /F1 12 Tf 150 445 Td (After spaces line) Tj ET",
        "0 0 1 rg 72 360 200 80 re f",
        "1 0 0 RG 3 w 300 360 m 500 440 l S",
    ), compress=False)
    p = g.page(w, pages, g.LETTER, c, b"<< /Font << /F1 %d 0 R /F2 %d 0 R >> >>" % (helv, bold))
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = w.reserve()
    if encrypt is not None:
        w.encryptor = encrypt
        encrypt.prepare(g.file_id_for(file_id_name))
        w.set(info, b"<< /Title " + w.enc_text(info, "Text lines (encrypted)") + b" /Producer " + w.enc_text(info, "onpdf pdfgen")
              + b" /CreationDate " + w.enc_text(info, "D:20260101000000Z") + b" >>")
    else:
        g.info_dict(w, "Text lines for line editing", num=info)
    return w.write(root=root, info=info, file_id=g.file_id_for(file_id_name), encrypt=encrypt)


def make_text_lines_kern() -> bytes:
    """줄 폭 맞춤(#77): TJ 로 넓힌 줄, Tc 자간 줄, 끝 공백 줄, 음수 방향(좁힘) 커닝 줄, 보통 줄, 큰 자간 줄(한계),
    배열 문자열 하나인 TJ 줄(qpdf 치환 단위 테스트)."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    c = w.stream(b"", content(
        # Tc 는 그래픽 상태라 BT/ET 를 넘어 남는다 → q/Q 로 줄마다 격리
        "q BT /F1 12 Tf 72 700 Td [(Kerned) -500 (wide) -500 (gaps) -500 (here)] TJ ET Q",
        "q BT /F1 12 Tf 2 Tc 72 670 Td (Char spaced line here) Tj ET Q",
        "q BT /F1 12 Tf 72 640 Td (Trailing spaces line          ) Tj ET Q",
        "q BT /F1 12 Tf 72 610 Td [(Ti) 80 (ght) 80 (ly) 80 ( kerned) 80 ( line)] TJ ET Q",
        "q BT /F1 12 Tf 72 580 Td (Plain line without spacing) Tj ET Q",
        "q BT /F1 12 Tf 20 Tc 72 550 Td (Huge spacing) Tj ET Q",
        "q BT /F1 12 Tf 72 520 Td [(Single array string)] TJ ET Q",
    ), compress=False)
    p = g.page(w, pages, g.LETTER, c, b"<< /Font << /F1 %d 0 R >> >>" % helv)
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = w.reserve()
    g.info_dict(w, "Text lines with kerning", num=info)
    return w.write(root=root, info=info, file_id=g.file_id_for("text-lines-kern.pdf"))


def make_text_lines_split() -> bytes:
    """줄 폭·수술(#81): /Contents 배열이 피연산자와 연산자 사이에서 나뉜다 (Word·LiveCycle 식). 1쪽 3 스트림:
    A 는 `(Split operands line)` 로 끝나고 B 가 `Tj ET` 로 시작, B 는 `[(Arr) -200 (ay split)]` 로 끝나고 C 가 `TJ ET` 로 시작."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    s1 = w.stream(b"", content("BT /F1 12 Tf 72 700 Td (Split operands line)"), compress=False)
    s2 = w.stream(b"", content("Tj ET", "BT /F1 12 Tf 72 670 Td [(Arr) -200 (ay split)]"), compress=False)
    s3 = w.stream(b"", content("TJ ET", "BT /F1 12 Tf 72 640 Td (Whole line here) Tj ET", "0 0 1 rg 72 500 100 40 re f"),
                  compress=False)
    p = w.add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents [%d 0 R %d 0 R %d 0 R] "
              b"/Resources << /Font << /F1 %d 0 R >> >> >>" % (pages, s1, s2, s3, helv))
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = w.reserve()
    g.info_dict(w, "Text lines split across content streams", num=info)
    return w.write(root=root, info=info, file_id=g.file_id_for("text-lines-split.pdf"))


def make_text_lines_basic_enc() -> bytes:
    return make_text_lines_basic(encrypt=g.Encryption("user", "owner"), file_id_name="text-lines-basic-enc.pdf")


def make_text_lines_form() -> bytes:
    """Form XObject 안의 줄(1단계 — 편집 가능)과 Form 안 Form 의 줄(2단계 — 불가), 쪽 직속 줄, 이미지 하나."""
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    nested = w.stream(b"/Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << /Font << /F1 %d 0 R >> >>" % helv,
                      content("BT /F1 12 Tf 72 560 Td (Nested form line stays) Tj ET"))
    form = w.stream(b"/Type /XObject /Subtype /Form /BBox [0 0 612 792] "
                    b"/Resources << /Font << /F1 %d 0 R >> /XObject << /Fm2 %d 0 R >> >>" % (helv, nested),
                    content("BT /F1 12 Tf 72 640 Td (Inside form first line) Tj ET",
                            "BT /F1 12 Tf 72 620 Td (Inside form second line) Tj ET",
                            "/Fm2 Do"))
    # 2×2 회색 이미지 (보존 확인)
    image = w.stream(b"/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceGray /BitsPerComponent 8",
                     bytes([40, 200, 200, 40]))
    c = w.stream(b"", content(
        "BT /F1 14 Tf 72 700 Td (Page level line above the form) Tj ET",
        "/Fm1 Do",
        "q 100 0 0 60 400 500 cm /Im1 Do Q",
        "BT /F1 12 Tf 72 480 Td (Page level line below the form) Tj ET",
    ), compress=False)
    p = g.page(w, pages, g.LETTER, c,
               b"<< /Font << /F1 %d 0 R >> /XObject << /Fm1 %d 0 R /Im1 %d 0 R >> >>" % (helv, form, image))
    # 2쪽: /Contents 배열이 BT 안에서 나뉜다 (스트림 경계를 넘는 밀림 검사) — 앞 스트림의 Tj 를 지우면 뒤 스트림의 Tj 가 밀린다
    s1 = w.stream(b"", content("BT /F1 12 Tf 72 700 Td (Split one) Tj"), compress=False)
    s2 = w.stream(b"", content("(Split two) Tj ET", "BT /F1 12 Tf 72 670 Td (Second page plain line) Tj ET"), compress=False)
    p2 = w.add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents [%d 0 R %d 0 R] "
               b"/Resources << /Font << /F1 %d 0 R >> >> >>" % (pages, s1, s2, helv))
    # 3·4쪽: 같은 Form 을 두 쪽이 그린다 (직접 /Resources) — 그 안의 줄은 공유 Form 이라 고칠 수 없다
    shared = w.stream(b"/Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << /Font << /F1 %d 0 R >> >>" % helv,
                      content("BT /F1 12 Tf 72 700 Td (Shared form line on two pages) Tj ET"))
    shared_c = w.stream(b"", content("/FmS Do"), compress=False)
    shared_res = b"<< /Font << /F1 %d 0 R >> /XObject << /FmS %d 0 R >> >>" % (helv, shared)
    p3 = g.page(w, pages, g.LETTER, shared_c, shared_res)
    p4 = g.page(w, pages, g.LETTER, shared_c, shared_res)
    g.pages_tree(w, pages, [p, p2, p3, p4])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Text lines inside form XObjects")
    return w.write(root=root, info=info, file_id=g.file_id_for("text-lines-form.pdf"))


def make_text_lines_type3() -> bytes:
    """Type3 글꼴 줄(ToUnicode 있음 — 원본 글꼴 재사용 불가 → 대체 글꼴)과 임베딩 CID 한글 줄(서브셋 밖 글자는 대체)."""
    meta = _load_font_meta()
    w = PdfWriter()
    pages = w.reserve()
    helv = g.simple_font(w)
    g1 = w.stream(b"", content("600 0 0 0 500 700 d1", "50 0 400 700 re f"))
    g2 = w.stream(b"", content("600 0 0 0 500 700 d1", "50 0 400 350 re f"))
    t3_tounicode = w.stream(b"", (
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "2 beginbfchar\n<41> <0041>\n<42> <0042>\nendbfchar\n"
        "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n").encode("latin-1"))
    t3 = w.add(b"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 600 700] /FontMatrix [0.001 0 0 0.001 0 0] "
               b"/CharProcs << /g1 %d 0 R /g2 %d 0 R >> /Encoding << /Type /Encoding /Differences [65 /g1 /g2] >> "
               b"/FirstChar 65 /LastChar 66 /Widths [600 600] /Resources << >> /ToUnicode %d 0 R >>" % (g1, g2, t3_tounicode))
    # 한글 CID 서브셋 글꼴 (korean-embedded 와 같은 조립)
    text = "한글 텍스트 줄"
    hexs, used = _cid_text(meta, text)
    ttf = FONT_TTF.read_bytes()
    font_file = w.stream(b"/Length1 %d" % len(ttf), ttf)
    gid_width = {v["gid"]: v["width"] for v in meta["glyphs"].values()}
    widths = b" ".join(b"%d [%d]" % (gid, gid_width[gid]) for gid in sorted(used))
    bbox = b" ".join(b"%d" % v for v in meta["bbox"])
    font_name = b"ABCDEF+" + meta["family"].encode()
    descriptor = w.add(
        b"<< /Type /FontDescriptor /FontName /%s /Flags 4 /FontBBox [%s] /ItalicAngle 0 /Ascent %d "
        b"/Descent %d /CapHeight %d /StemV 80 /FontFile2 %d 0 R >>"
        % (font_name, bbox, meta["ascent"], meta["descent"], meta["cap_height"], font_file))
    cid_font = w.add(
        b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s "
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
        b"/FontDescriptor %d 0 R /DW 1000 /W [%s] /CIDToGIDMap /Identity >>" % (font_name, descriptor, widths))
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
    c = w.stream(b"", content(
        _label(72, 740, 12, "Type3 line below, Korean CID line further down"),
        "BT /T3 24 Tf 72 690 Td (ABAB) Tj ET",
        f"BT /F0 18 Tf 72 640 Td {hexs} Tj ET",
        "BT /F1 12 Tf 3 Tr 72 600 Td (Invisible OCR layer text) Tj ET",
    ), compress=False)
    p = g.page(w, pages, g.LETTER, c, b"<< /Font << /F1 %d 0 R /T3 %d 0 R /F0 %d 0 R >> >>" % (helv, t3, type0))
    g.pages_tree(w, pages, [p])
    root = g.catalog(w, pages)
    info = g.info_dict(w, "Type3 and Korean CID lines")
    return w.write(root=root, info=info, file_id=g.file_id_for("text-lines-type3.pdf"))
