#include "board_sdram.h"
#include "debug_log.h"
#include <stdbool.h>
#include <string.h>

#define SDRAM_COMMAND_TIMEOUT_MS 100u
#define SDRAM_SUPPORTED_HCLK_HZ 180000000u
#define SDRAM_MASK_PINS (GPIO_PIN_0 | GPIO_PIN_1)
#define SDRAM_REFRESH_MARGIN_CYCLES 20u
#define SDRAM_MODE_BURST_LENGTH_1 0x0000u
#define SDRAM_MODE_SEQUENTIAL 0x0000u
#define SDRAM_MODE_CAS_3 0x0030u
#define SDRAM_MODE_STANDARD 0x0000u
#define SDRAM_MODE_SINGLE_WRITE 0x0200u
#define SDRAM_MODE_REGISTER (SDRAM_MODE_BURST_LENGTH_1 | SDRAM_MODE_SEQUENTIAL | \
                             SDRAM_MODE_CAS_3 | SDRAM_MODE_STANDARD | SDRAM_MODE_SINGLE_WRITE)

static void SetPhase(BoardSDRAM_Report *report, const char *phase)
{
  report->phase = phase;
  DebugLog_SetStage(phase);
  (void)DebugLog_Printf("[SDRAM] %s\r\n", phase);
}

static BoardSDRAM_Status Mismatch(BoardSDRAM_Report *report,
                                  volatile uint16_t *address,
                                  uint16_t expected, uint16_t actual)
{
  report->failed_address = (uintptr_t)address;
  report->expected = expected;
  report->actual = actual;
  return BOARD_SDRAM_MEMORY_MISMATCH;
}

uint32_t BoardSDRAM_CalculateRefreshCount(uint32_t sdclock_hz)
{
  uint64_t cycles = (uint64_t)sdclock_hz * BOARD_SDRAM_REFRESH_WINDOW_US /
                    ((uint64_t)BOARD_SDRAM_REFRESH_ROWS * 1000000u);
  if (cycles <= SDRAM_REFRESH_MARGIN_CYCLES ||
      cycles - SDRAM_REFRESH_MARGIN_CYCLES > 0x1FFFu) {
    return 0u;
  }
  return (uint32_t)cycles - SDRAM_REFRESH_MARGIN_CYCLES;
}

static HAL_StatusTypeDef SendCommand(SDRAM_HandleTypeDef *sdram,
                                     uint32_t mode, uint32_t refreshes,
                                     uint32_t mode_register)
{
  uint32_t start = HAL_GetTick();
  FMC_SDRAM_CommandTypeDef command = {0};

  while (READ_BIT(sdram->Instance->SDSR, FMC_SDSR_BUSY) != 0u) {
    if ((uint32_t)(HAL_GetTick() - start) >= SDRAM_COMMAND_TIMEOUT_MS) {
      return HAL_TIMEOUT;
    }
  }
  command.CommandMode = mode;
  command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
  command.AutoRefreshNumber = refreshes;
  command.ModeRegisterDefinition = mode_register;
  /* HAL_SDRAM_SendCommand in FW_F4 V1.28.3 discards the LL timeout result.
     This boot-only driver uses LL directly so failures propagate unchanged. */
  return FMC_SDRAM_SendCommand(sdram->Instance, &command, SDRAM_COMMAND_TIMEOUT_MS);
}

