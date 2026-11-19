#ifndef BOARD_TEST_H
#define BOARD_TEST_H

#include "stm32f4xx_hal.h"
#include "display_surface.h"

#define BOARD_TEST_KEY_COUNT 5u
#define BOARD_TEST_DEBOUNCE_MS 30u
#define BOARD_TEST_BEEP_HZ 2000u
#define BOARD_TEST_BEEP_MS 80u
#define BOARD_TEST_BEEP_QUEUE 32u

typedef struct {
  uint32_t changed, presses;
  bool candidate, down;
} BoardTest_Key;

typedef struct {
  uint32_t requested, started, dropped, pending;
  bool ready, active;
} BoardTest_Buzzer;

typedef struct {
  BoardTest_Key keys[BOARD_TEST_KEY_COUNT];
  LTDC_HandleTypeDef *ltdc;
  volatile uint16_t *frames[2];
  size_t capacity;
  uint32_t last_log, last_draw, pending_since, last_scan, max_scan_gap;
  uint32_t display_errors, levels;
  uint16_t bridge_id;
  unsigned last_key, front;
  bool sdram_pass, bridge_pass, panel_init;
  bool attached, pending, dirty, display_failed, polled;
} BoardTest;

/* Diagnostic singleton: owns TIM7 and PD3; all other outputs remain untouched.
   Call after MX_GPIO_Init. Update from main, never from an interrupt. */
void BoardTest_Init(BoardTest *test, uint32_t tick);
void BoardTest_SetStartup(BoardTest *test, bool sdram_pass, bool bridge_pass,
                          uint16_t bridge_id, bool panel_init);
/* Requires BoardDisplayDiag_Prepare: both layers disabled, transparent cache,
   and no pending reload. Buffers must be disjoint SDRAM frames. */
bool BoardTest_AttachDisplay(BoardTest *test, LTDC_HandleTypeDef *ltdc,
                             volatile uint16_t *frame0, volatile uint16_t *frame1,
                             size_t capacity);
void BoardTest_Update(BoardTest *test, uint32_t tick);
BoardTest_Buzzer BoardTest_GetBuzzer(void);
/* Actual page renderer, also used for host preview and bounds checks. */
bool BoardTest_DrawPage(const BoardTest *test, volatile uint16_t *pixels,
                        size_t capacity, uint32_t tick);
void TIM7_IRQHandler(void);

#endif
