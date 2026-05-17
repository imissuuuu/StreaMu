#pragma once

/*
 * Minimal Helix configuration for the 3DS AAC PoC.
 *
 * The upstream Arduino wrapper enables SBR by default on non-ESP8266 targets.
 * This vendored import keeps the core LC-AAC path only to reduce memory and
 * binary footprint while the AAC path is experimental.
 */

#ifndef SYNCH_WORD_LEN
#define SYNCH_WORD_LEN 4
#endif

#ifndef AAC_MAX_OUTPUT_SIZE
#define AAC_MAX_OUTPUT_SIZE (1024 * 8)
#endif

#ifndef AAC_MAX_FRAME_SIZE
#define AAC_MAX_FRAME_SIZE 2100
#endif

#ifndef AAC_MIN_FRAME_SIZE
#define AAC_MIN_FRAME_SIZE 1024
#endif