BoardSDRAM_Status BoardSDRAM_Initialize(SDRAM_HandleTypeDef *sdram, BoardSDRAM_Report *report)
{
  if (report == NULL) {
    return BOARD_SDRAM_INVALID_CONFIG;
  }
  *report = (BoardSDRAM_Report){0};
  report->hal_status = HAL_OK;
  SetPhase(report, "validate-config");
  if (sdram == NULL || sdram->Instance != FMC_SDRAM_DEVICE ||
      sdram->State != HAL_SDRAM_STATE_READY ||
      sdram->Init.SDBank != FMC_SDRAM_BANK1 ||
      sdram->Init.ColumnBitsNumber != FMC_SDRAM_COLUMN_BITS_NUM_9 ||
      sdram->Init.RowBitsNumber != FMC_SDRAM_ROW_BITS_NUM_13 ||
      sdram->Init.MemoryDataWidth != FMC_SDRAM_MEM_BUS_WIDTH_16 ||
      sdram->Init.InternalBankNumber != FMC_SDRAM_INTERN_BANKS_NUM_4 ||
      sdram->Init.CASLatency != FMC_SDRAM_CAS_LATENCY_3 ||
      (sdram->Init.SDClockPeriod != FMC_SDRAM_CLOCK_PERIOD_2 &&
       sdram->Init.SDClockPeriod != FMC_SDRAM_CLOCK_PERIOD_3)) {
    return BOARD_SDRAM_INVALID_CONFIG;
  }
  uint32_t divider = sdram->Init.SDClockPeriod == FMC_SDRAM_CLOCK_PERIOD_2 ? 2u : 3u;
  report->sdclock_hz = HAL_RCC_GetHCLKFreq() / divider;
  report->refresh_count = BoardSDRAM_CalculateRefreshCount(report->sdclock_hz);
  report->mode_register = SDRAM_MODE_REGISTER;
  /* Timings satisfy W9825G6KH A04 -6I minima at 90MHz; unchanged cycle
     counts are conservative at the diagnostic 60MHz setting. */
  if (HAL_RCC_GetHCLKFreq() != SDRAM_SUPPORTED_HCLK_HZ || report->refresh_count == 0u) {
    return BOARD_SDRAM_INVALID_CONFIG;
  }
  (void)DebugLog_Printf("[SDRAM] clock=%luHz refresh=%lu mode=0x%04lX\r\n",
                       (unsigned long)report->sdclock_hz,
                       (unsigned long)report->refresh_count,
                       (unsigned long)report->mode_register);

  SetPhase(report, "clock-enable");
  report->hal_status = SendCommand(sdram, FMC_SDRAM_CMD_CLK_ENABLE, 1u, 0u);
  if (report->hal_status != HAL_OK) return BOARD_SDRAM_COMMAND_FAILED;
  HAL_Delay(1u); /* Conservatively exceed the power-up settling interval. */

  SetPhase(report, "precharge-all");
  report->hal_status = SendCommand(sdram, FMC_SDRAM_CMD_PALL, 1u, 0u);
  if (report->hal_status != HAL_OK) return BOARD_SDRAM_COMMAND_FAILED;

  SetPhase(report, "auto-refresh-8");
  report->hal_status = SendCommand(sdram, FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8u, 0u);
  if (report->hal_status != HAL_OK) return BOARD_SDRAM_COMMAND_FAILED;

  SetPhase(report, "load-mode");
  report->hal_status = SendCommand(sdram, FMC_SDRAM_CMD_LOAD_MODE, 1u, SDRAM_MODE_REGISTER);
  if (report->hal_status != HAL_OK) return BOARD_SDRAM_COMMAND_FAILED;

  SetPhase(report, "program-refresh");
  report->hal_status = HAL_SDRAM_ProgramRefreshRate(sdram, report->refresh_count);
  if (report->hal_status != HAL_OK) return BOARD_SDRAM_REFRESH_FAILED;
  __DSB();
  return BOARD_SDRAM_OK;
}

static uint16_t AddressPattern(size_t index, uint16_t inversion)
{
  return (uint16_t)(index ^ (index >> 16u) ^ inversion);
}

