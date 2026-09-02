#include <huxerui/huxerui.h>
#include <lib_video_component/lib_video_component.h>

#include <app_resources.h>

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

using namespace huxerui;
using lib_video_component::ExtractVideoCoverPng;
using lib_video_component::LastCoverDiagnostic;
using lib_video_component::Video;
using lib_video_component::VideoEvents;
using lib_video_component::VideoOptions;
using lib_video_component::VideoSource;
using lib_video_component::VideoStateEvent;
using lib_video_component::VideoStatus;

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

// Extracts the packaged sample video once and plays it back: cover decode,
// frame pipeline, and the Video component in one page. Each stage reports
// its own status so failures localize.
[[huxerui::composable]]
View PlayerProbe() {
  auto stage = UseState(std::string("starting"));
  auto cover = UseState(ImageAsset{});
  auto source = UseState(std::optional<VideoSource>{});
  auto playing = UseState(true);
  auto status = UseState(std::string("Idle"));
  auto files = UseService<FileSystem>();
  auto tasks = UseTaskScope();
  const RawAsset video_resource = UseRawResource(app::raw::kuaishou_mp4);

  Lifecycle([stage, cover, source, files, tasks, video_resource] {
    tasks.Launch([stage, cover, source, files, video_resource]() -> Task<void> {
      const File target = File(files->Directories().cache_directory, "kuaishou.mp4");
      if (!target.Exists()) {
        stage = "extracting";
        const auto bytes = video_resource.Bytes();
        const bool written = co_await target.WriteBytesAsync(Bytes(bytes.begin(), bytes.end()));
        if (!written) {
          stage = "cache write failed";
          co_return;
        }
      }
      stage = "decoding cover";
      const std::string path = target.Path();
      Bytes png = co_await RunWorker([](const std::string& source_path) { return ExtractVideoCoverPng(source_path); },
                                     path);
      if (!png.empty()) {
        cover = ImageAsset::FromEncoded(std::move(png));
      }
      stage = "playing";
      source = VideoSource{target.ToUri()};
    });
  });

  View cover_view = Text("cover: " + LastCoverDiagnostic(), TextRole::Label);
  if (!cover.Get().EncodedBytes().empty()) {
    cover_view = Image(cover.Get()).Fit(ImageFit::Cover).With(Frame{.width = 96.0F, .height = 96.0F},
                                                              CornerRadius(12.0F));
  }

  View player_view = Text(stage.Get(), TextRole::Label);
  if (const std::optional<VideoSource> current = source.Get(); current.has_value()) {
    player_view = Video(*current, playing, VideoOptions{.auto_play = true, .fit = ImageFit::Contain})
                      .On<VideoEvents::StateChanged>([status](const VideoStateEvent& event) {
                        std::string label = StatusLabel(event.status);
                        if (!event.error.empty()) {
                          label += ": " + event.error;
                        }
                        status = std::move(label);
                      })
                      .With(Frame{.height = 220.0F}, CornerRadius(8.0F), ClipChildren());
  }

  return Column {
    Row {
      std::move(cover_view),
      Column {
        Text("Lib-VideoComponent preview", TextRole::Title),
        Text::Format("status: {}", status),
      }.With(Spacing(4.0F)),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)),
    std::move(player_view),
    Button(playing.Get() ? std::string("Pause") : std::string("Play")).OnClick([playing] {
      playing = !playing.Get();
    }),
  }.With(Spacing(12.0F), Padding(24.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace

View App() {
  return MaterialTheme {
    PlayerProbe(),
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
