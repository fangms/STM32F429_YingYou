#include "display_test.h"

static const uint16_t solid_colors[] = {0x0000u, 0xFFFFu, 0xF800u, 0x07E0u, 0x001Fu};
static const uint16_t bars[] = {0xFFFFu, 0xFFE0u, 0x07FFu, 0x07E0u,
                                0xF81Fu, 0xF800u, 0x001Fu, 0x0000u};

const char *DisplayTest_PatternName(DisplayTest_Pattern pattern)
{
  static const char *const names[] = {"black", "white", "red", "green", "blue",
                                      "color-bars", "checkerboard", "geometry", "horizontal-bars",
                                      "gray16", "rgb-levels", "rgb-bits", "checker4",
                                      "vertical1", "horizontal1", "geometry-body"};
  return (unsigned)pattern < DISPLAY_PATTERN_COUNT ? names[pattern] : "invalid";
}

static uint16_t Geometry(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
  if (x == 0u || y == 0u || x == width - 1u || y == height - 1u) {
    return 0xFFFFu;
  }
  if (x < 33u && y < 33u) return 0xF800u;
  if (x >= width - 33u && y < 33u) return 0x07E0u;
  if (x < 33u && y >= height - 33u) return 0x001Fu;
  if (x >= width - 33u && y >= height - 33u) return 0xFFE0u;
  if (x == width / 2u || y == height / 2u) return 0xFFFFu;
  return 0x4208u;
}

bool DisplayTest_Draw(volatile uint16_t *pixels, size_t capacity_bytes, DisplayTest_Pattern pattern)
{
  return DisplayTest_DrawOriented(pixels, capacity_bytes, pattern, DISPLAY_ROTATION_0);
}

bool DisplayTest_DrawOriented(volatile uint16_t *pixels, size_t capacity_bytes,
                              DisplayTest_Pattern pattern, DisplayRotation rotation)
{
  DisplaySurface surface;
  if ((unsigned)pattern >= DISPLAY_PATTERN_COUNT ||
      !DisplaySurface_Init(&surface, pixels, capacity_bytes, rotation)) return false;
  uint32_t width = surface.width, height = surface.height;
  uint32_t header = rotation == DISPLAY_ROTATION_0 ? 144u : 80u;
  for (uint32_t y = 0u; y < height; ++y) {
    for (uint32_t x = 0u; x < width; ++x) {
      uint16_t color;
      if (pattern <= DISPLAY_PATTERN_BLUE) {
        color = solid_colors[pattern];
      } else if (pattern == DISPLAY_PATTERN_COLOR_BARS) {
        color = bars[x / (width / 8u)];
      } else if (pattern == DISPLAY_PATTERN_HORIZONTAL_BARS) {
        color = bars[y / (height / 8u)];
      } else if (pattern == DISPLAY_PATTERN_CHECKERBOARD) {
        color = ((x / 32u + y / 32u) & 1u) == 0u ? 0xFFFFu : 0x0000u;
      } else if (pattern == DISPLAY_PATTERN_GRAY16) {
        uint32_t level = x * 16u / width; /* 16 equal columns, dark to light. */
        uint32_t r = level * 31u / 15u, g = level * 63u / 15u;
        color = (uint16_t)((r << 11u) | (g << 5u) | r);
      } else if (pattern == DISPLAY_PATTERN_RGB_LEVELS) {
        /* Three equal-height strips below the orientation-specific header. */
        unsigned band = y < header ? 0u : (y - header) * 3u / (height - header);
        uint32_t level = x * (band == 1u ? 64u : 32u) / width;
        color = (uint16_t)(band == 0u ? level << 11u : band == 1u ? level << 5u : level);
      } else if (pattern == DISPLAY_PATTERN_RGB_BITS) {
        /* Left-to-right: B0..B4, G0..G5, R0..R4. Lower half is the inverse. */
        uint16_t bit = (uint16_t)(1u << (x * 16u / width));
        color = y < header + (height - header) / 2u ? bit : (uint16_t)(bit ^ 0xFFFFu);
      } else if (pattern == DISPLAY_PATTERN_CHECKER4) {
        color = ((x / 4u + y / 4u) & 1u) == 0u ? 0xFFFFu : 0x0000u;
      } else if (pattern == DISPLAY_PATTERN_VERTICAL1) {
        color = (x & 1u) == 0u ? 0xFFFFu : 0x0000u;
      } else if (pattern == DISPLAY_PATTERN_HORIZONTAL1) {
        color = (y & 1u) == 0u ? 0xFFFFu : 0x0000u;
      } else if (pattern == DISPLAY_PATTERN_GEOMETRY_BODY) {
        /* Visible geometry starts below the diagnostic identity header. */
        if (x < 4u || x >= width - 4u || y < header + 4u || y >= height - 4u) color = 0xFFFFu;
        else if (x < 37u && y < header + 37u) color = 0xF800u;
        else if (x >= width - 37u && y < header + 37u) color = 0x07E0u;
        else if (x < 37u && y >= height - 37u) color = 0x001Fu;
        else if (x >= width - 37u && y >= height - 37u) color = 0xFFE0u;
        else if ((x >= width / 2u - 2u && x < width / 2u + 2u) ||
                 (y >= header + (height - header) / 2u - 2u &&
                  y < header + (height - header) / 2u + 2u)) color = 0xFFFFu;
        else color = 0x4208u;
      } else {
        color = Geometry(x, y, width, height);
      }
      DisplaySurface_Pixel(&surface, x, y, color);
    }
  }
  return true;
}