static BoardSDRAM_Status FastByteSequence(volatile uint16_t *address, BoardSDRAM_Report *report)
{
  volatile uint8_t *bytes = (volatile uint8_t *)address;
  uint16_t low, high;
  SetPhase(report, "byte-fast");
  /* Retain v0.3's immediate halfword-to-byte write sequence. The extra seed
     read in the detailed probes below must not hide the original symptom. */
  *address = 0x5A5Au;
  __DSB();
  bytes[0] = 0xA5u;
  __DSB();
  low = *address;
  bytes[1] = 0x3Cu;
  __DSB();
  high = *address;
  (void)DebugLog_Printf("[SDRAM][FAST] addr=0x%08lX first=0x%04X second=0x%04X\r\n",
                       (unsigned long)(uintptr_t)address, (unsigned)low, (unsigned)high);
  if (low != 0x5AA5u || high != 0x3CA5u) {
    SetPhase(report, low != 0x5AA5u ? "byte-fast-low" : "byte-fast-high");
    return Mismatch(report, address, low != 0x5AA5u ? 0x5AA5u : 0x3CA5u,
                     low != 0x5AA5u ? low : high);
  }
  return BOARD_SDRAM_OK;
}

static BoardSDRAM_Status ByteSequence(volatile uint16_t *address, uint16_t seed,
                                      unsigned first_lane, BoardSDRAM_Report *report)
{
  volatile uint8_t *bytes = (volatile uint8_t *)address;
  uint16_t observed[3];
  uint16_t expected[3];
  unsigned second_lane = first_lane ^ 1u;
  expected[0] = seed;
  expected[1] = (uint16_t)((seed & (first_lane == 0u ? 0xFF00u : 0x00FFu)) |
                          (0xA5u << (8u * first_lane)));
  expected[2] = (uint16_t)((expected[1] & (second_lane == 0u ? 0xFF00u : 0x00FFu)) |
                          (0x3Cu << (8u * second_lane)));

  /* Capture the whole sequence before printing: UART delays must not hide a
     write/read turnaround issue. Keep actual 8-bit stores in both directions. */
  *address = seed;
  __DSB();
  observed[0] = *address;
  bytes[first_lane] = 0xA5u;
  __DSB();
  observed[1] = *address;
  bytes[second_lane] = 0x3Cu;
  __DSB();
  observed[2] = *address;
  (void)DebugLog_Printf("[SDRAM][BYTE] addr=0x%08lX order=%s seed=0x%04X read16=0x%04X first=0x%04X second=0x%04X\r\n",
                       (unsigned long)(uintptr_t)address, first_lane == 0u ? "L-H" : "H-L",
                       (unsigned)seed, (unsigned)observed[0],
                       (unsigned)observed[1], (unsigned)observed[2]);
  for (unsigned i = 0u; i < 3u; ++i) {
    if (observed[i] != expected[i]) {
      const char *phase = i == 0u ? "byte-seed" :
                           ((i == 1u ? first_lane : second_lane) == 0u ? "byte-low" : "byte-high");
      SetPhase(report, phase);
      return Mismatch(report, address, expected[i], observed[i]);
    }
  }
  return BOARD_SDRAM_OK;
}

static BoardSDRAM_Status TestByteLanes(volatile uint16_t *memory, size_t size_bytes,
                                       BoardSDRAM_Report *report, bool collect_all)
{
  static const uint16_t seeds[] = {0x5A5Au, 0xA5A5u, 0x00FFu, 0xFF00u};
  size_t words = size_bytes / sizeof(uint16_t);
  const size_t offsets[] = {0u, 1u, words / 2u, words - 1u};
  unsigned failures = 0u;
  BoardSDRAM_Report first_failure = {0};
  if (FastByteSequence(memory, report) != BOARD_SDRAM_OK) {
    if (!collect_all) return BOARD_SDRAM_MEMORY_MISMATCH;
    first_failure = *report;
    ++failures;
  }
  SetPhase(report, "byte-lanes");
  for (unsigned location = 0u; location < 4u; ++location) {
    for (unsigned seed = 0u; seed < 4u; ++seed) {
      for (unsigned lane = 0u; lane < 2u; ++lane) {
        BoardSDRAM_Status status = ByteSequence(memory + offsets[location], seeds[seed], lane, report);
        if (status != BOARD_SDRAM_OK) {
          if (!collect_all) return status;
          if (failures == 0u) first_failure = *report;
          ++failures;
        }
      }
    }
  }
  (void)DebugLog_Printf("[SDRAM][BYTE] sequences=33 failed=%u\r\n", failures);
  if (failures != 0u) {
    *report = first_failure;
    return BOARD_SDRAM_MEMORY_MISMATCH;
  }
  return BOARD_SDRAM_OK;
}

