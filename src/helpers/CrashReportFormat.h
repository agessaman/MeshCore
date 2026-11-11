#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Compact record of the panic that left a core dump on flash, kept for this boot.
struct CrashRecord {
  static const int kMaxBacktrace = 8;
  enum : uint8_t { ARCH_NONE = 0, ARCH_XTENSA = 1, ARCH_RISCV = 2 };

  uint32_t pc;
  uint32_t cause;   // Xtensa EXCCAUSE or RISC-V mcause
  uint32_t vaddr;   // Xtensa EXCVADDR or RISC-V mtval
  uint32_t bt[kMaxBacktrace];
  char task[16];
  char elf_sha[9];  // first 8 hex chars of the crashing image's ELF SHA-256
  uint8_t arch;     // ARCH_NONE means no record
  uint8_t bt_depth;
  uint8_t bt_corrupted;
};
static_assert(sizeof(CrashRecord) <= 128, "crash record must stay small");

namespace CrashReportFormat {

// Buffer size for the JSON fragment, terminator included.
static const size_t kJsonMax = 160;

// Appends whole tokens only, so a full buffer never holds half a token.
struct BoundedWriter {
  char* buf;
  size_t cap;
  size_t len;

  BoundedWriter(char* b, size_t c) : buf(b), cap(c), len(0) {
    if (buf && cap) buf[0] = '\0';
  }
  bool fits(size_t n, size_t reserve = 0) const {
    return buf != nullptr && cap > 0 && len + n + reserve + 1 <= cap;
  }
  bool put(const char* s, size_t n, size_t reserve = 0) {
    if (!fits(n, reserve)) return false;
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = '\0';
    return true;
  }
  bool str(const char* s, size_t reserve = 0) { return put(s, strlen(s), reserve); }
  bool fmt(const char* f, uint32_t v, size_t reserve = 0) {
    char t[24];
    int n = snprintf(t, sizeof(t), f, (unsigned long)v);
    return n > 0 && put(t, (size_t)n, reserve);
  }
};

// Copies a task name with quotes, backslashes, spaces and control bytes replaced by '?'.
static inline void sanitizeTask(const char* in, char* out, size_t out_size) {
  if (out_size == 0) return;
  size_t i = 0;
  if (in != nullptr) {
    for (; i + 1 < out_size && i < 15 && in[i] != '\0'; i++) {
      unsigned char c = (unsigned char)in[i];
      out[i] = (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') ? '?' : (char)c;
    }
  }
  if (i == 0 && out_size > 1) out[i++] = '?';
  out[i] = '\0';
}

static inline bool hasRecord(const CrashRecord* r) {
  return r != nullptr && r->arch != CrashRecord::ARCH_NONE;
}

static inline int depthOf(const CrashRecord& r) {
  return r.bt_depth > CrashRecord::kMaxBacktrace ? CrashRecord::kMaxBacktrace : r.bt_depth;
}

// One-line CLI reply: "> task=.. pc=.. exc=.. va=.. sha=.. bt=a b c [!]" or "> none".
static inline size_t formatCli(const CrashRecord* r, char* buf, size_t bufsize) {
  BoundedWriter w(buf, bufsize);
  if (!hasRecord(r)) {
    w.str("> none");
    return w.len;
  }
  char task[16];
  sanitizeTask(r->task, task, sizeof(task));
  w.str("> task=") && w.str(task);
  w.fmt(" pc=%08lx", r->pc);
  w.fmt(" exc=%lu", r->cause);
  w.fmt(" va=%08lx", r->vaddr);
  if (r->elf_sha[0] != '\0') {
    char sha[9];
    sanitizeTask(r->elf_sha, sha, sizeof(sha));
    w.str(" sha=") && w.str(sha);
  }
  const int depth = depthOf(*r);
  if (depth > 0 && w.str(" bt=")) {
    for (int i = 0; i < depth; i++) {
      if (!w.fmt(i == 0 ? "%08lx" : " %08lx", r->bt[i])) break;
    }
  }
  if (r->bt_corrupted) w.str(" !");
  return w.len;
}

// JSON object fragment for the status `crash` field. Returns 0 and leaves an empty
// string when there is no record or the mandatory fields do not fit; trailing
// backtrace entries are dropped first so the result is always well-formed.
static inline size_t formatJson(const CrashRecord* r, char* buf, size_t bufsize) {
  BoundedWriter w(buf, bufsize);
  if (!hasRecord(r)) return 0;
  char task[16];
  sanitizeTask(r->task, task, sizeof(task));
  char sha[9] = "";
  if (r->elf_sha[0] != '\0') sanitizeTask(r->elf_sha, sha, sizeof(sha));

  bool ok = w.str("{\"task\":\"") && w.str(task) &&
            w.fmt("\",\"pc\":\"%08lx\"", r->pc) &&
            w.fmt(",\"exc\":%lu", r->cause) &&
            w.fmt(",\"va\":\"%08lx\"", r->vaddr);
  if (ok && sha[0] != '\0') ok = w.str(",\"sha\":\"") && w.str(sha) && w.str("\"");
  const int depth = depthOf(*r);
  // Reserve room for the closing quote and brace before each optional entry.
  if (ok && (depth > 0 || r->bt_corrupted) && w.fits(7 + (depth > 0 ? 8 : 1), 2)) {
    ok = w.str(",\"bt\":\"", 2);
    bool first = true;
    for (int i = 0; ok && i < depth; i++) {
      if (!w.fmt(first ? "%08lx" : " %08lx", r->bt[i], 2)) break;
      first = false;
    }
    if (ok && r->bt_corrupted) w.str(first ? "!" : " !", 2);
    if (ok) ok = w.str("\"");
  }
  if (ok) ok = w.str("}");
  if (!ok) {
    if (buf && bufsize) buf[0] = '\0';
    return 0;
  }
  return w.len;
}

}  // namespace CrashReportFormat
