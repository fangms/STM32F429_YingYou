#ifndef BOOT_SCREEN_H
#define BOOT_SCREEN_H
#include "display_test.h"
bool BootScreen_Draw(volatile uint16_t *pixels, size_t capacity_bytes,
                     uint32_t seconds, bool scan_error);
/* Static diagnostic page, no live heartbeat implied. */
bool BootScreen_DrawDiagnostic(volatile uint16_t *pixels, size_t capacity_bytes, bool falling);
#endif