static void LogController(const SDRAM_HandleTypeDef *sdram)
{
  (void)DebugLog_Printf("[SDRAM][REG] SDCR1=0x%08lX SDTR1=0x%08lX SDRTR=0x%08lX SDSR=0x%08lX\r\n",
                       (unsigned long)sdram->Instance->SDCR[0],
                       (unsigned long)sdram->Instance->SDTR[0],
                       (unsigned long)sdram->Instance->SDRTR,
                       (unsigned long)sdram->Instance->SDSR);
}

static BoardSDRAM_Status TestMemory16(volatile uint16_t *memory, size_t size_bytes,
                                       BoardSDRAM_Report *report, bool include_bytes);

typedef struct {
  uint32_t mode, type, speed, pull, alternate, output;
} MaskPinState;

static void SetMaskPins(uint32_t mask)
{
  /* No memory transaction occurs between the two GPIO writes. */
  HAL_GPIO_WritePin(GPIOE, SDRAM_MASK_PINS, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOE, (uint16_t)(mask & SDRAM_MASK_PINS), GPIO_PIN_SET);
  __DSB();
  /* Both write setup and the two-cycle DQM read latency are exceeded. */
  HAL_Delay(1u);
}

static bool Precharge(SDRAM_HandleTypeDef *sdram, const char *step)
{
  HAL_StatusTypeDef status = SendCommand(sdram, FMC_SDRAM_CMD_PALL, 1u, 0u);
  if (status != HAL_OK) {
    (void)DebugLog_Printf("[SDRAM][CONTROL] step=%s PALL HAL=%u; abort profile\r\n",
                         step, (unsigned)status);
    return false;
  }
  return true;
}

static bool TestStaticMasks(SDRAM_HandleTypeDef *sdram, volatile uint16_t *memory,
                            size_t size_bytes, uint32_t clock_hz)
{
  const MaskPinState saved = {GPIOE->MODER, GPIOE->OTYPER, GPIOE->OSPEEDR,
                              GPIOE->PUPDR, GPIOE->AFR[0], GPIOE->ODR};
  GPIO_InitTypeDef gpio = {0};
  size_t words = size_bytes / sizeof(uint16_t);
  const size_t offsets[] = {0u, 1u, words / 2u, words - 1u};
  unsigned failed = 0u, pin_failed = 0u, completed = 0u;
  bool control_ok = true;
  DebugLog_SetStage("diag-mask-enter");
  if (!Precharge(sdram, "mask-enter")) return false;
  HAL_GPIO_WritePin(GPIOE, SDRAM_MASK_PINS, GPIO_PIN_RESET);
  gpio.Pin = SDRAM_MASK_PINS;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOE, &gpio);
  __DSB();
  HAL_Delay(1u);
  (void)DebugLog_Printf("[SDRAM][MASK] begin clock=%luHz LDQM=PE0 UDQM=PE1; 1=masked\r\n",
                       (unsigned long)clock_hz);
  for (unsigned location = 0u; location < 4u; ++location) {
    for (uint32_t mask = 0u; mask < 4u; ++mask) {
      volatile uint16_t *address = memory + offsets[location];
      uint16_t expected = (uint16_t)((mask & 1u ? 0x0034u : 0x00CDu) |
                                     (mask & 2u ? 0x1200u : 0xAB00u));
      DebugLog_SetStage("diag-mask-prepare");
      SetMaskPins(0u);
      DebugLog_SetStage("diag-mask-seed");
      *address = 0x1234u;
      __DSB();
      if (!Precharge(sdram, "mask-seed-drain")) { control_ok = false; goto cleanup; }
      uint16_t seed = *address;
      if (!Precharge(sdram, "mask-seed-read")) { control_ok = false; goto cleanup; }
      if (seed != 0x1234u) {
        ++failed;
        (void)DebugLog_Printf("[SDRAM][MASK] seed FAIL addr=0x%08lX actual=0x%04X; case skipped\r\n",
                             (unsigned long)(uintptr_t)address, (unsigned)seed);
        continue;
      }
      DebugLog_SetStage("diag-mask-set");
      SetMaskPins(mask);
      uint32_t output = GPIOE->ODR & SDRAM_MASK_PINS;
      uint32_t input = GPIOE->IDR & SDRAM_MASK_PINS;
      if (input != mask || output != mask) ++pin_failed;
      DebugLog_SetStage("diag-mask-write");
      *address = 0xABCDu; /* Always STRH: GPIO alone supplies byte masks. */
      __DSB();
      DebugLog_SetStage("diag-mask-drain");
      if (!Precharge(sdram, "mask-write-drain")) { control_ok = false; goto cleanup; }
      SetMaskPins(0u);
      uint16_t actual = *address;
      if (!Precharge(sdram, "mask-result-read")) { control_ok = false; goto cleanup; }
      ++completed;
      if (actual != expected) ++failed;
      (void)DebugLog_Printf("[SDRAM][MASK] addr=0x%08lX mask=%lu ODR=%lu IDR=%lu expected=0x%04X actual=0x%04X %s\r\n",
                           (unsigned long)(uintptr_t)address, (unsigned long)mask,
                           (unsigned long)output, (unsigned long)input,
                           (unsigned)expected, (unsigned)actual, actual == expected ? "PASS" : "FAIL");
    }
  }
