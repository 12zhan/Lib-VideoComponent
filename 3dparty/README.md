# Third-party dependencies

Vendored third-party libraries live under `3dparty/<name>/`, following the
HuxerUI repository convention:

- `huxerui_3p.cmake` defines one CMake target named `HuxerUI3p::<Name>`; the
  library CMakeLists include it when the dependency is needed.
- `METADATA` records the upstream name, version, source URL, and license.
- Every vendored copy keeps the upstream `LICENSE` file next to its sources.

## Vendored libraries

- `miniaudio` — single-file cross-platform audio playback and capture
  (Public Domain or MIT-0). Used for software audio paths where the video
  component owns decoding and needs a uniform audio sink on every backend.
  On Android it uses AAudio when available and OpenSLES otherwise; on Apple
  platforms Core Audio; on Linux and Windows the native backends.

## System dependencies (not vendored)

- **FFmpeg** — when a backend needs software demuxing or decoding beyond the
  platform media stack, FFmpeg stays a distribution- or SDK-provided
  dependency resolved through pkg-config or explicit paths. It is too large
  to vendor; do not copy it into this directory.

Adding a dependency: copy the source under `3dparty/<name>/` with its
license, record `METADATA`, add the target to `huxerui_3p.cmake`, and link
it from the root `CMakeLists.txt`.
