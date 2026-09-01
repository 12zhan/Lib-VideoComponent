# Lib-VideoComponent

A HuxerUI library component for video playback. The shared `Video` component
renders decoder frames through `ExternalTexture` and drives a per-platform
player session through a `PlatformModule`.

## Layout

- `include/lib_video_component/` — public API: `Video`, `VideoSource`,
  `VideoOptions`, `VideoEvents`, `Install`.
- `src/` — shared component and platform-module contracts.
- `platform/android/` — Android backend (MediaPlayer + ImageReader frames).
- `examples/preview/` — preview application exercising the component.
- `3dparty/` — vendored third-party libraries (see its README).

## Build

Standard hosts use the HuxerUI CLI from the preview directory:

```bash
huxerui build android
huxerui run android
```

### Termux (aarch64) hosts

The official NDK ships x86_64 host tools that cannot run on aarch64 Termux,
and AGP's maven `aapt2` is also x86_64. Two host-side preparations are needed
once:

1. Stage runnable host tools (`hcg`, `hrc`) outside the noexec sdcard mount
   and configure builds with `-DHUXERUI_HOST_TOOL_ROOT=<staged-dir>`.
2. Point AGP at an arm64 `aapt2` through
   `android.aapt2FromMavenOverride=<sdk>/build-tools/36.0.0/aapt2` in
   `~/.gradle/gradle.properties`.

Then build native libraries externally with the Termux toolchain, assemble
the APK with the prebuilt-native mode enabled through
`examples/preview/platform/android/local.properties`:

```properties
sdk.dir=<android-sdk>
huxeruiPrebuiltNative=true
huxeruiPrebuiltNativeLibs=<jniLibs-directory>
```

```bash
export ANDROID_NDK_HOME=<sdk>/ndk/<version>
export HUXERUI_HOME=<huxerui-source-or-sdk>
APP_BUILD=examples/preview/platform/android/app/build
cmake -S examples/preview -B <build-dir> -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=android-termux.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHUXERUI_HOME=$HUXERUI_HOME \
  -DHUXERUI_HOST_TOOL_ROOT=<staged-tools> \
  -DHUXERUI_ANDROID_RESOURCE_OUTPUT_ROOT=$APP_BUILD/generated/huxerui/resources/debug
cmake --build <build-dir> --target example_lib_video_component
mkdir -p <jniLibs-directory>/arm64-v8a
cp <build-dir>/libhuxerui_app.so \
   <build-dir>/huxerui-sdk/lib/libhuxerui.so \
   $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so \
   <jniLibs-directory>/arm64-v8a/
cd examples/preview/platform/android
java -cp gradle/wrapper/gradle-wrapper.jar org.gradle.wrapper.GradleWrapperMain :app:assembleDebug
```

The APK is emitted under `app/build/outputs/apk/debug/`.