cleanup:
  DebugLog_SetStage("diag-mask-restore");
  SetMaskPins(0u);
  if (!Precharge(sdram, "mask-restore")) control_ok = false;
  /* Even after a controller timeout, release GPIO ownership back to FMC.
     Such a profile is aborted, and the caller must not use the tested memory. */
  MODIFY_REG(GPIOE->AFR[0], 0xFFu, saved.alternate & 0xFFu);
  MODIFY_REG(GPIOE->OTYPER, 3u, saved.type & 3u);
  MODIFY_REG(GPIOE->PUPDR, 0xFu, saved.pull & 0xFu);
  MODIFY_REG(GPIOE->OSPEEDR, 0xFu, saved.speed & 0xFu);
  MODIFY_REG(GPIOE->MODER, 0xFu, saved.mode & 0xFu);
  HAL_GPIO_WritePin(GPIOE, SDRAM_MASK_PINS, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOE, (uint16_t)(saved.output & SDRAM_MASK_PINS), GPIO_PIN_SET);
  __DSB();
  bool restored = ((GPIOE->MODER ^ saved.mode) & 0xFu) == 0u &&
                  ((GPIOE->OTYPER ^ saved.type) & 3u) == 0u &&
                  ((GPIOE->OSPEEDR ^ saved.speed) & 0xFu) == 0u &&
                  ((GPIOE->PUPDR ^ saved.pull) & 0xFu) == 0u &&
                  ((GPIOE->AFR[0] ^ saved.alternate) & 0xFFu) == 0u &&
                  ((GPIOE->ODR ^ saved.output) & 3u) == 0u;
  (void)DebugLog_Printf("[SDRAM][MASK] end completed=%u/16 failed=%u pin-failed=%u restored=%u control=%s\r\n",
                       completed, failed, pin_failed, restored ? 1u : 0u, control_ok ? "OK" : "FAIL");
  return control_ok && restored;
}

static uint32_t WordPattern(size_t index)
{
  return (uint32_t)index * 0x9E3779B1u ^ 0xA55AA55Au;
}

static bool WordMismatch(volatile uint32_t *address, uint32_t expected, uint32_t actual)
{
  (void)DebugLog_Printf("[SDRAM][WIDE32] FAIL addr=0x%08lX expected=0x%08lX actual=0x%08lX\r\n",
                       (unsigned long)(uintptr_t)address, (unsigned long)expected, (unsigned long)actual);
  return false;
}

