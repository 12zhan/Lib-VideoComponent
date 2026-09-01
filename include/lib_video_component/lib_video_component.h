#pragma once

#include <memory>
#include <string>
#include <utility>

#include <huxerui/data.h>
#include <huxerui/event.h>
#include <huxerui/paint.h>
#include <huxerui/root.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

namespace lib_video_component {

namespace detail {
struct VideoControllerAccess;
struct VideoControllerState;
}  // namespace detail

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Payload of VideoEvents::StateChanged.
struct VideoStateEvent {
  VideoStatus status = VideoStatus::Idle;
  std::string error;
};

// Payload of VideoEvents::Prepared. Reported once per loaded source after the
// track layout is known and before the first frame is displayed.
struct VideoPreparedEvent {
  double width = 0.0;
  double height = 0.0;
  double duration_seconds = 0.0;
};

struct VideoEvents {
  struct StateChanged : huxerui::Event<const VideoStateEvent&> {};
  struct Prepared : huxerui::Event<const VideoPreparedEvent&> {};
};

// ---------------------------------------------------------------------------
// Styles and configuration
// ---------------------------------------------------------------------------

// Declarative presentation and playback configuration. Values apply when the
// player session opens; use a VideoController for imperative transport.
struct VideoOptions {
  bool auto_play = false;
  bool muted = false;
  bool loop = false;
  huxerui::ImageFit fit = huxerui::ImageFit::Contain;
  huxerui::Color surface_color = huxerui::Color::Rgb(12, 12, 14);
  huxerui::Color status_color = huxerui::Color::Rgb(200, 203, 208);

  bool operator==(const VideoOptions&) const = default;
};

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

// Imperative transport handle for one mounted Video component: seeking plus
// position and duration reads. Play and pause remain declarative state.
// Methods are safe to call before the component mounts and after it unmounts;
// they become no-ops while no session is attached.
class VideoController final {
public:
  VideoController();

  VideoController(const VideoController&) = default;
  VideoController& operator=(const VideoController&) = default;
  VideoController(VideoController&&) noexcept = default;
  VideoController& operator=(VideoController&&) noexcept = default;

  void SeekTo(double position_seconds) const;
  [[nodiscard]] double Position() const;  // seconds, 0 while no session reports
  [[nodiscard]] double Duration() const;  // seconds, 0 until prepared

  bool operator==(const VideoController& other) const noexcept;

private:
  friend struct detail::VideoControllerAccess;
  std::shared_ptr<const void> state_;
};

// ---------------------------------------------------------------------------
// Component
// ---------------------------------------------------------------------------

// Plays the source and owns its play state internally. The component starts
// paused unless options.auto_play is set.
[[nodiscard]] huxerui::View Video(const VideoSource& source, VideoOptions options = {});

// Plays the source under a controlled play state. The owner writes the next
// authoritative value; transitions are reported through VideoEvents.
[[nodiscard]] huxerui::View
Video(const VideoSource& source, const huxerui::State<bool>& playing, VideoOptions options = {});

// Same as the overloads above with an attached transport controller.
[[nodiscard]] huxerui::View Video(const VideoSource& source, const VideoController& controller, VideoOptions options = {});
[[nodiscard]] huxerui::View Video(
    const VideoSource& source, const VideoController& controller, const huxerui::State<bool>& playing,
    VideoOptions options = {}
);

// Registers the video platform module for the current host. Call from the
// application root hooks before composing Video views.
void Install(huxerui::RootContext& root);

} // namespace lib_video_component
