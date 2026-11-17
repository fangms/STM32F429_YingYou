#include "board_ssd2828.h"
#include "main.h"
#include "debug_log.h"
#include "gc9503_panel.h"

static uint32_t cycles_per_us;
static SSD2828_Bus bus;
static bool bridge_ready, panel_released;

static bool DelayMicroseconds(void *context, uint32_t microseconds)
{
  (void)context;
  if (cycles_per_us == 0u || microseconds > 1000u) return false;
  uint32_t cycles = cycles_per_us * microseconds;
  uint32_t start = DWT->CYCCNT;
  /* A stopped DWT must not hang startup; subtraction handles counter wrap. */
  for (uint32_t polls = 0u; polls < cycles + 64u; ++polls) {
    if ((uint32_t)(DWT->CYCCNT - start) >= cycles) return true;
  }
  return false;
}

static bool InitializeTimer(void)
{
  uint32_t clock_hz = HAL_RCC_GetHCLKFreq();
  if (clock_hz < 1000000u || clock_hz > 180000000u || clock_hz % 1000000u != 0u) {
    return false;
  }
  cycles_per_us = clock_hz / 1000000u;
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  __DSB();
  __ISB();
  return DelayMicroseconds(NULL, 2u);
}

static void WriteSignal(void *context, SSD2828_Signal signal, bool high)
{
  (void)context;
  GPIO_PinState state = high ? GPIO_PIN_SET : GPIO_PIN_RESET;
  switch (signal) {
    case SSD2828_SIGNAL_CS: HAL_GPIO_WritePin(SPI_NSS_GPIO_Port, SPI_NSS_Pin, state); break;
    case SSD2828_SIGNAL_CLOCK: HAL_GPIO_WritePin(SPI_CLK_GPIO_Port, SPI_CLK_Pin, state); break;
    case SSD2828_SIGNAL_MOSI: HAL_GPIO_WritePin(SPI_MOSI_GPIO_Port, SPI_MOSI_Pin, state); break;
  }
}

static bool ReadMiso(void *context)
{
  (void)context;
  return HAL_GPIO_ReadPin(SPI_MISO_GPIO_Port, SPI_MISO_Pin) == GPIO_PIN_SET;
}

