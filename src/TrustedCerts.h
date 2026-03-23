#pragma once

#include <WiFiClientSecure.h>

#include "TrustedCertsBundle.h"

inline void configureSecureClient(WiFiClientSecure& client) {
  client.setCACertBundle(kSoundPanel7TrustedCaBundle, kSoundPanel7TrustedCaBundleLen);
}
