#ifndef TEST_STM32F4XX_HAL_H
#define TEST_STM32F4XX_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t marker; } UART_HandleTypeDef;
typedef struct { volatile uint32_t SDCR[2], SDTR[2], SDCMR, SDRTR, SDSR; } FMC_SDRAM_TypeDef;
typedef struct {
  uint32_t SDBank, ColumnBitsNumber, RowBitsNumber, MemoryDataWidth;
  uint32_t InternalBankNumber, CASLatency, SDClockPeriod;
} FMC_SDRAM_InitTypeDef;
typedef struct {
  FMC_SDRAM_TypeDef *Instance;
  FMC_SDRAM_InitTypeDef Init;
  uint32_t State;
} SDRAM_HandleTypeDef;
typedef struct {
  uint32_t CommandMode, CommandTarget, AutoRefreshNumber, ModeRegisterDefinition;
} FMC_SDRAM_CommandTypeDef;

extern FMC_SDRAM_TypeDef test_fmc;
extern uint32_t test_ipsr, test_primask;
#define FMC_SDRAM_DEVICE (&test_fmc)
#define FMC_SDRAM_BANK1 0u
#define FMC_SDRAM_COLUMN_BITS_NUM_9 9u
#define FMC_SDRAM_ROW_BITS_NUM_13 13u
#define FMC_SDRAM_MEM_BUS_WIDTH_16 16u
#define FMC_SDRAM_INTERN_BANKS_NUM_4 4u
#define FMC_SDRAM_CAS_LATENCY_3 3u
#define FMC_SDRAM_CLOCK_PERIOD_2 0x800u
#define FMC_SDRAM_CLOCK_PERIOD_3 0xC00u
#define FMC_SDCR1_SDCLK 0xC00u
#define FMC_SDRAM_CMD_CLK_ENABLE 1u
#define FMC_SDRAM_CMD_PALL 2u
#define FMC_SDRAM_CMD_AUTOREFRESH_MODE 3u
#define FMC_SDRAM_CMD_LOAD_MODE 4u
#define FMC_SDRAM_CMD_TARGET_BANK1 0x10u
#define HAL_SDRAM_STATE_READY 1u
#define FMC_SDSR_BUSY 0x20u
#define FMC_SDCR1_RBURST 0x1000u
#define READ_BIT(reg, mask) ((reg) & (mask))
#define MODIFY_REG(reg, clear, set) ((reg) = ((reg) & ~(clear)) | (set))
static inline uint32_t __get_IPSR(void) { return test_ipsr; }
static inline uint32_t __get_PRIMASK(void) { return test_primask; }
void TestDSB(void);
static inline void __DSB(void) { __asm__ volatile ("" ::: "memory"); TestDSB(); }

uint32_t HAL_GetTick(void);
uint32_t HAL_RCC_GetHCLKFreq(void);
void HAL_Delay(uint32_t delay);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *, const uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef FMC_SDRAM_SendCommand(FMC_SDRAM_TypeDef *, const FMC_SDRAM_CommandTypeDef *, uint32_t);
HAL_StatusTypeDef HAL_SDRAM_ProgramRefreshRate(SDRAM_HandleTypeDef *, uint32_t);
typedef struct { volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, AFR[2]; } GPIO_TypeDef;
extern GPIO_TypeDef test_gpioe;
#define GPIOE (&test_gpioe)
#define GPIO_PIN_0 1u
#define GPIO_PIN_1 2u
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
#define GPIO_MODE_OUTPUT_PP 1u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_VERY_HIGH 3u
void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t, GPIO_PinState);
void HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *);
#endif
