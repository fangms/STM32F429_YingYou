#include "board_screen.h"
#include "display_config.h"

bool BoardScreen_Init(BoardScreen *s, LTDC_HandleTypeDef *ltdc,
                      volatile uint16_t *frame0, volatile uint16_t *frame1)
{
  if (s == NULL || ltdc == NULL || ltdc->Instance == NULL || frame0 == NULL || frame1 == NULL ||
      (((uintptr_t)frame0 | (uintptr_t)frame1) & 1u) != 0u) return false;
  uintptr_t distance = (uintptr_t)frame0 > (uintptr_t)frame1 ?
                       (uintptr_t)frame0 - (uintptr_t)frame1 : (uintptr_t)frame1 - (uintptr_t)frame0;
  if (distance < DISPLAY_TEST_FRAME_BYTES) return false;
  *s = (BoardScreen){.ltdc = ltdc, .frames = {frame0, frame1}, .last_tick = HAL_GetTick()};
  return true;
}

bool BoardScreen_Update(BoardScreen *s, uint32_t tick)
{
  if (s == NULL || s->ltdc == NULL) return false;
  if ((s->ltdc->Instance->ISR & (LTDC_ISR_FUIF | LTDC_ISR_TERRIF)) != 0u) {
    s->scan_error = true; /* Latch: a later good frame must not conceal the error. */
    ++s->errors;
    s->ltdc->Instance->ICR = LTDC_ICR_CFUIF | LTDC_ICR_CTERRIF;
  }
  if (s->pending) {
    if ((s->ltdc->Instance->SRCR & LTDC_SRCR_VBR) != 0u) {
      s->scan_error = true;
      return false; /* Never modify either possibly active/pending buffer. */
    }
    s->front ^= 1u;
    s->pending = false;
  }
  uint32_t elapsed = (uint32_t)(tick - s->last_tick) / 1000u;
  if (elapsed == 0u) return true;
  s->last_tick += elapsed * 1000u;
  s->seconds += elapsed;
  unsigned back = s->front ^ 1u;
  if (!BootScreen_DrawOriented(s->frames[back], DISPLAY_TEST_FRAME_BYTES, s->seconds,
                               s->scan_error, (DisplayRotation)H863_DISPLAY_ROTATION)) return false;
  __DSB();
  if (HAL_LTDC_SetAddress_NoReload(s->ltdc, (uint32_t)(uintptr_t)s->frames[back], 0u) != HAL_OK) {
    s->scan_error = true;
    return false;
  }
  /* Poll VBR in main; direct reload avoids enabling an unhandled RR interrupt. */
  s->ltdc->Instance->SRCR = LTDC_SRCR_VBR;
  s->pending = true;
  return true;
}
