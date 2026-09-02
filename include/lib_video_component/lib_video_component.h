#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <huxerui/data.h>
#include <huxerui/event.h>
#include <huxerui/paint.h>
#include <huxerui/root.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

namespace lib_video_component {

// ---------------------------------------------------------------------------
// Cover extraction
// ---------------------------------------------------------------------------

// Returns PNG-encoded cover art for a local video file, or empty bytes when
// the platform cannot decode one. Blocks while decoding; call from a worker.
[[nodiscard]] huxerui::Bytes ExtractVideoCoverPng(std::string_view path);

// Human-readable reason for the most recent ExtractVideoCoverPng result.
[[nodiscard]] std::string LastCoverDiagnostic();

// ---------------------------------------------------------------------------
// Video component
// ---------------------------------------------------------------------------

// The media location played by a Video component.
struct VideoSource {
  huxerui::Uri uri;

  bool operator==(const VideoSource&) const = default;
};

// Coarse playback status of a Video component.
enum class VideoStatus {
  Idle,
  Preparing,
  Playing,
  Paused,
  Completed,
  Failed,
};

// Payload of VideoEvents::StateChanged.
struct VideoStateEvent {
  VideoStatus status = VideoStatus::Idle;
  std::string error;
};

struct VideoEvents {
  struct StateChanged : huxerui::Event<const VideoStateEvent&> {};
};

// Presentation and playback configuration applied when playback starts.
struct VideoOptions {
  bool auto_play = false;
  bool loop = false;
  huxerui::ImageFit fit = huxerui::ImageFit::Contain;
  huxerui::Color surface_color = huxerui::Color::Rgb(12, 12, 14);
  huxerui::Color status_color = huxerui::Color::Rgb(200, 203, 208);

  bool operator==(const VideoOptions&) const = default;
};

// Plays the source and owns its play state internally.
[[nodiscard]] huxerui::View Video(const VideoSource& source, VideoOptions options = {});

// Plays the source under a controlled play state.
[[nodiscard]] huxerui::View
Video(const VideoSource& source, const huxerui::State<bool>& playing, VideoOptions options = {});

void Install(huxerui::RootContext& root);

} // namespace lib_video_component
