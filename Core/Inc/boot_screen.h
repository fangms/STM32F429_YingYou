#ifndef BOOT_SCREEN_H
#define BOOT_SCREEN_H
#include "display_test.h"
#define BOOT_SCREEN_DIAG_HEADER_ROWS 144u
#define BOOT_SCREEN_LANDSCAPE_HEADER_ROWS 80u
/* Small shared font for diagnostic pages; clipped to the logical surface. */
bool BootScreen_DrawText(volatile uint16_t *pixels, size_t capacity_bytes,
                         unsigned x, unsigned y, const char *text, unsigned scale,
                         uint16_t color, DisplayRotation rotation);
bool BootScreen_Draw(volatile uint16_t *pixels, size_t capacity_bytes,
                     uint32_t seconds, bool scan_error);
/* Static diagnostic page, no live heartbeat implied. */
bool BootScreen_DrawDiagnostic(volatile uint16_t *pixels, size_t capacity_bytes, bool falling);
/* Overlay only the first 144 rows; body pixels remain available for measurement. */
bool BootScreen_DrawTestLabel(volatile uint16_t *pixels, size_t capacity_bytes,
                              bool falling, unsigned page, const char *title);
/* Same physical buffer, logical 960x400 for either 90-degree direction. */
bool BootScreen_DrawOriented(volatile uint16_t *pixels, size_t capacity_bytes,
                             uint32_t seconds, bool scan_error, DisplayRotation rotation);
bool BootScreen_DrawDiagnosticOriented(volatile uint16_t *pixels, size_t capacity_bytes,
                                       bool falling, DisplayRotation rotation);
bool BootScreen_DrawTestLabelOriented(volatile uint16_t *pixels, size_t capacity_bytes,
                                      bool falling, unsigned page, const char *title,
                                      DisplayRotation rotation);
#endif
