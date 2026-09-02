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

namespace {

// Renders the first bytes as hex so the resource read is visibly verifiable.
std::string HexHead(const std::span<const std::byte> bytes, std::size_t count) {
  std::string text;
  const std::size_t shown = bytes.size() < count ? bytes.size() : count;
  for (std::size_t index = 0; index < shown; ++index) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02X ", static_cast<unsigned>(bytes[index]));
    text += buffer;
  }
  return text;
}

// Layered probe: raw resource bytes, a decoded cover frame, and image
// rendering. Each step reports its own status so failures localize.
[[huxerui::composable]]
View CoverProbe() {
  auto status = UseState(std::string("starting"));
  auto cover = UseState(ImageAsset{});
  auto files = UseService<FileSystem>();
  auto tasks = UseTaskScope();
  const RawAsset video = UseRawResource(app::raw::kuaishou_mp4);
  const std::span<const std::byte> bytes = video.Bytes();

  Lifecycle([status, cover, files, tasks, video] {
    tasks.Launch([status, cover, files, video]() -> Task<void> {
      const File target = File(files->Directories().cache_directory, "kuaishou.mp4");
      if (!target.Exists()) {
        const auto source = video.Bytes();
        const bool written = co_await target.WriteBytesAsync(Bytes(source.begin(), source.end()));
        if (!written) {
          status = "cache write failed";
          co_return;
        }
      }
      status = "decoding cover";
      const std::string path = target.Path();
      Bytes png = co_await RunWorker([](const std::string& source) { return ExtractVideoCoverPng(source); }, path);
      if (png.empty()) {
        status = "cover failed: " + LastCoverDiagnostic();
        co_return;
      }
      cover = ImageAsset::FromEncoded(std::move(png));
      status = "cover ready";
    });
  });

  View preview = Text(status.Get(), TextRole::Label);
  if (cover.Get().EncodedBytes().empty() == false) {
    preview = Image(cover.Get()).Fit(ImageFit::Contain).With(Frame{.height = 220.0F}, CornerRadius(8.0F));
  }

  return Column {
    Text("Lib-VideoComponent preview", TextRole::Title),
    Text::Format("resource bytes: {}", std::to_string(bytes.size())),
    Text::Format("first bytes: {}", HexHead(bytes, 16)),
    std::move(preview),
    Text::Format("status: {}", status),
  }.With(Spacing(8.0F), Padding(24.0F), CrossAlign(CrossAxisAlignment::Start));
}

} // namespace

View App() {
  return MaterialTheme {
    CoverProbe(),
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
