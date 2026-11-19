#ifndef BOARD_TEST_HAL_STUB_H
#define BOARD_TEST_HAL_STUB_H
#include "../../display/hal_stub/stm32f4xx_hal.h"

typedef struct { uint32_t CR1, PSC, ARR, EGR, SR, DIER, CNT; } TIM_TypeDef;
typedef struct { uint32_t CFGR, DCKCFGR; } RCC_TypeDef;
extern TIM_TypeDef test_tim7;
extern RCC_TypeDef test_rcc;
#define TIM7 (&test_tim7)
#define RCC (&test_rcc)
#define RCC_CFGR_PPRE1_Pos 10u
#define RCC_CFGR_PPRE1 (7u << 10u)
#define RCC_DCKCFGR_TIMPRE (1u << 24u)
#define TIM_CR1_CEN 1u
#define TIM_CR1_URS 4u
#define TIM_EGR_UG 1u
#define TIM_DIER_UIE 1u
#define TIM_SR_UIF 1u
#define TIM7_IRQn 55
#define __HAL_RCC_TIM7_CLK_ENABLE() ((void)0)
#define __HAL_RCC_TIM7_FORCE_RESET() TestTIM7Reset()
#define __HAL_RCC_TIM7_RELEASE_RESET() ((void)0)
void TestTIM7Reset(void);
uint32_t HAL_RCC_GetPCLK1Freq(void);
void HAL_NVIC_DisableIRQ(int);
void HAL_NVIC_EnableIRQ(int);
void HAL_NVIC_ClearPendingIRQ(int);
void HAL_NVIC_SetPriority(int, uint32_t, uint32_t);
static inline void __disable_irq(void) { test_primask=1u; }
static inline void __set_PRIMASK(uint32_t p) { test_primask=p; }
#endif
