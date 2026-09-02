// Android cover extraction through MediaMetadataRetriever, driven entirely
// from C++ over JNI. No Java sources are compiled into the library; the Java
// VM is resolved at runtime from the process.

#include <jni.h>

#include <dlfcn.h>

#include <atomic>
#include <string>
#include <utility>

#include <huxerui/data.h>

#include "lib_video_component/lib_video_component.h"

namespace lib_video_component {
namespace {

using GetCreatedJavaVms = jint (*)(JavaVM**, jsize, jsize*);

// Captured when the hosting library is loaded; dlsym remains a fallback for
// hosts that never invoke JNI_OnLoad.
std::atomic<JavaVM*> loaded_java_vm{nullptr};

// Records the most recent extraction failure for on-device diagnosis.
std::string last_cover_error = "not attempted";

JavaVM* ProcessJavaVm() {
  if (const JavaVM* loaded = loaded_java_vm.load()) {
    return const_cast<JavaVM*>(loaded);
  }
  const auto get_vms = reinterpret_cast<GetCreatedJavaVms>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
  if (get_vms == nullptr) {
    last_cover_error = "java vm not found";
    return nullptr;
  }
  JavaVM* found = nullptr;
  jsize count = 0;
  if (get_vms(&found, 1, &count) != JNI_OK || count < 1) {
    last_cover_error = "no created java vm";
    return nullptr;
  }
  return found;
}

bool ClearException(JNIEnv* environment) {
  if (!environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

// Attaches the calling worker thread to the VM for the duration of the scope.
class ScopedJniEnv final {
public:
  explicit ScopedJniEnv(JavaVM* virtual_machine) : virtual_machine_(virtual_machine) {
    if (virtual_machine_ == nullptr) {
      return;
    }
    if (virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment_), JNI_VERSION_1_6) != JNI_OK) {
      if (virtual_machine_->AttachCurrentThreadAsDaemon(&environment_, nullptr) == JNI_OK) {
        attached_ = true;
      } else {
        environment_ = nullptr;
      }
    }
  }

  ~ScopedJniEnv() {
    if (attached_) {
      virtual_machine_->DetachCurrentThread();
    }
  }

  [[nodiscard]] JNIEnv* Get() const noexcept {
    return environment_;
  }

private:
  JavaVM* virtual_machine_ = nullptr;
  JNIEnv* environment_ = nullptr;
  bool attached_ = false;
};

} // namespace

huxerui::Bytes ExtractVideoCoverPng(std::string_view path) {
  const ScopedJniEnv scoped(ProcessJavaVm());
  JNIEnv* environment = scoped.Get();
  if (environment == nullptr || path.empty()) {
    return {};
  }

  const jclass retriever_class = environment->FindClass("android/media/MediaMetadataRetriever");
  if (ClearException(environment) || retriever_class == nullptr) {
    last_cover_error = "MediaMetadataRetriever class not found";
    return {};
  }
  const jmethodID constructor = environment->GetMethodID(retriever_class, "<init>", "()V");
  const jmethodID set_data_source =
      environment->GetMethodID(retriever_class, "setDataSource", "(Ljava/lang/String;)V");
  const jmethodID get_frame =
      environment->GetMethodID(retriever_class, "getFrameAtTime", "()Landroid/graphics/Bitmap;");
  const jmethodID release = environment->GetMethodID(retriever_class, "release", "()V");
  if (constructor == nullptr || set_data_source == nullptr || get_frame == nullptr || release == nullptr) {
    ClearException(environment);
    last_cover_error = "retriever method signatures changed";
    return {};
  }

  const jobject retriever = environment->NewObject(retriever_class, constructor);
  if (ClearException(environment) || retriever == nullptr) {
    last_cover_error = "retriever construction failed";
    return {};
  }
  huxerui::Bytes result;
  do {
    const jstring source = environment->NewStringUTF(std::string(path).c_str());
    environment->CallVoidMethod(retriever, set_data_source, source);
    const bool failed = ClearException(environment);
    environment->DeleteLocalRef(source);
    if (failed) {
      last_cover_error = "setDataSource threw";
      break;
    }

    const jobject bitmap = environment->CallObjectMethod(retriever, get_frame);
    if (ClearException(environment) || bitmap == nullptr) {
      last_cover_error = "getFrameAtTime returned no frame";
      break;
    }

    // Compress the frame to PNG through a ByteArrayOutputStream.
    const jclass bitmap_class = environment->FindClass("android/graphics/Bitmap");
    const jclass format_class = environment->FindClass("android/graphics/Bitmap$CompressFormat");
    const jclass stream_class = environment->FindClass("java/io/ByteArrayOutputStream");
    if (ClearException(environment) || bitmap_class == nullptr || format_class == nullptr ||
        stream_class == nullptr) {
      last_cover_error = "bitmap compress classes not found";
      environment->DeleteLocalRef(bitmap);
      break;
    }
    const jfieldID png_field =
        environment->GetStaticFieldID(format_class, "PNG", "Landroid/graphics/Bitmap$CompressFormat;");
    const jmethodID compress = environment->GetMethodID(
        bitmap_class, "compress", "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z"
    );
    const jmethodID stream_constructor = environment->GetMethodID(stream_class, "<init>", "()V");
    const jmethodID to_bytes = environment->GetMethodID(stream_class, "toByteArray", "()[B");
    const jobject png_format = environment->GetStaticObjectField(format_class, png_field);
    const jobject stream = environment->NewObject(stream_class, stream_constructor);
    if (ClearException(environment) || png_field == nullptr || compress == nullptr ||
        stream_constructor == nullptr || to_bytes == nullptr || png_format == nullptr || stream == nullptr) {
      last_cover_error = "png compress methods unavailable";
      environment->DeleteLocalRef(bitmap);
      break;
    }

    environment->CallBooleanMethod(bitmap, compress, png_format, 90, stream);
    const bool compress_failed = ClearException(environment);
    environment->DeleteLocalRef(bitmap);
    if (compress_failed) {
      last_cover_error = "bitmap compress threw";
      break;
    }

    const jbyteArray encoded = static_cast<jbyteArray>(environment->CallObjectMethod(stream, to_bytes));
    if (ClearException(environment) || encoded == nullptr) {
      last_cover_error = "encoded bytes unavailable";
      break;
    }
    const jsize size = environment->GetArrayLength(encoded);
    jbyte* elements = environment->GetByteArrayElements(encoded, nullptr);
    if (elements != nullptr) {
      const auto* bytes = reinterpret_cast<const std::byte*>(elements);
      result.assign(bytes, bytes + size);
      environment->ReleaseByteArrayElements(encoded, elements, JNI_ABORT);
    }
    environment->DeleteLocalRef(encoded);
  } while (false);

  environment->CallVoidMethod(retriever, release);
  ClearException(environment);
  environment->DeleteLocalRef(retriever);
  last_cover_error = "ok";
  return result;
}

std::string LastCoverDiagnostic() {
  return last_cover_error;
}

void Install(huxerui::RootContext&) {}

} // namespace lib_video_component

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* virtual_machine, void*) {
  lib_video_component::loaded_java_vm.store(virtual_machine);
  return JNI_VERSION_1_6;
}
