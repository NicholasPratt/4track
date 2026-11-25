// 8track's way in to CloudSeedCore. Its build settings come from the compiler's command line
// in the original project; they're set here instead, along with a stand-in for strcpy_s,
// which only Windows has.
#pragma once

#ifndef BUFFER_SIZE
 #define BUFFER_SIZE 1024
#endif
#ifndef MAX_STR_SIZE
 #define MAX_STR_SIZE 32
#endif
#include <cstdio>
#if !defined(_WIN32)
 #define strcpy_s(dest, size, src) std::snprintf((dest), (size), "%s", (src))
#endif

#include "DSP/ReverbController.h"
