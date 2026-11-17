#ifndef BOARD_SCREEN_H
#define BOARD_SCREEN_H
#include "stm32f4xx_hal.h"
#include "boot_screen.h"
typedef struct {
  LTDC_HandleTypeDef *ltdc;
  volatile uint16_t *frames[2];
  uint32_t last_tick, seconds, errors;
  unsigned front;
  bool pending, scan_error;
} BoardScreen;
/* Frame0 must already be drawn and configured as active LTDC layer0. */
bool BoardScreen_Init(BoardScreen *screen, LTDC_HandleTypeDef *ltdc,
                      volatile uint16_t *frame0, volatile uint16_t *frame1);
/* Call from main every second. false means swap stalled or HAL rejected address. */
bool BoardScreen_Update(BoardScreen *screen, uint32_t tick);
#endif
