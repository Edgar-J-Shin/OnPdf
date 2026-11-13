#!/usr/bin/env python3
"""simple.pdf 를 생성한다. 외부 의존성 없이 xref 를 직접 계산한다."""

from pathlib import Path

OUT = Path(__file__).resolve().parent / "simple.pdf"

page1 = b"""BT /F1 24 Tf 72 720 Td (OnPdf M0 test page) Tj ET
BT /F1 12 Tf 72 690 Td (Letter portrait, Helvetica, Apache-2.0) Tj ET
0 0 1 rg 72 500 200 120 re f
1 0 0 RG 4 w 320 500 m 540 620 l S
0.5 g 72 100 468 40 re f
"""
page2 = b"""BT /F1 24 Tf 72 540 Td (Page 2: landscape) Tj ET
0 0.6 0 rg 72 72 300 300 re f
"""

objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 7 0 R >> >> >>",
    b"<< /Length %d >>\nstream\n" % len(page1) + page1 + b"endstream",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 792 612] /Contents 6 0 R /Resources << /Font << /F1 7 0 R >> >> >>",
    b"<< /Length %d >>\nstream\n" % len(page2) + page2 + b"endstream",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
]

out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
offsets = []
for i, body in enumerate(objects, start=1):
    offsets.append(len(out))
    out += b"%d 0 obj\n" % i + body + b"\nendobj\n"

xref_pos = len(out)
out += b"xref\n0 %d\n" % (len(objects) + 1)
out += b"0000000000 65535 f \n"
for off in offsets:
    out += b"%010d 00000 n \n" % off
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, xref_pos)

OUT.write_bytes(out)
print(f"wrote {OUT} ({len(out)} bytes)")
