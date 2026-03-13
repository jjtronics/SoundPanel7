\
#pragma once
#include <stdint.h>

#define SOUNDPANEL7_VERSION "0.2.1"
#define SOUNDPANEL7_BUILD_DATE __DATE__ " " __TIME__

#ifndef SOUNDPANEL7_BUILD_ENV
#define SOUNDPANEL7_BUILD_ENV "unknown"
#endif

#ifndef SOUNDPANEL7_RELEASE_MANIFEST_URL
#define SOUNDPANEL7_RELEASE_MANIFEST_URL "https://github.com/jjtronics/SoundPanel7/releases/latest/download/release-manifest.json"
#endif

// Compile-time default (can be overridden by build_flags)
#ifndef SOUNDPANEL7_MOCK_AUDIO
#define SOUNDPANEL7_MOCK_AUDIO 1
#endif
