#include <string>
#include <utility>

#include <huxerui/huxerui.h>

#include "video_session.h"

namespace lib_video_component::detail {

// Grants the component implementation typed access to controller state.
struct VideoControllerAccess {
  static std::shared_ptr<VideoControllerState> Get(const VideoController& controller) {
    return std::const_pointer_cast<VideoControllerState>(
        std::static_pointer_cast<const VideoControllerState>(controller.state_)
    );
  }
};

} // namespace lib_video_component::detail

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

// Placeholder surface shown while no frame is available. Uses the configured
// surface and status styling so the skeleton matches the final presentation.
View PlaceholderSurface(const VideoOptions& options, VideoStatus status, const std::string& error) {
  const std::string label = status == VideoStatus::Failed && !error.empty() ? error : StatusLabel(status);
  return Column {
      Text(label, TextRole::Label).With(Foreground(options.status_color)),
  }.With(
      Background(options.surface_color),
      MainAlign(MainAxisAlignment::Center),
      CrossAlign(CrossAxisAlignment::Center),
      Padding(16.0F)
  );
}

// One shared body for all Video overloads. The optional playing state selects
// controlled mode; the optional controller wires transport commands.
[[huxerui::composable]]
View VideoImpl(
    const VideoSource& source, const State<bool>* playing, const VideoController* controller, VideoOptions options
) {
  auto texture = UseState(ExternalTexture{});
  auto status = UseState(VideoStatus::Idle);
  auto error = UseState(std::string{});
  auto session = UseState(std::shared_ptr<detail::VideoSession>{});
  // Allocated unconditionally so the state slot identity stays stable when the
  // caller switches between controlled and uncontrolled use.
  auto internal_playing = UseState(options.auto_play);
  auto events = UseEvents();

  const bool initial_playing = playing != nullptr ? playing->Get() : internal_playing.Get();

  // Capture controller state by value; the caller's VideoController may be a
  // temporary that dies right after this declaration is constructed.
  const std::shared_ptr<detail::VideoControllerState> controller_state =
      controller != nullptr ? detail::VideoControllerAccess::Get(*controller) : nullptr;

  detail::VideoOpenOptions open{source, options, controller_state, nullptr, nullptr, nullptr};
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
  open.on_prepared = [events, controller_state](double width, double height, double duration_seconds) {
    if (controller_state != nullptr) {
      controller_state->duration.store(duration_seconds);
    }
    events.Emit<VideoEvents::Prepared>(VideoPreparedEvent{width, height, duration_seconds});
  };

  // Open one playback session for this component; the source selects it.
  Lifecycle(
      [open, session]() mutable {
        auto handle = OpenPlatformModule<std::shared_ptr<detail::VideoSession>>(detail::kVideoPlatformModule, open);
        session = handle;
        if (open.controller != nullptr) {
          std::lock_guard lock(open.controller->mutex);
          open.controller->session = handle;
        }
        return [open, handle] {
          if (open.controller != nullptr) {
            std::lock_guard lock(open.controller->mutex);
            open.controller->session.reset();
          }
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
  return PlaceholderSurface(options, status.Get(), error.Get());
}

} // namespace

VideoController::VideoController() : state_(std::make_shared<detail::VideoControllerState>()) {}

void VideoController::SeekTo(double position_seconds) const {
  const auto state = std::const_pointer_cast<detail::VideoControllerState>(
      std::static_pointer_cast<const detail::VideoControllerState>(state_)
  );
  std::lock_guard lock(state->mutex);
  if (const std::shared_ptr<detail::VideoSession> session = state->session.lock()) {
    session->SeekTo(position_seconds);
  }
}

double VideoController::Position() const {
  return static_cast<const detail::VideoControllerState*>(state_.get())->position.load();
}

double VideoController::Duration() const {
  return static_cast<const detail::VideoControllerState*>(state_.get())->duration.load();
}

bool VideoController::operator==(const VideoController& other) const noexcept {
  return state_ == other.state_;
}

View Video(const VideoSource& source, VideoOptions options) {
  return VideoImpl(source, nullptr, nullptr, std::move(options));
}

View Video(const VideoSource& source, const State<bool>& playing, VideoOptions options) {
  return VideoImpl(source, &playing, nullptr, std::move(options));
}

View Video(const VideoSource& source, const VideoController& controller, VideoOptions options) {
  return VideoImpl(source, nullptr, &controller, std::move(options));
}

View Video(const VideoSource& source, const VideoController& controller, const State<bool>& playing,
           VideoOptions options) {
  return VideoImpl(source, &playing, &controller, std::move(options));
}

} // namespace lib_video_component
