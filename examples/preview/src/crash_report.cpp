#if defined(__ANDROID__)

#include "crash_report.h"

#include <atomic>
#include <csetjmp>
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

// Recovery point for faults inside the handler itself; the nested signal
// jumps back here so the already-written report survives.
std::jmp_buf handler_recovery;
std::atomic<bool> handler_recovery_active{false};

constexpr std::size_t kAlternateStackSize = 64 * 1024;
std::byte alternate_stack[kAlternateStackSize] __attribute__((aligned(16)));

inline uintptr_t StripPac(uintptr_t address) {
  return address & 0x0000FFFFFFFFFFFFULL;
}

void WriteReport(const char* text, int length) {
  const int descriptor = open(crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (descriptor >= 0) {
    if (write(descriptor, text, static_cast<size_t>(length)) < 0) {
      // The system tombstone remains the authoritative record.
    }
    close(descriptor);
  }
}

int FormatFrame(char* buffer, int capacity, int index, void* address) {
  Dl_info library {};
  if (dladdr(address, &library) != 0 && library.dli_fname != nullptr) {
    const char* separator = strrchr(library.dli_fname, '/');
    const char* file_name = separator != nullptr ? separator + 1 : library.dli_fname;
    return snprintf(
        buffer, capacity, "#%02d pc %p  %s(%s)\n", index, address, file_name,
        library.dli_sname != nullptr ? library.dli_sname : "?"
    );
  }
  return snprintf(buffer, capacity, "#%02d pc %p  (unresolved)\n", index, address);
}

void CrashHandler(int signal_number, siginfo_t* info, void* context) {
  static std::atomic<bool> in_handler{false};
  if (in_handler.exchange(true)) {
    // A fault inside this handler: recover to the saved point when possible.
    if (handler_recovery_active.load()) {
      siglongjmp(handler_recovery, 1);
    }
    signal(signal_number, SIG_DFL);
    raise(signal_number);
    return;
  }

  // Phase 1: register-only report. Reading the ucontext cannot fault, so this
  // part always produces a file.
  const ucontext_t* registers = static_cast<const ucontext_t*>(context);
  const uintptr_t pc = registers != nullptr ? StripPac(registers->uc_mcontext.pc) : 0;
  const uintptr_t lr = registers != nullptr ? StripPac(registers->uc_mcontext.regs[30]) : 0;

  char buffer[4096];
  int offset = snprintf(
      buffer, sizeof(buffer), "signal %d code %d fault_addr %p tid %d\n", signal_number,
      info != nullptr ? info->si_code : 0, info != nullptr ? info->si_addr : nullptr, gettid()
  );
  if (pc != 0) {
    offset += FormatFrame(buffer + offset, sizeof(buffer) - offset, 0, reinterpret_cast<void*>(pc));
  }
  if (lr != 0) {
    offset += FormatFrame(buffer + offset, sizeof(buffer) - offset, 1, reinterpret_cast<void*>(lr));
  }
  WriteReport(buffer, offset);

  // Phase 2: walk the frame-pointer chain under a recovery point; faults in
  // the walk jump back and the phase-one report is already on disk.
  if (registers != nullptr && sigsetjmp(handler_recovery, 1) == 0) {
    handler_recovery_active.store(true);
    uintptr_t frame_pointer = registers->uc_mcontext.regs[29];
    for (int step = 0; step < 20 && offset < static_cast<int>(sizeof(buffer)) - 192; ++step) {
      if (frame_pointer == 0 || (frame_pointer & 0x7U) != 0 || frame_pointer > 0x7FFFFFFFFFFFULL) {
        break;
      }
      const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(frame_pointer);
      const uintptr_t next = frame[0];
      const uintptr_t return_address = StripPac(frame[1]);
      if (next <= frame_pointer || next > 0x7FFFFFFFFFFFULL) {
        break;
      }
      if (return_address != 0) {
        offset += FormatFrame(buffer + offset, sizeof(buffer) - offset, step + 2,
                              reinterpret_cast<void*>(return_address));
      }
      frame_pointer = next;
    }
    handler_recovery_active.store(false);
    WriteReport(buffer, offset);
  } else {
    handler_recovery_active.store(false);
    // The walk faulted; the register-only report is already written. Extend
    // it with a marker instead of losing it.
    const int marker = snprintf(buffer + offset, sizeof(buffer) - offset, "(stack walk faulted)\n");
    if (marker > 0) {
      WriteReport(buffer, offset + marker);
    }
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

  // Run the handler on an alternate stack so a smashed main stack cannot
  // prevent the report from being written.
  stack_t alternate {};
  alternate.ss_sp = alternate_stack;
  alternate.ss_size = sizeof(alternate_stack);
  alternate.ss_flags = 0;
  sigaltstack(&alternate, nullptr);

  struct sigaction action {};
  action.sa_sigaction = CrashHandler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, nullptr);
  sigaction(SIGABRT, &action, nullptr);
  sigaction(SIGBUS, &action, nullptr);
}

} // namespace lib_video_component::preview

#endif
