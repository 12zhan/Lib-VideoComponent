#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <jni.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/jni.h>
#include <huxerui/android/platform_registry.h>
#include <huxerui/platform_adapter.h>

#include "video_session.h"

namespace lib_video_component::detail {
namespace {

constexpr char kVideoPlayerClass[] = "com/example/libvideocomponent/VideoPlayer";

// Status codes shared with the Java VideoPlayer implementation.
enum JavaVideoStatus {
  kJavaIdle = 0,
  kJavaPreparing = 1,
  kJavaPlaying = 2,
  kJavaPaused = 3,
  kJavaCompleted = 4,
  kJavaFailed = 5,
};

bool ClearJavaException(JNIEnv* environment) {
  if (!environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

VideoStatus StatusFromJava(jint code) {
  switch (code) {
  case kJavaPreparing:
    return VideoStatus::Preparing;
  case kJavaPlaying:
    return VideoStatus::Playing;
  case kJavaPaused:
    return VideoStatus::Paused;
  case kJavaCompleted:
    return VideoStatus::Completed;
  case kJavaFailed:
    return VideoStatus::Failed;
  default:
    return VideoStatus::Idle;
  }
}

std::uint8_t ClampChannel(float value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 255.0F));
}

// BT.601 limited-range YUV to sRGB with generic plane strides following
// ImageFormat.YUV_420_888, which covers planar and semi-planar layouts.
void ConvertYuvToRgba(
    const std::uint8_t* y_plane, const std::uint8_t* u_plane, const std::uint8_t* v_plane, int y_row_stride,
    int u_row_stride, int v_row_stride, int y_pixel_stride, int u_pixel_stride, int v_pixel_stride, int width,
    int height, std::uint8_t* rgba
) {
  for (int row = 0; row < height; ++row) {
    const int chroma_row = row >> 1;
    const std::uint8_t* y_row = y_plane + row * y_row_stride;
    const std::uint8_t* u_row = u_plane + chroma_row * u_row_stride;
    const std::uint8_t* v_row = v_plane + chroma_row * v_row_stride;
    std::uint8_t* out = rgba + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4U;

    for (int column = 0; column < width; ++column) {
      const int chroma_column = column >> 1;
      const float luma = static_cast<float>(y_row[column * y_pixel_stride]);
      const float cb = static_cast<float>(u_row[chroma_column * u_pixel_stride]) - 128.0F;
      const float cr = static_cast<float>(v_row[chroma_column * v_pixel_stride]) - 128.0F;
      *out++ = ClampChannel(luma + 1.402F * cr);
      *out++ = ClampChannel(luma - 0.344136F * cb - 0.714136F * cr);
      *out++ = ClampChannel(luma + 1.772F * cb);
      *out++ = 255U;
    }
  }
}

// One Android playback session. The Java VideoPlayer owns MediaPlayer and the
// ImageReader surface; each frame crosses JNI once, is converted here, copied
// into a reusable Bitmap, and published through ExternalTextureSource.
class AndroidVideoSession final : public VideoSession, public std::enable_shared_from_this<AndroidVideoSession> {
public:
  AndroidVideoSession(huxerui::PlatformAdapter& adapter, const VideoOpenOptions& open)
      : adapter_(&adapter),
        on_texture_(open.on_texture),
        on_status_(open.on_status),
        // MediaPlayer.setDataSource expects a bare path for local files, not a file: URI.
        source_uri_(open.source.uri.Scheme() == "file" ? std::string{open.source.uri.Path()}
                                                       : open.source.uri.ToString()),
        initial_playing_(open.options.auto_play),
        muted_(open.options.muted),
        loop_(open.options.loop) {}

  ~AndroidVideoSession() override {
    Close();
  }

  AndroidVideoSession(const AndroidVideoSession&) = delete;
  AndroidVideoSession& operator=(const AndroidVideoSession&) = delete;
  AndroidVideoSession(AndroidVideoSession&&) = delete;
  AndroidVideoSession& operator=(AndroidVideoSession&&) = delete;

