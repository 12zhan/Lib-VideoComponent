// Android cover extraction through MediaMetadataRetriever, driven entirely
// from C++ over JNI. No Java sources are compiled into the library.

#include <jni.h>

#include <string>
#include <utility>

#include "android_jni_support.h"

#include <huxerui/data.h>

#include "lib_video_component/lib_video_component.h"

namespace lib_video_component {
namespace {

// Records the most recent extraction result for on-device diagnosis.
std::string last_cover_error = "not attempted";

} // namespace

huxerui::Bytes ExtractVideoCoverPng(std::string_view path) {
  const detail::ScopedJniEnv scoped;
  JNIEnv* environment = scoped.Get();
  if (environment == nullptr || path.empty()) {
    last_cover_error = "java vm unavailable";
    return {};
  }

  const jclass retriever_class = environment->FindClass("android/media/MediaMetadataRetriever");
  if (detail::ClearException(environment) || retriever_class == nullptr) {
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
    detail::ClearException(environment);
    last_cover_error = "retriever method signatures changed";
    return {};
  }

  const jobject retriever = environment->NewObject(retriever_class, constructor);
  if (detail::ClearException(environment) || retriever == nullptr) {
    last_cover_error = "retriever construction failed";
    return {};
  }
  huxerui::Bytes result;
  do {
    const jstring source = environment->NewStringUTF(std::string(path).c_str());
    environment->CallVoidMethod(retriever, set_data_source, source);
    const bool failed = detail::ClearException(environment);
    environment->DeleteLocalRef(source);
    if (failed) {
      last_cover_error = "setDataSource threw";
      break;
    }

    const jobject bitmap = environment->CallObjectMethod(retriever, get_frame);
    if (detail::ClearException(environment) || bitmap == nullptr) {
      last_cover_error = "getFrameAtTime returned no frame";
      break;
    }

    const jclass bitmap_class = environment->FindClass("android/graphics/Bitmap");
    const jclass format_class = environment->FindClass("android/graphics/Bitmap$CompressFormat");
    const jclass stream_class = environment->FindClass("java/io/ByteArrayOutputStream");
    if (detail::ClearException(environment) || bitmap_class == nullptr || format_class == nullptr ||
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
    if (detail::ClearException(environment) || png_field == nullptr || compress == nullptr ||
        stream_constructor == nullptr || to_bytes == nullptr || png_format == nullptr || stream == nullptr) {
      last_cover_error = "png compress methods unavailable";
      environment->DeleteLocalRef(bitmap);
      break;
    }

    environment->CallBooleanMethod(bitmap, compress, png_format, 90, stream);
    const bool compress_failed = detail::ClearException(environment);
    environment->DeleteLocalRef(bitmap);
    if (compress_failed) {
      last_cover_error = "bitmap compress threw";
      break;
    }

    const jbyteArray encoded = static_cast<jbyteArray>(environment->CallObjectMethod(stream, to_bytes));
    if (detail::ClearException(environment) || encoded == nullptr) {
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
  detail::ClearException(environment);
  environment->DeleteLocalRef(retriever);
  last_cover_error = "ok";
  return result;
}

std::string LastCoverDiagnostic() {
  return last_cover_error;
}

} // namespace lib_video_component
