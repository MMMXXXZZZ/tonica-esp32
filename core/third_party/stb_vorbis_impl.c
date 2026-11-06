/* Compiles the stb_vorbis implementation (pushdata API only). */
#define STB_VORBIS_NO_STDIO 1
#define STB_VORBIS_NO_PULLDATA_API 1
#define STB_VORBIS_NO_INTEGER_CONVERSION 1
/* Smaller tables beat fewer instructions on the ESP32-S3: the decoder's tables live in PSRAM
   behind a 32 KB cache. Measured on the board (2026-11-03, 320 kbps): 211 KB -> 123 KB and
   5.4x -> 5.8x realtime (the PC goes 7% slower; it has no such cache pressure). Same output. */
#define STB_VORBIS_DIVIDES_IN_CODEBOOK 1  /* lookup1 codebooks stay compact; values by division */
#define STB_VORBIS_FAST_HUFFMAN_LENGTH 8  /* 256-entry fast tables instead of 1024 */
#include "stb_vorbis.c"
