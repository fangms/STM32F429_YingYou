#include "board_display_diag.h"
#include "board_ssd2828.h"
#include "boot_screen.h"
#include "debug_log.h"
#include "firmware_version.h"
#include "display_config.h"

static const DisplayTest_Pattern frame_patterns[] = {
  DISPLAY_PATTERN_BLACK, DISPLAY_PATTERN_WHITE, DISPLAY_PATTERN_WHITE,
  DISPLAY_PATTERN_COLOR_BARS, DISPLAY_PATTERN_CHECKERBOARD,
  DISPLAY_PATTERN_GRAY16, DISPLAY_PATTERN_VERTICAL1, DISPLAY_PATTERN_GEOMETRY
};
static const char *const titles[] = {
  "FIXED " H863_DISPLAY_EDGE_NAME, "FB RAW WHITE", "BG RAW WHITE", "COLOR BARS",
  "CHECKER 32", "GRAY 16", "VERTICAL 1", "FULL GEOMETRY"
};
_Static_assert(sizeof frame_patterns / sizeof frame_patterns[0] == DISPLAY_DIAG_STEPS_PER_CYCLE,
               "Diagnostic pattern count");
_Static_assert(sizeof titles / sizeof titles[0] == DISPLAY_DIAG_STEPS_PER_CYCLE, "Diagnostic titles");

static const char *Edge(const BoardDisplayDiag *s) { return s->falling ? "FALLING" : "RISING"; }
static bool IsFrame(const BoardDisplayDiag *s) { return !s->bist_active && s->step != DISPLAY_DIAG_BG_STEP; }

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
  for (unsigned i = 0u; i < 2u; ++i) {
    __HAL_LTDC_LAYER_DISABLE(s->ltdc, i);
    /* SetAddress_NoReload calls LTDC_SetConfig and replays this HAL cache.
       Keep it in sync: register-only writes are otherwise restored to 0405. */
    LTDC_LayerCfgTypeDef *config = &s->ltdc->LayerCfg[i];
    config->Alpha0 = 0u;
    config->Backcolor = (LTDC_ColorTypeDef){0};
    config->BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA;
    config->BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA;
    /* RM0090 Rev22 16.4.2: disabled layers still blend their default color.
       CA=255 / BFCR=0405 masks BCCR with default black even with LEN=0.
       Transparent default + pixel-alpha blending passes the background;
       enabled RGB565 pixels have alpha=255 and remain fully opaque. */
    LTDC_LAYER(s->ltdc, i)->DCCR = 0u;
    LTDC_LAYER(s->ltdc, i)->BFCR = config->BlendingFactor1 | config->BlendingFactor2;
  }
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
  (void)DebugLog_Printf("[DIAG][BRIDGE] source=%s edge=%s step=%u status=%u read=%u/%u\r\n",
                       s->bist_active ? "BRIDGE-BIST" : "RGB", Edge(s), s->step + 1u,
                       (unsigned)status, snapshot.count, (unsigned)BOARD_SSD2828_SNAPSHOT_COUNT);
  for (unsigned i = 0u; i < snapshot.count; ++i)
    (void)DebugLog_Printf("[DIAG][REG] %02X=%04X\r\n", (unsigned)snapshot.registers[i],
                         (unsigned)snapshot.values[i]);
  if (status != SSD2828_OK) return Fail(s, "diag-bridge-snapshot");
  if (snapshot.values[0] != 0x2828u ||
      s->falling != (H863_DISPLAY_SAMPLING_FALLING != 0) ||
      snapshot.values[6] != H863_DISPLAY_VICR6 ||
      snapshot.values[7] != 0x034Bu || (snapshot.values[11] & 0x0080u) == 0u ||
      (snapshot.values[16] & 0x0600u) != (s->bist_active ? 0x0600u : 0u))
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
  (void)DebugLog_Printf("[DIAG][LAYER] WHPCR=%08lX WVPCR=%08lX CACR=%08lX BFCR=%08lX DCCR=%08lX\r\n",
                       (unsigned long)layer->WHPCR, (unsigned long)layer->WVPCR,
                       (unsigned long)layer->CACR, (unsigned long)layer->BFCR, (unsigned long)layer->DCCR);
  LTDC_Layer_TypeDef *second = LTDC_LAYER(s->ltdc, 1u);
  (void)DebugLog_Printf("[DIAG][LAYER2] CR=%08lX CACR=%08lX BFCR=%08lX DCCR=%08lX\r\n",
                       (unsigned long)second->CR, (unsigned long)second->CACR,
                       (unsigned long)second->BFCR, (unsigned long)second->DCCR);
  if (layer->BFCR != 0x0607u || second->BFCR != 0x0607u ||
      layer->DCCR != 0u || second->DCCR != 0u || (second->CR & LTDC_LxCR_LEN) != 0u ||
      ((layer->CR & LTDC_LxCR_LEN) != 0u) != IsFrame(s))
    return Fail(s, "diag-layer-state");
  return CheckFrame(s) && ScanOK(s);
}

