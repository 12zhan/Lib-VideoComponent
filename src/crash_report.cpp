#if defined(__ANDROID__)

#include "crash_report.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

namespace lib_video_component::detail {
namespace {

std::atomic<bool> installed{false};
char crash_path[320] = {};

struct BacktraceState {
  void** frames;
  int capacity;
  int size = 0;
};

_Unwind_Reason_Code CaptureFrame(_Unwind_Context* context, void* state_context) {
  auto* state = static_cast<BacktraceState*>(state_context);
  const uintptr_t pc = _Unwind_GetIP(context);
  if (state->size < state->capacity && pc != 0) {
    state->frames[state->size++] = reinterpret_cast<void*>(pc);
  }
  return _URC_NO_REASON;
}

// Async-signal context: only raw open/write/close and reentrant helpers run here.
void CrashHandler(int signal_number, siginfo_t* info, void*) {
  void* frames[40];
  BacktraceState state{frames, 40};
  _Unwind_Backtrace(CaptureFrame, &state);

  char buffer[6144];
  int offset = snprintf(
      buffer, sizeof(buffer), "signal %d code %d fault_addr %p tid %d\n", signal_number,
      info != nullptr ? info->si_code : 0, info != nullptr ? info->si_addr : nullptr, gettid()
  );
  for (int index = 0; index < state.size && offset < static_cast<int>(sizeof(buffer)) - 200; ++index) {
    void* address = frames[index];
    Dl_info library{};
    if (dladdr(address, &library) != 0 && library.dli_fname != nullptr) {
      const char* separator = strrchr(library.dli_fname, '/');
      const char* file_name = separator != nullptr ? separator + 1 : library.dli_fname;
      if (library.dli_sname != nullptr) {
        offset += snprintf(
            buffer + offset, sizeof(buffer) - offset, "#%02d pc %p  %s(%s+%ld)\n", index, address, file_name,
            library.dli_sname, static_cast<long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(library.dli_saddr))
        );
      } else {
        offset += snprintf(
            buffer + offset, sizeof(buffer) - offset, "#%02d pc %p  %s+%lx\n", index, address, file_name,
            static_cast<unsigned long>(reinterpret_cast<char*>(address) - reinterpret_cast<char*>(library.dli_fbase))
        );
      }
    } else {
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "#%02d pc %p  (unresolved)\n", index, address);
    }
  }

  const int descriptor = open(crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (descriptor >= 0) {
    if (write(descriptor, buffer, static_cast<size_t>(offset)) < 0) {
      // Best effort only; the system tombstone remains the authoritative record.
    }
    close(descriptor);
  }

  // Chain to the default handler so the system still records its tombstone.
  signal(signal_number, SIG_DFL);
  raise(signal_number);
}

} // namespace

void InstallCrashReporter(const char* cache_directory) {
  if (installed.exchange(true)) {
    return;
  }
  snprintf(crash_path, sizeof(crash_path), "%s/native_crash.txt", cache_directory);
  struct sigaction action {};
  action.sa_sigaction = CrashHandler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, nullptr);
  sigaction(SIGABRT, &action, nullptr);
  sigaction(SIGBUS, &action, nullptr);
}

} // namespace lib_video_component::detail

#endif
