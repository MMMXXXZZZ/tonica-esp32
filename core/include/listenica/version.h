#pragma once
// LISTENICA_VERSION ("0.1.0", told to Spotify and other clients) and LISTENICA_BUILD_VERSION
// ("0.1.0", or "0.1.0-dev+<commit>" outside a release; logs and mDNS) come from VERSION at the
// repo root, through cmake/listenica_version.cmake.
#if !defined(LISTENICA_VERSION) || !defined(LISTENICA_BUILD_VERSION)
#error "LISTENICA_VERSION and LISTENICA_BUILD_VERSION are set by the build (cmake/listenica_version.cmake)"
#endif
// Client identity values mirror librespot core/src/{version,config}.rs.
#define SPOTIFY_VERSION 124200290ULL
#define SPOTIFY_SEMANTIC_VERSION "1.2.52.442"
#define SPOTIFY_SPIRC_VERSION "3.2.6"
#define KEYMASTER_CLIENT_ID "65b708073fc0480ea92a077233ca87bd"
