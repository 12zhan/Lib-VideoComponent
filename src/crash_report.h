#pragma once

#if defined(__ANDROID__)

#include <jni.h>

namespace huxerui {
class RootContext;
}

namespace lib_video_component::detail {

// Installs a process-wide native crash reporter. The handler captures a
// frame-pointer backtrace, writes it to <cache>/native_crash.txt, and chains
// to the default handler so the system tombstone is unchanged.
void InstallCrashReporter(const char* cache_directory);

// Resolves the application cache directory through the Android Context and
// installs the crash reporter once per process.
void InstallCrashReporterFromContext(JNIEnv* environment, jobject context);

} // namespace lib_video_component::detail

#endif
