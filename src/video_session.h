#pragma once

#include <atomic>
#include <mutex>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <huxerui/external_texture.h>
#include <huxerui/platform_adapter.h>
#include <huxerui/root.h>

#include "lib_video_component/lib_video_component.h"

namespace lib_video_component::detail {

// Stable PlatformModule registration name for the video player session.
inline constexpr char kVideoPlatformModule[] = "lib_video_component/VideoPlayer";

// Shared mutable state behind a VideoController. The component wires the
// session pointer when the player opens and clears it on close; transport
// reads and writes go through the session or the cached scalar values.
struct VideoControllerState {
  std::mutex mutex;
  std::weak_ptr<class VideoSession> session;
  std::atomic<double> position{0.0};
  std::atomic<double> duration{0.0};
};

// Construction options passed through OpenPlatformModule to the platform
// factory. Callbacks are invoked on the Runtime UI thread.
struct VideoOpenOptions {
  VideoSource source;
  VideoOptions options;
  std::shared_ptr<VideoControllerState> controller;
  std::function<void(huxerui::ExternalTexture)> on_texture;
  std::function<void(VideoStatus, std::string error)> on_status;
  std::function<void(double width, double height, double duration_seconds)> on_prepared;
};

// One open playback session owned by one mounted Video component. The
// platform implementation owns the whole media pipeline; every method is
// called on the Runtime UI thread and Close releases the pipeline.
class VideoSession {
public:
  virtual ~VideoSession() = default;

  VideoSession(const VideoSession&) = delete;
  VideoSession& operator=(const VideoSession&) = delete;
  VideoSession(VideoSession&&) = delete;
  VideoSession& operator=(VideoSession&&) = delete;

  virtual void SetPlaying(bool playing) = 0;
  virtual void SeekTo(double position_seconds) = 0;
  virtual void Close() noexcept = 0;

protected:
  VideoSession() = default;
};

// Registers the platform module implementing VideoSession for the current
// host. The stub backend reports Failed until a real backend exists.
void InstallVideoPlatformModule(huxerui::RootContext& root);

} // namespace lib_video_component::detail
