#include "board_test.h"
#include "main.h"
#include "boot_screen.h"
#include "debug_log.h"
#include "display_config.h"
#include "firmware_version.h"
#include <stdio.h>
#include <string.h>

#define BEEP_IRQ_HZ (BOARD_TEST_BEEP_HZ * 2u)
#define BEEP_TICKS (BEEP_IRQ_HZ * BOARD_TEST_BEEP_MS / 1000u)
#define BEEP_GAP_TICKS (BEEP_IRQ_HZ * 30u / 1000u)
#define FRAME_TIMEOUT_MS 100u

static const struct { GPIO_TypeDef *port; uint16_t pin; const char *name, *label; } keys[] = {
  {ON_OFF_KEY_GPIO_Port, ON_OFF_KEY_Pin, "ON_OFF", "POWER"},
  {SET_KEY_GPIO_Port, SET_KEY_Pin, "SET", "SET"},
  {UP_KEY_GPIO_Port, UP_KEY_Pin, "UP", "UP"},
  {DN_KEY_GPIO_Port, DN_KEY_Pin, "DOWN", "DOWN"},
  {OK_KEY_GPIO_Port, OK_KEY_Pin, "OK", "OK"}
};
_Static_assert(sizeof keys / sizeof keys[0] == BOARD_TEST_KEY_COUNT, "Five physical keys");
_Static_assert(H863_DISPLAY_ROTATION == DISPLAY_ROTATION_CW90, "Validated landscape direction");

static volatile struct {
  uint32_t requested, started, dropped, pending, tone_ticks, gap_ticks;
  bool ready, active, high;
} buzzer;

static void BuzzerLow(void)
{
  HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET);
  buzzer.high = false;
}

static uint32_t TimerClock(void)
{
  uint32_t divider = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
  uint32_t pclk = HAL_RCC_GetPCLK1Freq();
  /* RM0090: TIMPRE=1 selects HCLK for APB divisors 1/2/4, else 4*PCLK. */
  if ((RCC->DCKCFGR & RCC_DCKCFGR_TIMPRE) != 0u)
    return divider <= 5u ? HAL_RCC_GetHCLKFreq() : pclk * 4u;
  return divider < 4u ? pclk : pclk * 2u;
}

static void BuzzerInit(void)
{
  uint32_t saved = __get_PRIMASK();
  __disable_irq();
  HAL_NVIC_DisableIRQ(TIM7_IRQn);
  __HAL_RCC_TIM7_CLK_ENABLE();
  __HAL_RCC_TIM7_FORCE_RESET();
  __HAL_RCC_TIM7_RELEASE_RESET();
  buzzer.requested = buzzer.started = buzzer.dropped = buzzer.pending = 0u;
  buzzer.tone_ticks = buzzer.gap_ticks = 0u;
  buzzer.ready = buzzer.active = false;
  BuzzerLow();
  uint32_t hz = TimerClock();
  if (hz >= 1000000u && hz % 1000000u == 0u && hz / 1000000u <= 65536u) {
    TIM7->CR1 = TIM_CR1_URS;
    TIM7->PSC = hz / 1000000u - 1u;
    TIM7->ARR = 1000000u / BEEP_IRQ_HZ - 1u;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0u;
    TIM7->DIER = TIM_DIER_UIE;
    HAL_NVIC_ClearPendingIRQ(TIM7_IRQn);
    HAL_NVIC_SetPriority(TIM7_IRQn, 6u, 0u);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
    buzzer.ready = true;
  }
  __set_PRIMASK(saved);
  (void)DebugLog_Printf("[TEST][BEEP] TIM7 clock=%luHz ready=%u tone=%uHz duration=%ums gap=30ms queue=%u; audible result requires listening\r\n",
                       (unsigned long)hz, (unsigned)buzzer.ready,
                       BOARD_TEST_BEEP_HZ, BOARD_TEST_BEEP_MS, BOARD_TEST_BEEP_QUEUE);
}

/* Called only with interrupts masked, or by TIM7 itself. */
static void NextTone(void)
{
  --buzzer.pending;
  ++buzzer.started;
  buzzer.tone_ticks = BEEP_TICKS;
  buzzer.gap_ticks = 0u;
  buzzer.active = true;
  BuzzerLow();
}

