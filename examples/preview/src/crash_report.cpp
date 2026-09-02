#if defined(__ANDROID__)

#include "crash_report.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

namespace lib_video_component::preview {
namespace {

std::atomic<bool> installed{false};
char crash_path[320] = {};

inline uintptr_t StripPac(uintptr_t address) {
  return address & 0x0000FFFFFFFFFFFFULL;
}

void CrashHandler(int signal_number, siginfo_t* info, void* context) {
  static std::atomic<bool> in_handler{false};
  if (in_handler.exchange(true)) {
    // A crash inside the handler chains straight to the default action.
    signal(signal_number, SIG_DFL);
    raise(signal_number);
    return;
  }

  uintptr_t frames[32];
  int size = 0;
  const ucontext_t* registers = static_cast<const ucontext_t*>(context);
  if (registers != nullptr) {
    if (registers->uc_mcontext.pc != 0) {
      frames[size++] = StripPac(registers->uc_mcontext.pc);
    }
    if (registers->uc_mcontext.regs[30] != 0) {
      frames[size++] = StripPac(registers->uc_mcontext.regs[30]);
    }
    uintptr_t frame_pointer = registers->uc_mcontext.regs[29];
    for (int step = 0; step < 24 && size < 32; ++step) {
      if (frame_pointer == 0 || (frame_pointer & 0x7U) != 0 || frame_pointer > 0x7FFFFFFFFFFFULL) {
        break;
      }
      const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(frame_pointer);
      const uintptr_t next = frame[0];
      const uintptr_t return_address = frame[1];
      if (next <= frame_pointer || next > 0x7FFFFFFFFFFFULL) {
        break;
      }
      if (return_address != 0) {
        frames[size++] = StripPac(return_address);
      }
      frame_pointer = next;
    }
  }

  char buffer[5120];
  int offset = snprintf(
      buffer, sizeof(buffer), "signal %d code %d fault_addr %p tid %d\n", signal_number,
      info != nullptr ? info->si_code : 0, info != nullptr ? info->si_addr : nullptr, gettid()
  );
  for (int index = 0; index < size && offset < static_cast<int>(sizeof(buffer)) - 192; ++index) {
    void* address = reinterpret_cast<void*>(frames[index]);
    Dl_info library {};
    if (dladdr(address, &library) != 0 && library.dli_fname != nullptr) {
      const char* separator = strrchr(library.dli_fname, '/');
      const char* file_name = separator != nullptr ? separator + 1 : library.dli_fname;
      offset += snprintf(
          buffer + offset, sizeof(buffer) - offset, "#%02d pc %p  %s(%s)\n", index, address, file_name,
          library.dli_sname != nullptr ? library.dli_sname : "?"
      );
    } else {
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "#%02d pc %p  (unresolved)\n", index, address);
    }
  }

  const int descriptor = open(crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (descriptor >= 0) {
    if (write(descriptor, buffer, static_cast<size_t>(offset)) < 0) {
      // The system tombstone remains the authoritative record.
    }
    close(descriptor);
  }

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

} // namespace lib_video_component::preview

#endif
