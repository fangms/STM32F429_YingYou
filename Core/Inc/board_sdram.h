#ifndef BOARD_SDRAM_H
#define BOARD_SDRAM_H

#include "stm32f4xx_hal.h"
#include <stddef.h>
#include <stdint.h>

#define BOARD_SDRAM_BASE_ADDRESS ((uintptr_t)0xC0000000u)
#define BOARD_SDRAM_SIZE_BYTES (32u * 1024u * 1024u)
/* Winbond W9825G6KH A04, -6I grade: 8192 refreshes / 64ms, -40..85C. */
#define BOARD_SDRAM_REFRESH_WINDOW_US 64000u
#define BOARD_SDRAM_REFRESH_ROWS 8192u

typedef enum {
  BOARD_SDRAM_OK = 0,
  BOARD_SDRAM_INVALID_CONFIG,
  BOARD_SDRAM_COMMAND_FAILED,
  BOARD_SDRAM_REFRESH_FAILED,
  BOARD_SDRAM_INVALID_BUFFER,
  BOARD_SDRAM_MEMORY_MISMATCH
} BoardSDRAM_Status;

typedef struct {
  const char *phase;
  HAL_StatusTypeDef hal_status;
  uint32_t sdclock_hz;
  uint32_t refresh_count;
  uint32_t mode_register;
  uintptr_t failed_address;
  uint16_t expected;
  uint16_t actual;
  size_t tested_bytes;
} BoardSDRAM_Report;

uint32_t BoardSDRAM_CalculateRefreshCount(uint32_t sdclock_hz);
BoardSDRAM_Status BoardSDRAM_Initialize(SDRAM_HandleTypeDef *sdram, BoardSDRAM_Report *report);
/* Destructive boot-only test. No LTDC/DMA or other users may access this range. */
BoardSDRAM_Status BoardSDRAM_TestMemory(volatile uint16_t *memory, size_t size_bytes,
                                      BoardSDRAM_Report *report);
/* Boot failure diagnostics only. Does not qualify memory or clear the original
   failure. Compares GPIO byte masks and 90/60MHz at boot; restores pins/clock. */
void BoardSDRAM_DiagnoseByteAccess(SDRAM_HandleTypeDef *sdram,
                                  volatile uint16_t *memory, size_t size_bytes,
                                  const BoardSDRAM_Report *failure);

#endif
