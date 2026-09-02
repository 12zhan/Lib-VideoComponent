#include <huxerui/huxerui.h>
#include <lib_video_component/lib_video_component.h>

#include <app_resources.h>

#include <cstdio>
#include <string>

using namespace huxerui;

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

// Reads the packaged sample video through the raw resource API and proves the
// binary is reachable: byte count plus a hex view of the leading bytes.
[[huxerui::composable]]
View ResourceProbe() {
  const RawAsset video = UseRawResource(app::raw::kuaishou_mp4);
  const std::span<const std::byte> bytes = video.Bytes();

  return Column {
    Text("Lib-VideoComponent preview", TextRole::Title),
    Text::Format("resource present: {}", video.HasValue() ? std::string("yes") : std::string("no")),
    Text::Format("resource bytes: {}", std::to_string(bytes.size())),
    Text::Format("first bytes: {}", HexHead(bytes, 16)),
  }.With(Spacing(8.0F), Padding(24.0F), CrossAlign(CrossAxisAlignment::Start));
}

} // namespace

View App() {
  return MaterialTheme {
    ResourceProbe(),
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