static bool TestMemory32(volatile uint32_t *memory, size_t size_bytes)
{
  static const uint32_t patterns[] = {0x0000FFFFu, 0xFFFF0000u, 0x55AAAA55u, 0xAA5555AAu};
  size_t words = size_bytes / sizeof(uint32_t);
  DebugLog_SetStage("diag32-fixed");
  for (unsigned p = 0u; p < 4u; ++p) {
    (void)DebugLog_Printf("[SDRAM][WIDE32] pattern=0x%08lX bytes=%lu\r\n",
                         (unsigned long)patterns[p], (unsigned long)size_bytes);
    for (size_t i = 0u; i < words; ++i) memory[i] = patterns[p];
    __DSB();
    for (size_t i = 0u; i < words; ++i) {
      uint32_t actual = memory[i];
      if (actual != patterns[p]) return WordMismatch(memory + i, patterns[p], actual);
    }
  }
  DebugLog_SetStage("diag32-address");
  (void)DebugLog_Write("[SDRAM][WIDE32] address-pattern\r\n");
  for (size_t i = 0u; i < words; ++i) memory[i] = WordPattern(i);
  __DSB();
  for (size_t i = 0u; i < words; ++i) {
    uint32_t actual = memory[i];
    if (actual != WordPattern(i)) return WordMismatch(memory + i, WordPattern(i), actual);
  }
  DebugLog_SetStage("diag32-retention-100ms");
  (void)DebugLog_Write("[SDRAM][WIDE32] retention-100ms\r\n");
  HAL_Delay(100u);
  for (size_t i = 0u; i < words; ++i) {
    uint32_t actual = memory[i];
    if (actual != WordPattern(i)) return WordMismatch(memory + i, WordPattern(i), actual);
  }
  return true;
}

static BoardSDRAM_Status SelectDiagnosticClock(SDRAM_HandleTypeDef *sdram,
                                               uint32_t period, BoardSDRAM_Report *report)
{
  report->phase = "diag-clock-stop";
  report->hal_status = SendCommand(sdram, FMC_SDRAM_CMD_PALL, 1u, 0u);
  if (report->hal_status != HAL_OK) return BOARD_SDRAM_COMMAND_FAILED;
  (void)DebugLog_Write("[SDRAM][CLOCK] SDCLK OFF before divider change; memory contents discarded\r\n");
  MODIFY_REG(sdram->Instance->SDCR[0], FMC_SDCR1_SDCLK, 0u);
  __DSB();
  HAL_Delay(1u);
  sdram->Init.SDClockPeriod = period;
  MODIFY_REG(sdram->Instance->SDCR[0], FMC_SDCR1_SDCLK, period);
  __DSB();
  return BoardSDRAM_Initialize(sdram, report);
}

