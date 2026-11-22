#include "cff_charset.h"

#include <algorithm>
#include <cstring>

namespace onpdf::engine::detail {
namespace {

// 범위를 검사하는 읽기. 실패하면 ok 를 false 로
struct Reader {
  std::span<const std::uint8_t> b;
  bool ok = true;

  [[nodiscard]] std::uint32_t be(std::size_t off, int n) {
    if (n <= 0 || n > 4 || off > b.size() || b.size() - off < static_cast<std::size_t>(n)) {
      ok = false;
      return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | b[off + static_cast<std::size_t>(i)];
    return v;
  }
  [[nodiscard]] std::uint8_t u8(std::size_t off) { return static_cast<std::uint8_t>(be(off, 1)); }
};

struct Range {
  std::size_t begin = 0;
  std::size_t end = 0;
};

// INDEX 를 읽고 항목 범위를 돌려준다. off 는 INDEX 끝으로 옮긴다
std::vector<Range> read_index(Reader& r, std::size_t& off) {
  std::vector<Range> items;
  const std::uint32_t count = r.be(off, 2);
  if (!r.ok) return items;
  if (count == 0) {
    off += 2;
    return items;
  }
  const int osz = r.u8(off + 2);
  if (!r.ok || osz < 1 || osz > 4) {
    r.ok = false;
    return items;
  }
  const std::size_t offsets = off + 3;
  const std::size_t base =
      offsets + (static_cast<std::size_t>(count) + 1) * static_cast<std::size_t>(osz) - 1;
  std::uint32_t prev = r.be(offsets, osz);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t next =
        r.be(offsets + (static_cast<std::size_t>(i) + 1) * static_cast<std::size_t>(osz), osz);
    if (!r.ok || next < prev) {
      r.ok = false;
      return items;
    }
    items.push_back({base + prev, base + next});
    prev = next;
  }
  off = base + prev;
  if (off > r.b.size()) r.ok = false;
  return items;
}

// DICT 에서 op 의 첫 정수 피연산자. present 가 있으면 op 존재 여부
long dict_int(Reader& r, Range d, int want_op, bool* present = nullptr) {
  std::vector<long> ops;
  for (std::size_t i = d.begin; i < d.end && r.ok;) {
    const int b0 = r.u8(i);
    if (!r.ok) break;
    if (b0 <= 21) {
      int op = b0;
      ++i;
      if (b0 == 12) {
        op = 1200 + r.u8(i);
        ++i;
      }
      if (op == want_op) {
        if (present != nullptr) *present = true;
        return ops.empty() ? -1 : ops.front();
      }
      ops.clear();
    } else if (b0 == 28) {
      ops.push_back(static_cast<std::int16_t>(r.be(i + 1, 2)));
      i += 3;
    } else if (b0 == 29) {
      ops.push_back(static_cast<std::int32_t>(r.be(i + 1, 4)));
      i += 5;
    } else if (b0 == 30) {  // 실수: 니블이 0xf 로 끝난다
      ++i;
      while (r.ok && i < d.end) {  // DICT 범위 안에서만 (잘린 실수가 다음 구조를 읽지 않게)
        const int v = r.u8(i++);
        if ((v & 0x0f) == 0x0f || (v >> 4) == 0x0f) break;
      }
      ops.push_back(0);
    } else if (b0 >= 32 && b0 <= 246) {
      ops.push_back(b0 - 139);
      ++i;
    } else if (b0 >= 247 && b0 <= 250) {
      ops.push_back((b0 - 247) * 256 + r.u8(i + 1) + 108);
      i += 2;
    } else if (b0 >= 251 && b0 <= 254) {
      ops.push_back(-(b0 - 251) * 256 - r.u8(i + 1) - 108);
      i += 2;
    } else {
      ++i;
    }
  }
  return -1;
}

}  // namespace

std::optional<CffCharsetInfo> inspect_cff_charset(std::span<const std::uint8_t> otf) {
  if (otf.size() < 12 || std::memcmp(otf.data(), "OTTO", 4) != 0) return std::nullopt;
  Reader r{otf};
  const std::uint32_t tables = r.be(4, 2);
  std::size_t cff = 0, cff_len = 0;
  for (std::uint32_t i = 0; i < tables && r.ok; ++i) {
    const std::size_t rec = 12 + static_cast<std::size_t>(i) * 16;
    if (rec + 16 > otf.size()) return std::nullopt;
    if (std::memcmp(otf.data() + rec, "CFF ", 4) == 0) {
      cff = r.be(rec + 8, 4);
      cff_len = r.be(rec + 12, 4);
    }
  }
  if (!r.ok || cff == 0 || cff_len < 4 || cff > otf.size() || otf.size() - cff < cff_len)
    return std::nullopt;
  const std::size_t hdr = r.u8(cff + 2);
  std::size_t off = cff + hdr;
  (void)read_index(r, off);             // Name INDEX
  const auto top = read_index(r, off);  // Top DICT INDEX
  if (!r.ok || top.empty()) return std::nullopt;
  CffCharsetInfo info;
  bool has_ros = false;
  (void)dict_int(r, top.front(), 1230, &has_ros);
  info.cid_keyed = has_ros;
  const long charset = dict_int(r, top.front(), 15);
  const long charstrings = dict_int(r, top.front(), 17);
  if (!r.ok || charset <= 2 || charstrings <= 0)
    return std::nullopt;  // 0·1·2 는 사전 정의 charset (CID 글꼴에는 없다)
  info.glyphs = r.be(cff + static_cast<std::size_t>(charstrings), 2);
  info.charset_offset = cff + static_cast<std::size_t>(charset);
  const int fmt = r.u8(info.charset_offset);
  if (!r.ok || info.glyphs == 0) return std::nullopt;
  std::size_t i = info.charset_offset + 1;
  unsigned gid = 1;
  info.identity = true;
  if (fmt == 0) {
    for (; gid < info.glyphs && r.ok; ++gid, i += 2) {
      if (r.be(i, 2) != gid) info.identity = false;
    }
  } else if (fmt == 1 || fmt == 2) {
    while (gid < info.glyphs && r.ok) {
      const unsigned first = r.be(i, 2);
      const unsigned left = fmt == 1 ? r.u8(i + 2) : r.be(i + 2, 2);
      i += fmt == 1 ? 3 : 4;
      for (unsigned k = 0; k <= left && gid < info.glyphs; ++k, ++gid) {
        if (first + k != gid) info.identity = false;
      }
    }
  } else {
    return std::nullopt;
  }
  if (!r.ok || i > cff + cff_len) return std::nullopt;
  info.charset_len = i - info.charset_offset;
  return info;
}

bool write_identity_charset(std::vector<std::uint8_t>& otf) {
  const auto info = inspect_cff_charset(otf);
  if (!info || !info->cid_keyed) return false;
  if (info->identity) return true;
  const unsigned n = info->glyphs;
  if (n <= 1) return true;  // .notdef 만: charset 비어 있음
  std::vector<std::uint8_t> out;
  if (n - 2 <= 255 && info->charset_len >= 4) {
    out = {1, 0, 1, static_cast<std::uint8_t>(n - 2)};  // 형식 1: first=1, nLeft=n-2
  } else if (info->charset_len >= 5) {
    out = {2, 0, 1, static_cast<std::uint8_t>((n - 2) >> 8),
           static_cast<std::uint8_t>((n - 2) & 0xFF)};
  } else if (info->charset_len >= 1 + 2 * (static_cast<std::size_t>(n) - 1)) {
    out.push_back(0);
    for (unsigned g = 1; g < n; ++g) {
      out.push_back(static_cast<std::uint8_t>(g >> 8));
      out.push_back(static_cast<std::uint8_t>(g & 0xFF));
    }
  } else {
    return false;
  }
  std::copy(out.begin(), out.end(),
            otf.begin() + static_cast<std::ptrdiff_t>(info->charset_offset));
  return true;
}

}  // namespace onpdf::engine::detail
