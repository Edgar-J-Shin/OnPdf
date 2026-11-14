"""표준 라이브러리만 쓰는 소형 PDF 작성기.

테스트 PDF 를 결정적으로(날짜·난수 없이) 만들기 위한 최소 기능:
  - 객체 테이블, 번호 예약(reserve), 스트림(Flate)
  - 고전 xref 테이블 또는 PDF 1.5 xref 스트림 + 객체 스트림
  - 증분 갱신(append_update)
  - 표준 보안 핸들러 R3 / V2 / RC4 128-bit (Encryption)
  - 문자열 헬퍼(리터럴 이스케이프, UTF-16BE 텍스트 문자열)
"""

from __future__ import annotations

import hashlib
import struct
import zlib
from dataclasses import dataclass

# ----------------------------------------------------------------- 문자열 헬퍼


def lit(s: str | bytes) -> bytes:
    """(…) 리터럴 문자열. 바이트는 그대로, str 은 latin-1 로 인코딩한다."""
    raw = s if isinstance(s, bytes) else s.encode("latin-1")
    out = bytearray(b"(")
    for b in raw:
        if b in (0x28, 0x29, 0x5C):  # ( ) \
            out += b"\\" + bytes([b])
        elif b < 0x20 or b > 0x7E:
            out += b"\\%03o" % b
        else:
            out.append(b)
    out += b")"
    return bytes(out)


def hexstr(data: bytes) -> bytes:
    return b"<" + data.hex().upper().encode() + b">"


def text_string(s: str) -> bytes:
    """PDF 텍스트 문자열. ASCII 면 리터럴, 아니면 BOM 붙은 UTF-16BE hex."""
    try:
        s.encode("ascii")
        return lit(s)
    except UnicodeEncodeError:
        return hexstr(b"\xfe\xff" + s.encode("utf-16-be"))


def ucs2(s: str) -> bytes:
    """UCS-2BE hex 문자열 (UniKS-UCS2-H 같은 CMap 용)."""
    return hexstr(s.encode("utf-16-be"))


def name(s: str) -> bytes:
    return b"/" + s.encode("ascii")


def fmt(v: float) -> str:
    """좌표·크기 숫자. 불필요한 소수점 제거."""
    if float(v).is_integer():
        return str(int(v))
    return ("%.3f" % v).rstrip("0").rstrip(".")


def content(*ops: str) -> bytes:
    return ("\n".join(ops) + "\n").encode("latin-1")


# ----------------------------------------------------------------- 암호화 (RC4, R3)

