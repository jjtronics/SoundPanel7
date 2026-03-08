\
#pragma once
#include <stdint.h>

#define SOUNDPANEL7_VERSION "0.1.0"
#define SOUNDPANEL7_BUILD_DATE __DATE__ " " __TIME__

#ifndef SOUNDPANEL7_BUILD_ENV
#define SOUNDPANEL7_BUILD_ENV "unknown"
#endif

// Compile-time default (can be overridden by build_flags)
#ifndef SOUNDPANEL7_MOCK_AUDIO
#define SOUNDPANEL7_MOCK_AUDIO 1
#endif