void BoardSDRAM_DiagnoseByteAccess(SDRAM_HandleTypeDef *sdram,
                                  volatile uint16_t *memory, size_t size_bytes,
                                  const BoardSDRAM_Report *failure)
{
  BoardSDRAM_Report diagnostic;
  if (sdram == NULL || sdram->Instance != FMC_SDRAM_DEVICE || failure == NULL ||
      failure->phase == NULL || strncmp(failure->phase, "byte-", 5u) != 0 ||
      sdram->Init.SDClockPeriod != FMC_SDRAM_CLOCK_PERIOD_2 ||
      HAL_RCC_GetHCLKFreq() != SDRAM_SUPPORTED_HCLK_HZ ||
      memory == NULL || ((uintptr_t)memory & 3u) != 0u || size_bytes < 64u ||
      size_bytes > BOARD_SDRAM_SIZE_BYTES || (size_bytes & (size_bytes - 1u)) != 0u) return;

  uint32_t saved_control = sdram->Instance->SDCR[0];
  uint32_t saved_timing = sdram->Instance->SDTR[0];
  uint32_t saved_refresh = sdram->Instance->SDRTR;
  bool clock_touched = false;
  (void)DebugLog_Write("[SDRAM][DIAG2] begin; original failure retained; display remains OFF\r\n");
  for (unsigned profile = 0u; profile < 2u; ++profile) {
    diagnostic = *failure;
    if (profile == 1u) {
      clock_touched = true;
      BoardSDRAM_Status init = SelectDiagnosticClock(sdram, FMC_SDRAM_CLOCK_PERIOD_3, &diagnostic);
      if (init != BOARD_SDRAM_OK) {
        (void)DebugLog_Printf("[SDRAM][DIAG2] clock-change FAIL status=%u phase=%s HAL=%u\r\n",
                             (unsigned)init, diagnostic.phase, (unsigned)diagnostic.hal_status);
        break;
      }
    }
    uint32_t clock = HAL_RCC_GetHCLKFreq() / (profile == 0u ? 2u : 3u);
    (void)DebugLog_Printf("[SDRAM][PROFILE] id=%u clock=%luHz refresh=%lu\r\n", profile,
                         (unsigned long)clock, (unsigned long)BoardSDRAM_CalculateRefreshCount(clock));
    LogController(sdram);
    if (!TestStaticMasks(sdram, memory, size_bytes, clock)) break;

    diagnostic = *failure;
    BoardSDRAM_Status wide16 = TestMemory16(memory, size_bytes, &diagnostic, false);
    (void)DebugLog_Printf("[SDRAM][WIDE16] result=%s bytes=%lu phase=%s\r\n",
                         wide16 == BOARD_SDRAM_OK ? "PASS" : "FAIL",
                         (unsigned long)diagnostic.tested_bytes, diagnostic.phase);
    if (wide16 == BOARD_SDRAM_MEMORY_MISMATCH) {
      (void)DebugLog_Printf("[SDRAM][WIDE16] addr=0x%08lX expected=0x%04X actual=0x%04X\r\n",
                           (unsigned long)diagnostic.failed_address,
                           (unsigned)diagnostic.expected, (unsigned)diagnostic.actual);
    }
    bool wide32 = TestMemory32((volatile uint32_t *)memory, size_bytes);
    (void)DebugLog_Printf("[SDRAM][WIDE32] result=%s bytes=%lu\r\n",
                         wide32 ? "PASS" : "FAIL", wide32 ? (unsigned long)size_bytes : 0ul);
    diagnostic = *failure;
    BoardSDRAM_Status bytes = TestByteLanes(memory, size_bytes, &diagnostic, true);
    (void)DebugLog_Printf("[SDRAM][PROFILE] id=%u native-byte=%s; diagnostic results do not clear boot failure\r\n",
                         profile, bytes == BOARD_SDRAM_OK ? "PASS" : "FAIL");
  }
  if (clock_touched) {
    BoardSDRAM_Status restored = SelectDiagnosticClock(sdram, FMC_SDRAM_CLOCK_PERIOD_2, &diagnostic);
    bool registers_match = sdram->Instance->SDCR[0] == saved_control &&
                           sdram->Instance->SDTR[0] == saved_timing &&
                           sdram->Instance->SDRTR == saved_refresh;
    (void)DebugLog_Printf("[SDRAM][DIAG2] restore-90MHz=%s registers-match=%u\r\n",
                         restored == BOARD_SDRAM_OK ? "OK" : "FAIL", registers_match ? 1u : 0u);
    LogController(sdram);
  }
  (void)DebugLog_Write("[SDRAM][DIAG2] end; original failure retained; display remains OFF\r\n");
  DebugLog_SetStage(failure->phase);
}

