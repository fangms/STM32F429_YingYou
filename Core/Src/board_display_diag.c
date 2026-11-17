#include "board_display_diag.h"
#include "board_ssd2828.h"
#include "boot_screen.h"
#include "debug_log.h"

static const uint32_t backgrounds[] = {0x000000u,0xFFFFFFu,0xFF0000u,0x00FF00u,0x0000FFu};
static const DisplayTest_Pattern frame_patterns[] = {
  DISPLAY_PATTERN_BLACK, DISPLAY_PATTERN_WHITE, DISPLAY_PATTERN_RED,
  DISPLAY_PATTERN_GREEN, DISPLAY_PATTERN_BLUE, DISPLAY_PATTERN_COLOR_BARS,
  DISPLAY_PATTERN_HORIZONTAL_BARS, DISPLAY_PATTERN_CHECKERBOARD, DISPLAY_PATTERN_GEOMETRY
};

static const char *Edge(const BoardDisplayDiag *s) { return s->falling ? "FALLING" : "RISING"; }
static bool IsFrame(const BoardDisplayDiag *s) { return s->step >= 5u; }
static uint32_t Duration(const BoardDisplayDiag *s) { return s->step == 14u ? 10000u : 5000u; }

static uint32_t FrameHash(const volatile uint16_t *p)
{
  /* FNV-style halfword hash, not a hardware CRC or proof of correct display. */
  uint32_t hash = 2166136261u;
  for (size_t i = 0u; i < DISPLAY_TEST_FRAME_BYTES / 2u; ++i)
    hash = (hash ^ p[i]) * 16777619u;
  return hash;
}

static bool Fail(BoardDisplayDiag *s, const char *phase)
{
  s->phase = phase;
  s->running = s->prepared = false;
  ++s->errors;
  DebugLog_SetStage(phase);
  (void)DebugLog_Printf("[DIAG][FAIL] phase=%s edge=%s step=%u LTDC_ISR=0x%08lX errors=%lu\r\n",
                       phase, Edge(s), s->step + 1u, (unsigned long)s->last_isr,
                       (unsigned long)s->errors);
  BoardSSD2828_Shutdown();
  __HAL_LTDC_DISABLE(s->ltdc);
  return false;
}

static bool ScanOK(BoardDisplayDiag *s)
{
  uint32_t flags = s->ltdc->Instance->ISR & (LTDC_ISR_FUIF | LTDC_ISR_TERRIF);
  if (flags == 0u) return true;
  s->last_isr = s->ltdc->Instance->ISR;
  s->ltdc->Instance->ICR = LTDC_ICR_CFUIF | LTDC_ICR_CTERRIF;
  return Fail(s, "diag-ltdc-scan-error");
}

static bool Reload(BoardDisplayDiag *s)
{
  /* Never enable RRIE: the application intentionally polls, with a bound. */
  __DSB();
  s->ltdc->Instance->SRCR = LTDC_SRCR_VBR;
  for (unsigned poll = 0u; poll < 50u; ++poll) {
    if ((s->ltdc->Instance->SRCR & LTDC_SRCR_VBR) == 0u) return ScanOK(s);
    HAL_Delay(1u);
  }
  return Fail(s, "diag-vblank-timeout");
}

static bool HideFrame(BoardDisplayDiag *s, uint32_t background)
{
  __HAL_LTDC_LAYER_DISABLE(s->ltdc, 0u);
  __HAL_LTDC_LAYER_DISABLE(s->ltdc, 1u);
  s->ltdc->Instance->BCCR = background;
  return Reload(s); /* No framebuffer writes until disable is active. */
}

static bool CheckFrame(BoardDisplayDiag *s)
{
  if (!IsFrame(s)) return true; /* BG tests never dereference the framebuffer. */
  uint32_t actual = FrameHash(s->frame);
  (void)DebugLog_Printf("[DIAG][FRAME] step=%u expected=%08lX actual=%08lX unchanged=%u\r\n",
                       s->step + 1u, (unsigned long)s->frame_hash, (unsigned long)actual,
                       (unsigned)(actual == s->frame_hash));
  if (actual != s->frame_hash) return Fail(s, "diag-frame-changed");
  return ScanOK(s);
}

static bool Snapshot(BoardDisplayDiag *s)
{
  BoardSSD2828_Snapshot snapshot;
  SSD2828_Status status = BoardSSD2828_ReadSnapshot(&snapshot);
  (void)DebugLog_Printf("[DIAG][BRIDGE] edge=%s step=%u status=%u read=%u/16\r\n",
                       Edge(s), s->step + 1u, (unsigned)status, snapshot.count);
  for (unsigned i = 0u; i < snapshot.count; ++i)
    (void)DebugLog_Printf("[DIAG][REG] %02X=%04X\r\n", (unsigned)snapshot.registers[i],
                         (unsigned)snapshot.values[i]);
  if (status != SSD2828_OK) return Fail(s, "diag-bridge-snapshot");
  if (snapshot.values[0] != 0x2828u ||
      snapshot.values[6] != (s->falling ? 0x2008u : 0x0008u) ||
      snapshot.values[7] != 0x034Bu || (snapshot.values[11] & 0x0080u) == 0u)
    return Fail(s, "diag-bridge-state");
  /* C6/C7 are post-video observations. Keep raw ESR; never clear sticky evidence
     or label a zero ESR as panel acknowledgement/visual PASS. */
  LTDC_TypeDef *r = s->ltdc->Instance;
  LTDC_Layer_TypeDef *layer = LTDC_LAYER(s->ltdc, 0u);
  (void)DebugLog_Printf("[DIAG][LTDC] GCR=%08lX SSCR=%08lX BPCR=%08lX AWCR=%08lX TWCR=%08lX\r\n",
                       (unsigned long)r->GCR, (unsigned long)r->SSCR, (unsigned long)r->BPCR,
                       (unsigned long)r->AWCR, (unsigned long)r->TWCR);
  (void)DebugLog_Printf("[DIAG][LTDC] BCCR=%08lX ISR=%08lX SRCR=%08lX CPSR=%08lX CDSR=%08lX\r\n",
                       (unsigned long)r->BCCR, (unsigned long)r->ISR, (unsigned long)r->SRCR,
                       (unsigned long)r->CPSR, (unsigned long)r->CDSR);
  (void)DebugLog_Printf("[DIAG][LAYER] CR=%08lX PFCR=%08lX CFBAR=%08lX CFBLR=%08lX CFBLNR=%08lX\r\n",
                       (unsigned long)layer->CR, (unsigned long)layer->PFCR,
                       (unsigned long)layer->CFBAR, (unsigned long)layer->CFBLR,
                       (unsigned long)layer->CFBLNR);
  (void)DebugLog_Printf("[DIAG][LAYER] WHPCR=%08lX WVPCR=%08lX CACR=%08lX BFCR=%08lX\r\n",
                       (unsigned long)layer->WHPCR, (unsigned long)layer->WVPCR,
                       (unsigned long)layer->CACR, (unsigned long)layer->BFCR);
  return CheckFrame(s) && ScanOK(s);
}

