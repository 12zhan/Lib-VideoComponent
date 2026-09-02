#pragma once

#if defined(__ANDROID__)

namespace lib_video_component::preview {

// App-level debug aid: captures a native crash backtrace into
// <cache>/native_crash.txt so the next launch can display it.
void InstallCrashReporter(const char* cache_directory);

} // namespace lib_video_component::preview

#endif
