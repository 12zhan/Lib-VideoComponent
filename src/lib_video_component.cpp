#include "lib_video_component/lib_video_component.h"

#if defined(__ANDROID__)

#include "video_session.h"

namespace lib_video_component {

void Install(huxerui::RootContext& root) {
  detail::InstallVideoPlatformModule(root);
}

} // namespace lib_video_component

#else

namespace lib_video_component {

huxerui::Bytes ExtractVideoCoverPng(std::string_view) {
  return {};
}

std::string LastCoverDiagnostic() {
  return "cover extraction is not implemented for this platform";
}

void Install(huxerui::RootContext&) {}

} // namespace lib_video_component

#endif
