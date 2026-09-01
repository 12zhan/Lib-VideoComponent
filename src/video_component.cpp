#include <string>
#include <utility>

#include <huxerui/huxerui.h>

#include "video_session.h"

namespace lib_video_component {

using namespace huxerui;

namespace {

const char* StatusLabel(VideoStatus status) {
  switch (status) {
  case VideoStatus::Idle:
    return "Idle";
  case VideoStatus::Preparing:
    return "Preparing";
  case VideoStatus::Playing:
    return "Playing";
  case VideoStatus::Paused:
    return "Paused";
  case VideoStatus::Completed:
    return "Completed";
  case VideoStatus::Failed:
    return "Failed";
  }
  return "Idle";
}

View PlaceholderSurface(VideoStatus status, const std::string& error) {
  const Color background = Color::Rgb(12, 12, 14);
  const Color foreground = Color::Rgb(200, 203, 208);

  Column content{
      Text(status == VideoStatus::Failed && !error.empty() ? error : StatusLabel(status), TextRole::Label)
          .With(Foreground(foreground)),
  };
  return std::move(content)
      .With(
          Background(background),
          MainAlign(MainAxisAlignment::Center),
          CrossAlign(CrossAxisAlignment::Center),
          Padding(16.0F)
      );
}

// One shared body for both Video overloads. The optional playing state selects
// controlled mode; otherwise the component owns its play state.
[[huxerui::composable]]
View VideoImpl(const VideoSource& source, const State<bool>* playing, VideoOptions options) {
  auto texture = UseState(ExternalTexture{});
  auto status = UseState(VideoStatus::Idle);
  auto error = UseState(std::string{});
  auto session = UseState(std::shared_ptr<detail::VideoSession>{});
  // Allocated unconditionally so the state slot identity stays stable when the
  // caller switches between controlled and uncontrolled use.
  auto internal_playing = UseState(options.auto_play);
  auto events = UseEvents();

  const bool initial_playing = playing != nullptr ? playing->Get() : internal_playing.Get();

  detail::VideoOpenOptions open{source, options, nullptr, nullptr};
  open.options.auto_play = initial_playing;
  open.on_texture = [texture](ExternalTexture value) {
    texture = std::move(value);
  };
  open.on_status = [status, error, events, internal_playing, playing](VideoStatus next, std::string message) {
    status = next;
    error = message;
    if (next == VideoStatus::Completed && playing == nullptr) {
      internal_playing = false;
    }
    events.Emit<VideoEvents::StateChanged>(VideoStateEvent{next, std::move(message)});
  };

  // Open one playback session for this component; the source selects it.
  Lifecycle(
      [open, session]() mutable {
        auto handle = OpenPlatformModule<std::shared_ptr<detail::VideoSession>>(detail::kVideoPlatformModule, open);
        session = handle;
        return [handle] {
          handle->Close();
        };
      },
      source
  );

  // Follow play-state changes without recreating the session.
  if (playing != nullptr) {
    const State<bool> playing_value = *playing;
    Lifecycle(
        [session, playing_value] {
          if (const std::shared_ptr<detail::VideoSession> current = session.Get()) {
            current->SetPlaying(playing_value.Get());
          }
        },
        playing_value
    );
  } else {
    Lifecycle(
        [session, internal_playing] {
          if (const std::shared_ptr<detail::VideoSession> current = session.Get()) {
            current->SetPlaying(internal_playing.Get());
          }
        },
        internal_playing
    );
  }

  const ExternalTexture current_texture = texture.Get();
  if (current_texture.HasValue()) {
    return Image(current_texture).Fit(options.fit);
  }
  return PlaceholderSurface(status.Get(), error.Get());
}

} // namespace

View Video(const VideoSource& source, VideoOptions options) {
  return VideoImpl(source, nullptr, std::move(options));
}

View Video(const VideoSource& source, const State<bool>& playing, VideoOptions options) {
  return VideoImpl(source, &playing, std::move(options));
}

} // namespace lib_video_component