static bool EnterStep(BoardDisplayDiag *s)
{
  if (!HideFrame(s, IsFrame(s) ? 0u : 0xFFFFFFu)) return false;
  const char *pattern;
  s->frame_hash = 0u;
  if (IsFrame(s)) {
    bool drawn;
    if (s->step == 0u) {
      pattern = H863_DISPLAY_CARD_PATTERN;
      drawn = BootScreen_DrawDiagnosticOriented(s->frame, s->capacity, s->falling,
                                                (DisplayRotation)H863_DISPLAY_ROTATION);
    } else {
      DisplayTest_Pattern p = frame_patterns[s->step];
      pattern = DisplayTest_PatternName(p);
      drawn = DisplayTest_DrawOriented(s->frame, s->capacity, p,
                                        (DisplayRotation)H863_DISPLAY_ROTATION);
    }
    /* Raw white matches BCCR white pixel-for-pixel; full geometry retains all
       four physical corners. Neither page may be obscured by an identity bar. */
    bool label = s->step != DISPLAY_DIAG_FB_WHITE_STEP &&
                 s->step != DISPLAY_DIAG_GEOMETRY_STEP;
    if (!drawn || (label && !BootScreen_DrawTestLabelOriented(s->frame, s->capacity, s->falling,
                          s->step + 1u, titles[s->step], (DisplayRotation)H863_DISPLAY_ROTATION)))
      return Fail(s, "diag-draw-arguments");
    __DSB();
    s->frame_hash = FrameHash(s->frame);
    if (HAL_LTDC_SetAddress_NoReload(s->ltdc, (uint32_t)(uintptr_t)s->frame, 0u) != HAL_OK)
      return Fail(s, "diag-frame-address");
    __HAL_LTDC_LAYER_ENABLE(s->ltdc, 0u);
    if (!Reload(s)) return false;
  } else pattern = "white"; /* No framebuffer read/write or overlay for BG. */
  s->sampled = false;
  s->phase = "display-diagnostic";
  DebugLog_SetStage(s->phase);
  (void)DebugLog_Printf("[DIAG][STEP] cycle=%lu edge=%s step=%u/%u source=%s pattern=%s hold=%lums hash=%08lX\r\n",
                       (unsigned long)s->cycle, Edge(s), s->step + 1u,
                       (unsigned)DISPLAY_DIAG_STEPS_PER_CYCLE, IsFrame(s) ? "FB" : "BG",
                       pattern, (unsigned long)DISPLAY_DIAG_HOLD_MS, (unsigned long)s->frame_hash);
  s->started = HAL_GetTick(); /* Duration starts after drawing, reload and logging. */
  return true;
}

bool BoardDisplayDiag_Prepare(BoardDisplayDiag *s, LTDC_HandleTypeDef *ltdc,
                              volatile uint16_t *frame, size_t capacity)
{
  if (s == NULL || ltdc == NULL || ltdc->Instance == NULL || frame == NULL ||
      ((uintptr_t)frame & 1u) != 0u || capacity < DISPLAY_TEST_FRAME_BYTES) return false;
  *s = (BoardDisplayDiag){.ltdc = ltdc, .frame = frame, .capacity = capacity,
                          .phase = "diag-prepare", .cycle = 1u,
                          .falling = H863_DISPLAY_SAMPLING_FALLING != 0};
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
  (void)DebugLog_Write("[DIAG] " H863_FIRMWARE_VERSION ": " H863_DISPLAY_ORIENTATION_NAME "; FIXED " H863_DISPLAY_FIXED_LABEL "; 8 pages x4s; continuous VEN; no auto BIST/edge switch; visual check required\r\n");
  return EnterStep(s);
}

bool BoardDisplayDiag_StartBridgeBIST(BoardDisplayDiag *s)
{
  if (s == NULL || !s->prepared || s->running) return false;
  s->running = true;
  s->bist_active = true;
  if (!HideFrame(s, 0u)) return false;
  BoardSSD2828_DisplayReport report;
  SSD2828_Status status = BoardSSD2828_SetBIST(true, &report);
  if (status != SSD2828_OK) return Fail(s, report.phase);
  s->phase = "bridge-internal-bist";
  DebugLog_SetStage(s->phase);
  (void)DebugLog_Write("[DIAG][BIST] START source=SSD2828-INTERNAL EE[10:9]=11 hold=30000ms; auto red/green/blue/black/white; no text; visual check required\r\n");
  s->started = HAL_GetTick();
  s->sampled = false;
  return true;
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
  if (s->bist_active) {
    if (elapsed < DISPLAY_DIAG_BIST_HOLD_MS) return true;
    BoardSSD2828_DisplayReport report;
    SSD2828_Status status = BoardSSD2828_SetBIST(false, &report);
    if (status != SSD2828_OK) return Fail(s, report.phase);
    s->bist_active = false;
    (void)DebugLog_Write("[DIAG][BIST] END EE[10:9]=00; return to RGB " H863_DISPLAY_FIXED_LABEL " page 1\r\n");
    s->step = 0u;
    s->falling = H863_DISPLAY_SAMPLING_FALLING != 0;
    return EnterStep(s);
  }
  if (elapsed < DISPLAY_DIAG_HOLD_MS) return true;
  if (!CheckFrame(s)) return false;
  if (++s->step == DISPLAY_DIAG_STEPS_PER_CYCLE) {
    s->step = 0u;
    ++s->cycle;
  }
  return EnterStep(s);
}
