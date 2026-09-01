#pragma once

#include <functional>
#include <string>
#include <utility>

#include <huxerui/data.h>
#include <huxerui/event.h>
#include <huxerui/paint.h>
#include <huxerui/root.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

namespace lib_video_component {

// The media location played by a Video component. The Uri may reference a
// remote stream (http/https) or a local file (file).
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

// Declarative playback configuration applied when the player session opens.
struct VideoOptions {
  bool auto_play = false;
  bool muted = false;
  bool loop = false;
  huxerui::ImageFit fit = huxerui::ImageFit::Contain;

  bool operator==(const VideoOptions&) const = default;
};

// Plays the source and owns its play state internally. The component starts
// paused unless options.auto_play is set.
[[nodiscard]] huxerui::View Video(const VideoSource& source, VideoOptions options = {});

// Plays the source under a controlled play state. The owner writes the next
// authoritative value; state changes are reported through
// VideoEvents::StateChanged so the owner can observe completion.
[[nodiscard]] huxerui::View
Video(const VideoSource& source, const huxerui::State<bool>& playing, VideoOptions options = {});

// Registers the video platform module for the current host. Call from the
// application root hooks before composing Video views.
void Install(huxerui::RootContext& root);

} // namespace lib_video_component
