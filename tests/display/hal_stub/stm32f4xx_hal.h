#ifndef TEST_STM32F4XX_HAL_H
#define TEST_STM32F4XX_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t marker; } UART_HandleTypeDef;
typedef struct { volatile uint32_t SDSR; } FMC_SDRAM_TypeDef;
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
#define FMC_SDRAM_CLOCK_PERIOD_2 2u
#define FMC_SDRAM_CMD_CLK_ENABLE 1u
#define FMC_SDRAM_CMD_PALL 2u
#define FMC_SDRAM_CMD_AUTOREFRESH_MODE 3u
#define FMC_SDRAM_CMD_LOAD_MODE 4u
#define FMC_SDRAM_CMD_TARGET_BANK1 0x10u
#define HAL_SDRAM_STATE_READY 1u
#define FMC_SDSR_BUSY 0x20u
#define READ_BIT(reg, mask) ((reg) & (mask))
static inline uint32_t __get_IPSR(void) { return test_ipsr; }
static inline uint32_t __get_PRIMASK(void) { return test_primask; }
static inline void __DSB(void) { __asm__ volatile ("" ::: "memory"); }

uint32_t HAL_GetTick(void);
uint32_t HAL_RCC_GetHCLKFreq(void);
void HAL_Delay(uint32_t delay);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *, const uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef FMC_SDRAM_SendCommand(FMC_SDRAM_TypeDef *, const FMC_SDRAM_CommandTypeDef *, uint32_t);
HAL_StatusTypeDef HAL_SDRAM_ProgramRefreshRate(SDRAM_HandleTypeDef *, uint32_t);

typedef struct { uint32_t ODR; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
extern GPIO_TypeDef test_gpio[9];
#define GPIOA (&test_gpio[0])
#define GPIOB (&test_gpio[1])
#define GPIOC (&test_gpio[2])
#define GPIOD (&test_gpio[3])
#define GPIOE (&test_gpio[4])
#define GPIOF (&test_gpio[5])
#define GPIOG (&test_gpio[6])
#define GPIOH (&test_gpio[7])
#define GPIOI (&test_gpio[8])
#define GPIO_PIN_0 0x0001u
#define GPIO_PIN_1 0x0002u
#define GPIO_PIN_2 0x0004u
#define GPIO_PIN_3 0x0008u
#define GPIO_PIN_4 0x0010u
#define GPIO_PIN_5 0x0020u
#define GPIO_PIN_6 0x0040u
#define GPIO_PIN_7 0x0080u
#define GPIO_PIN_8 0x0100u
#define GPIO_PIN_9 0x0200u
#define GPIO_PIN_10 0x0400u
#define GPIO_PIN_11 0x0800u
#define GPIO_PIN_12 0x1000u
#define GPIO_PIN_13 0x2000u
#define GPIO_PIN_14 0x4000u
#define GPIO_PIN_15 0x8000u
typedef struct { volatile uint32_t CTRL, CYCCNT; } DWT_Type;
typedef struct { uint32_t DEMCR; } CoreDebug_Type;
extern CoreDebug_Type test_coredebug;
DWT_Type *TestDWT(void);
#define DWT TestDWT()
#define CoreDebug (&test_coredebug)
#define CoreDebug_DEMCR_TRCENA_Msk 1u
#define DWT_CTRL_CYCCNTENA_Msk 1u
static inline void __ISB(void) { __asm__ volatile ("" ::: "memory"); }
void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t, GPIO_PinState);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *, uint16_t);
typedef struct { uint32_t Pin, Mode, Pull, Speed; } GPIO_InitTypeDef;
#define GPIO_MODE_OUTPUT_PP 1u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_LOW 0u
void HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *);
typedef struct {
  uint32_t CR, WHPCR, WVPCR, PFCR, CACR, BFCR, CFBAR, CFBLR, CFBLNR;
} LTDC_Layer_TypeDef;
typedef struct {
  uint32_t ISR, ICR, SRCR, GCR, SSCR, BPCR, AWCR, TWCR, BCCR, CPSR, CDSR;
  LTDC_Layer_TypeDef layers[2];
} LTDC_TypeDef;
typedef struct { LTDC_TypeDef *Instance; uint32_t address; } LTDC_HandleTypeDef;
#define LTDC_ISR_FUIF 2u
#define LTDC_ISR_TERRIF 4u
#define LTDC_ICR_CFUIF 2u
#define LTDC_ICR_CTERRIF 4u
#define LTDC_SRCR_VBR 2u
#define LTDC_LxCR_LEN 1u
#define LTDC_GCR_LTDCEN 1u
#define LTDC_LAYER(h, layer) (&((h)->Instance->layers[layer]))
#define __HAL_LTDC_LAYER_ENABLE(h, layer) (LTDC_LAYER(h, layer)->CR |= LTDC_LxCR_LEN)
#define __HAL_LTDC_LAYER_DISABLE(h, layer) (LTDC_LAYER(h, layer)->CR &= ~LTDC_LxCR_LEN)
#define __HAL_LTDC_DISABLE(h) ((h)->Instance->GCR &= ~LTDC_GCR_LTDCEN)
HAL_StatusTypeDef HAL_LTDC_SetAddress_NoReload(LTDC_HandleTypeDef *, uint32_t, uint32_t);
#endif
