#ifndef BOARD_DISPLAY_DIAG_H
#define BOARD_DISPLAY_DIAG_H

#include "stm32f4xx_hal.h"
#include "display_test.h"

#define DISPLAY_DIAG_STEPS_PER_CYCLE 8u
#define DISPLAY_DIAG_HOLD_MS 4000u
#define DISPLAY_DIAG_FB_WHITE_STEP 1u
#define DISPLAY_DIAG_BG_STEP 2u /* zero based: page 3, no framebuffer or label */
#define DISPLAY_DIAG_GEOMETRY_STEP 7u
#define DISPLAY_DIAG_BIST_HOLD_MS 30000u

typedef struct {
  LTDC_HandleTypeDef *ltdc;
  volatile uint16_t *frame;
  size_t capacity;
  uint32_t started, cycle, errors, frame_hash, last_isr;
  unsigned step;
  const char *phase;
  bool prepared, running, falling, sampled, bist_active;
} BoardDisplayDiag;

/* Call Prepare after MX_LTDC_Init, before panel/backlight startup. */
bool BoardDisplayDiag_Prepare(BoardDisplayDiag *diag, LTDC_HandleTypeDef *ltdc,
                              volatile uint16_t *frame, size_t capacity);
/* Call Start only after panel initialization succeeded. Update from main. */
bool BoardDisplayDiag_Start(BoardDisplayDiag *diag);
/* Optional standalone diagnostic; normal boot starts fixed RGB directly. */
bool BoardDisplayDiag_StartBridgeBIST(BoardDisplayDiag *diag);
bool BoardDisplayDiag_Update(BoardDisplayDiag *diag, uint32_t tick);

#endif
