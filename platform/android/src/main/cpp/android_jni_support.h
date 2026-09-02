#pragma once

#include <jni.h>

namespace lib_video_component::detail {

// Attaches the calling thread to the process Java VM for the scope lifetime.
// Get() returns null when no VM is available.
class ScopedJniEnv final {
public:
  ScopedJniEnv();
  ~ScopedJniEnv();

  ScopedJniEnv(const ScopedJniEnv&) = delete;
  ScopedJniEnv& operator=(const ScopedJniEnv&) = delete;

  [[nodiscard]] JNIEnv* Get() const noexcept {
    return environment_;
  }

private:
  JNIEnv* environment_ = nullptr;
  bool attached_ = false;
};

bool ClearException(JNIEnv* environment);

} // namespace lib_video_component::detail
