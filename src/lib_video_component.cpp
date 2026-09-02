#include "lib_video_component/lib_video_component.h"

#if !defined(__ANDROID__)

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