static BoardSDRAM_Status TestMemory16(volatile uint16_t *memory, size_t size_bytes,
                                       BoardSDRAM_Report *report, bool include_bytes)
{
  static const uint16_t patterns[] = {0x0000u, 0xFFFFu, 0x5555u, 0xAAAAu};
  size_t words;
  uint16_t actual;

  if (report == NULL) return BOARD_SDRAM_INVALID_BUFFER;
  report->tested_bytes = 0u;
  report->failed_address = 0u;
  report->expected = report->actual = 0u;
  SetPhase(report, "validate-buffer");
  if (memory == NULL || ((uintptr_t)memory & 1u) != 0u ||
      size_bytes < 64u || size_bytes > BOARD_SDRAM_SIZE_BYTES ||
      (size_bytes & (size_bytes - 1u)) != 0u) {
    return BOARD_SDRAM_INVALID_BUFFER;
  }
  words = size_bytes / sizeof(uint16_t);

  SetPhase(report, "data-bus");
  for (uint32_t bit = 0u; bit < 16u; ++bit) {
    uint16_t expected = (uint16_t)(1u << bit);
    memory[0] = expected;
    __DSB();
    actual = memory[0];
    if (actual != expected) return Mismatch(report, memory, expected, actual);
    expected = (uint16_t)~expected;
    memory[0] = expected;
    __DSB();
    actual = memory[0];
    if (actual != expected) return Mismatch(report, memory, expected, actual);
  }

  if (include_bytes) {
    BoardSDRAM_Status byte_status = TestByteLanes(memory, size_bytes, report, false);
    if (byte_status != BOARD_SDRAM_OK) return byte_status;
  }

  SetPhase(report, "address-bus");
  for (size_t offset = 1u; offset < words; offset <<= 1u) memory[offset] = 0xAAAAu;
  memory[0] = 0x5555u;
  __DSB();
  for (size_t offset = 1u; offset < words; offset <<= 1u) {
    actual = memory[offset];
    if (actual != 0xAAAAu) return Mismatch(report, memory + offset, 0xAAAAu, actual);
  }
  memory[0] = 0xAAAAu;
  for (size_t target = 1u; target < words; target <<= 1u) {
    memory[target] = 0x5555u;
    __DSB();
    actual = memory[0];
    if (actual != 0xAAAAu) return Mismatch(report, memory, 0xAAAAu, actual);
    for (size_t offset = 1u; offset < words; offset <<= 1u) {
      if (offset == target) continue;
      actual = memory[offset];
      if (actual != 0xAAAAu) return Mismatch(report, memory + offset, 0xAAAAu, actual);
    }
    memory[target] = 0xAAAAu;
  }

  SetPhase(report, "fixed-patterns");
  for (size_t pattern = 0u; pattern < sizeof patterns / sizeof patterns[0]; ++pattern) {
    (void)DebugLog_Printf("[SDRAM] pattern=0x%04X bytes=%lu\r\n",
                         (unsigned)patterns[pattern], (unsigned long)size_bytes);
    for (size_t i = 0u; i < words; ++i) memory[i] = patterns[pattern];
    __DSB();
    for (size_t i = 0u; i < words; ++i) {
      actual = memory[i];
      if (actual != patterns[pattern]) return Mismatch(report, memory + i, patterns[pattern], actual);
    }
  }

  SetPhase(report, "address-patterns");
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    uint16_t inversion = pass == 0u ? 0x0000u : 0xFFFFu;
    for (size_t i = 0u; i < words; ++i) memory[i] = AddressPattern(i, inversion);
    __DSB();
    for (size_t i = 0u; i < words; ++i) {
      uint16_t expected = AddressPattern(i, inversion);
      actual = memory[i];
      if (actual != expected) return Mismatch(report, memory + i, expected, actual);
    }
  }

  SetPhase(report, "retention-100ms");
  HAL_Delay(100u);
  for (size_t i = 0u; i < words; ++i) {
    uint16_t expected = AddressPattern(i, 0xFFFFu);
    actual = memory[i];
    if (actual != expected) return Mismatch(report, memory + i, expected, actual);
  }
  report->tested_bytes = size_bytes;
  SetPhase(report, include_bytes ? "memory-pass" : "diag16-pass");
  return BOARD_SDRAM_OK;
}

BoardSDRAM_Status BoardSDRAM_TestMemory(volatile uint16_t *memory, size_t size_bytes,
                                      BoardSDRAM_Report *report)
{
  return TestMemory16(memory, size_bytes, report, true);
}
