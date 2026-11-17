#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include "stm32f4xx_hal.h"

#define DEBUG_LOG_BUFFER_SIZE 256u

/* Main-context, polling-only logger. Not an ISR/fault handler transport. */
void DebugLog_Init(UART_HandleTypeDef *uart);
HAL_StatusTypeDef DebugLog_Write(const char *text);
HAL_StatusTypeDef DebugLog_Printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
void DebugLog_SetStage(const char *stage);
const char *DebugLog_GetStage(void);
int __io_putchar(int ch);

#endif
