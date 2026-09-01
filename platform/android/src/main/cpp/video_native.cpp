// Pure C++ Android playback pipeline.
//
// AMediaExtractor demuxes a local file, AMediaCodec decodes video into an
// AImageReader window (YUV_420_888 with reliable plane strides), and the audio
// decoder feeds PCM into miniaudio. Frames cross into HuxerUI through the
// ExternalTexture bitmap contract; JNI is limited to bitmap allocation and
// pixel copies issued from the worker thread.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <android/native_window.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/jni.h>
#include <huxerui/android/platform_registry.h>
#include <huxerui/platform_adapter.h>

#include <miniaudio.h>

#include "crash_report.h"
#include "video_session.h"


// The NdkImageReader family ships at API 24 while this library targets API 23.
// Declare the small surface used here with weak linkage; the pipeline checks
// availability at runtime and reports a clear status on older devices.
typedef struct AImage AImage;
typedef struct AImageReader AImageReader;
constexpr std::int32_t kAImageFormatYuv420 = 35;  // kAImageFormatYuv420

extern "C" {
__attribute__((weak)) media_status_t
AImageReader_new(std::int32_t width, std::int32_t height, std::int32_t format, std::int32_t max_images, AImageReader** reader);
__attribute__((weak)) media_status_t AImageReader_getWindow(AImageReader* reader, ANativeWindow** window);
__attribute__((weak)) media_status_t AImageReader_acquireLatestImage(AImageReader* reader, AImage** image);
__attribute__((weak)) media_status_t AImage_getWidth(const AImage* image, std::int32_t* width);
__attribute__((weak)) media_status_t AImage_getHeight(const AImage* image, std::int32_t* height);
__attribute__((weak)) media_status_t AImage_getPlaneRowStride(const AImage* image, int plane, std::int32_t* row_stride);
__attribute__((weak)) media_status_t AImage_getPlanePixelStride(const AImage* image, int plane, std::int32_t* pixel_stride);
__attribute__((weak)) media_status_t AImage_getPlaneData(const AImage* image, int plane, std::uint8_t** data, std::int32_t* length);
__attribute__((weak)) void AImage_delete(AImage* image);
}

bool NativeImageApiAvailable() {
  return AImageReader_new != nullptr && AImageReader_getWindow != nullptr && AImage_delete != nullptr;
}

