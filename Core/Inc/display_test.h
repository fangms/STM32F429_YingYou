#ifndef DISPLAY_TEST_H
#define DISPLAY_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_TEST_WIDTH 400u
#define DISPLAY_TEST_HEIGHT 960u
#define DISPLAY_TEST_STRIDE_BYTES (DISPLAY_TEST_WIDTH * 2u)
#define DISPLAY_TEST_FRAME_BYTES (DISPLAY_TEST_STRIDE_BYTES * DISPLAY_TEST_HEIGHT)

typedef enum {
  DISPLAY_PATTERN_BLACK, DISPLAY_PATTERN_WHITE, DISPLAY_PATTERN_RED,
  DISPLAY_PATTERN_GREEN, DISPLAY_PATTERN_BLUE, DISPLAY_PATTERN_COLOR_BARS,
  DISPLAY_PATTERN_CHECKERBOARD, DISPLAY_PATTERN_GEOMETRY,
  DISPLAY_PATTERN_HORIZONTAL_BARS, DISPLAY_PATTERN_COUNT
} DisplayTest_Pattern;

const char *DisplayTest_PatternName(DisplayTest_Pattern pattern);
/* Draw exactly one RGB565 frame; caller must disable the scanning layer and confirm reload first. */
bool DisplayTest_Draw(volatile uint16_t *pixels, size_t capacity_bytes, DisplayTest_Pattern pattern);

#endif
