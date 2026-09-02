#include "android_jni_support.h"

#include <atomic>
#include <dlfcn.h>

namespace lib_video_component::detail {
namespace {

std::atomic<JavaVM*> loaded_java_vm{nullptr};

using GetCreatedJavaVms = jint (*)(JavaVM**, jsize, jsize*);

} // namespace

void StoreLoadedJavaVm(JavaVM* virtual_machine) noexcept {
  loaded_java_vm.store(virtual_machine);
}

namespace {

JavaVM* ProcessJavaVm() {
  if (JavaVM* const loaded = loaded_java_vm.load()) {
    return loaded;
  }
  const auto get_vms = reinterpret_cast<GetCreatedJavaVms>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
  if (get_vms == nullptr) {
    return nullptr;
  }
  JavaVM* found = nullptr;
  jsize count = 0;
  if (get_vms(&found, 1, &count) != JNI_OK || count < 1) {
    return nullptr;
  }
  return found;
}

} // namespace

ScopedJniEnv::ScopedJniEnv() {
  JavaVM* const virtual_machine = ProcessJavaVm();
  if (virtual_machine == nullptr) {
    return;
  }
  // Daemon threads stay attached for the thread lifetime; repeat scopes on
  // the same thread reuse the cached environment without detach churn.
  if (virtual_machine->GetEnv(reinterpret_cast<void**>(&environment_), JNI_VERSION_1_6) != JNI_OK) {
    if (virtual_machine->AttachCurrentThreadAsDaemon(&environment_, nullptr) == JNI_OK) {
      attached_ = true;
    } else {
      environment_ = nullptr;
    }
  }
}

ScopedJniEnv::~ScopedJniEnv() = default;

bool ClearException(JNIEnv* environment) {
  if (!environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

} // namespace lib_video_component::detail

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* virtual_machine, void*) {
  lib_video_component::detail::StoreLoadedJavaVm(virtual_machine);
  return JNI_VERSION_1_6;
}
