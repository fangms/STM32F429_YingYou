#include "boot_screen.h"
#include "firmware_version.h"

/* Original minimal 5x7 glyphs, columns left-to-right, bit 0 at top. */
static const uint8_t letters[26][5] = {
  {0x7E,0x09,0x09,0x09,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
  {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
  {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
  {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
  {0x41,0x41,0x7F,0x41,0x41}, {0x20,0x40,0x41,0x3F,0x01},
  {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
  {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
  {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
  {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
  {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
  {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}
};
static const uint8_t digits[10][5] = {
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
  {0x62,0x51,0x49,0x49,0x46}, {0x22,0x41,0x49,0x49,0x36},
  {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}
};

static void Rect(volatile uint16_t *p, unsigned x, unsigned y,
                 unsigned width, unsigned height, uint16_t color)
{
  for (unsigned row = y; row < y + height && row < DISPLAY_TEST_HEIGHT; ++row)
    for (unsigned col = x; col < x + width && col < DISPLAY_TEST_WIDTH; ++col)
      p[(size_t)row * DISPLAY_TEST_WIDTH + col] = color;
}

static void Text(volatile uint16_t *p, unsigned x, unsigned y, const char *text,
                 unsigned scale, uint16_t color)
{
  for (; *text != '\0' && x + 5u * scale <= DISPLAY_TEST_WIDTH; ++text, x += 6u * scale) {
    const uint8_t *glyph = NULL;
    if (*text >= 'A' && *text <= 'Z') glyph = letters[(unsigned)(*text - 'A')];
    else if (*text >= '0' && *text <= '9') glyph = digits[(unsigned)(*text - '0')];
    if (glyph != NULL) {
      for (unsigned col = 0u; col < 5u; ++col)
        for (unsigned row = 0u; row < 7u; ++row)
          if ((glyph[col] & (1u << row)) != 0u)
            Rect(p, x + col * scale, y + row * scale, scale, scale, color);
    } else if (*text == '.') Rect(p, x + scale, y + 6u * scale, scale, scale, color);
  }
}

bool BootScreen_Draw(volatile uint16_t *p, size_t capacity, uint32_t seconds, bool scan_error)
{
  if (p == NULL || ((uintptr_t)p & 1u) != 0u || capacity < DISPLAY_TEST_FRAME_BYTES) return false;
  Rect(p, 0u, 0u, 400u, 960u, 0x0841u);
  uint16_t status = scan_error ? 0xF800u : 0x07E0u;
  Rect(p, 0u, 0u, 400u, 4u, status);
  Rect(p, 0u, 956u, 400u, 4u, status);
  Rect(p, 0u, 0u, 4u, 960u, status);
  Rect(p, 396u, 0u, 4u, 960u, status);
  Text(p, 28u, 44u, "H863 " H863_FIRMWARE_VERSION, 4u, 0xFFFFu);
  Text(p, 28u, 116u, scan_error ? "LTDC ERROR" : "BOOT OK", 4u, status);
  Text(p, 28u, 208u, "SDRAM PASS", 3u, 0xFFFFu);
  Text(p, 28u, 256u, "BRIDGE PASS", 3u, 0xFFFFu);
  Text(p, 28u, 304u, "PANEL INIT SENT", 3u, 0xFFE0u);
  Text(p, 28u, 374u, "RUN", 3u, 0xFFFFu);
  unsigned beat = seconds % 8u;
  char count[11];
  count[10] = '\0';
  for (unsigned i = 10u; i > 0u; --i) {
    count[i - 1u] = (char)('0' + seconds % 10u);
    seconds /= 10u;
  }
  Text(p, 100u, 374u, count, 3u, 0x07FFu);
  Text(p, 28u, 420u, "SECONDS SINCE SCREEN", 2u, 0xC618u);
  /* A different square is highlighted each second. */
  for (unsigned i = 0u; i < 8u; ++i)
    Rect(p, 28u + i * 44u, 474u, 36u, 36u, i == beat ? status : 0x2104u);
  Text(p, 28u, 558u, "BASIC STARTUP CHECK", 2u, 0xFFFFu);
  Text(p, 28u, 590u, "PUMP SENSOR NOT TESTED", 2u, 0xFFE0u);
  static const uint16_t bars[] = {0xFFFFu,0xFFE0u,0x07FFu,0x07E0u,0xF81Fu,0xF800u,0x001Fu,0u};
  for (unsigned i = 0u; i < 8u; ++i) Rect(p, 24u + i * 44u, 670u, 44u, 150u, bars[i]);
  Text(p, 28u, 854u, "400 X 960 RGB565", 3u, 0xFFFFu);
  Text(p, 28u, 902u, "CHECK BORDER AND COLORS", 2u, 0xC618u);
  return true;
}

bool BootScreen_DrawDiagnostic(volatile uint16_t *p, size_t capacity, bool falling)
{
  if (!BootScreen_Draw(p, capacity, 0u, false)) return false;
  Rect(p, 20u, 110u, 370u, 70u, 0x0841u);
  Text(p, 28u, 116u, "DISPLAY TEST", 4u, 0xFFFFu);
  Rect(p, 20u, 360u, 370u, 270u, 0x0841u);
  Text(p, 28u, 374u, falling ? "EDGE FALLING" : "EDGE RISING", 4u, 0x07FFu);
  Text(p, 28u, 440u, "STATIC FRAME", 4u, 0xFFE0u);
  Text(p, 28u, 510u, "10 SECOND TEST PAGE", 3u, 0xFFFFu);
  Text(p, 28u, 558u, "CHECK TEXT AND BORDER", 2u, 0xFFFFu);
  Text(p, 28u, 590u, "VISUAL CHECK REQUIRED", 2u, 0xFFE0u);
  return true;
}
