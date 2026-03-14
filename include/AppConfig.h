\
#pragma once
#include <stdint.h>

#define SOUNDPANEL7_VERSION "0.2.10"
#define SOUNDPANEL7_BUILD_DATE __DATE__ " " __TIME__

#ifndef SOUNDPANEL7_BUILD_ENV
#define SOUNDPANEL7_BUILD_ENV "unknown"
#endif

#ifndef SOUNDPANEL7_RELEASE_MANIFEST_URL
#define SOUNDPANEL7_RELEASE_MANIFEST_URL "https://github.com/jjtronics/SoundPanel7/releases/latest/download/release-manifest.json"
#endif

#ifndef SOUNDPANEL7_RELEASE_LATEST_API_URL
#define SOUNDPANEL7_RELEASE_LATEST_API_URL "https://api.github.com/repos/jjtronics/SoundPanel7/releases/latest"
#endif

// Compile-time default (can be overridden by build_flags)
#ifndef SOUNDPANEL7_MOCK_AUDIO
#define SOUNDPANEL7_MOCK_AUDIO 1
#endif
