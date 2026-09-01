#pragma once

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

// Construction options passed through OpenPlatformModule to the platform
// factory. Callbacks are invoked on the Runtime UI thread.
struct VideoOpenOptions {
  VideoSource source;
  VideoOptions options;
  std::function<void(huxerui::ExternalTexture)> on_texture;
  std::function<void(VideoStatus, std::string error)> on_status;
};

// One open playback session owned by one mounted Video component. The
// platform implementation owns the media pipeline; Close releases it and is
// idempotent and safe to call from the UI thread.
class VideoSession {
public:
  virtual ~VideoSession() = default;

  VideoSession(const VideoSession&) = delete;
  VideoSession& operator=(const VideoSession&) = delete;
  VideoSession(VideoSession&&) = delete;
  VideoSession& operator=(VideoSession&&) = delete;

  virtual void SetPlaying(bool playing) = 0;
  virtual void Close() noexcept = 0;

protected:
  VideoSession() = default;
};

// Registers the platform module implementing VideoSession for the current
// host. One definition exists per backend; the stub backend reports Failed.
void InstallVideoPlatformModule(huxerui::RootContext& root);

} // namespace lib_video_component::detail
