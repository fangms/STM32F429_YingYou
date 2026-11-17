/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define DN_KEY_Pin GPIO_PIN_3
#define DN_KEY_GPIO_Port GPIOE
#define OK_KEY_Pin GPIO_PIN_4
#define OK_KEY_GPIO_Port GPIOE
#define Radar_OUT_Pin GPIO_PIN_5
#define Radar_OUT_GPIO_Port GPIOE
#define H863_EN_Pin GPIO_PIN_13
#define H863_EN_GPIO_Port GPIOC
#define LoRa_M0_Pin GPIO_PIN_1
#define LoRa_M0_GPIO_Port GPIOC
#define LoRa_M1_Pin GPIO_PIN_2
#define LoRa_M1_GPIO_Port GPIOC
#define SPI_SDC_Pin GPIO_PIN_4
#define SPI_SDC_GPIO_Port GPIOA
#define SPI_CLK_Pin GPIO_PIN_5
#define SPI_CLK_GPIO_Port GPIOA
#define SPI_MISO_Pin GPIO_PIN_6
#define SPI_MISO_GPIO_Port GPIOA
#define SPI_MOSI_Pin GPIO_PIN_7
#define SPI_MOSI_GPIO_Port GPIOA
#define FLOW_IN_Pin GPIO_PIN_1
#define FLOW_IN_GPIO_Port GPIOB
#define SPI2_CS_Pin GPIO_PIN_12
#define SPI2_CS_GPIO_Port GPIOB
#define PMU_EN1_Pin GPIO_PIN_11
#define PMU_EN1_GPIO_Port GPIOD
#define PMU_EN2_Pin GPIO_PIN_12
#define PMU_EN2_GPIO_Port GPIOD
#define PMU_EN4_Pin GPIO_PIN_13
#define PMU_EN4_GPIO_Port GPIOD
#define Valve_EN_Pin GPIO_PIN_6
#define Valve_EN_GPIO_Port GPIOC
#define Water_pump_E_Pin GPIO_PIN_8
#define Water_pump_E_GPIO_Port GPIOC
#define SPI_NSS_Pin GPIO_PIN_9
#define SPI_NSS_GPIO_Port GPIOC
#define LCD_SHUT_Pin GPIO_PIN_8
#define LCD_SHUT_GPIO_Port GPIOA
#define PMU_EN3_Pin GPIO_PIN_3
#define PMU_EN3_GPIO_Port GPIOI
#define LCDBL_EN_Pin GPIO_PIN_10
#define LCDBL_EN_GPIO_Port GPIOC
#define BEEP_Pin GPIO_PIN_3
#define BEEP_GPIO_Port GPIOD
#define LCD_RST_Pin GPIO_PIN_6
#define LCD_RST_GPIO_Port GPIOD
#define SSD2828_LCD_RESET_Pin GPIO_PIN_7
#define SSD2828_LCD_RESET_GPIO_Port GPIOD
#define ON_OFF_KEY_Pin GPIO_PIN_12
#define ON_OFF_KEY_GPIO_Port GPIOG
#define SET_KEY_Pin GPIO_PIN_13
#define SET_KEY_GPIO_Port GPIOG
#define UP_KEY_Pin GPIO_PIN_14
#define UP_KEY_GPIO_Port GPIOG
#define FLOW_EN_Pin GPIO_PIN_5
#define FLOW_EN_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