static void PowerOff(void)
{
  bridge_ready = false;
  bus.initialized = false;
  HAL_GPIO_WritePin(PMU_EN4_GPIO_Port, PMU_EN4_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(SSD2828_LCD_RESET_GPIO_Port, SSD2828_LCD_RESET_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LCD_SHUT_GPIO_Port, LCD_SHUT_Pin, GPIO_PIN_SET); /* High=shutdown. */
  if (panel_released) HAL_Delay(120u);
  panel_released = false;
  HAL_GPIO_WritePin(PMU_EN1_GPIO_Port, PMU_EN1_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(PMU_EN3_GPIO_Port, PMU_EN3_Pin, GPIO_PIN_RESET);
  HAL_Delay(1u);
  HAL_GPIO_WritePin(PMU_EN2_GPIO_Port, PMU_EN2_Pin, GPIO_PIN_RESET);
}

SSD2828_Status BoardSSD2828_Probe(SSD2828_ProbeReport *report)
{
  const SSD2828_IO io = {NULL, WriteSignal, ReadMiso, DelayMicroseconds};
  if (report == NULL) return SSD2828_INVALID_ARGUMENT;
  *report = (SSD2828_ProbeReport){0};
  report->phase = "board-prepare";
  if (__get_IPSR() != 0u || __get_PRIMASK() != 0u) return SSD2828_INVALID_ARGUMENT;
  DebugLog_SetStage("bridge-prepare");
  PowerOff();
  /* SDC is unused in PS[1:0]=01; leave its static high level alone.
     PC10 stays input until video and panel commands have completed. */
  report->phase = "dwt-timer";
  if (!InitializeTimer()) return SSD2828_TIMING_FAILED;
  SSD2828_Status status = SSD2828_BusInit(&bus, &io);
  if (status != SSD2828_OK) return status;

  DebugLog_SetStage("bridge-power-reset");
  (void)DebugLog_Write("[BRIDGE] power: 1V8 -> 1V2; panel-reset=LOW 2V8=OFF BL=OFF\r\n");
  /* Board-validation delay candidates; no power-good measurement is available.
     1V2 LDO input comes from 1V8. VDDIO is supplied by the fixed 3V3 rail. */
  HAL_GPIO_WritePin(PMU_EN2_GPIO_Port, PMU_EN2_Pin, GPIO_PIN_SET);
  HAL_Delay(10u);
  HAL_GPIO_WritePin(PMU_EN3_GPIO_Port, PMU_EN3_Pin, GPIO_PIN_SET);
  HAL_Delay(10u);
  HAL_GPIO_WritePin(LCD_SHUT_GPIO_Port, LCD_SHUT_Pin, GPIO_PIN_RESET);
  HAL_Delay(5u);
  HAL_GPIO_WritePin(SSD2828_LCD_RESET_GPIO_Port, SSD2828_LCD_RESET_Pin, GPIO_PIN_SET);
  HAL_Delay(10u);

  DebugLog_SetStage("bridge-register-probe");
  status = SSD2828_Probe(&bus, report);
  (void)DebugLog_Printf("[BRIDGE] IDs count=%u values=0x%04X/0x%04X/0x%04X\r\n",
                       (unsigned)report->id_count, (unsigned)report->id[0],
                       (unsigned)report->id[1], (unsigned)report->id[2]);
  if (report->register_test_started) {
    (void)DebugLog_Printf("[BRIDGE] VICR4 saved=0x%04X expected=0x%04X actual=0x%04X restored=%u value=0x%04X\r\n",
                         (unsigned)report->original, (unsigned)report->expected,
                         (unsigned)report->actual, (unsigned)report->register_restored,
                         (unsigned)report->restored_value);
  }
  if (status != SSD2828_OK) {
    (void)DebugLog_Printf("[BRIDGE] FAIL status=%u phase=%s; powering down\r\n",
                         (unsigned)status, report->phase);
    PowerOff();
    return status;
  }
  (void)DebugLog_Write("[PASS] SSD2828 ID x3 + register readback/restore\r\n");
  bridge_ready = true;
  return SSD2828_OK;
}

SSD2828_Status BoardSSD2828_StartPanel(bool rgb_scan_ready, BoardSSD2828_DisplayReport *report)
{
  if (report == NULL) return SSD2828_INVALID_ARGUMENT;
  *report = (BoardSSD2828_DisplayReport){.phase = "panel-prerequisites"};
  if (!rgb_scan_ready || !bridge_ready || panel_released ||
      __get_IPSR() != 0u || __get_PRIMASK() != 0u) return SSD2828_INVALID_ARGUMENT;
  SSD2828_Video video = {.bus = &bus, .delay_ms = HAL_Delay};
  DebugLog_SetStage("panel-power");
  HAL_GPIO_WritePin(PMU_EN1_GPIO_Port, PMU_EN1_Pin, GPIO_PIN_SET);
  HAL_Delay(20u); /* Both panel rails stable while RESX stays low. */
  DebugLog_SetStage("bridge-dsi-configure");
  SSD2828_Status status = SSD2828_VideoConfigure(&video);
  if (status == SSD2828_OK) {
    (void)DebugLog_Printf("[DSI] PLL=400Mbps/lane lanes=2 LP=10MHz ISR=0x%04X\r\n",
                         (unsigned)video.last_isr);
    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
    panel_released = true;
    HAL_Delay(120u);
    DebugLog_SetStage("gc9503-factory-commands");
    status = GC9503_Initialize(&video);
  }
  if (status == SSD2828_OK) {
    report->commands_sent = true;
    DebugLog_SetStage("dsi-video-enable");
    status = SSD2828_VideoEnable(&video);
  }
  report->phase = video.phase;
  report->last_isr = video.last_isr;
  report->last_command = video.last_command;
  if (status != SSD2828_OK) {
    (void)DebugLog_Printf("[PANEL] FAIL status=%u phase=%s cmd=0x%02X ISR=0x%04X\r\n",
                         (unsigned)status, report->phase, (unsigned)report->last_command,
                         (unsigned)report->last_isr);
    PowerOff();
    return status;
  }
  report->video_enabled = true;
  HAL_Delay(100u); /* Several complete frames before illumination. */
  /* Schematic 20261113: D3 cathode at LCDBL_EN, anode from radar-controlled Q1.
     Drive ONLY high: a low output would sink Q1/D3 current. T2 has 1k series,
     10k pulldown. PMU_EN4 alone switches backlight; never pull PC10 low. */
  HAL_GPIO_WritePin(LCDBL_EN_GPIO_Port, LCDBL_EN_Pin, GPIO_PIN_SET);
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = LCDBL_EN_Pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LCDBL_EN_GPIO_Port, &gpio);
  HAL_GPIO_WritePin(PMU_EN4_GPIO_Port, PMU_EN4_Pin, GPIO_PIN_SET);
  report->phase = "panel-init-sent";
  (void)DebugLog_Write("[PANEL] factory-init SENT RGB565 video=ON backlight=ON; visual check required\r\n");
  return SSD2828_OK;
}

void BoardSSD2828_Shutdown(void)
{
  PowerOff();
}

SSD2828_Status BoardSSD2828_SetSamplingEdge(bool falling, BoardSSD2828_DisplayReport *report)
{
  if (report == NULL) return SSD2828_INVALID_ARGUMENT;
  *report = (BoardSSD2828_DisplayReport){.phase = "edge-prerequisites"};
  if (!bridge_ready || !panel_released || __get_IPSR() != 0u || __get_PRIMASK() != 0u)
    return SSD2828_INVALID_ARGUMENT;
  HAL_GPIO_WritePin(PMU_EN4_GPIO_Port, PMU_EN4_Pin, GPIO_PIN_RESET);
  SSD2828_Video video = {.bus = &bus, .delay_ms = HAL_Delay, .ready = true};
  SSD2828_Status status = SSD2828_VideoSetSamplingEdge(&video, falling);
  report->phase = video.phase;
  if (status != SSD2828_OK) {
    PowerOff();
    return status;
  }
  report->commands_sent = true; /* Existing initialization, no new DCS commands. */
  report->video_enabled = true;
  HAL_Delay(100u);
  HAL_GPIO_WritePin(PMU_EN4_GPIO_Port, PMU_EN4_Pin, GPIO_PIN_SET);
  return SSD2828_OK;
}

SSD2828_Status BoardSSD2828_ReadSnapshot(BoardSSD2828_Snapshot *snapshot)
{
  if (snapshot == NULL) return SSD2828_INVALID_ARGUMENT;
  *snapshot = (BoardSSD2828_Snapshot){0};
  if (!bridge_ready || !panel_released || __get_IPSR() != 0u || __get_PRIMASK() != 0u)
    return SSD2828_INVALID_ARGUMENT;
  static const uint8_t registers[] = {0xB0u,0xB1u,0xB2u,0xB3u,0xB4u,0xB5u,0xB6u,0xB7u,
                                     0xB9u,0xBAu,0xBBu,0xC6u,0xC7u,0xD6u,0xDDu,0xDEu};
  _Static_assert(sizeof registers == BOARD_SSD2828_SNAPSHOT_COUNT, "Snapshot count");
  for (unsigned i = 0u; i < sizeof registers; ++i) {
    snapshot->registers[i] = registers[i];
    SSD2828_Status status = SSD2828_ReadRegister(&bus, registers[i], &snapshot->values[i]);
    if (status != SSD2828_OK) return status; /* count identifies partial data. */
    ++snapshot->count;
  }
  return SSD2828_OK; /* ESR is read only here; sticky evidence is never cleared. */
}
