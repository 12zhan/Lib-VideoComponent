#pragma once

#include <string_view>

#include <huxerui/data.h>
#include <huxerui/root.h>

namespace lib_video_component {

// Returns PNG-encoded cover art for a local video file, or empty bytes when
// the platform cannot decode one. Blocks while decoding; call from a worker.
[[nodiscard]] huxerui::Bytes ExtractVideoCoverPng(std::string_view path);

// Human-readable reason for the most recent ExtractVideoCoverPng result.
[[nodiscard]] std::string LastCoverDiagnostic();

void Install(huxerui::RootContext& root);

} // namespace lib_video_component
