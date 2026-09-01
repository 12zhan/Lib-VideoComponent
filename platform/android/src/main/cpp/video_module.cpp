// Android platform module for the video component.
//
// The full native pipeline lands here; this stub keeps the module contract
// wired so the component framework runs end to end and reports a clear
// status while the implementation is pending.

#include <memory>
#include <utility>

#include <huxerui/android/platform_registry.h>

#include "video_session.h"

namespace lib_video_component::detail {
namespace {

class AndroidStubVideoSession final : public VideoSession {
public:
  void SetPlaying(bool) override {}
  void SeekTo(double) override {}
  void Close() noexcept override {}
};

std::shared_ptr<VideoSession> CreateAndroidStubVideoSession(
    huxerui::PlatformAdapter& adapter, JNIEnv*, jobject, const VideoOpenOptions& open
) {
  auto session = std::make_shared<AndroidStubVideoSession>();
  auto on_status = open.on_status;
  adapter.DispatchToUIThread([on_status] {
    if (on_status) {
      on_status(VideoStatus::Failed, "Android native video playback is not implemented yet");
    }
  });
  return session;
}

} // namespace

void InstallVideoPlatformModule(huxerui::RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<VideoSession>, VideoOpenOptions>(
      kVideoPlatformModule,
      huxerui::android::PlatformModuleFactory<std::shared_ptr<VideoSession>, VideoOpenOptions>{
          .create = CreateAndroidStubVideoSession,
      }
  );
}

} // namespace lib_video_component::detail
