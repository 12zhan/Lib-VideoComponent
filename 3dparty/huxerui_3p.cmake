# Third-party dependency targets for Lib-VideoComponent.
#
# Each vendored library under 3dparty/<name>/ contributes one CMake target
# named HuxerUI3p::<Name>. The library CMakeLists includes this file and
# links the targets it needs. See 3dparty/README.md for the conventions.

if (NOT TARGET HuxerUI3p::miniaudio)
    add_library(miniaudio_3p STATIC "${CMAKE_CURRENT_LIST_DIR}/miniaudio/miniaudio.cpp")
    target_include_directories(miniaudio_3p PUBLIC "${CMAKE_CURRENT_LIST_DIR}/miniaudio")
    if (ANDROID)
        target_link_libraries(miniaudio_3p PUBLIC OpenSLES log)
    elseif (APPLE)
        target_link_libraries(
            miniaudio_3p PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation"
        )
    elseif (UNIX)
        target_link_libraries(miniaudio_3p PUBLIC pthread dl m)
    endif ()
    add_library(HuxerUI3p::miniaudio ALIAS miniaudio_3p)
endif ()
