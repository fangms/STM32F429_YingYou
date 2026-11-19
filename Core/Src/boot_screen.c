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

static void Rect(const DisplaySurface *s, unsigned x, unsigned y,
                 unsigned width, unsigned height, uint16_t color)
{
  for (unsigned row = y; row < y + height && row < s->height; ++row)
    for (unsigned col = x; col < x + width && col < s->width; ++col)
      DisplaySurface_Pixel(s, col, row, color);
}

static void Text(const DisplaySurface *s, unsigned x, unsigned y, const char *text,
                 unsigned scale, uint16_t color)
{
  for (; *text != '\0' && x + 5u * scale <= s->width; ++text, x += 6u * scale) {
    const uint8_t *glyph = NULL;
    if (*text >= 'A' && *text <= 'Z') glyph = letters[(unsigned)(*text - 'A')];
    else if (*text >= '0' && *text <= '9') glyph = digits[(unsigned)(*text - '0')];
    if (glyph != NULL) {
      for (unsigned col = 0u; col < 5u; ++col)
        for (unsigned row = 0u; row < 7u; ++row)
          if ((glyph[col] & (1u << row)) != 0u)
            Rect(s, x + col * scale, y + row * scale, scale, scale, color);
    } else if (*text == '.') Rect(s, x + scale, y + 6u * scale, scale, scale, color);
  }
}

bool BootScreen_DrawText(volatile uint16_t *p, size_t capacity, unsigned x, unsigned y,
                         const char *text, unsigned scale, uint16_t color, DisplayRotation rotation)
{
  DisplaySurface surface;
  if (text == NULL || scale == 0u || scale > 32u ||
      !DisplaySurface_Init(&surface,p,capacity,rotation) ||
      x >= surface.width || y >= surface.height) return false;
  Text(&surface,x,y,text,scale,color);
  return true;
}

bool BootScreen_Draw(volatile uint16_t *p, size_t capacity, uint32_t seconds, bool scan_error)
{
  return BootScreen_DrawOriented(p, capacity, seconds, scan_error, DISPLAY_ROTATION_0);
}