static bool EnterStep(BoardDisplayDiag *s)
{
  if (!HideFrame(s, IsFrame(s) ? 0u : backgrounds[s->step])) return false;
  if (s->step == 0u) {
    BoardSSD2828_DisplayReport report;
    SSD2828_Status status = BoardSSD2828_SetSamplingEdge(s->falling, &report);
    (void)DebugLog_Printf("[DIAG][EDGE] cycle=%lu edge=%s B6=%04X status=%u phase=%s\r\n",
                         (unsigned long)s->cycle, Edge(s), s->falling ? 0x2008u : 0x0008u,
                         (unsigned)status, report.phase);
    if (status != SSD2828_OK) return Fail(s, report.phase);
  }
  const char *pattern;
  s->frame_hash = 0u;
  if (IsFrame(s)) {
    bool drawn;
    if (s->step == 14u) {
      pattern = "static-page";
      drawn = BootScreen_DrawDiagnostic(s->frame, s->capacity, s->falling);
    } else {
      DisplayTest_Pattern p = frame_patterns[s->step - 5u];
      pattern = DisplayTest_PatternName(p);
      drawn = DisplayTest_Draw(s->frame, s->capacity, p);
    }
    if (!drawn) return Fail(s, "diag-draw-arguments");
    __DSB();
    s->frame_hash = FrameHash(s->frame);
    if (HAL_LTDC_SetAddress_NoReload(s->ltdc, (uint32_t)(uintptr_t)s->frame, 0u) != HAL_OK)
      return Fail(s, "diag-frame-address");
    __HAL_LTDC_LAYER_ENABLE(s->ltdc, 0u);
    if (!Reload(s)) return false;
  } else pattern = DisplayTest_PatternName((DisplayTest_Pattern)s->step);
  s->sampled = false;
  s->phase = "display-diagnostic";
  DebugLog_SetStage(s->phase);
  (void)DebugLog_Printf("[DIAG][STEP] cycle=%lu edge=%s step=%u/15 source=%s pattern=%s hold=%lums hash=%08lX\r\n",
                       (unsigned long)s->cycle, Edge(s), s->step + 1u, IsFrame(s) ? "FB" : "BG",
                       pattern, (unsigned long)Duration(s), (unsigned long)s->frame_hash);
  s->started = HAL_GetTick(); /* Duration starts after drawing, reload and logging. */
  return true;
}

bool BoardDisplayDiag_Prepare(BoardDisplayDiag *s, LTDC_HandleTypeDef *ltdc,
                              volatile uint16_t *frame, size_t capacity)
{
  if (s == NULL || ltdc == NULL || ltdc->Instance == NULL || frame == NULL ||
      ((uintptr_t)frame & 1u) != 0u || capacity < DISPLAY_TEST_FRAME_BYTES) return false;
  *s = (BoardDisplayDiag){.ltdc = ltdc, .frame = frame, .capacity = capacity,
                          .phase = "diag-prepare", .cycle = 1u};
  /* ConfigLayer may have briefly scanned old self-test contents. Clear only
     those pre-diagnostic flags, then verify all subsequent reloads/scans. */
  ltdc->Instance->ICR = LTDC_ICR_CFUIF | LTDC_ICR_CTERRIF;
  if (!HideFrame(s, 0u)) return false;
  s->prepared = true;
  return true;
}

bool BoardDisplayDiag_Start(BoardDisplayDiag *s)
{
  if (s == NULL || !s->prepared || s->running) return false;
  s->running = true;
  (void)DebugLog_Write("[DIAG] V0.03: 15 steps/edge, 80s/edge, auto RISING/FALLING loop; visual check required\r\n");
  return EnterStep(s);
}

bool BoardDisplayDiag_Update(BoardDisplayDiag *s, uint32_t tick)
{
  if (s == NULL || !s->running) return false;
  if (!ScanOK(s)) return false;
  uint32_t elapsed = (uint32_t)(tick - s->started);
  if (!s->sampled && elapsed >= 1000u) {
    if (!Snapshot(s)) return false;
    s->sampled = true;
  }
  if (elapsed < Duration(s)) return true;
  if (!CheckFrame(s)) return false;
  if (++s->step == DISPLAY_DIAG_STEPS_PER_EDGE) {
    s->step = 0u;
    s->falling = !s->falling;
    if (!s->falling) ++s->cycle;
  }
  return EnterStep(s);
}
