// Spike #135: PDFium 을 자식 프로세스로 격리하면 크래시(JPX CHECK)·멈춤(렌더 폭탄)에도 부모가
// 살아남는가, 타일 왕복 비용·복구 시간은 얼마인가. POSIX(macOS·Linux) 전용 실험 — 제품 코드 아님.
//
//   spike inproc <pdf> [n]            같은 프로세스에서 타일 n 회 (기준)
//   spike child  <pdf> [n]            자식 프로세스 + 공유 메모리로 타일 n 회 (왕복 비용)
//   spike crash  <crash.pdf> <ok.pdf> 자식이 죽는 파일 → 감지 → 재시작 → 정상 파일 타일 (복구)
//   spike hang   <bomb.pdf> <ok.pdf>  자식이 멈추는 파일 → 시간 제한 → kill → 재시작 (복구)
//   spike --child <pdf> <shm> <in_fd> <out_fd>   (내부) 자식 본체
//
// 프로토콜: 부모 → 자식 파이프로 Request(쪽·배율·영역), 자식은 공유 메모리(shm_open) 에 BGRA 로
// 그린 뒤 Response(상태·렌더 ns) 를 파이프로 돌려준다. 타일은 512×512 고정 상한.

#include <fpdfview.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

constexpr int kTile = 512;
constexpr std::size_t kShmBytes = static_cast<std::size_t>(kTile) * kTile * 4;