bool BootScreen_DrawOriented(volatile uint16_t *p, size_t capacity, uint32_t seconds,
                             bool scan_error, DisplayRotation rotation)
{
  DisplaySurface surface;
  if (!DisplaySurface_Init(&surface, p, capacity, rotation)) return false;
  const DisplaySurface *s = &surface;
  if (rotation != DISPLAY_ROTATION_0) {
    Rect(s, 0u, 0u, s->width, s->height, 0x0841u);
    uint16_t status = scan_error ? 0xF800u : 0x07E0u;
    Rect(s, 0u, 0u, 960u, 4u, status);
    Rect(s, 0u, 396u, 960u, 4u, status);
    Rect(s, 0u, 0u, 4u, 400u, status);
    Rect(s, 956u, 0u, 4u, 400u, status);
    Text(s, 24u, 20u, "H863 " H863_FIRMWARE_VERSION "  960 X 400", 4u, 0xFFFFu);
    Text(s, 24u, 72u, scan_error ? "LTDC ERROR" : "BOOT OK", 5u, status);
    Text(s, 24u, 140u, "SDRAM PASS   BRIDGE PASS", 3u, 0xFFFFu);
    Text(s, 24u, 182u, "PANEL INIT SENT", 3u, 0xFFE0u);
    char count[11];
    uint32_t value = seconds;
    count[10] = '\0';
    for (unsigned i = 10u; i > 0u; --i) {
      count[i - 1u] = (char)('0' + value % 10u);
      value /= 10u;
    }
    Text(s, 24u, 226u, "RUN", 3u, 0xFFFFu);
    Text(s, 110u, 226u, count, 3u, 0x07FFu);
    for (unsigned i = 0u; i < 8u; ++i)
      Rect(s, 24u + i * 44u, 276u, 36u, 28u, i == seconds % 8u ? status : 0x2104u);
    static const uint16_t colors[] = {0xFFFFu,0xFFE0u,0x07FFu,0x07E0u,0xF81Fu,0xF800u,0x001Fu,0u};
    for (unsigned i = 0u; i < 8u; ++i) Rect(s, 560u + i * 46u, 80u, 46u, 220u, colors[i]);
    Text(s, 24u, 344u, "PUMP SENSOR NOT TESTED", 3u, 0xFFE0u);
    return true;
  }
  Rect(s, 0u, 0u, 400u, 960u, 0x0841u);
  uint16_t status = scan_error ? 0xF800u : 0x07E0u;
  Rect(s, 0u, 0u, 400u, 4u, status);
  Rect(s, 0u, 956u, 400u, 4u, status);
  Rect(s, 0u, 0u, 4u, 960u, status);
  Rect(s, 396u, 0u, 4u, 960u, status);
  Text(s, 28u, 44u, "H863 " H863_FIRMWARE_VERSION, 4u, 0xFFFFu);
  Text(s, 28u, 116u, scan_error ? "LTDC ERROR" : "BOOT OK", 4u, status);
  Text(s, 28u, 208u, "SDRAM PASS", 3u, 0xFFFFu);
  Text(s, 28u, 256u, "BRIDGE PASS", 3u, 0xFFFFu);
  Text(s, 28u, 304u, "PANEL INIT SENT", 3u, 0xFFE0u);
  Text(s, 28u, 374u, "RUN", 3u, 0xFFFFu);
  unsigned beat = seconds % 8u;
  char count[11];
  count[10] = '\0';
  for (unsigned i = 10u; i > 0u; --i) {
    count[i - 1u] = (char)('0' + seconds % 10u);
    seconds /= 10u;
  }
  Text(s, 100u, 374u, count, 3u, 0x07FFu);
  Text(s, 28u, 420u, "SECONDS SINCE SCREEN", 2u, 0xC618u);
  /* A different square is highlighted each second. */
  for (unsigned i = 0u; i < 8u; ++i)
    Rect(s, 28u + i * 44u, 474u, 36u, 36u, i == beat ? status : 0x2104u);
  Text(s, 28u, 558u, "BASIC STARTUP CHECK", 2u, 0xFFFFu);
  Text(s, 28u, 590u, "PUMP SENSOR NOT TESTED", 2u, 0xFFE0u);
  static const uint16_t bars[] = {0xFFFFu,0xFFE0u,0x07FFu,0x07E0u,0xF81Fu,0xF800u,0x001Fu,0u};
  for (unsigned i = 0u; i < 8u; ++i) Rect(s, 24u + i * 44u, 670u, 44u, 150u, bars[i]);
  Text(s, 28u, 854u, "400 X 960 RGB565", 3u, 0xFFFFu);
  Text(s, 28u, 902u, "CHECK BORDER AND COLORS", 2u, 0xC618u);
  return true;
}

bool BootScreen_DrawDiagnostic(volatile uint16_t *p, size_t capacity, bool falling)
{
  return BootScreen_DrawDiagnosticOriented(p, capacity, falling, DISPLAY_ROTATION_0);
}

