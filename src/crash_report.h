#pragma once

#if defined(__ANDROID__)

namespace lib_video_component::detail {

// Installs a process-wide native crash reporter. The handler captures an
// unwind backtrace, writes it to <cache_directory>/native_crash.txt, and then
// chains to the default handler so the system tombstone is unchanged.
void InstallCrashReporter(const char* cache_directory);

} // namespace lib_video_component::detail

#endif