  // Runs on the UI thread inside the module factory.
  void Initialize(JNIEnv* environment, jobject context) {
    bridge_ = new std::weak_ptr<AndroidVideoSession>(shared_from_this());

    huxerui::android::LocalRef<jclass> player_class(environment, environment->FindClass(kVideoPlayerClass));
    if (!player_class) {
      ClearJavaException(environment);
      delete bridge_;
      bridge_ = nullptr;
      throw std::logic_error("HuxerUI video player class could not be found");
    }
    const jmethodID constructor =
        environment->GetMethodID(player_class.Get(), "<init>", "(Landroid/content/Context;JLjava/lang/String;ZZZ)V");
    const jmethodID dispose = environment->GetMethodID(player_class.Get(), "dispose", "()V");
    const jmethodID set_playing = environment->GetMethodID(player_class.Get(), "setPlaying", "(Z)V");
    if (constructor == nullptr || dispose == nullptr || set_playing == nullptr) {
      ClearJavaException(environment);
      delete bridge_;
      bridge_ = nullptr;
      throw std::logic_error("HuxerUI video player methods do not match the platform bridge");
    }

    huxerui::android::LocalRef<jstring> source(environment, environment->NewStringUTF(source_uri_.c_str()));
    huxerui::android::LocalRef<jobject> player(
        environment,
        environment->NewObject(
            player_class.Get(), constructor, context,
            static_cast<jlong>(reinterpret_cast<std::uintptr_t>(bridge_)), source.Get(),
            initial_playing_ ? JNI_TRUE : JNI_FALSE, muted_ ? JNI_TRUE : JNI_FALSE, loop_ ? JNI_TRUE : JNI_FALSE
        )
    );
    if (!player || ClearJavaException(environment)) {
      delete bridge_;
      bridge_ = nullptr;
      throw std::logic_error("HuxerUI video player could not be created");
    }

    const std::lock_guard lock(lifecycle_mutex_);
    ui_environment_ = environment;
    player_ = environment->NewGlobalRef(player.Get());
    dispose_ = dispose;
    set_playing_ = set_playing;
  }

  void SetPlaying(bool playing) override {
    JNIEnv* environment = nullptr;
    jobject player = nullptr;
    jmethodID method = nullptr;
    {
      const std::lock_guard lock(lifecycle_mutex_);
      if (closed_ || player_ == nullptr) {
        return;
      }
      environment = ui_environment_;
      player = player_;
      method = set_playing_;
    }
    environment->CallVoidMethod(player, method, playing ? JNI_TRUE : JNI_FALSE);
    ClearJavaException(environment);
  }

  void Close() noexcept override {
    JNIEnv* environment = nullptr;
    jobject player = nullptr;
    jmethodID method = nullptr;
    {
      const std::lock_guard lock(lifecycle_mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
      environment = ui_environment_;
      player = player_;
      method = dispose_;
      player_ = nullptr;
    }
    if (environment == nullptr) {
      return;
    }
    {
      // Stop publishing first; late frames observe closed_ under frame_mutex_.
      const std::lock_guard frame_lock(frame_mutex_);
      if (source_.has_value()) {
        source_->Finish();
      }
      ReleaseFrameObjects(environment);
    }
    if (player != nullptr && method != nullptr) {
      environment->CallVoidMethod(player, method);
      ClearJavaException(environment);
      environment->DeleteGlobalRef(player);
    }
    // bridge_ stays allocated until Java confirms shutdown through
    // OnDestroyed; late player-thread callbacks may still dereference it.
  }

  void OnDestroyed() noexcept {
    // Runs on the player thread after release() finished; no further native
    // callbacks can arrive, so the bridge can finally be freed.
    delete bridge_;
    bridge_ = nullptr;
  }

  // JNI entry points invoked from the Java player thread.

  void OnPrepared(JNIEnv* environment, jint width, jint height) {
    const std::lock_guard lock(frame_mutex_);
    if (closed_) {
      return;
    }

    try {
      source_.emplace(huxerui::Size{static_cast<float>(width), static_cast<float>(height)});
    } catch (const std::invalid_argument&) {
      ReportFailed("HuxerUI video frame surface could not be created");
      return;
    }

    huxerui::android::LocalRef<jclass> bitmap_class(environment, environment->FindClass("android/graphics/Bitmap"));
    huxerui::android::LocalRef<jclass> config_class(
        environment, environment->FindClass("android/graphics/Bitmap$Config")
    );
    if (!bitmap_class || !config_class) {
      ClearJavaException(environment);
      ReportFailed("HuxerUI video bitmap classes are unavailable");
      return;
    }
    const jfieldID argb_field =
        environment->GetStaticFieldID(config_class.Get(), "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    const jmethodID create_bitmap = environment->GetStaticMethodID(
        bitmap_class.Get(), "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"
    );
    if (argb_field == nullptr || create_bitmap == nullptr) {
      ClearJavaException(environment);
      ReportFailed("HuxerUI video bitmap allocation methods are unavailable");
      return;
    }
    const huxerui::android::LocalRef<jobject> config(
        environment, static_cast<jobject>(environment->GetStaticObjectField(config_class.Get(), argb_field))
    );
    huxerui::android::LocalRef<jobject> bitmap(
        environment,
        environment->CallStaticObjectMethod(bitmap_class.Get(), create_bitmap, width, height, config.Get())
    );
    if (ClearJavaException(environment) || !bitmap) {
      ReportFailed("HuxerUI video bitmap allocation failed");
      return;
    }

    bitmap_class_ = static_cast<jclass>(environment->NewGlobalRef(bitmap_class.Get()));
    copy_pixels_ = environment->GetMethodID(bitmap_class_, "copyPixelsFromBuffer", "(Ljava/nio/Buffer;)V");
    bitmap_ = environment->NewGlobalRef(bitmap.Get());
    rgba_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0U);

    const huxerui::ExternalTexture texture = source_->Texture();
    const auto on_texture = on_texture_;
    adapter_->DispatchToUIThread([on_texture, texture] {
      if (on_texture) {
        on_texture(texture);
      }
    });
  }

