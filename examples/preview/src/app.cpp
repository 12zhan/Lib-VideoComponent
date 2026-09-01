#include <huxerui/huxerui.h>
#include <lib_video_component/lib_video_component.h>

using namespace huxerui;

View App() {
  return Text("Lib-VideoComponent preview");
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