static bool RequestTone(void)
{
  uint32_t saved = __get_PRIMASK();
  __disable_irq();
  ++buzzer.requested;
  bool accepted = buzzer.ready && buzzer.pending < BOARD_TEST_BEEP_QUEUE;
  if (accepted) {
    ++buzzer.pending;
    if (!buzzer.active) {
      NextTone();
      TIM7->CNT = 0u;
      TIM7->SR = 0u;
      TIM7->CR1 |= TIM_CR1_CEN;
    }
  } else ++buzzer.dropped;
  __set_PRIMASK(saved);
  return accepted;
}

void TIM7_IRQHandler(void)
{
  if ((TIM7->SR & TIM_SR_UIF) == 0u) return;
  TIM7->SR = 0u;
  if (buzzer.tone_ticks != 0u) {
    buzzer.high = !buzzer.high;
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, buzzer.high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    if (--buzzer.tone_ticks == 0u) {
      BuzzerLow();
      buzzer.gap_ticks = BEEP_GAP_TICKS;
    }
  } else if (buzzer.gap_ticks != 0u && --buzzer.gap_ticks == 0u) {
    if (buzzer.pending != 0u) NextTone();
    else {
      TIM7->CR1 &= ~TIM_CR1_CEN;
      buzzer.active = false;
      BuzzerLow();
    }
  }
}

BoardTest_Buzzer BoardTest_GetBuzzer(void)
{
  uint32_t saved = __get_PRIMASK();
  __disable_irq();
  BoardTest_Buzzer result = {.requested=buzzer.requested, .started=buzzer.started,
    .dropped=buzzer.dropped, .pending=buzzer.pending, .ready=buzzer.ready, .active=buzzer.active};
  __set_PRIMASK(saved);
  return result;
}

static uint32_t ReadLevels(void)
{
  /* Input bits are pad levels; output bits below are software ODR commands,
     not proof of sensor operation, supply voltage, or physical pump motion. */
  return (HAL_GPIO_ReadPin(Radar_OUT_GPIO_Port, Radar_OUT_Pin) == GPIO_PIN_SET ? 1u : 0u) |
         (HAL_GPIO_ReadPin(FLOW_IN_GPIO_Port, FLOW_IN_Pin) == GPIO_PIN_SET ? 2u : 0u) |
         ((FLOW_EN_GPIO_Port->ODR & FLOW_EN_Pin) == 0u ? 4u : 0u) |
         ((Water_pump_E_GPIO_Port->ODR & Water_pump_E_Pin) != 0u ? 8u : 0u) |
         ((Valve_EN_GPIO_Port->ODR & Valve_EN_Pin) != 0u ? 16u : 0u);
}

void BoardTest_Init(BoardTest *s, uint32_t tick)
{
  if (s == NULL) return;
  *s = (BoardTest){.last_key=BOARD_TEST_KEY_COUNT, .last_log=tick,
                   .last_draw=tick, .last_scan=tick, .levels=ReadLevels(), .dirty=true};
  for (unsigned i=0u; i<BOARD_TEST_KEY_COUNT; ++i) {
    s->keys[i].candidate = HAL_GPIO_ReadPin(keys[i].port, keys[i].pin) == GPIO_PIN_RESET;
    s->keys[i].changed = tick;
  }
  BuzzerInit();
  (void)DebugLog_Write("[TEST] " H863_FIRMWARE_VERSION " CW90 960x400 fixed KEY/BEEP page; five active-low keys, debounce=30ms; no hold repeat; pumps/valve untouched\r\n");
}

void BoardTest_SetStartup(BoardTest *s, bool sdram, bool bridge, uint16_t id, bool panel)
{
  if (s == NULL) return;
  s->sdram_pass = sdram;
  s->bridge_pass = bridge;
  s->bridge_id = id;
  s->panel_init = panel;
  s->dirty = true;
}

static void Rect(const DisplaySurface *s, unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
  if (x >= s->width || y >= s->height) return;
  if (w > s->width-x) w = s->width-x;
  if (h > s->height-y) h = s->height-y;
  for (unsigned row=y; row<y+h; ++row)
    for (unsigned col=x; col<x+w; ++col) DisplaySurface_Pixel(s,col,row,color);
}