_PAD = bytes(
    [
        0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
        0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A,
    ]
)


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    klen = len(key)
    for i in range(256):
        j = (j + s[i] + key[i % klen]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray(len(data))
    i = j = 0
    for n, b in enumerate(data):
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out[n] = b ^ s[(s[i] + s[j]) & 0xFF]
    return bytes(out)


def _pad(pw: str) -> bytes:
    raw = pw.encode("latin-1")
    return (raw + _PAD)[:32]


class Encryption:
    """표준 보안 핸들러 R3, V2, Length 128 (RC4). 알고리즘 번호는 PDF 1.7 참조 3.5 절."""

    KEY_LEN = 16

    def __init__(self, user_password: str, owner_password: str, permissions: int = -1):
        self.user = user_password
        self.owner = owner_password or user_password
        self.permissions = permissions
        self.key = b""
        self.o_value = b""
        self.u_value = b""

    def prepare(self, file_id: bytes) -> None:
        # 알고리즘 3.3: O
        h = hashlib.md5(_pad(self.owner)).digest()
        for _ in range(50):
            h = hashlib.md5(h[: self.KEY_LEN]).digest()
        okey = h[: self.KEY_LEN]
        o = rc4(okey, _pad(self.user))
        for i in range(1, 20):
            o = rc4(bytes(b ^ i for b in okey), o)
        self.o_value = o
        # 알고리즘 3.2: 키
        p = struct.pack("<i", self.permissions)
        h = hashlib.md5(_pad(self.user) + self.o_value + p + file_id).digest()
        for _ in range(50):
            h = hashlib.md5(h[: self.KEY_LEN]).digest()
        self.key = h[: self.KEY_LEN]
        # 알고리즘 3.5: U (R3)
        u = hashlib.md5(_PAD + file_id).digest()
        u = rc4(self.key, u)
        for i in range(1, 20):
            u = rc4(bytes(b ^ i for b in self.key), u)
        self.u_value = u + bytes(16)

    def object_key(self, num: int, gen: int) -> bytes:
        extra = struct.pack("<I", num)[:3] + struct.pack("<H", gen)
        return hashlib.md5(self.key + extra).digest()[: min(self.KEY_LEN + 5, 16)]

    def encrypt(self, num: int, gen: int, data: bytes) -> bytes:
        return rc4(self.object_key(num, gen), data)

    def dict_body(self) -> bytes:
        return (
            b"<< /Filter /Standard /V 2 /R 3 /Length 128 /P %d /O %s /U %s >>"
            % (self.permissions, hexstr(self.o_value), hexstr(self.u_value))
        )


# ----------------------------------------------------------------- 작성기


@dataclass
class _Stream:
    entries: bytes  # 사전 본문 (/Length, /Filter 제외)
    data: bytes
    compress: bool


class PdfWriter:
    def __init__(self, *, version: str = "1.4", compress: bool = True):
        self.version = version
        self.default_compress = compress
        self.objects: list[bytes | _Stream | None] = []
        self.encryptor: Encryption | None = None

    # -- 객체 등록 --------------------------------------------------------
    def reserve(self) -> int:
        self.objects.append(None)
        return len(self.objects)

    def add(self, body: bytes | str) -> int:
        num = self.reserve()
        self.set(num, body)
        return num

    def set(self, num: int, body: bytes | str) -> None:
        self.objects[num - 1] = body if isinstance(body, bytes) else body.encode("latin-1")

    def stream(self, entries: bytes | str, data: bytes, *, compress: bool | None = None,
               num: int | None = None) -> int:
        if num is None:
            num = self.reserve()
        e = entries if isinstance(entries, bytes) else entries.encode("latin-1")
        self.objects[num - 1] = _Stream(e, data, self.default_compress if compress is None else compress)
        return num

    # 암호 문서에서 문자열을 쓸 때: 객체 번호별 키로 미리 암호화한 hex 문자열
    def enc_text(self, num: int, s: str) -> bytes:
        raw = s.encode("latin-1") if s.isascii() else b"\xfe\xff" + s.encode("utf-16-be")
        if self.encryptor is None:
            return hexstr(raw)
        return hexstr(self.encryptor.encrypt(num, 0, raw))

    # -- 직렬화 -----------------------------------------------------------
    def _serialize(self, num: int, obj: bytes | _Stream) -> bytes:
        if isinstance(obj, bytes):
            return obj
        data = obj.data
        filt = b""
        if obj.compress:
            data = zlib.compress(data, 9)
            filt = b" /Filter /FlateDecode"
        if self.encryptor is not None:
            data = self.encryptor.encrypt(num, 0, data)
        entries = obj.entries.strip()
        # 사전 전체가 << … >> 로 감싸진 경우에만 벗긴다. "/Resources << … >>" 처럼 중첩 사전으로
        # 끝나는 항목 목록의 마지막 >> 를 잘라내면 사전이 깨져 PDFium 이 스트림을 버린다.
        if entries.startswith(b"<<") and entries.endswith(b">>"):
            entries = entries[2:-2]
        return b"<<" + entries + b" /Length %d%s >>\nstream\n" % (len(data), filt) + data + b"\nendstream"

    def write(self, *, root: int, info: int | None = None, file_id: bytes,
              xref_stream: bool = False, encrypt: Encryption | None = None) -> bytes:
        assert not (xref_stream and encrypt), "xref 스트림과 암호화 동시 사용은 지원하지 않음"
        self.encryptor = encrypt
        if encrypt is not None:
            encrypt.prepare(file_id)
        version = self.version
        if xref_stream and tuple(int(v) for v in version.split(".")) < (1, 5):
            version = "1.5"  # xref 스트림·객체 스트림은 PDF 1.5 기능
        out = bytearray(b"%%PDF-%s\n%%\xe2\xe3\xcf\xd3\n" % version.encode())
        if xref_stream:
            return self._write_xref_stream(out, root, info, file_id)
        return self._write_classic(out, root, info, file_id, encrypt)

    def _write_classic(self, out: bytearray, root: int, info: int | None, file_id: bytes,
                       encrypt: Encryption | None) -> bytes:
        offsets: dict[int, int] = {}
        for i, obj in enumerate(self.objects, start=1):
            assert obj is not None, f"객체 {i} 가 비어 있음"
            offsets[i] = len(out)
            out += b"%d 0 obj\n" % i + self._serialize(i, obj) + b"\nendobj\n"
        enc_num = 0
        if encrypt is not None:
            enc_num = len(self.objects) + 1
            offsets[enc_num] = len(out)
            out += b"%d 0 obj\n" % enc_num + encrypt.dict_body() + b"\nendobj\n"
        size = len(offsets) + 1
        startxref = len(out)
        out += b"xref\n0 %d\n" % size
        out += b"0000000000 65535 f \n"
        for i in range(1, size):
            out += b"%010d 00000 n \n" % offsets[i]
        trailer = b"<< /Size %d /Root %d 0 R" % (size, root)
        if info is not None:
            trailer += b" /Info %d 0 R" % info
        if encrypt is not None:
            trailer += b" /Encrypt %d 0 R" % enc_num
        trailer += b" /ID [%s %s] >>" % (hexstr(file_id), hexstr(file_id))
        out += b"trailer\n" + trailer + b"\nstartxref\n%d\n%%%%EOF\n" % startxref
        return bytes(out)

    def _write_xref_stream(self, out: bytearray, root: int, info: int | None, file_id: bytes) -> bytes:
        for i, obj in enumerate(self.objects, start=1):
            assert obj is not None, f"객체 {i} 가 비어 있음"
        # 비스트림 객체는 전부 객체 스트림 하나에 넣는다
        plain = [(i, o) for i, o in enumerate(self.objects, start=1) if isinstance(o, bytes)]
        streams = [(i, o) for i, o in enumerate(self.objects, start=1) if isinstance(o, _Stream)]
        objstm_num = len(self.objects) + 1
        xref_num = objstm_num + 1
        size = xref_num + 1

        entries: dict[int, tuple[int, int, int]] = {0: (0, 0, 65535)}
        header = bytearray()
        body = bytearray()
        for idx, (i, o) in enumerate(plain):
            header += b"%d %d " % (i, len(body))
            body += o + b"\n"
            entries[i] = (2, objstm_num, idx)
        objstm_data = bytes(header) + bytes(body)
        objstm = _Stream(b"/Type /ObjStm /N %d /First %d" % (len(plain), len(header)), objstm_data, True)

        for i, o in streams:
            entries[i] = (1, len(out), 0)
            out += b"%d 0 obj\n" % i + self._serialize(i, o) + b"\nendobj\n"
        entries[objstm_num] = (1, len(out), 0)
        out += b"%d 0 obj\n" % objstm_num + self._serialize(objstm_num, objstm) + b"\nendobj\n"

        startxref = len(out)
        entries[xref_num] = (1, startxref, 0)
        rows = bytearray()
        for i in range(size):
            t, f2, f3 = entries[i]
            rows += struct.pack(">BIH", t, f2, f3)
        xdict = b"/Type /XRef /Size %d /W [1 4 2] /Root %d 0 R" % (size, root)
        if info is not None:
            xdict += b" /Info %d 0 R" % info
        xdict += b" /ID [%s %s]" % (hexstr(file_id), hexstr(file_id))
        xref = _Stream(xdict, bytes(rows), True)
        out += b"%d 0 obj\n" % xref_num + self._serialize(xref_num, xref) + b"\nendobj\n"
        out += b"startxref\n%d\n%%%%EOF\n" % startxref
        return bytes(out)

    @staticmethod
    def append_update(base: bytes, objects: dict[int, bytes], *, root: int, size: int,
                      file_id: bytes, info: int | None = None) -> bytes:
        """base 뒤에 증분 갱신 섹션을 붙인다. objects: {번호: 직렬화된 본문}."""
        marker = base.rstrip().rfind(b"startxref")
        prev = int(base[marker + len(b"startxref"):].split()[0])
        out = bytearray(base)
        if not out.endswith(b"\n"):
            out += b"\n"
        offsets: dict[int, int] = {}
        for num in sorted(objects):
            offsets[num] = len(out)
            out += b"%d 0 obj\n" % num + objects[num] + b"\nendobj\n"
        startxref = len(out)
        out += b"xref\n"
        for num in sorted(offsets):
            out += b"%d 1\n%010d 00000 n \n" % (num, offsets[num])
        trailer = b"<< /Size %d /Root %d 0 R /Prev %d" % (size, root, prev)
        if info is not None:
            trailer += b" /Info %d 0 R" % info
        # /ID 첫 요소는 유지, 두 번째는 갱신마다 달라져야 한다 (결정적으로: 이전 두 번째 요소의 MD5)
        updated_id = hashlib.md5(file_id + b"update").digest()
        trailer += b" /ID [%s %s] >>" % (hexstr(file_id), hexstr(updated_id))
        out += b"trailer\n" + trailer + b"\nstartxref\n%d\n%%%%EOF\n" % startxref
        return bytes(out)


# ----------------------------------------------------------------- 문서 조립 헬퍼

LETTER = (612.0, 792.0)
A4 = (595.276, 841.89)
A5 = (419.528, 595.276)


def file_id_for(name_: str) -> bytes:
    return hashlib.md5(("onpdf-testdata:" + name_).encode()).digest()


def simple_font(w: PdfWriter, base: str = "Helvetica") -> int:
    return w.add(b"<< /Type /Font /Subtype /Type1 /BaseFont /%s /Encoding /WinAnsiEncoding >>" % base.encode())


def page(w: PdfWriter, parent: int, size: tuple[float, float], contents: int | None,
         resources: bytes = b"<< >>", *, rotate: int = 0, annots: list[int] | None = None,
         num: int | None = None) -> int:
    body = b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %s %s]" % (
        parent, fmt(size[0]).encode(), fmt(size[1]).encode())
    if contents is not None:
        body += b" /Contents %d 0 R" % contents
    body += b" /Resources " + resources
    if rotate:
        body += b" /Rotate %d" % rotate
    if annots:
        body += b" /Annots [" + b" ".join(b"%d 0 R" % a for a in annots) + b"]"
    body += b" >>"
    if num is None:
        return w.add(body)
    w.set(num, body)
    return num


def pages_tree(w: PdfWriter, num: int, kids: list[int]) -> None:
    w.set(num, b"<< /Type /Pages /Kids [" + b" ".join(b"%d 0 R" % k for k in kids) + b"] /Count %d >>" % len(kids))


def catalog(w: PdfWriter, pages: int, extra: bytes = b"") -> int:
    return w.add(b"<< /Type /Catalog /Pages %d 0 R" % pages + extra + b" >>")


def info_dict(w: PdfWriter, title: str, author: str = "OnPdf testdata", *, num: int | None = None) -> int:
    body = (b"<< /Title " + text_string(title) + b" /Author " + text_string(author)
            + b" /Producer (onpdf pdfgen) /CreationDate (D:20260101000000Z) >>")
    if num is None:
        return w.add(body)
    w.set(num, body)
    return num


def lcg(seed: int):
    """결정적 의사난수 (0..2^31-1)."""
    x = seed & 0x7FFFFFFF
    while True:
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        yield x