namespace lib_video_component::detail {
namespace {

bool ClearJavaException(JNIEnv* environment) {
  if (!environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

VideoStatus StatusFromPipeline(int code) {
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

// One native playback session owned by one mounted Video component.
class NativeVideoSession final : public VideoSession, public std::enable_shared_from_this<NativeVideoSession> {
public:
  NativeVideoSession(huxerui::PlatformAdapter& adapter, JNIEnv* environment, const VideoOpenOptions& open)
      : adapter_(&adapter),
        on_texture_(open.on_texture),
        on_status_(open.on_status),
        source_path_(open.source.uri.Scheme() == "file" ? std::string{open.source.uri.Path()}
                                                        : std::string{}),
        initial_playing_(open.options.auto_play),
        muted_(open.options.muted),
        loop_(open.options.loop) {
    environment->GetJavaVM(&virtual_machine_);
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
    TeardownAudio();
    {
      const std::lock_guard lock(frame_mutex_);
      if (source_.has_value()) {
        source_->Finish();
      }
      JNIEnv* environment = AttachWorker();
      ReleaseFrameObjects(environment);
      DetachWorker();
    }
  }

private:
  void Wake() {
    const std::lock_guard lock(wake_mutex_);
    wake_.notify_all();
  }

  JNIEnv* AttachWorker() noexcept {
    JNIEnv* environment = nullptr;
    if (virtual_machine_ != nullptr) {
      JavaVMAttachArgs args{JNI_VERSION_1_6, "HuxerUI-VideoWorker", nullptr};
      if (virtual_machine_->AttachCurrentThread(&environment, &args) == JNI_OK) {
        worker_attached_.store(true);
      }
    }
    return environment;
  }

  void DetachWorker() noexcept {
    if (worker_attached_.exchange(false) && virtual_machine_ != nullptr) {
      virtual_machine_->DetachCurrentThread();
    }
  }

  void ReportStatus(int code, std::string message = {}) {
    const auto on_status = on_status_;
    const VideoStatus status = StatusFromPipeline(code);
    adapter_->DispatchToUIThread([on_status, status, message] {
      if (on_status) {
        on_status(status, message);
      }
    });
  }

  // ---- audio ring shared with the miniaudio callback ----

  struct AudioRing {
    std::mutex mutex;
    std::vector<std::int16_t> samples;
    std::size_t read_index = 0;
    std::size_t write_index = 0;
    std::atomic<std::size_t> available{0};
    std::atomic<bool> overflow{false};
  };

  static void AudioDataCallback(ma_device* device, void* output, const void*, std::uint32_t frame_count) {
    auto* session = static_cast<NativeVideoSession*>(device->pUserData);
    session->ConsumeAudio(static_cast<std::int16_t*>(output), frame_count);
  }

  void ConsumeAudio(std::int16_t* output, std::uint32_t frame_count) {
    const std::size_t channels = audio_channels_.load();
    const std::size_t needed = static_cast<std::size_t>(frame_count) * channels;
    std::size_t copied = 0;
    {
      const std::lock_guard lock(audio_.mutex);
      const std::size_t available = audio_.available.load();
      copied = std::min(needed, available);
      for (std::size_t index = 0; index < copied; ++index) {
        output[index] = audio_.samples[audio_.read_index];
        audio_.read_index = (audio_.read_index + 1) % audio_.samples.size();
      }
      audio_.available.store(available - copied);
    }
    audio_frames_played_.fetch_add(copied / channels);
    for (std::size_t index = copied; index < needed; ++index) {
      output[index] = 0;
    }
  }

  bool PushAudio(const std::int16_t* input, std::size_t sample_count) {
    const std::lock_guard lock(audio_.mutex);
    const std::size_t capacity = audio_.samples.size();
    const std::size_t available = audio_.available.load();
    if (capacity - available < sample_count) {
      audio_.overflow.store(true);
      return false;
    }
    for (std::size_t index = 0; index < sample_count; ++index) {
      audio_.samples[audio_.write_index] = input[index];
      audio_.write_index = (audio_.write_index + 1) % capacity;
    }
    audio_.available.store(available + sample_count);
    return true;
  }

  void TeardownAudio() noexcept {
    if (audio_device_ != nullptr) {
      ma_device_uninit(audio_device_.get());
      audio_device_.reset();
    }
  }

  // ---- worker pipeline ----

  void Run() noexcept {
    JNIEnv* environment = AttachWorker();
    InstallCrashReporterForWorker();
    RunPipeline(environment);
    DetachWorker();
  }

  void InstallCrashReporterForWorker() {
    // The reporter is installed by the factory on the UI thread; nothing to do here.
  }

  void RunPipeline(JNIEnv* environment) noexcept {
    try {
      char level[PROP_VALUE_MAX] = {};
      if (!NativeImageApiAvailable() ||
          (__system_property_get("ro.build.version.sdk", level) > 0 && std::atoi(level) < 24)) {
        ReportStatus(5, "native playback requires Android 7.0 (API 24) or newer");
        return;
      }
      if (source_path_.empty()) {
        ReportStatus(5, "only local file sources are supported by the native pipeline");
        return;
      }
      ReportStatus(1);
      if (!OpenAndPlay(environment)) {
        return;
      }
      PumpLoop(environment);
    } catch (const std::exception& failure) {
      ReportStatus(5, failure.what());
    } catch (...) {
      ReportStatus(5, "unknown native pipeline failure");
    }
  }

  bool OpenAndPlay(JNIEnv* environment) {
    file_descriptor_ = open(source_path_.c_str(), O_RDONLY);
    if (file_descriptor_ < 0) {
      ReportStatus(5, "could not open " + source_path_);
      return false;
    }
    const off_t length = lseek(file_descriptor_, 0, SEEK_END);
    lseek(file_descriptor_, 0, SEEK_SET);
    extractor_ = AMediaExtractor_new();
    media_status_t status = AMediaExtractor_setDataSourceFd(extractor_, file_descriptor_, 0, length);
    if (status != AMEDIA_OK) {
      ReportStatus(5, "extractor rejected the source");
      return false;
    }

    const std::size_t track_count = AMediaExtractor_getTrackCount(extractor_);
    for (std::size_t track = 0; track < track_count; ++track) {
      AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, track);
      const char* mime = nullptr;
      if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime)) {
        if (video_codec_ == nullptr && strncmp(mime, "video/", 6) == 0) {
          if (!OpenVideoTrack(format, mime)) {
            AMediaFormat_delete(format);
            return false;
          }
          AMediaExtractor_selectTrack(extractor_, track);
        } else if (audio_codec_ == nullptr && strncmp(mime, "audio/", 6) == 0) {
          OpenAudioTrack(format, mime);
          AMediaExtractor_selectTrack(extractor_, track);
        }
      }
      AMediaFormat_delete(format);
    }
    if (video_codec_ == nullptr) {
      ReportStatus(5, "the source has no decodable video track");
      return false;
    }
    ReportStatus(2);
    return true;
  }

  bool OpenVideoTrack(AMediaFormat* format, const char* mime) {
    int32_t width = 0;
    int32_t height = 0;
    if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) ||
        !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height) || width <= 0 || height <= 0) {
      ReportStatus(5, "the video track has no usable dimensions");
      return false;
    }

