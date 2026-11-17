#include "display_test.h"

static const uint16_t solid_colors[] = {0x0000u, 0xFFFFu, 0xF800u, 0x07E0u, 0x001Fu};
static const uint16_t bars[] = {0xFFFFu, 0xFFE0u, 0x07FFu, 0x07E0u,
                                0xF81Fu, 0xF800u, 0x001Fu, 0x0000u};

const char *DisplayTest_PatternName(DisplayTest_Pattern pattern)
{
  static const char *const names[] = {"black", "white", "red", "green", "blue",
                                      "color-bars", "checkerboard", "geometry", "horizontal-bars"};
  return (unsigned)pattern < DISPLAY_PATTERN_COUNT ? names[pattern] : "invalid";
}

static uint16_t Geometry(uint32_t x, uint32_t y)
{
  if (x == 0u || y == 0u || x == DISPLAY_TEST_WIDTH - 1u || y == DISPLAY_TEST_HEIGHT - 1u) {
    return 0xFFFFu;
  }
  if (x < 33u && y < 33u) return 0xF800u;
  if (x >= DISPLAY_TEST_WIDTH - 33u && y < 33u) return 0x07E0u;
  if (x < 33u && y >= DISPLAY_TEST_HEIGHT - 33u) return 0x001Fu;
  if (x >= DISPLAY_TEST_WIDTH - 33u && y >= DISPLAY_TEST_HEIGHT - 33u) return 0xFFE0u;
  if (x == DISPLAY_TEST_WIDTH / 2u || y == DISPLAY_TEST_HEIGHT / 2u) return 0xFFFFu;
  return 0x4208u;
}

bool DisplayTest_Draw(volatile uint16_t *pixels, size_t capacity_bytes, DisplayTest_Pattern pattern)
{
  if (pixels == NULL || ((uintptr_t)pixels & 1u) != 0u ||
      capacity_bytes < DISPLAY_TEST_FRAME_BYTES || (unsigned)pattern >= DISPLAY_PATTERN_COUNT) {
    return false;
  }
  for (uint32_t y = 0u; y < DISPLAY_TEST_HEIGHT; ++y) {
    for (uint32_t x = 0u; x < DISPLAY_TEST_WIDTH; ++x) {
      uint16_t color;
      if (pattern <= DISPLAY_PATTERN_BLUE) {
        color = solid_colors[pattern];
      } else if (pattern == DISPLAY_PATTERN_COLOR_BARS) {
        color = bars[x / (DISPLAY_TEST_WIDTH / 8u)];
      } else if (pattern == DISPLAY_PATTERN_HORIZONTAL_BARS) {
        color = bars[y / (DISPLAY_TEST_HEIGHT / 8u)];
      } else if (pattern == DISPLAY_PATTERN_CHECKERBOARD) {
        color = ((x / 32u + y / 32u) & 1u) == 0u ? 0xFFFFu : 0x0000u;
      } else {
        color = Geometry(x, y);
      }
      pixels[(size_t)y * DISPLAY_TEST_WIDTH + x] = color;
    }
  }
  return true;
}