static void Text(const DisplaySurface *s, unsigned x, unsigned y, const char *text, unsigned scale, uint16_t color)
{
  (void)BootScreen_DrawText(s->pixels, DISPLAY_NATIVE_FRAME_BYTES, x, y, text, scale, color, s->rotation);
}

bool BoardTest_DrawPage(const BoardTest *s, volatile uint16_t *pixels, size_t capacity, uint32_t tick)
{
  DisplaySurface surface;
  if (s == NULL || !DisplaySurface_Init(&surface,pixels,capacity,(DisplayRotation)H863_DISPLAY_ROTATION)) return false;
  const DisplaySurface *d = &surface;
  BoardTest_Buzzer b = BoardTest_GetBuzzer();
  for (size_t i=0u; i<DISPLAY_NATIVE_FRAME_BYTES/2u; ++i) pixels[i]=0x0841u;
  uint16_t border = s->display_errors != 0u || !b.ready || b.dropped != 0u ? 0xF800u : 0x07E0u;
  Rect(d,0u,0u,960u,4u,border); Rect(d,0u,396u,960u,4u,border);
  Rect(d,0u,0u,4u,400u,border); Rect(d,956u,0u,4u,400u,border);
  Text(d,16u,16u,"H863 " H863_FIRMWARE_VERSION "  KEY BEEP TEST",3u,0xFFFFu);
  char line[80];
  (void)snprintf(line,sizeof line,"UPTIME %lu S",(unsigned long)(tick/1000u));
  Text(d,658u,18u,line,2u,0x07FFu);
  Text(d,16u,58u,"PRESSED",2u,0xFFFFu);
  unsigned x=120u;
  for (unsigned i=0u; i<BOARD_TEST_KEY_COUNT; ++i) if (s->keys[i].down) {
    Text(d,x,58u,keys[i].label,2u,0x07E0u);
    x += ((unsigned)strlen(keys[i].label)+1u)*12u;
  }
  if (x==120u) Text(d,x,58u,"NONE",2u,0xC618u);
  for (unsigned i=0u; i<BOARD_TEST_KEY_COUNT; ++i) {
    x=16u+i*124u;
    bool down=s->keys[i].down;
    Rect(d,x,94u,116u,92u,down ? 0x07E0u : 0x2104u);
    Text(d,x+10u,108u,keys[i].label,3u,down ? 0u : 0xFFFFu);
    Text(d,x+10u,150u,down ? "DOWN" : "UP",3u,down ? 0u : 0xC618u);
    (void)snprintf(line,sizeof line,"%lu",(unsigned long)s->keys[i].presses);
    Text(d,x+6u,198u,line,strlen(line)>8u ? 1u : 2u,0x07FFu);
  }
  (void)snprintf(line,sizeof line,"LAST %s",s->last_key<BOARD_TEST_KEY_COUNT ? keys[s->last_key].label : "NONE");
  Text(d,16u,238u,line,3u,0xFFE0u);
  (void)snprintf(line,sizeof line,"BEEP REQUEST %lu",(unsigned long)b.requested);
  Text(d,16u,280u,line,2u,0xFFFFu);
  (void)snprintf(line,sizeof line,"STARTED %lu",(unsigned long)b.started);
  Text(d,330u,280u,line,2u,0x07FFu);
  (void)snprintf(line,sizeof line,"BEEP DROPPED %lu",(unsigned long)b.dropped);
  Text(d,16u,310u,line,2u,b.dropped==0u ? 0xC618u : 0xF800u);
  Text(d,16u,340u,b.ready ? "2KHZ 80MS VERIFY SOUND BY EAR" : "BEEP TIMER FAILED",2u,b.ready ? 0xFFE0u : 0xF800u);
  Rect(d,638u,52u,2u,300u,0x7BEFu);
  (void)snprintf(line,sizeof line,"SDRAM %s",s->sdram_pass ? "PASS" : "FAIL");
  Text(d,658u,58u,line,2u,s->sdram_pass ? 0x07E0u : 0xF800u);
  (void)snprintf(line,sizeof line,"BRIDGE %04X %s",s->bridge_id,s->bridge_pass ? "OK" : "FAIL");
  Text(d,658u,84u,line,2u,s->bridge_pass ? 0x07E0u : 0xF800u);
  Text(d,658u,110u,s->panel_init ? "PANEL INIT SENT" : "PANEL INIT FAIL",2u,0xFFE0u);
  (void)snprintf(line,sizeof line,"LTDC ERR %lu",(unsigned long)s->display_errors);
  Text(d,658u,136u,line,2u,s->display_errors==0u ? 0x07E0u : 0xF800u);
  (void)snprintf(line,sizeof line,"RADAR PIN %s",(s->levels&1u)!=0u ? "HIGH" : "LOW");
  Text(d,658u,174u,line,2u,0xFFFFu);
  (void)snprintf(line,sizeof line,"FLOW PIN %s",(s->levels&2u)!=0u ? "HIGH" : "LOW");
  Text(d,658u,200u,line,2u,0xFFFFu);
  (void)snprintf(line,sizeof line,"FLOW PWR CMD %s",(s->levels&4u)!=0u ? "ON" : "OFF");
  Text(d,658u,226u,line,2u,0xFFFFu);
  (void)snprintf(line,sizeof line,"PUMP CMD %s",(s->levels&8u)!=0u ? "ON" : "OFF");
  Text(d,658u,252u,line,2u,0xFFFFu);
  (void)snprintf(line,sizeof line,"VALVE CMD %s",(s->levels&16u)!=0u ? "ON" : "OFF");
  Text(d,658u,278u,line,2u,0xFFFFu);
  Text(d,658u,322u,"GPIO LEVELS ONLY",2u,0xC618u);
  Text(d,16u,374u,"SENSOR FLASH MODULE DATA NOT TESTED",2u,0xFFE0u);
  return true;
}

