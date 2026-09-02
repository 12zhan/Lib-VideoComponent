// Pure C++ Android video pipeline: image only.
//
// One worker thread owns every media object: AMediaExtractor demuxes a local
// file, AMediaCodec decodes the video track into an AImageReader window, each
// decoded frame is converted from YUV to RGBA, copied into a rotating bitmap
// ring, and published through ExternalTextureSource. Presentation follows the
// stream timestamps against a monotonic clock; audio is not handled yet.

#include <fcntl.h>
#include <unistd.h>

#include <android/native_window.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/platform_registry.h>
#include <huxerui/platform_adapter.h>

#include "android_jni_support.h"
#include "video_session.h"

namespace lib_video_component::detail {
namespace {

// The NdkImageReader family ships at API 24 while this library targets API
// 23. Declare the small surface used here with weak linkage; the pipeline
// checks availability at runtime and reports a clear status on older devices.
typedef struct AImage AImage;
typedef struct AImageReader AImageReader;
constexpr std::int32_t kAImageFormatYuv420 = 35;  // AIMAGE_FORMAT_YUV_420_888

extern "C" {
__attribute__((weak)) media_status_t AImageReader_new(
    std::int32_t width, std::int32_t height, std::int32_t format, std::int32_t max_images, AImageReader** reader
);
__attribute__((weak)) media_status_t AImageReader_getWindow(AImageReader* reader, ANativeWindow** window);
__attribute__((weak)) media_status_t AImageReader_acquireLatestImage(AImageReader* reader, AImage** image);
__attribute__((weak)) media_status_t AImage_getWidth(const AImage* image, std::int32_t* width);
__attribute__((weak)) media_status_t AImage_getHeight(const AImage* image, std::int32_t* height);
__attribute__((weak)) media_status_t
AImage_getPlaneRowStride(const AImage* image, int plane, std::int32_t* row_stride);
__attribute__((weak)) media_status_t
AImage_getPlanePixelStride(const AImage* image, int plane, std::int32_t* pixel_stride);
__attribute__((weak)) media_status_t
AImage_getPlaneData(const AImage* image, int plane, std::uint8_t** data, std::int32_t* length);
__attribute__((weak)) void AImage_delete(AImage* image);
__attribute__((weak)) void AImageReader_delete(AImageReader* reader);
}

bool NativeImageApiAvailable() {
  return AImageReader_new != nullptr && AImageReader_getWindow != nullptr && AImage_delete != nullptr;
}

std::uint8_t ClampChannel(float value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 255.0F));
}

// BT.601 limited-range YUV to sRGB with generic plane strides.
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

using SteadyClock = std::chrono::steady_clock;

// One native playback session. The worker thread creates, uses, and destroys
// every media object; the UI thread only flips two atomics and joins.
class NativeVideoSession final : public VideoSession {
public:
  NativeVideoSession(
      huxerui::PlatformAdapter& adapter, JNIEnv* environment, jobject context, const VideoOpenOptions& open
  )
      : adapter_(&adapter),
        on_texture_(open.on_texture),
        on_status_(open.on_status),
        source_path_(open.source.uri.Scheme() == "file" ? std::string{open.source.uri.Path()} : std::string{}),
        loop_(open.options.loop),
        initial_playing_(open.options.auto_play) {
    playing_.store(initial_playing_);
    worker_ = std::thread([this] {
      Run();
    });
  }

  ~NativeVideoSession() override {
    Close();
  }

  NativeVideoSession(const NativeVideoSession&) = delete;
  NativeVideoSession& operator=(const NativeVideoSession&) = delete;
  NativeVideoSession(NativeVideoSession&&) = delete;
  NativeVideoSession& operator=(NativeVideoSession&&) = delete;

  void SetPlaying(bool playing) override {
    playing_.store(playing);
    Wake();
  }

