// Single translation unit that compiles the vendored miniaudio implementation.
// Consumers link HuxerUI3p::miniaudio and include "miniaudio.h" normally.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