  void OnFrame(
      JNIEnv* environment, jobject y_buffer, jobject u_buffer, jobject v_buffer, jint y_row_stride, jint u_row_stride,
      jint v_row_stride, jint y_pixel_stride, jint u_pixel_stride, jint v_pixel_stride, jint width, jint height
  ) {
    const std::uint8_t* y_plane = static_cast<const std::uint8_t*>(environment->GetDirectBufferAddress(y_buffer));
    const std::uint8_t* u_plane = static_cast<const std::uint8_t*>(environment->GetDirectBufferAddress(u_buffer));
    const std::uint8_t* v_plane = static_cast<const std::uint8_t*>(environment->GetDirectBufferAddress(v_buffer));

    const std::lock_guard lock(frame_mutex_);
    if (closed_ || !source_.has_value() || bitmap_ == nullptr || copy_pixels_ == nullptr) {
      return;
    }
    if (y_plane == nullptr || u_plane == nullptr || v_plane == nullptr) {
      return;
    }

    ConvertYuvToRgba(
        y_plane, u_plane, v_plane, y_row_stride, u_row_stride, v_row_stride, y_pixel_stride, u_pixel_stride,
        v_pixel_stride, width, height, rgba_.data()
    );

    const jobject pixels = environment->NewDirectByteBuffer(rgba_.data(), static_cast<jlong>(rgba_.size()));
    environment->CallVoidMethod(bitmap_, copy_pixels_, pixels);
    if (ClearJavaException(environment)) {
      return;
    }
    source_->Publish(environment, bitmap_);
  }

  void OnStatus(JNIEnv* environment, jint code, jstring message) {
    std::string text;
    if (message != nullptr) {
      const char* characters = environment->GetStringUTFChars(message, nullptr);
      if (characters != nullptr) {
        text = characters;
        environment->ReleaseStringUTFChars(message, characters);
      }
    }
    const auto on_status = on_status_;
    const VideoStatus status = StatusFromJava(code);
    adapter_->DispatchToUIThread([on_status, status, text] {
      if (on_status) {
        on_status(status, text);
      }
    });
  }

private:
  void ReleaseFrameObjects(JNIEnv* environment) noexcept {
    if (bitmap_ != nullptr) {
      environment->DeleteGlobalRef(bitmap_);
      bitmap_ = nullptr;
    }
    if (bitmap_class_ != nullptr) {
      environment->DeleteGlobalRef(bitmap_class_);
      bitmap_class_ = nullptr;
    }
    copy_pixels_ = nullptr;
    source_.reset();
    rgba_.clear();
    rgba_.shrink_to_fit();
  }