  void Close() noexcept override {
    if (closed_.exchange(true)) {
      return;
    }
    playing_.store(false);
    Wake();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  void Wake() {
    const std::lock_guard lock(wake_mutex_);
    wake_.notify_all();
  }

  void ReportStatus(int code, std::string message = {}) {
    const auto on_status = on_status_;
    adapter_->DispatchToUIThread([on_status, code, message] {
      if (on_status) {
        on_status(StatusFromCode(code), message);
      }
    });
  }

  static VideoStatus StatusFromCode(int code) {
    switch (code) {
    case 1:
      return VideoStatus::Preparing;
    case 2:
      return VideoStatus::Playing;
    case 3:
      return VideoStatus::Paused;
    case 4:
      return VideoStatus::Completed;
    default:
      return VideoStatus::Failed;
    }
  }

  // ---- worker thread ----

  void Run() noexcept {
    try {
      if (!NativeImageApiAvailable()) {
        ReportStatus(5, "native playback requires Android 7.0 (API 24) or newer");
        return;
      }
      if (source_path_.empty()) {
        ReportStatus(5, "only local file sources are supported");
        return;
      }
      ReportStatus(1);
      if (!Open()) {
        return;
      }
      Pump();
    } catch (const std::exception& failure) {
      ReportStatus(5, failure.what());
    } catch (...) {
      ReportStatus(5, "unknown native pipeline failure");
    }
    Teardown();
  }

  bool Open() {
    file_descriptor_ = open(source_path_.c_str(), O_RDONLY);
    if (file_descriptor_ < 0) {
      ReportStatus(5, "could not open the video file");
      return false;
    }
    const off_t length = lseek(file_descriptor_, 0, SEEK_END);
    lseek(file_descriptor_, 0, SEEK_SET);

    extractor_ = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(extractor_, file_descriptor_, 0, length) != AMEDIA_OK) {
      ReportStatus(5, "the extractor rejected the source");
      return false;
    }

    const std::size_t track_count = AMediaExtractor_getTrackCount(extractor_);
    bool have_video = false;
    for (std::size_t track = 0; track < track_count; ++track) {
      AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, track);
      const char* mime = nullptr;
      if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime)) {
        if (!have_video && std::strncmp(mime, "video/", 6) == 0) {
          if (!OpenVideoTrack(format, mime)) {
            AMediaFormat_delete(format);
            return false;
          }
          AMediaExtractor_selectTrack(extractor_, track);
          have_video = true;
        }
      }
      AMediaFormat_delete(format);
    }
    if (!have_video) {
      ReportStatus(5, "the source has no decodable video track");
      return false;
    }
    return true;
  }

  bool OpenVideoTrack(AMediaFormat* format, const char* mime) {
    int32_t width = 0;
    int32_t height = 0;
    if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) ||
        !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height) || width <= 0 || height <= 0 ||
        width > 8192 || height > 8192) {
      ReportStatus(5, "the video track has no usable dimensions");
      return false;
    }

    if (AImageReader_new(width, height, kAImageFormatYuv420, 4, &image_reader_) != AMEDIA_OK ||
        image_reader_ == nullptr) {
      ReportStatus(5, "could not create the frame reader");
      return false;
    }
    ANativeWindow* window = nullptr;
    if (AImageReader_getWindow(image_reader_, &window) != AMEDIA_OK || window == nullptr) {
      ReportStatus(5, "could not obtain the decoder window");
      return false;
    }

    video_codec_ = AMediaCodec_createDecoderByType(mime);
    if (video_codec_ == nullptr) {
      ReportStatus(5, "no platform decoder for this codec");
      return false;
    }
    if (AMediaCodec_configure(video_codec_, format, window, nullptr, 0) != AMEDIA_OK) {
      ReportStatus(5, "decoder configuration failed");
      return false;
    }
    if (AMediaCodec_start(video_codec_) != AMEDIA_OK) {
      ReportStatus(5, "decoder failed to start");
      return false;
    }

    frame_width_ = width;
    frame_height_ = height;
    source_.emplace(huxerui::Size{static_cast<float>(width), static_cast<float>(height)});
    rgba_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0U);
    const huxerui::ExternalTexture texture = source_->Texture();
    const auto on_texture = on_texture_;
    adapter_->DispatchToUIThread([on_texture, texture] {
      if (on_texture) {
        on_texture(texture);
      }
    });
    return true;
  }

  void Pump() {
    bool input_eos = false;
    bool output_eos = false;
    bool playing_reported = false;
    while (!closed_.load()) {
      if (!playing_.load()) {
        if (playing_reported) {
          ReportStatus(3);
          playing_reported = false;
        }
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_.wait(lock, [this] { return playing_.load() || closed_.load(); });
        if (closed_.load()) {
          return;
        }
        const auto now = SteadyClock::now();
        paused_at_ = now;
        ResumeClock(now);
      }

      if (!input_eos) {
        input_eos = FeedInput();
      }
      if (!output_eos) {
        output_eos = DrainOutput(&playing_reported);
      }

      if (input_eos && output_eos) {
        if (loop_) {
          Restart();
          input_eos = output_eos = false;
        } else {
          ReportStatus(4);
          playing_.store(false);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
  }

  bool FeedInput() {
    const ssize_t index = AMediaCodec_dequeueInputBuffer(video_codec_, 0);
    if (index < 0) {
      return false;
    }
    size_t capacity = 0;
    std::uint8_t* buffer = AMediaCodec_getInputBuffer(video_codec_, index, &capacity);
    const ssize_t read = buffer != nullptr ? AMediaExtractor_readSampleData(extractor_, buffer, capacity) : -1;
    if (read < 0) {
      AMediaCodec_queueInputBuffer(
          video_codec_, index, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM
      );
      return true;
    }
    const std::int64_t presentation = AMediaExtractor_getSampleTime(extractor_);
    AMediaCodec_queueInputBuffer(video_codec_, index, 0, static_cast<size_t>(read), presentation, 0);
    AMediaExtractor_advance(extractor_);
    return false;
  }

  bool DrainOutput(bool* playing_reported) {
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(video_codec_, &info, 0);
    if (index < 0) {
      return false;
    }
    const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    if (!eos && info.size > 0) {
      WaitUntilPresentation(info.presentationTimeUs);
      if (!*playing_reported) {
        ReportStatus(2);
        *playing_reported = true;
      }
    }
    AMediaCodec_releaseOutputBuffer(video_codec_, index, !eos);
    if (!eos && info.size > 0) {
      PublishLatestImage();
    }
    return eos;
  }

  // Presentation pacing against a monotonic clock anchored at the first frame.
  void WaitUntilPresentation(std::int64_t presentation_us) {
    const auto now = SteadyClock::now();
    if (!clock_anchor_.has_value()) {
      clock_anchor_ = now - std::chrono::microseconds(presentation_us);
      return;
    }
    const auto target = *clock_anchor_ + std::chrono::microseconds(presentation_us);
    if (target > now) {
      const auto pause_shift = pause_shift_;
      const auto adjusted = target + std::chrono::nanoseconds(pause_shift);
      if (adjusted > now) {
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_.wait_until(lock, adjusted, [this] { return closed_.load() || !playing_.load(); });
      }
    }
  }

  void ResumeClock(SteadyClock::time_point now) {
    if (paused_at_.has_value()) {
      pause_shift_ += std::chrono::duration_cast<std::chrono::nanoseconds>(now - *paused_at_).count();
      paused_at_.reset();
    }
  }

  void Restart() {
    AMediaExtractor_seekTo(extractor_, 0, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
    AMediaCodec_flush(video_codec_);
    clock_anchor_.reset();
    pause_shift_ = 0;
  }

  // Every decoder-provided value is hostile: the frame is validated against
  // the plane lengths and the rgba_ allocation, and rejected when anything
  // would read or write out of bounds.
  void PublishLatestImage() {
    AImage* image = nullptr;
    if (AImageReader_acquireLatestImage(image_reader_, &image) != AMEDIA_OK || image == nullptr) {
      return;
    }
    int32_t image_width = 0;
    int32_t image_height = 0;
    int32_t y_stride = 0;
    int32_t u_stride = 0;
    int32_t v_stride = 0;
    int32_t y_pixel = 1;
    int32_t u_pixel = 1;
    int32_t v_pixel = 1;
    std::uint8_t* y_data = nullptr;
    std::uint8_t* u_data = nullptr;
    std::uint8_t* v_data = nullptr;
    int32_t y_length = 0;
    int32_t u_length = 0;
    int32_t v_length = 0;
    const bool planes =
        AImage_getWidth(image, &image_width) == AMEDIA_OK && AImage_getHeight(image, &image_height) == AMEDIA_OK &&
        AImage_getPlaneRowStride(image, 0, &y_stride) == AMEDIA_OK &&
        AImage_getPlaneRowStride(image, 1, &u_stride) == AMEDIA_OK &&
        AImage_getPlaneRowStride(image, 2, &v_stride) == AMEDIA_OK &&
        AImage_getPlanePixelStride(image, 0, &y_pixel) == AMEDIA_OK &&
        AImage_getPlanePixelStride(image, 1, &u_pixel) == AMEDIA_OK &&
        AImage_getPlanePixelStride(image, 2, &v_pixel) == AMEDIA_OK &&
        AImage_getPlaneData(image, 0, &y_data, &y_length) == AMEDIA_OK &&
        AImage_getPlaneData(image, 1, &u_data, &u_length) == AMEDIA_OK &&
        AImage_getPlaneData(image, 2, &v_data, &v_length) == AMEDIA_OK;

    if (planes && PlaneCoversFrame(
                      image_width, image_height, y_stride, u_stride, v_stride, y_pixel, u_pixel, v_pixel, y_length,
                      u_length, v_length
                  )) {
      // The converted frame must fit the rgba_ allocation taken from the
      // track dimensions; mismatched decoder output is skipped, not trusted.
      const int width = std::min(image_width, frame_width_);
      const int height = std::min(image_height, frame_height_);
      if (width > 0 && height > 0) {
        PublishFrame(
            y_data, u_data, v_data, y_stride, u_stride, v_stride, y_pixel, u_pixel, v_pixel, width, height
        );
      }
    }
    AImage_delete(image);
  }

  // Confirms the plane buffers cover every byte the converter will touch.
  static bool PlaneCoversFrame(
      int width, int height, int y_stride, int u_stride, int v_stride, int y_pixel, int u_pixel, int v_pixel,
      int32_t y_length, int32_t u_length, int32_t v_length
  ) {
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (y_stride <= 0 || u_stride <= 0 || v_stride <= 0 || y_pixel <= 0 || u_pixel <= 0 || v_pixel <= 0) {
      return false;
    }
    const auto needed = [](int stride, int rows, int pixel, int columns) {
      return static_cast<std::int64_t>(stride) * (rows - 1) + static_cast<std::int64_t>(pixel) * (columns - 1) + 1;
    };
    const int chroma_rows = (height + 1) / 2;
    const int chroma_columns = (width + 1) / 2;
    return needed(y_stride, height, y_pixel, width) <= y_length &&
           needed(u_stride, chroma_rows, u_pixel, chroma_columns) <= u_length &&
           needed(v_stride, chroma_rows, v_pixel, chroma_columns) <= v_length;
  }

  void PublishFrame(
      const std::uint8_t* y_plane, const std::uint8_t* u_plane, const std::uint8_t* v_plane, int y_row_stride,
      int u_row_stride, int v_row_stride, int y_pixel_stride, int u_pixel_stride, int v_pixel_stride, int width,
      int height
  ) {
    const ScopedJniEnv scoped;
    JNIEnv* environment = scoped.Get();
    if (environment == nullptr) {
      return;
    }
    if (!EnsureBitmaps(environment)) {
      return;
    }
    ConvertYuvToRgba(
        y_plane, u_plane, v_plane, y_row_stride, u_row_stride, v_row_stride, y_pixel_stride, u_pixel_stride,
        v_pixel_stride, width, height, rgba_.data()
    );
    const jobject bitmap = bitmaps_[bitmap_index_];
    bitmap_index_ = (bitmap_index_ + 1U) % std::size(bitmaps_);
    const jobject pixels = environment->NewDirectByteBuffer(rgba_.data(), static_cast<jlong>(rgba_.size()));
    environment->CallVoidMethod(bitmap, copy_pixels_, pixels);
    if (ClearException(environment)) {
      return;
    }
    try {
      source_->Publish(environment, bitmap);
    } catch (const std::exception&) {
      // Publishing after Finish throws; frames racing close are expected.
    }
  }

  // Rotating bitmaps keep a published frame stable while the renderer holds
  // it; rewriting one shared bitmap races the draw pass.
  bool EnsureBitmaps(JNIEnv* environment) {
    if (bitmaps_[0] != nullptr) {
      return true;
    }
    const jclass bitmap_class = environment->FindClass("android/graphics/Bitmap");
    const jclass config_class = environment->FindClass("android/graphics/Bitmap$Config");
    if (ClearException(environment) || bitmap_class == nullptr || config_class == nullptr) {
      return false;
    }
    const jfieldID argb_field =
        environment->GetStaticFieldID(config_class, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    const jmethodID create_bitmap = environment->GetStaticMethodID(
        bitmap_class, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"
    );
    const jobject config = environment->GetStaticObjectField(config_class, argb_field);
    copy_pixels_ = environment->GetMethodID(bitmap_class, "copyPixelsFromBuffer", "(Ljava/nio/Buffer;)V");
    if (argb_field == nullptr || create_bitmap == nullptr || copy_pixels_ == nullptr || config == nullptr ||
        ClearException(environment)) {
      return false;
    }
    for (jobject& slot : bitmaps_) {
      const jobject bitmap = environment->CallStaticObjectMethod(
          bitmap_class, create_bitmap, static_cast<jint>(frame_width_), static_cast<jint>(frame_height_), config
      );
      if (ClearException(environment) || bitmap == nullptr) {
        ReleaseBitmaps(environment);
        return false;
      }
      slot = environment->NewGlobalRef(bitmap);
      environment->DeleteLocalRef(bitmap);
    }
    bitmap_index_ = 0;
    return true;
  }

  void ReleaseBitmaps(JNIEnv* environment) noexcept {
    for (jobject& slot : bitmaps_) {
      if (slot != nullptr && environment != nullptr) {
        environment->DeleteGlobalRef(slot);
      }
      slot = nullptr;
    }
  }

  // Runs on the worker thread after the pump loop ends; every media object
  // dies here, on the thread that created it.
  void Teardown() noexcept {
    if (source_.has_value()) {
      source_->Finish();
    }
    {
      const ScopedJniEnv scoped;
      ReleaseBitmaps(scoped.Get());
    }
    if (video_codec_ != nullptr) {
      AMediaCodec_stop(video_codec_);
      AMediaCodec_delete(video_codec_);
      video_codec_ = nullptr;
    }
    if (image_reader_ != nullptr) {
      AImageReader_delete(image_reader_);
      image_reader_ = nullptr;
    }
    if (extractor_ != nullptr) {
      AMediaExtractor_delete(extractor_);
      extractor_ = nullptr;
    }
    if (file_descriptor_ >= 0) {
      close(file_descriptor_);
      file_descriptor_ = -1;
    }
  }

  huxerui::PlatformAdapter* adapter_ = nullptr;
  std::function<void(huxerui::ExternalTexture)> on_texture_;
  std::function<void(VideoStatus, std::string)> on_status_;
  std::string source_path_;
  bool loop_ = false;
  bool initial_playing_ = false;

  std::thread worker_;
  std::atomic<bool> closed_{false};
  std::atomic<bool> playing_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;

  int file_descriptor_ = -1;
  AMediaExtractor* extractor_ = nullptr;
  AMediaCodec* video_codec_ = nullptr;
  AImageReader* image_reader_ = nullptr;
  int32_t frame_width_ = 0;
  int32_t frame_height_ = 0;
  bool input_eos_reported_ = false;

  std::optional<SteadyClock::time_point> clock_anchor_;
  std::optional<SteadyClock::time_point> paused_at_;
  std::int64_t pause_shift_ = 0;

  std::optional<huxerui::android::ExternalTextureSource> source_;
  jobject bitmaps_[4] = {};
  std::size_t bitmap_index_ = 0;
  jmethodID copy_pixels_ = nullptr;
  std::vector<std::uint8_t> rgba_;
};

} // namespace

void InstallVideoPlatformModule(huxerui::RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<VideoSession>, VideoOpenOptions>(
      kVideoPlatformModule,
      huxerui::android::PlatformModuleFactory<std::shared_ptr<VideoSession>, VideoOpenOptions>{
          .create = [](huxerui::PlatformAdapter& adapter, JNIEnv* environment, jobject context,
                       const VideoOpenOptions& open) {
              auto session = std::make_shared<NativeVideoSession>(adapter, environment, context, open);
              return std::static_pointer_cast<VideoSession>(session);
          },
      }
  );
}

} // namespace lib_video_component::detail