    media_status_t status = AImageReader_new(
        static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), kAImageFormatYuv420, 4,
        &image_reader_
    );
    if (status != AMEDIA_OK || image_reader_ == nullptr) {
      ReportStatus(5, "could not create the video frame reader");
      return false;
    }
    ANativeWindow* window = nullptr;
    if (AImageReader_getWindow(image_reader_, &window) != AMEDIA_OK || window == nullptr) {
      ReportStatus(5, "could not obtain the video frame window");
      return false;
    }
    video_codec_ = AMediaCodec_createDecoderByType(mime);
    if (video_codec_ == nullptr) {
      ReportStatus(5, "no video decoder for this codec");
      return false;
    }
    status = AMediaCodec_configure(video_codec_, format, window, nullptr, 0);
    if (status != AMEDIA_OK) {
      ReportStatus(5, "video decoder configuration failed");
      return false;
    }
    status = AMediaCodec_start(video_codec_);
    if (status != AMEDIA_OK) {
      ReportStatus(5, "video decoder failed to start");
      return false;
    }

    {
      const std::lock_guard lock(frame_mutex_);
      source_.emplace(huxerui::Size{static_cast<float>(width), static_cast<float>(height)});
      rgba_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0U);
      const huxerui::ExternalTexture texture = source_->Texture();
      const auto on_texture = on_texture_;
      adapter_->DispatchToUIThread([on_texture, texture] {
        if (on_texture) {
          on_texture(texture);
        }
      });
    }
    return true;
  }

  void OpenAudioTrack(AMediaFormat* format, const char* mime) {
    audio_codec_ = AMediaCodec_createDecoderByType(mime);
    if (audio_codec_ == nullptr) {
      return;
    }
    if (AMediaCodec_configure(audio_codec_, format, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(audio_codec_) != AMEDIA_OK) {
      AMediaCodec_delete(audio_codec_);
      audio_codec_ = nullptr;
      return;
    }
    int32_t rate = 0;
    int32_t channels = 0;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
    audio_sample_rate_ = rate > 0 ? rate : 48000;
    audio_channels_.store(channels > 0 ? static_cast<std::size_t>(channels) : std::size_t{2});
    audio_.samples.assign(audio_sample_rate_ * audio_channels_.load() * 2U, 0);
  }

  bool StartAudioDevice() {
    audio_device_ = std::make_unique<ma_device>();
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = audio_channels_.load();
    config.sampleRate = audio_sample_rate_;
    config.dataCallback = &AudioDataCallback;
    config.pUserData = this;
    if (ma_device_init(nullptr, &config, audio_device_.get()) != MA_SUCCESS) {
      audio_device_.reset();
      return false;
    }
    return ma_device_start(audio_device_.get()) == MA_SUCCESS;
  }

  void PumpLoop(JNIEnv* environment) {
    bool audio_started = false;
    bool video_eos = false;
    bool audio_eos = audio_codec_ == nullptr;
    while (!closed_.load()) {
      if (!playing_.load()) {
        if (audio_device_ != nullptr) {
          ma_device_stop(audio_device_.get());
        }
        ReportStatus(3);
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_.wait(lock, [this] { return playing_.load() || closed_.load(); });
        if (closed_.load()) {
          break;
        }
        if (audio_device_ != nullptr) {
          ma_device_start(audio_device_.get());
        }
        ReportStatus(2);
      }

      if (!video_eos) {
        video_eos = PumpCodecInput(video_codec_, &video_input_eos_) || PumpVideoOutput();
      }
      if (!audio_eos) {
        audio_eos = PumpCodecInput(audio_codec_, &audio_input_eos_) || PumpAudioOutput();
        if (!audio_started && audio_codec_ != nullptr) {
          const std::size_t ready = audio_.available.load() / audio_channels_.load();
          if (ready > audio_sample_rate_ / 20U && StartAudioDevice()) {
            audio_started = true;
          }
        }
      }

      if (video_eos && audio_eos) {
        if (loop_) {
          SeekToStart();
          video_eos = audio_eos = false;
          video_input_eos_ = audio_input_eos_ = false;
        } else {
          ReportStatus(4);
          playing_.store(false);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
  }

  bool PumpCodecInput(AMediaCodec* codec, bool* input_eos) {
    if (codec == nullptr || *input_eos) {
      return false;
    }
    ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 0);
    if (index < 0) {
      return false;
    }
    size_t capacity = 0;
    std::uint8_t* buffer = AMediaCodec_getInputBuffer(codec, index, &capacity);
    const ssize_t read = AMediaExtractor_readSampleData(extractor_, buffer, capacity);
    if (read < 0) {
      AMediaCodec_queueInputBuffer(codec, index, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      *input_eos = true;
      return false;
    }
    const std::int64_t presentation = AMediaExtractor_getSampleTime(extractor_);
    AMediaCodec_queueInputBuffer(codec, index, 0, static_cast<size_t>(read), presentation, 0);
    AMediaExtractor_advance(extractor_);
    return false;
  }

  bool PumpVideoOutput() {
    AMediaCodecBufferInfo info;
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(video_codec_, &info, 0);
    if (index >= 0) {
      const bool render = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) == 0;
      AMediaCodec_releaseOutputBuffer(video_codec_, index, render);
      if (render) {
        PublishLatestImage();
      }
      return (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    }
    return false;
  }

  void PublishLatestImage() {
    AImage* image = nullptr;
    if (AImageReader_acquireLatestImage(image_reader_, &image) != AMEDIA_OK || image == nullptr) {
      return;
    }
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
    int32_t width = 0;
    int32_t height = 0;
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);
    const bool planes =
        AImage_getPlaneRowStride(image, 0, &y_stride) == AMEDIA_OK &&
        AImage_getPlaneRowStride(image, 1, &u_stride) == AMEDIA_OK &&
        AImage_getPlaneRowStride(image, 2, &v_stride) == AMEDIA_OK &&
        AImage_getPlanePixelStride(image, 0, &y_pixel) == AMEDIA_OK &&
        AImage_getPlanePixelStride(image, 1, &u_pixel) == AMEDIA_OK &&
        AImage_getPlanePixelStride(image, 2, &v_pixel) == AMEDIA_OK &&
        AImage_getPlaneData(image, 0, &y_data, &y_length) == AMEDIA_OK &&
        AImage_getPlaneData(image, 1, &u_data, &u_length) == AMEDIA_OK &&
        AImage_getPlaneData(image, 2, &v_data, &v_length) == AMEDIA_OK;
    if (planes) {
      JNIEnv* environment = AttachWorker();
      PublishFrame(environment, y_data, u_data, v_data, y_stride, u_stride, v_stride, y_pixel, u_pixel, v_pixel,
                   width, height);
      DetachWorker();
    }
    AImage_delete(image);
  }

  void PublishFrame(
      JNIEnv* environment, const std::uint8_t* y_plane, const std::uint8_t* u_plane, const std::uint8_t* v_plane,
      int y_row_stride, int u_row_stride, int v_row_stride, int y_pixel_stride, int u_pixel_stride, int v_pixel_stride,
      int width, int height
  ) {
    if (environment == nullptr || closed_.load()) {
      return;
    }
    const std::lock_guard lock(frame_mutex_);
    if (!source_.has_value() || width <= 0 || height <= 0) {
      return;
    }
    if (!EnsureBitmaps(environment, width, height)) {
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
    if (ClearJavaException(environment)) {
      return;
    }
    try {
      source_->Publish(environment, bitmap);
    } catch (const std::exception&) {
      // Publish throws once the source finished; frames after close are expected.
    }
  }

  bool EnsureBitmaps(JNIEnv* environment, int width, int height) {
    if (bitmaps_[0] != nullptr) {
      return true;
    }
    const jclass bitmap_class = environment->FindClass("android/graphics/Bitmap");
    const jclass config_class = environment->FindClass("android/graphics/Bitmap$Config");
    if (!bitmap_class || !config_class) {
      ClearJavaException(environment);
      return false;
    }
    const jfieldID argb_field =
        environment->GetStaticFieldID(config_class, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    const jmethodID create_bitmap = environment->GetStaticMethodID(
        bitmap_class, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"
    );
    const jobject config = environment->GetStaticObjectField(config_class, argb_field);
    copy_pixels_ = environment->GetMethodID(bitmap_class, "copyPixelsFromBuffer", "(Ljava/nio/Buffer;)V");
    if (argb_field == nullptr || create_bitmap == nullptr || copy_pixels_ == nullptr || config == nullptr) {
      ClearJavaException(environment);
      return false;
    }
    for (jobject& slot : bitmaps_) {
      const jobject bitmap = environment->CallStaticObjectMethod(
          bitmap_class, create_bitmap, static_cast<jint>(width), static_cast<jint>(height), config
      );
      if (ClearJavaException(environment) || bitmap == nullptr) {
        ReleaseFrameObjects(environment);
        return false;
      }
      slot = environment->NewGlobalRef(bitmap);
      environment->DeleteLocalRef(bitmap);
    }
    bitmap_index_ = 0;
    return true;
  }

  void ReleaseFrameObjects(JNIEnv* environment) noexcept {
    for (jobject& slot : bitmaps_) {
      if (slot != nullptr && environment != nullptr) {
        environment->DeleteGlobalRef(slot);
      }
      slot = nullptr;
    }
    bitmap_index_ = 0;
    source_.reset();
    rgba_.clear();
    rgba_.shrink_to_fit();
  }

  bool PumpAudioOutput() {
    AMediaCodecBufferInfo info;
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(audio_codec_, &info, 0);
    if (index >= 0) {
      size_t capacity = 0;
      std::uint8_t* buffer = AMediaCodec_getOutputBuffer(audio_codec_, index, &capacity);
      if (buffer != nullptr && info.size > 0) {
        const std::size_t sample_count = static_cast<std::size_t>(info.size) / sizeof(std::int16_t);
        while (!closed_.load() && !PushAudio(reinterpret_cast<const std::int16_t*>(buffer + info.offset), sample_count)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
      AMediaCodec_releaseOutputBuffer(audio_codec_, index, false);
      return (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    }
    return false;
  }

  void SeekToStart() {
    AMediaExtractor_seekTo(extractor_, 0, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
    if (video_codec_ != nullptr) {
      AMediaCodec_flush(video_codec_);
    }
    if (audio_codec_ != nullptr) {
      AMediaCodec_flush(audio_codec_);
      const std::lock_guard lock(audio_.mutex);
      audio_.read_index = audio_.write_index = 0;
      audio_.available.store(0);
    }
  }

  huxerui::PlatformAdapter* adapter_ = nullptr;
  std::function<void(huxerui::ExternalTexture)> on_texture_;
  std::function<void(VideoStatus, std::string)> on_status_;
  std::string source_path_;
  bool initial_playing_ = false;
  bool muted_ = false;
  bool loop_ = false;

  JavaVM* virtual_machine_ = nullptr;
  std::atomic<bool> worker_attached_{false};
  std::thread worker_;
  std::atomic<bool> closed_{false};
  std::atomic<bool> playing_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;

  int file_descriptor_ = -1;
  AMediaExtractor* extractor_ = nullptr;
  AMediaCodec* video_codec_ = nullptr;
  AMediaCodec* audio_codec_ = nullptr;
  AImageReader* image_reader_ = nullptr;
  bool video_input_eos_ = false;
  bool audio_input_eos_ = false;

  std::unique_ptr<ma_device> audio_device_;
  AudioRing audio_;
  std::size_t audio_sample_rate_ = 48000;
  std::atomic<std::size_t> audio_channels_{2};
  std::atomic<std::uint64_t> audio_frames_played_{0};

  std::mutex frame_mutex_;
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
              InstallCrashReporterFromContext(environment, context);
              auto session = std::make_shared<NativeVideoSession>(adapter, environment, open);
              return std::static_pointer_cast<VideoSession>(session);
          },
      }
  );
}

} // namespace lib_video_component::detail
