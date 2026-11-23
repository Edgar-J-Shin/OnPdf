// Spike #55: 콘텐츠 스트림 수술 — qpdf 로 대상 텍스트 표시 연산자만 지우고 나머지 바이트는 그대로
// 둔다. PDFium 의 GenerateContent 는 스트림 전체를 다시 써 Type3·Tc/Tw·선 폭 등을 잃는다(E0) → 대안
// 검증.
#include "text_edit_surgery.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <set>
#include <string>

namespace spike {
namespace {

bool non_empty_show(const std::string& op, const std::vector<QPDFObjectHandle>& operands) {
  if (operands.empty()) return false;
  const QPDFObjectHandle& last = operands.back();
  if (op == "TJ") {
    if (!last.isArray()) return false;
    for (auto& item : last.getArrayAsVector())
      if (item.isString() && !item.getStringValue().empty()) return true;
    return false;
  }
  return last.isString() && !last.getStringValue().empty();
}

struct Op {
  std::string name;
  std::size_t start = 0;  // 첫 피연산자(없으면 연산자) 위치
  std::size_t end = 0;
  std::vector<QPDFObjectHandle> operands;
};

struct Collect : QPDFObjectHandle::ParserCallbacks {
  std::vector<Op> ops;
  std::vector<QPDFObjectHandle> pending;
  std::size_t pending_start = 0;
  bool has_pending = false;
  void handleObject(QPDFObjectHandle obj, size_t offset, size_t length) override {
    if (obj.isOperator()) {
      Op op;
      op.name = obj.getOperatorValue();
      op.start = has_pending ? pending_start : offset;
      op.end = offset + length;
      op.operands = std::move(pending);
      ops.push_back(std::move(op));
      pending.clear();
      has_pending = false;
    } else {
      if (!has_pending) pending_start = offset;
      has_pending = true;
      pending.push_back(obj);
    }
  }
  void handleEOF() override {}
};

bool is_show(const std::string& n) {
  return n == "Tj" || n == "TJ" || n == "'" || n == "\"";
}
bool sets_line(const std::string& n) {
  return n == "Td" || n == "TD" || n == "Tm" || n == "T*" || n == "'" || n == "\"" || n == "BT";
}

// data 에서 ordinals 번째(0 기준, 비어 있지 않은 표시 연산자 순서) 연산자를 지운 새 바이트
std::string rewrite(const std::string& data, const std::set<int>& ordinals, int& show_ops,
                    int& needs_disp, int& do_forms_seen, std::vector<std::string>* do_names) {
  Collect c;
  // parseContentStream 은 스트림/배열을 받는다 — 임시 QPDF 에 바이트 스트림을 만든다
  QPDF scratch;
  scratch.emptyPDF();
  auto stream = QPDFObjectHandle::newStream(&scratch, data);
  QPDFObjectHandle::parseContentStream(stream, &c);

  std::vector<int> ordinal_of(c.ops.size(), -1);
  int n = 0;
  for (std::size_t i = 0; i < c.ops.size(); ++i) {
    if (is_show(c.ops[i].name) && non_empty_show(c.ops[i].name, c.ops[i].operands))
      ordinal_of[i] = n++;
    if (c.ops[i].name == "Do" && do_names && !c.ops[i].operands.empty() &&
        c.ops[i].operands.back().isName()) {
      do_names->push_back(c.ops[i].operands.back().getName());
      ++do_forms_seen;
    }
  }
  show_ops = n;

  std::string out;
  std::size_t pos = 0;
  for (std::size_t i = 0; i < c.ops.size(); ++i) {
    if (ordinal_of[i] < 0 || !ordinals.count(ordinal_of[i])) continue;
    const Op& op = c.ops[i];
    out.append(data, pos, op.start - pos);
    if (op.name == "'") {
      out += " T* ";
    } else if (op.name == "\"" && op.operands.size() >= 3) {
      out += " " + op.operands[0].unparse() + " Tw " + op.operands[1].unparse() + " Tc T* ";
    } else {
      out += " ";
    }
    pos = op.end;
    // 같은 BT 안에서 다음 표시 연산자가 줄 위치 연산자 없이 이어지면 위치가 밀린다(Tj/TJ 는 Tlm 을
    // 바꾸지 않는다)
    for (std::size_t k = i + 1; k < c.ops.size(); ++k) {
      const std::string& nm = c.ops[k].name;
      if (nm == "ET" || sets_line(nm)) break;
      if ((nm == "Tj" || nm == "TJ") && ordinal_of[k] >= 0 && !ordinals.count(ordinal_of[k])) {
        ++needs_disp;
        break;
      }
    }
  }
  out.append(data, pos, std::string::npos);
  return out;
}

std::string read_stream(QPDFObjectHandle h) {
  auto buf = h.getStreamData(qpdf_dl_all);
  return {reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize()};
}

}  // namespace

SurgeryResult remove_show_ops(const std::vector<std::uint8_t>& in, int page_index,
                              const std::set<int>& page_ordinals,
                              const std::map<int, std::set<int>>& form_ordinals) {
  SurgeryResult r;
  try {
    QPDF q;
    q.setSuppressWarnings(true);
    q.processMemoryFile("in.pdf", reinterpret_cast<const char*>(in.data()), in.size());
    auto pages = QPDFPageDocumentHelper(q).getAllPages();
    if (page_index >= static_cast<int>(pages.size())) {
      r.error = "page";
      return r;
    }
    auto& page = pages[static_cast<std::size_t>(page_index)];
    Pl_Buffer pl("contents");
    page.pipeContents(&pl);
    std::unique_ptr<Buffer> b(pl.getBuffer());
    const std::string data(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());

    int forms_seen = 0;
    std::vector<std::string> do_names;
    const std::string out =
        rewrite(data, page_ordinals, r.page_show_ops, r.needs_displacement, forms_seen, &do_names);
    page.getObjectHandle().replaceKey("/Contents", QPDFObjectHandle::newStream(&q, out));

    // Form XObject: ordinal = 쪽의 Form Do 순서(이미지 Do 제외)
    if (!form_ordinals.empty()) {
      auto xobjs = page.getObjectHandle().getKey("/Resources").getKey("/XObject");
      std::vector<QPDFObjectHandle> forms;
      for (const auto& name : do_names) {
        auto x = xobjs.isDictionary() ? xobjs.getKey(name) : QPDFObjectHandle::newNull();
        if (x.isStream() && x.getDict().getKey("/Subtype").isNameAndEquals("/Form"))
          forms.push_back(x);
      }
      for (const auto& [fi, ords] : form_ordinals) {
        if (fi >= static_cast<int>(forms.size())) {
          r.error = "form-map";
          return r;
        }
        int show = 0, seen = 0;
        const std::string fdata = read_stream(forms[static_cast<std::size_t>(fi)]);
        const std::string fout = rewrite(fdata, ords, show, r.needs_displacement, seen, nullptr);
        forms[static_cast<std::size_t>(fi)].replaceStreamData(fout, QPDFObjectHandle::newNull(),
                                                              QPDFObjectHandle::newNull());
        ++r.forms_rewritten;
      }
    }

    QPDFWriter w(q);
    w.setOutputMemory();
    w.write();
    auto buf = w.getBufferSharedPointer();
    r.bytes.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());
    r.ok = true;
  } catch (const std::exception& e) {
    r.error = std::string("qpdf: ") + e.what();
  }
  return r;
}

}  // namespace spike
