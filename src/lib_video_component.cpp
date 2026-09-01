#include "lib_video_component/lib_video_component.h"

#include "video_session.h"

namespace lib_video_component {

void Install(huxerui::RootContext& root) {
  detail::InstallVideoPlatformModule(root);
}

} // namespace lib_video_component
