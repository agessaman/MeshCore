#if defined(ESP_PLATFORM) && defined(WITH_MQTT_BRIDGE)

#include "CrashReport.h"

#include <atomic>
#include <stdlib.h>
#include <string.h>
#include <sdkconfig.h>
#include <esp_err.h>
#include <esp_core_dump.h>

// The summary API needs an ELF dump on flash; RISC-V targets have no observer env, so only Xtensa reads it.
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF) && \
    defined(CONFIG_IDF_TARGET_ARCH_XTENSA)
#define CRASH_REPORT_SUPPORTED 1
#else
#define CRASH_REPORT_SUPPORTED 0
#endif

namespace {
// Written once in setup() before any reader exists, then never modified.
CrashRecord s_record;
std::atomic<bool> s_valid{false};
std::atomic<bool> s_on_flash{false};
std::atomic<bool> s_erase_due{false};

bool eraseFlashDump() {
#if CRASH_REPORT_SUPPORTED
  bool expected = true;
  if (!s_on_flash.compare_exchange_strong(expected, false)) return true;
  if (esp_core_dump_image_erase() != ESP_OK) {
    s_on_flash.store(true);
    return false;
  }
#endif
  return true;
}
}  // namespace

namespace CrashReport {

void captureAtBoot() {
#if CRASH_REPORT_SUPPORTED
  static bool done = false;
  if (done) return;
  done = true;
  if (esp_core_dump_image_check() != ESP_OK) return;
  s_on_flash.store(true);

  // Several hundred bytes, so borrow it from the heap only for this read.
  esp_core_dump_summary_t* s = static_cast<esp_core_dump_summary_t*>(calloc(1, sizeof(esp_core_dump_summary_t)));
  if (s == nullptr) return;
  if (esp_core_dump_get_summary(s) == ESP_OK) {
    CrashRecord r = {};
    r.arch = CrashRecord::ARCH_XTENSA;
    r.pc = s->exc_pc;
    r.cause = s->ex_info.exc_cause;
    r.vaddr = s->ex_info.exc_vaddr;
    uint32_t depth = s->exc_bt_info.depth;
    const uint32_t src_cap = sizeof(s->exc_bt_info.bt) / sizeof(s->exc_bt_info.bt[0]);
    if (depth > src_cap) depth = src_cap;
    if (depth > (uint32_t)CrashRecord::kMaxBacktrace) depth = CrashRecord::kMaxBacktrace;
    for (uint32_t i = 0; i < depth; i++) r.bt[i] = s->exc_bt_info.bt[i];
    r.bt_depth = (uint8_t)depth;
    r.bt_corrupted = s->exc_bt_info.corrupted ? 1 : 0;
    memcpy(r.task, s->exc_task, sizeof(r.task) - 1);
    r.task[sizeof(r.task) - 1] = '\0';
    const char* sha = reinterpret_cast<const char*>(s->app_elf_sha256);
    size_t n = 0;
    while (n < sizeof(r.elf_sha) - 1 && n < sizeof(s->app_elf_sha256) && sha[n] != '\0') {
      r.elf_sha[n] = sha[n];
      n++;
    }
    r.elf_sha[n] = '\0';
    s_record = r;
    s_valid.store(true);
  }
  free(s);
#endif
}

size_t formatJson(char* buf, size_t bufsize) {
  if (!s_valid.load()) {
    if (buf && bufsize) buf[0] = '\0';
    return 0;
  }
  return CrashReportFormat::formatJson(&s_record, buf, bufsize);
}

void formatCli(char* buf, size_t bufsize) {
  if (s_valid.load()) {
    CrashReportFormat::formatCli(&s_record, buf, bufsize);
  } else if (s_on_flash.load()) {
    snprintf(buf, bufsize, "> none (dump unreadable)");
  } else {
    CrashReportFormat::formatCli(nullptr, buf, bufsize);
  }
}

void notePublished() {
  if (s_on_flash.load()) s_erase_due.store(true);
}

void service() {
  if (s_erase_due.exchange(false)) eraseFlashDump();
}

bool clear() {
  s_erase_due.store(false);
  const bool ok = eraseFlashDump();
  s_valid.store(false);
  return ok;
}

}  // namespace CrashReport

#endif  // ESP_PLATFORM && WITH_MQTT_BRIDGE