static void DisplayFail(BoardTest *s, const char *phase)
{
  ++s->display_errors;
  s->attached=false;
  s->display_failed=true;
  __HAL_LTDC_DISABLE(s->ltdc);
  (void)DebugLog_Printf("[TEST][DISPLAY][FAIL] phase=%s ISR=%08lX errors=%lu; key logs/beep continue\r\n",
                       phase,(unsigned long)s->ltdc->Instance->ISR,(unsigned long)s->display_errors);
}

bool BoardTest_AttachDisplay(BoardTest *s, LTDC_HandleTypeDef *ltdc,
                             volatile uint16_t *frame0, volatile uint16_t *frame1, size_t capacity)
{
  uintptr_t a=(uintptr_t)frame0, b=(uintptr_t)frame1;
  if (s==NULL || ltdc==NULL || ltdc->Instance==NULL || frame0==NULL || frame1==NULL ||
      ((a|b)&1u)!=0u || (a>b ? a-b : b-a)<DISPLAY_NATIVE_FRAME_BYTES ||
      capacity<DISPLAY_NATIVE_FRAME_BYTES || s->attached || s->display_failed ||
      (ltdc->Instance->SRCR&LTDC_SRCR_VBR)!=0u ||
      (LTDC_LAYER(ltdc,0u)->CR&LTDC_LxCR_LEN)!=0u ||
      (LTDC_LAYER(ltdc,1u)->CR&LTDC_LxCR_LEN)!=0u) return false;
  s->ltdc=ltdc;
  s->frames[0]=frame0; s->frames[1]=frame1; s->capacity=capacity;
  uint32_t now=HAL_GetTick();
  if (!BoardTest_DrawPage(s,frame0,capacity,now) ||
      HAL_LTDC_SetAddress_NoReload(ltdc,(uint32_t)(uintptr_t)frame0,0u)!=HAL_OK) {
    DisplayFail(s,"attach");
    return false;
  }
  __HAL_LTDC_LAYER_ENABLE(ltdc,0u);
  __DSB();
  ltdc->Instance->SRCR=LTDC_SRCR_VBR;
  s->front=1u; /* First pending frame is frame0; neither layer was active. */
  s->pending=s->attached=true;
  s->pending_since=s->last_draw=now;
  s->dirty=false;
  (void)DebugLog_Write("[TEST][DISPLAY] CW90 960x400 double-buffer VBlank; fixed key page, no automatic page change\r\n");
  return true;
}

