#ifndef GPUKEY_PATTERN_H
#define GPUKEY_PATTERN_H

#include <stdint.h>

/* Synthetic fixture only. Positive dimensions and width*height*4 writable bytes
 * are required; every uint64_t index preserves the original wrapping pattern. */
void gk_test_pattern(uint8_t *pixels, int width, int height, uint64_t index);

#endif