struct Request {
  int page = 0;
  float scale = 2.0f;
  int x = 0, y = 0, w = kTile, h = kTile;
};
struct Response {
  int status = 0;  // 0 ok, 1 fail
  long long render_ns = 0;
};

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool read_all(int fd, void* buf, std::size_t n) {
  auto* p = static_cast<char*>(buf);
  while (n > 0) {
    const ssize_t r = ::read(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}
bool write_all(int fd, const void* buf, std::size_t n) {
  auto* p = static_cast<const char*>(buf);
  while (n > 0) {
    const ssize_t r = ::write(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}

// ---- PDFium 렌더 (부모 inproc 과 자식이 공유) ----
struct Renderer {
  FPDF_DOCUMENT doc = nullptr;
  FPDF_PAGE page = nullptr;
  int page_index = -1;
  bool open(const char* path) {
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
    doc = FPDF_LoadDocument(path, nullptr);
    return doc != nullptr;
  }
  // region 을 그린 BGRA 를 out(stride w*4) 에. 쪽 전체를 scale 로 놓고 (x,y) 오프셋
  bool render(const Request& rq, void* out) {
    if (page == nullptr || page_index != rq.page) {
      if (page != nullptr) FPDF_ClosePage(page);
      page = FPDF_LoadPage(doc, rq.page);
      page_index = rq.page;
      if (page == nullptr) return false;
    }
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(rq.w, rq.h, FPDFBitmap_BGRA, out, rq.w * 4);
    if (bmp == nullptr) return false;
    FPDFBitmap_FillRect(bmp, 0, 0, rq.w, rq.h, 0xFFFFFFFF);
    const int full_w = static_cast<int>(FPDF_GetPageWidthF(page) * rq.scale);
    const int full_h = static_cast<int>(FPDF_GetPageHeightF(page) * rq.scale);
    FPDF_RenderPageBitmap(bmp, page, -rq.x, -rq.y, full_w, full_h, 0, FPDF_ANNOT);
    FPDFBitmap_Destroy(bmp);
    return true;
  }
  ~Renderer() {
    if (page != nullptr) FPDF_ClosePage(page);
    if (doc != nullptr) FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
  }
};

// ---- 자식 본체 ----
int run_child(const char* pdf, const char* shm_name, int in_fd, int out_fd) {
  const bool verbose = std::getenv("SPIKE_VERBOSE") != nullptr;
  const int shm = shm_open(shm_name, O_RDWR, 0600);
  if (shm < 0) return 10;
  void* pixels = mmap(nullptr, kShmBytes, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
  if (pixels == MAP_FAILED) return 11;
  if (verbose) std::fprintf(stderr, "[child] shm ok, opening %s\n", pdf);
  Renderer r;
  if (!r.open(pdf)) return 12;
  if (verbose) std::fprintf(stderr, "[child] doc ok, waiting for requests on fd %d\n", in_fd);
  Request rq;
  while (read_all(in_fd, &rq, sizeof rq)) {
    if (verbose) std::fprintf(stderr, "[child] request page %d\n", rq.page);
    const auto t0 = Clock::now();
    Response rs;
    rs.status = r.render(rq, pixels) ? 0 : 1;
    rs.render_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    if (!write_all(out_fd, &rs, sizeof rs)) break;
  }
  return 0;
}

// ---- 부모 쪽 자식 관리 ----
struct Child {
  pid_t pid = -1;
  int to_child = -1;    // 부모가 쓴다
  int from_child = -1;  // 부모가 읽는다
  void* pixels = nullptr;
  std::string shm_name;

  bool spawn(const char* self, const char* pdf) {
    shm_name = "/onpdf-spike-" + std::to_string(getpid());
    shm_unlink(shm_name.c_str());
    const int shm = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0600);
    if (shm < 0 || ftruncate(shm, static_cast<off_t>(kShmBytes)) != 0) return false;
    pixels = mmap(nullptr, kShmBytes, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    close(shm);
    if (pixels == MAP_FAILED) return false;
    int req[2], rsp[2];
    if (pipe(req) != 0 || pipe(rsp) != 0) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // 높은 번호로 복제한다 — 3·4 로 dup2 하면 부모의 파이프 fd(3~6)와 겹쳐 닫기 순서에 따라
    // 응답 파이프가 닫힌다 (첫 실행에서 자식이 바로 죽은 원인)
    posix_spawn_file_actions_adddup2(&actions, req[0], 10);
    posix_spawn_file_actions_adddup2(&actions, rsp[1], 11);
    for (int fd : {req[0], req[1], rsp[0], rsp[1]}) posix_spawn_file_actions_addclose(&actions, fd);
    std::vector<std::string> args = {self, "--child", pdf, shm_name, "10", "11"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const int rc = posix_spawn(&pid, self, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(req[0]);
    close(rsp[1]);
    if (rc != 0) return false;
    to_child = req[1];
    from_child = rsp[0];
    return true;
  }
  // 응답을 timeout_ms 안에 받으면 true. EOF(자식 사망)·시간 초과면 false 와 사유
  bool request(const Request& rq, Response& rs, int timeout_ms, const char*& why) {
    if (!write_all(to_child, &rq, sizeof rq)) {
      why = "write failed (child gone)";
      return false;
    }
    pollfd pfd{from_child, POLLIN, 0};
    const int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0) {
      why = "timeout";
      return false;
    }
    if (!read_all(from_child, &rs, sizeof rs)) {
      why = "eof (child died)";
      return false;
    }
    why = "";
    return true;
  }
  int kill_and_reap() {
    if (pid > 0) kill(pid, SIGKILL);
    return reap();
  }
  int reap() {
    // 요청 파이프를 먼저 닫아야 자식이 EOF 로 끝난다 (그 뒤 waitpid)
    if (to_child >= 0) close(to_child);
    if (from_child >= 0) close(from_child);
    to_child = from_child = -1;
    int status = 0;
    if (pid > 0) waitpid(pid, &status, 0);
    pid = -1;
    if (pixels != nullptr && pixels != MAP_FAILED) munmap(pixels, kShmBytes);
    pixels = nullptr;
    shm_unlink(shm_name.c_str());
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
  }
};

int cmd_inproc(const char* pdf, int n) {
  Renderer r;
  if (!r.open(pdf)) return 1;
  std::vector<unsigned char> buf(kShmBytes);
  Request rq;
  if (!r.render(rq, buf.data())) {  // 워밍업
    std::printf("inproc: render failed\n");
    return 1;
  }
  const auto t0 = Clock::now();
  unsigned long sum = 0;
  for (int i = 0; i < n; ++i) {
    rq.x = (i % 2) * 256;  // 캐시 효과를 줄이려 영역을 조금 바꾼다
    if (!r.render(rq, buf.data())) return 1;
    sum += buf[static_cast<std::size_t>(i) * 4099 % kShmBytes];
  }
  std::printf("inproc: %d tiles, mean %.3f ms/tile (checksum %lu)\n", n, ms_since(t0) / n, sum);
  return 0;
}

int cmd_child(const char* self, const char* pdf, int n) {
  Child c;
  const auto ts = Clock::now();
  if (!c.spawn(self, pdf)) return 1;
  Request rq;
  Response rs;
  const char* why = "";
  if (!c.request(rq, rs, 10000, why)) {
    std::printf("child: first tile failed: %s, child exit %d\n", why, c.kill_and_reap());
    return 1;
  }
  std::printf("child: spawn + open + first tile %.1f ms\n", ms_since(ts));
  std::vector<unsigned char> local(kShmBytes);
  const auto t0 = Clock::now();
  long long render_ns = 0;
  for (int i = 0; i < n; ++i) {
    rq.x = (i % 2) * 256;
    if (!c.request(rq, rs, 10000, why)) {
      std::printf("child: tile %d failed: %s, child exit %d\n", i, why, c.kill_and_reap());
      return 1;
    }
    std::memcpy(local.data(), c.pixels, kShmBytes);  // 부모가 Bitmap 으로 복사하는 비용 포함
    render_ns += rs.render_ns;
    if (rs.status != 0) std::printf("child: tile %d status %d\n", i, rs.status);
  }
  const double total = ms_since(t0);
  std::printf(
      "child: %d tiles, mean %.3f ms/tile round trip (render inside child %.3f ms/tile, "
      "overhead %.3f ms/tile)\n",
      n, total / n, static_cast<double>(render_ns) / 1e6 / n,
      total / n - static_cast<double>(render_ns) / 1e6 / n);
  c.reap();
  return 0;
}

int cmd_crash_or_hang(const char* self, const char* bad, const char* ok, bool hang) {
  Child c;
  if (!c.spawn(self, bad)) return 1;
  Request rq;
  Response rs;
  const char* why = "";
  const auto t0 = Clock::now();
  const bool got = c.request(rq, rs, hang ? 2000 : 10000, why);
  const double detect = ms_since(t0);
  std::printf("%s: first request on bad file → %s (%s) after %.1f ms\n", hang ? "hang" : "crash",
              got ? "RESPONDED" : "failed", why, detect);
  const int code = hang ? c.kill_and_reap() : c.reap();
  std::printf("%s: child exit code %d (parent alive: yes)\n", hang ? "hang" : "crash", code);
  const auto t1 = Clock::now();
  if (!c.spawn(self, ok)) return 1;
  if (!c.request(rq, rs, 10000, why)) {
    std::printf("recovery failed: %s, child exit %d\n", why, c.kill_and_reap());
    return 1;
  }
  std::printf("%s: respawn + first good tile %.1f ms (total since failure %.1f ms)\n",
              hang ? "hang" : "crash", ms_since(t1), ms_since(t0));
  c.reap();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 6 && std::strcmp(argv[1], "--child") == 0)
    return run_child(argv[2], argv[3], std::atoi(argv[4]), std::atoi(argv[5]));
  if (argc < 3) {
    std::fprintf(stderr, "usage: spike inproc|child <pdf> [n] | crash|hang <bad.pdf> <ok.pdf>\n");
    return 2;
  }
  const std::string mode = argv[1];
  const int n = argc > 3 ? std::atoi(argv[3]) : 50;
  if (mode == "inproc") return cmd_inproc(argv[2], n);
  if (mode == "child") return cmd_child(argv[0], argv[2], n);
  if (mode == "crash" && argc > 3) return cmd_crash_or_hang(argv[0], argv[2], argv[3], false);
  if (mode == "hang" && argc > 3) return cmd_crash_or_hang(argv[0], argv[2], argv[3], true);
  return 2;
}
