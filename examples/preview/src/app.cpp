#include <huxerui/huxerui.h>
#include <lib_video_component/lib_video_component.h>

#include <string>
#include <utility>

using namespace huxerui;
using lib_video_component::Video;
using lib_video_component::VideoEvents;
using lib_video_component::VideoOptions;
using lib_video_component::VideoSource;
using lib_video_component::VideoStateEvent;
using lib_video_component::VideoStatus;

namespace {

const VideoSource& SampleSource() {
  static const VideoSource source{
      Uri{"https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4"},
  };
  return source;
}

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

[[huxerui::composable]]
View PlayerSection() {
  auto playing = UseState(false);
  auto status = UseState(std::string("Idle"));

  return Column {
    Video(SampleSource(), playing, VideoOptions{.auto_play = true, .fit = ImageFit::Contain})
        .On<VideoEvents::StateChanged>([status](const VideoStateEvent& event) {
          std::string label = StatusLabel(event.status);
          if (!event.error.empty()) {
            label += ": " + event.error;
          }
          status = std::move(label);
        })
        .With(Frame{.height = 220.0F}, CornerRadius(8.0F), ClipChildren()),
    Button(playing.Get() ? std::string("Pause") : std::string("Play")).OnClick([playing] {
      playing = !playing.Get();
    }),
    Text(status.Get()),
  }.With(Spacing(12.0F), Padding(16.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace

View App() {
  return MaterialTheme {
    PlayerSection(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "Lib-VideoComponent Preview",
        },
        .root_hooks = {
            lib_video_component::Install,
        },
    }
};