  void ReportFailed(const std::string& message) {
    const auto on_status = on_status_;
    adapter_->DispatchToUIThread([on_status, message] {
      if (on_status) {
        on_status(VideoStatus::Failed, message);
      }
    });
  }

  huxerui::PlatformAdapter* adapter_ = nullptr;
  std::function<void(huxerui::ExternalTexture)> on_texture_;
  std::function<void(VideoStatus, std::string)> on_status_;
  std::string source_uri_;
  bool initial_playing_ = false;
  bool muted_ = false;
  bool loop_ = false;

  std::mutex lifecycle_mutex_;
  JNIEnv* ui_environment_ = nullptr;  // Borrowed; every use stays on the UI thread.
  jobject player_ = nullptr;
  jmethodID dispose_ = nullptr;
  jmethodID set_playing_ = nullptr;
  bool closed_ = false;

  std::mutex frame_mutex_;
  std::optional<huxerui::android::ExternalTextureSource> source_;
  jclass bitmap_class_ = nullptr;
  jobject bitmap_ = nullptr;
  jmethodID copy_pixels_ = nullptr;
  std::vector<std::uint8_t> rgba_;

  std::weak_ptr<AndroidVideoSession>* bridge_ = nullptr;
};

std::weak_ptr<AndroidVideoSession>* VideoBridge(jlong bridge) {
  return reinterpret_cast<std::weak_ptr<AndroidVideoSession>*>(static_cast<std::uintptr_t>(bridge));
}

std::shared_ptr<AndroidVideoSession> LockedBridge(jlong bridge) {
  const std::weak_ptr<AndroidVideoSession>* state = VideoBridge(bridge);
  return state != nullptr ? state->lock() : nullptr;
}

} // namespace

void InstallVideoPlatformModule(huxerui::RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<VideoSession>, VideoOpenOptions>(
      kVideoPlatformModule,
      huxerui::android::PlatformModuleFactory<std::shared_ptr<VideoSession>, VideoOpenOptions>{
          .create = [](huxerui::PlatformAdapter& adapter, JNIEnv* environment, jobject context,
                       const VideoOpenOptions& open) {
              auto session = std::make_shared<AndroidVideoSession>(adapter, open);
              session->Initialize(environment, context);
              return std::static_pointer_cast<VideoSession>(session);
          },
      }
  );
}

} // namespace lib_video_component::detail

extern "C" JNIEXPORT void JNICALL
Java_com_example_libvideocomponent_VideoPlayer_nativeOnPrepared(JNIEnv* environment, jclass, jlong bridge, jint width,
                                                               jint height) {
  try {
    if (const auto session = lib_video_component::detail::LockedBridge(bridge)) {
      session->OnPrepared(environment, width, height);
    }
  } catch (...) {
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_libvideocomponent_VideoPlayer_nativeFrame(
    JNIEnv* environment, jclass, jlong bridge, jobject y_buffer, jobject u_buffer, jobject v_buffer, jint y_row_stride,
    jint u_row_stride, jint v_row_stride, jint y_pixel_stride, jint u_pixel_stride, jint v_pixel_stride, jint width,
    jint height
) {
  try {
    if (const auto session = lib_video_component::detail::LockedBridge(bridge)) {
      session->OnFrame(
          environment, y_buffer, u_buffer, v_buffer, y_row_stride, u_row_stride, v_row_stride, y_pixel_stride,
          u_pixel_stride, v_pixel_stride, width, height
      );
    }
  } catch (...) {
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_libvideocomponent_VideoPlayer_nativeStatus(JNIEnv* environment, jclass, jlong bridge, jint status,
                                                            jstring message) {
  try {
    if (const auto session = lib_video_component::detail::LockedBridge(bridge)) {
      session->OnStatus(environment, status, message);
    }
  } catch (...) {
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_libvideocomponent_VideoPlayer_nativeDestroyed(JNIEnv*, jclass, jlong bridge) {
  // The Java player finished release(); its bridge pointer is never used again.
  if (const auto session = lib_video_component::detail::LockedBridge(bridge)) {
    session->OnDestroyed();
  }
}