static void UpdateDisplay(BoardTest *s, uint32_t tick)
{
  if (!s->attached) return;
  if ((s->ltdc->Instance->ISR&(LTDC_ISR_FUIF|LTDC_ISR_TERRIF))!=0u) {
    DisplayFail(s,"scan");
    return;
  }
  if (s->pending) {
    if ((s->ltdc->Instance->SRCR&LTDC_SRCR_VBR)!=0u) {
      if ((uint32_t)(tick-s->pending_since)>=FRAME_TIMEOUT_MS) DisplayFail(s,"vblank-timeout");
      return; /* Both possibly-owned buffers remain untouched until commit. */
    }
    s->front^=1u;
    s->pending=false;
  }
  if (!s->dirty && tick/1000u==s->last_draw/1000u) return;
  if ((uint32_t)(tick-s->last_draw)<40u) return;
  unsigned back=s->front^1u;
  if (!BoardTest_DrawPage(s,s->frames[back],s->capacity,tick) ||
      HAL_LTDC_SetAddress_NoReload(s->ltdc,(uint32_t)(uintptr_t)s->frames[back],0u)!=HAL_OK) {
    DisplayFail(s,"draw-address");
    return;
  }
  __DSB();
  s->ltdc->Instance->SRCR=LTDC_SRCR_VBR;
  s->pending=true;
  s->pending_since=s->last_draw=HAL_GetTick();
  s->dirty=false;
}

void BoardTest_Update(BoardTest *s, uint32_t tick)
{
  if (s==NULL) return;
  if (s->polled && (uint32_t)(tick-s->last_scan)>s->max_scan_gap)
    s->max_scan_gap=tick-s->last_scan;
  s->last_scan=tick; s->polled=true;
  bool raw[BOARD_TEST_KEY_COUNT];
  for (unsigned i=0u;i<BOARD_TEST_KEY_COUNT;++i)
    raw[i]=HAL_GPIO_ReadPin(keys[i].port,keys[i].pin)==GPIO_PIN_RESET;
  for (unsigned i=0u;i<BOARD_TEST_KEY_COUNT;++i) {
    BoardTest_Key *k=&s->keys[i];
    if (raw[i]!=k->candidate) { k->candidate=raw[i]; k->changed=tick; }
    if (k->down!=k->candidate && (uint32_t)(tick-k->changed)>=BOARD_TEST_DEBOUNCE_MS) {
      k->down=k->candidate;
      s->dirty=true;
      if (k->down) {
        ++k->presses;
        s->last_key=i;
        bool accepted=RequestTone();
        (void)DebugLog_Printf("[KEY] name=%s state=DOWN count=%lu tick=%lu beep=%s\r\n",
                             keys[i].name,(unsigned long)k->presses,(unsigned long)tick,
                             accepted ? "QUEUED" : "DROPPED");
      } else {
        (void)DebugLog_Printf("[KEY] name=%s state=UP count=%lu tick=%lu\r\n",
                             keys[i].name,(unsigned long)k->presses,(unsigned long)tick);
      }
    }
  }
  uint32_t levels=ReadLevels();
  if (levels!=s->levels) { s->levels=levels; s->dirty=true; }
  UpdateDisplay(s,HAL_GetTick());
  tick=HAL_GetTick();
  if ((uint32_t)(tick-s->last_log)>=1000u) {
    s->last_log=tick;
    BoardTest_Buzzer b=BoardTest_GetBuzzer();
    unsigned mask=0u;
    for (unsigned i=0u;i<BOARD_TEST_KEY_COUNT;++i) if (s->keys[i].down) mask|=1u<<i;
    (void)DebugLog_Printf("[ALIVE] tick=%lu TEST=KEY-BEEP SCREEN=%s SDRAM=%s BRIDGE=%s PANEL=%s KEYS=%02X BEEP_REQ=%lu START=%lu DROP=%lu LTDC_ERRORS=%lu GPIO=%02lX scan_gap_max=%lums\r\n",
                         (unsigned long)tick,s->attached ? "RUN" : "OFF",
                         s->sdram_pass ? "PASS" : "FAIL",s->bridge_pass ? "PASS" : "FAIL",
                         s->panel_init ? "INIT-SENT" : "FAIL",mask,
                         (unsigned long)b.requested,(unsigned long)b.started,(unsigned long)b.dropped,
                         (unsigned long)s->display_errors,(unsigned long)s->levels,
                         (unsigned long)s->max_scan_gap);
  }
}