bool BootScreen_DrawDiagnosticOriented(volatile uint16_t *p, size_t capacity,
                                       bool falling, DisplayRotation rotation)
{
  DisplaySurface surface;
  if (!DisplaySurface_Init(&surface, p, capacity, rotation) ||
      !DisplayTest_DrawOriented(p, capacity, DISPLAY_PATTERN_BLACK, rotation)) return false;
  const DisplaySurface *s = &surface;
  if (rotation != DISPLAY_ROTATION_0) {
    Rect(s, 0u, 80u, 4u, 320u, 0xFFFFu);
    Rect(s, 956u, 80u, 4u, 320u, 0xFFFFu);
    Rect(s, 0u, 396u, 960u, 4u, 0xFFFFu);
    Text(s, 56u, 120u, falling ? "B" : "A", 24u, 0xFFFFu);
    Text(s, 240u, 116u, falling ? "FALLING" : "RISING", 7u, 0xFFFFu);
    Rect(s, 620u, 136u, 96u, 96u, 0xFFFFu);
    if (falling) Rect(s, 748u, 136u, 96u, 96u, 0xFFFFu);
    Text(s, 240u, 242u, "VISUAL CHECK", 4u, 0xFFFFu);
    Text(s, 240u, 306u, "LANDSCAPE 960 X 400", 3u, 0xFFFFu);
    return BootScreen_DrawTestLabelOriented(p, capacity, falling, 1u, "EDGE CARD", rotation);
  }
  Rect(s, 0u, 144u, 8u, 816u, 0xFFFFu);
  Rect(s, 392u, 144u, 8u, 816u, 0xFFFFu);
  Rect(s, 0u, 952u, 400u, 8u, 0xFFFFu);
  Text(s, 140u, 240u, falling ? "B" : "A", 20u, 0xFFFFu);
  Text(s, falling ? 32u : 56u, 440u, falling ? "FALLING" : "RISING", 8u, 0xFFFFu);
  /* One/two large blocks identify the edge even when text is unreadable. */
  Rect(s, 64u, 600u, 112u, 112u, 0xFFFFu);
  if (falling) Rect(s, 224u, 600u, 112u, 112u, 0xFFFFu);
  Text(s, 32u, 800u, "VISUAL CHECK", 4u, 0xFFFFu);
  return BootScreen_DrawTestLabel(p, capacity, falling, 1u, "EDGE CARD");
}

bool BootScreen_DrawTestLabel(volatile uint16_t *p, size_t capacity,
                              bool falling, unsigned page, const char *title)
{
  return BootScreen_DrawTestLabelOriented(p, capacity, falling, page, title, DISPLAY_ROTATION_0);
}

bool BootScreen_DrawTestLabelOriented(volatile uint16_t *p, size_t capacity,
                                      bool falling, unsigned page, const char *title,
                                      DisplayRotation rotation)
{
  DisplaySurface surface;
  if (title == NULL || page == 0u || page > 99u ||
      !DisplaySurface_Init(&surface, p, capacity, rotation)) return false;
  const DisplaySurface *s = &surface;
  if (rotation != DISPLAY_ROTATION_0) {
    Rect(s, 0u, 0u, 960u, BOOT_SCREEN_LANDSCAPE_HEADER_ROWS, 0xFFFFu);
    Text(s, 12u, 8u, "H863 " H863_FIRMWARE_VERSION, 3u, 0u);
    Text(s, 640u, 8u, rotation == DISPLAY_ROTATION_CW90 ? "960 X 400 CW" : "960 X 400 CCW", 3u, 0u);
    char identity[] = {falling ? 'B' : 'A', ' ', (char)('0' + page / 10u),
                        (char)('0' + page % 10u), '\0'};
    Text(s, 12u, 36u, identity, 4u, 0u);
    Text(s, 160u, 36u, title, 3u, 0u);
    Rect(s, 0u, 76u, 960u, 4u, 0u);
    Rect(s, 832u, 32u, 32u, 32u, 0u);
    if (falling) Rect(s, 888u, 32u, 32u, 32u, 0u);
    return true;
  }
  Rect(s, 0u, 0u, 400u, BOOT_SCREEN_DIAG_HEADER_ROWS, 0xFFFFu);
  Text(s, 12u, 8u, "H863 " H863_FIRMWARE_VERSION, 3u, 0u);
  char identity[] = {falling ? 'B' : 'A', ' ', (char)('0' + page / 10u),
                      (char)('0' + page % 10u), '\0'};
  Text(s, 12u, 36u, identity, 7u, 0u);
  Text(s, 12u, 100u, title, 4u, 0u);
  Rect(s, 0u, 140u, 400u, 4u, 0u);
  Rect(s, 264u, 40u, 40u, 40u, 0u);
  if (falling) Rect(s, 336u, 40u, 40u, 40u, 0u);
  return true;
}
