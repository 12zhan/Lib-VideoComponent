#if !defined(__ANDROID__)

#include <memory>
#include <utility>

#include "video_session.h"

namespace lib_video_component::detail {

namespace {

// Fallback backend for hosts without a video implementation. It keeps the
// component contract intact and reports one Failed transition.
class StubVideoSession final : public VideoSession {
public:
  void SetPlaying(bool) override {}
  void Close() noexcept override {}
};

} // namespace

void InstallVideoPlatformModule(huxerui::RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<VideoSession>, VideoOpenOptions>(
      kVideoPlatformModule,
      [](huxerui::PlatformAdapter& adapter, const VideoOpenOptions& open) {
        auto session = std::make_shared<StubVideoSession>();
        auto on_status = open.on_status;
        adapter.DispatchToUIThread([on_status] {
          if (on_status) {
            on_status(VideoStatus::Failed, "HuxerUI video playback is not implemented for this platform");
          }
        });
        return session;
      }
  );
}

} // namespace lib_video_component::detail

#endif
