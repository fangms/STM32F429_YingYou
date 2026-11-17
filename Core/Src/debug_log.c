#include "debug_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DEBUG_LOG_TIMEOUT_MS 100u

static UART_HandleTypeDef *log_uart;
static const char *log_stage = "before-uart5";

void DebugLog_Init(UART_HandleTypeDef *uart)
{
  log_uart = uart;
}

static HAL_StatusTypeDef Transmit(const uint8_t *data, uint16_t length)
{
  if (log_uart == NULL) {
    return HAL_ERROR;
  }
  /* HAL UART timeouts need SysTick. Never wait from an exception or with IRQs off. */
  if (__get_IPSR() != 0u || __get_PRIMASK() != 0u) {
    return HAL_BUSY;
  }
  if (length == 0u) {
    return HAL_OK;
  }
  return HAL_UART_Transmit(log_uart, data, length, DEBUG_LOG_TIMEOUT_MS);
}

HAL_StatusTypeDef DebugLog_Write(const char *text)
{
  size_t length;
  if (text == NULL) {
    return HAL_ERROR;
  }
  length = strlen(text);
  if (length > UINT16_MAX) {
    return HAL_ERROR;
  }
  return Transmit((const uint8_t *)text, (uint16_t)length);
}

HAL_StatusTypeDef DebugLog_Printf(const char *format, ...)
{
  char buffer[DEBUG_LOG_BUFFER_SIZE];
  va_list args;
  int length;

  if (format == NULL) {
    return HAL_ERROR;
  }
  va_start(args, format);
  length = vsnprintf(buffer, sizeof buffer, format, args);
  va_end(args);
  if (length < 0) {
    return HAL_ERROR;
  }
  return DebugLog_Write(buffer);
}

void DebugLog_SetStage(const char *stage)
{
  /* The caller must supply a string with static lifetime. */
  log_stage = stage != NULL ? stage : "unknown";
}

const char *DebugLog_GetStage(void)
{
  return log_stage;
}

int __io_putchar(int ch)
{
  uint8_t byte = (uint8_t)ch;
  return Transmit(&byte, 1u) == HAL_OK ? byte : EOF;
}
