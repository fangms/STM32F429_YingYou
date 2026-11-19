/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "board_sdram.h"
#include "debug_log.h"
#include "firmware_version.h"
#include "display_config.h"
#include "board_ssd2828.h"
#include "board_display_diag.h"
#include "board_test.h"
  /* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SCREEN_FRAME0 ((volatile uint16_t *)BOARD_SDRAM_BASE_ADDRESS)
#define SCREEN_FRAME1 ((volatile uint16_t *)(BOARD_SDRAM_BASE_ADDRESS + DISPLAY_TEST_FRAME_BYTES))
_Static_assert(2u * DISPLAY_TEST_FRAME_BYTES <= BOARD_SDRAM_SIZE_BYTES, "Two test frames must fit SDRAM");
  /* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc3;

LTDC_HandleTypeDef hltdc;

SPI_HandleTypeDef hspi2;

UART_HandleTypeDef huart5;
UART_HandleTypeDef huart7;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;

SDRAM_HandleTypeDef hsdram1;

/* USER CODE BEGIN PV */
static BoardSDRAM_Report sdram_report;
static SSD2828_ProbeReport bridge_report;
static SSD2828_Status bridge_status = SSD2828_INVALID_ARGUMENT;
static BoardSSD2828_DisplayReport panel_report;
static SSD2828_Status panel_status = SSD2828_INVALID_ARGUMENT;
static BoardDisplayDiag display_diag;
static BoardTest board_test;
static bool ltdc_scan_active, screen_active;
  /* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_UART5_Init(void);
static void MX_FMC_Init(void);
static void MX_ADC1_Init(void);
static void MX_LTDC_Init(void);
static void MX_UART7_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_ADC3_Init(void);
static void MX_SPI2_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* RM0090 calls the output "Not Data Enable". DEPOL=0 is the physical
   active-HIGH DE used by Linux/U-Boot STM32 LTDC and required by SSD2828.
   Observe the MCU pad in wide interior regions, not CDSR internal status.
   Run ONLY before the bridge video path is enabled. This is logic-level
   evidence at PF10, not a measurement at the bridge or of setup/hold time. */
static unsigned DisplayDE_Region(uint32_t position)
{
  uint32_t x = (position & LTDC_CPSR_CXPOS) >> LTDC_CPSR_CXPOS_Pos;
  uint32_t y = (position & LTDC_CPSR_CYPOS) >> LTDC_CPSR_CYPOS_Pos;
  /* Current scan: HBP=30, VBP=32, active=400x960, totals=450x1004. */
  if (x < 64u || x > 395u) return 0u;
  if (y >= 64u && y <= 959u) return 1u;
  if (y >= 10u && y <= 23u) return 2u; /* Wide vertical back porch. */
  return 0u;
}

static void DisplayDE_LogPad(const char *phase)
{
  uint32_t counts[2][2] = {{0u, 0u}, {0u, 0u}};
  uint32_t start = HAL_GetTick();
  uint32_t polls = 0u;
  /* Both bounds apply: broken scan/tick must not block panel startup. */
  while ((uint32_t)(HAL_GetTick() - start) < 120u && polls < 2000000u)
  {
    ++polls;
    uint32_t before = LTDC->CPSR;
    uint32_t pad = (GPIOF->IDR & GPIO_PIN_10) != 0u;
    uint32_t after = LTDC->CPSR;
    unsigned region = DisplayDE_Region(before);
    uint32_t xb = before >> LTDC_CPSR_CXPOS_Pos;
    uint32_t xa = after >> LTDC_CPSR_CXPOS_Pos;
    /* Reject scan boundaries, wrap and samples delayed by interrupts. */
    if (region != 0u && region == DisplayDE_Region(after) &&
        (before & LTDC_CPSR_CYPOS) == (after & LTDC_CPSR_CYPOS) &&
        xa >= xb && xa - xb <= 32u)
      ++counts[region - 1u][pad];
  }
  uint32_t mode = (GPIOF->MODER >> 20u) & 3u;
  uint32_t af = (GPIOF->AFR[1] >> 8u) & 15u;
  const char *physical = "INSUFFICIENT";
  if (mode != 2u || af != 14u) physical = "PIN-CONFIG";
  else if (counts[0][0] + counts[0][1] >= 32u &&
           counts[1][0] + counts[1][1] >= 32u)
  {
    if (counts[0][0] == 0u && counts[1][1] == 0u) physical = "ACTIVE-HIGH";
    else if (counts[0][1] == 0u && counts[1][0] == 0u) physical = "ACTIVE-LOW";
    else physical = "MIXED";
  }
  (void)DebugLog_Printf("[DE-PAD] phase=%s GCR=%08lX cache=%08lX PF10mode=%lu AF=%lu active(H/L)=%lu/%lu blank(H/L)=%lu/%lu physical=%s polls=%lu\r\n",
                       phase, (unsigned long)LTDC->GCR,
                       (unsigned long)hltdc.Init.DEPolarity,
                       (unsigned long)mode, (unsigned long)af,
                       (unsigned long)counts[0][1], (unsigned long)counts[0][0],
                       (unsigned long)counts[1][1], (unsigned long)counts[1][0],
                       physical, (unsigned long)polls);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_UART5_Init();
  MX_FMC_Init();
  MX_ADC1_Init();
  MX_UART7_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  MX_ADC3_Init();
  MX_SPI2_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  BoardTest_Init(&board_test, HAL_GetTick());
  bridge_status = BoardSSD2828_Probe(&bridge_report);
  if (bridge_status == SSD2828_OK)
  {
    DebugLog_SetStage("ltdc-start");
    MX_LTDC_Init();
    ltdc_scan_active = true;
    (void)DebugLog_Write("[LTDC] scan=ON PCLK=27100000Hz totals=450x1004 RGB565\r\n");
    if (BoardDisplayDiag_Prepare(&display_diag, &hltdc, SCREEN_FRAME0, DISPLAY_TEST_FRAME_BYTES))
    {
      bridge_status = BoardSSD2828_CollectPanelEvidence(&bridge_report);
      panel_status = bridge_status;
      if (bridge_status == SSD2828_OK)
        panel_status = BoardSSD2828_StartPanel(true, &panel_report);
      BoardTest_SetStartup(&board_test, true, bridge_status == SSD2828_OK,
                           bridge_report.id_count != 0u ? bridge_report.id[0] : 0u,
                           panel_status == SSD2828_OK);
      if (panel_status == SSD2828_OK)
        screen_active = BoardTest_AttachDisplay(&board_test, &hltdc,
                                                SCREEN_FRAME0, SCREEN_FRAME1, DISPLAY_TEST_FRAME_BYTES);
    }
    if (!screen_active)
    {
      BoardSSD2828_Shutdown();
      __HAL_LTDC_DISABLE(&hltdc);
      ltdc_scan_active = false;
    }
  }
  else
  {
    (void)DebugLog_Printf("[LTDC] scan=OFF bridge-status=%u phase=%s\r\n",
                         (unsigned)bridge_status, bridge_report.phase);
  }
  BoardTest_SetStartup(&board_test, true, bridge_status == SSD2828_OK,
                       bridge_report.id_count != 0u ? bridge_report.id[0] : 0u,
                       panel_status == SSD2828_OK);
  DebugLog_SetStage(screen_active ? "board-key-buzzer-test" : "display-failed-key-test");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_Delay(5u);
    BoardTest_Update(&board_test, HAL_GetTick());
    if (screen_active && !board_test.attached)
    {
      BoardSSD2828_Shutdown();
      screen_active = ltdc_scan_active = false;
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 360;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ENABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 2;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_144CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC3_Init(void)
{

  /* USER CODE BEGIN ADC3_Init 0 */

  /* USER CODE END ADC3_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC3_Init 1 */

  /* USER CODE END ADC3_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc3.Instance = ADC3;
  hadc3.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc3.Init.Resolution = ADC_RESOLUTION_12B;
  hadc3.Init.ScanConvMode = DISABLE;
  hadc3.Init.ContinuousConvMode = DISABLE;
  hadc3.Init.DiscontinuousConvMode = DISABLE;
  hadc3.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc3.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc3.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc3.Init.NbrOfConversion = 1;
  hadc3.Init.DMAContinuousRequests = DISABLE;
  hadc3.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc3) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_6;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_144CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC3_Init 2 */

  /* USER CODE END ADC3_Init 2 */

}

/**
  * @brief LTDC Initialization Function
  * @param None
  * @retval None
  */
static void MX_LTDC_Init(void)
{

  /* USER CODE BEGIN LTDC_Init 0 */

  /* USER CODE END LTDC_Init 0 */

  LTDC_LayerCfgTypeDef pLayerCfg = {0};

  /* USER CODE BEGIN LTDC_Init 1 */

  /* USER CODE END LTDC_Init 1 */
  hltdc.Instance = LTDC;
  hltdc.Init.HSPolarity = LTDC_HSPOLARITY_AL;
  hltdc.Init.VSPolarity = LTDC_VSPOLARITY_AL;
  hltdc.Init.DEPolarity = LTDC_DEPOLARITY_AH;
  hltdc.Init.PCPolarity = LTDC_PCPOLARITY_IPC;
  hltdc.Init.HorizontalSync = 9;
  hltdc.Init.VerticalSync = 7;
  hltdc.Init.AccumulatedHBP = 29;
  hltdc.Init.AccumulatedVBP = 31;
  hltdc.Init.AccumulatedActiveW = 429;
  hltdc.Init.AccumulatedActiveH = 991;
  hltdc.Init.TotalWidth = 449;
  hltdc.Init.TotalHeigh = 1003;
  hltdc.Init.Backcolor.Blue = 0;
  hltdc.Init.Backcolor.Green = 0;
  hltdc.Init.Backcolor.Red = 0;
  if (HAL_LTDC_Init(&hltdc) != HAL_OK)
  {
    Error_Handler();
  }
  pLayerCfg.WindowX0 = 0;
  pLayerCfg.WindowX1 = 400;
  pLayerCfg.WindowY0 = 0;
  pLayerCfg.WindowY1 = 960;
  pLayerCfg.PixelFormat = LTDC_PIXEL_FORMAT_RGB565;
  pLayerCfg.Alpha = 255;
  pLayerCfg.Alpha0 = 0;
  pLayerCfg.BlendingFactor1 = LTDC_BLENDING_FACTOR1_CA;
  pLayerCfg.BlendingFactor2 = LTDC_BLENDING_FACTOR2_CA;
  pLayerCfg.FBStartAdress = 0xC0000000;
  pLayerCfg.ImageWidth = 400;
  pLayerCfg.ImageHeight = 960;
  pLayerCfg.Backcolor.Blue = 0;
  pLayerCfg.Backcolor.Green = 0;
  pLayerCfg.Backcolor.Red = 0;
  if (HAL_LTDC_ConfigLayer(&hltdc, &pLayerCfg, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LTDC_Init 2 */
  /* Keep generated/IOC content intact until the user selects DE Active Low
     in CubeMX. Sync both the HAL cache and the actual non-shadow GCR bit.
     Bridge VEN is still 0 here, so changing DE cannot interrupt video. */
  DisplayDE_LogPad("generated");
  hltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
  CLEAR_BIT(hltdc.Instance->GCR, LTDC_GCR_DEPOL);
  __DSB();
  DisplayDE_LogPad("ssd2828");
  /* USER CODE END LTDC_Init 2 */

}

/**
  * @brief SPI2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI2_Init(void)
{

  /* USER CODE BEGIN SPI2_Init 0 */

  /* USER CODE END SPI2_Init 0 */

  /* USER CODE BEGIN SPI2_Init 1 */

  /* USER CODE END SPI2_Init 1 */
  /* SPI2 parameter configuration*/
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI2_Init 2 */

  /* USER CODE END SPI2_Init 2 */

}

/**
  * @brief UART5 Initialization Function
  * @param None
  * @retval None
  */
static void MX_UART5_Init(void)
{

  /* USER CODE BEGIN UART5_Init 0 */

  /* USER CODE END UART5_Init 0 */

  /* USER CODE BEGIN UART5_Init 1 */

  /* USER CODE END UART5_Init 1 */
  huart5.Instance = UART5;
  huart5.Init.BaudRate = 115200;
  huart5.Init.WordLength = UART_WORDLENGTH_8B;
  huart5.Init.StopBits = UART_STOPBITS_1;
  huart5.Init.Parity = UART_PARITY_NONE;
  huart5.Init.Mode = UART_MODE_TX_RX;
  huart5.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart5.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart5) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN UART5_Init 2 */
  DebugLog_Init(&huart5);
  DebugLog_SetStage("uart5-ready");
  (void)DebugLog_Write("\r\n[BOOT] H863 " H863_FIRMWARE_VERSION ": CW90 960x400 KEY/BEEP test; DE active-HIGH (DEPOL=0) + pad check; PD6 open-drain; boot panel-read + bridge/panel restart; FIXED " H863_DISPLAY_FIXED_LABEL "; GC9503 BOE3.99\r\n");
  (void)DebugLog_Printf("[CLOCK] SYSCLK=%lu HCLK=%lu PCLK1=%lu PCLK2=%lu Hz\r\n",
                       (unsigned long)HAL_RCC_GetSysClockFreq(),
                       (unsigned long)HAL_RCC_GetHCLKFreq(),
                       (unsigned long)HAL_RCC_GetPCLK1Freq(),
                       (unsigned long)HAL_RCC_GetPCLK2Freq());
  /* Register configuration only: a 1V8 reset high is not a 3V3 GPIO IDR test. */
  (void)DebugLog_Printf("[RESET] PD6 mode=%lu type=%lu pull=%lu latch=%lu; external 1V8 pull-up required (GPIO config, not volts)\r\n",
                       (unsigned long)((LCD_RST_GPIO_Port->MODER >> 12u) & 3u),
                       (unsigned long)((LCD_RST_GPIO_Port->OTYPER >> 6u) & 1u),
                       (unsigned long)((LCD_RST_GPIO_Port->PUPDR >> 12u) & 3u),
                       (unsigned long)((LCD_RST_GPIO_Port->ODR >> 6u) & 1u));
  /* USER CODE END UART5_Init 2 */

}

/**
  * @brief UART7 Initialization Function
  * @param None
  * @retval None
  */
static void MX_UART7_Init(void)
{

  /* USER CODE BEGIN UART7_Init 0 */

  /* USER CODE END UART7_Init 0 */

  /* USER CODE BEGIN UART7_Init 1 */

  /* USER CODE END UART7_Init 1 */
  huart7.Instance = UART7;
  huart7.Init.BaudRate = 115200;
  huart7.Init.WordLength = UART_WORDLENGTH_8B;
  huart7.Init.StopBits = UART_STOPBITS_1;
  huart7.Init.Parity = UART_PARITY_NONE;
  huart7.Init.Mode = UART_MODE_TX_RX;
  huart7.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart7.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart7) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN UART7_Init 2 */

  /* USER CODE END UART7_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/* FMC initialization function */
static void MX_FMC_Init(void)
{

  /* USER CODE BEGIN FMC_Init 0 */
  DebugLog_SetStage("fmc-controller-init");
  (void)DebugLog_Write("[FMC] controller-init\r\n");
  /* USER CODE END FMC_Init 0 */

  FMC_SDRAM_TimingTypeDef SdramTiming = {0};

  /* USER CODE BEGIN FMC_Init 1 */

  /* USER CODE END FMC_Init 1 */

  /** Perform the SDRAM1 memory initialization sequence
  */
  hsdram1.Instance = FMC_SDRAM_DEVICE;
  /* hsdram1.Init */
  hsdram1.Init.SDBank = FMC_SDRAM_BANK1;
  hsdram1.Init.ColumnBitsNumber = FMC_SDRAM_COLUMN_BITS_NUM_9;
  hsdram1.Init.RowBitsNumber = FMC_SDRAM_ROW_BITS_NUM_13;
  hsdram1.Init.MemoryDataWidth = FMC_SDRAM_MEM_BUS_WIDTH_16;
  hsdram1.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
  hsdram1.Init.CASLatency = FMC_SDRAM_CAS_LATENCY_3;
  hsdram1.Init.WriteProtection = FMC_SDRAM_WRITE_PROTECTION_DISABLE;
  hsdram1.Init.SDClockPeriod = FMC_SDRAM_CLOCK_PERIOD_2;
  hsdram1.Init.ReadBurst = FMC_SDRAM_RBURST_ENABLE;
  hsdram1.Init.ReadPipeDelay = FMC_SDRAM_RPIPE_DELAY_1;
  /* SdramTiming */
  SdramTiming.LoadToActiveDelay = 2;
  SdramTiming.ExitSelfRefreshDelay = 8;
  SdramTiming.SelfRefreshTime = 5;
  SdramTiming.RowCycleDelay = 8;
  SdramTiming.WriteRecoveryTime = 3;
  SdramTiming.RPDelay = 3;
  SdramTiming.RCDDelay = 3;

  if (HAL_SDRAM_Init(&hsdram1, &SdramTiming) != HAL_OK)
  {
    Error_Handler( );
  }

  /* USER CODE BEGIN FMC_Init 2 */
  (void)DebugLog_Printf("[MCU] IDCODE=0x%08lX\r\n", (unsigned long)DBGMCU->IDCODE);
  (void)DebugLog_Printf("[FMC][REG] SDCR1=0x%08lX SDTR1=0x%08lX\r\n",
                       (unsigned long)hsdram1.Instance->SDCR[0],
                       (unsigned long)hsdram1.Instance->SDTR[0]);
  (void)DebugLog_Printf("[GPIOE] MODER=0x%08lX AFRL=0x%08lX PUPDR=0x%08lX OSPEEDR=0x%08lX\r\n",
                       (unsigned long)GPIOE->MODER, (unsigned long)GPIOE->AFR[0],
                       (unsigned long)GPIOE->PUPDR, (unsigned long)GPIOE->OSPEEDR);
  BoardSDRAM_Status sdram_status = BoardSDRAM_Initialize(&hsdram1, &sdram_report);
  if (sdram_status == BOARD_SDRAM_OK)
  {
    sdram_status = BoardSDRAM_TestMemory((volatile uint16_t *)BOARD_SDRAM_BASE_ADDRESS,
                                        BOARD_SDRAM_SIZE_BYTES, &sdram_report);
  }
  if (sdram_status != BOARD_SDRAM_OK)
  {
    (void)DebugLog_Printf("[FAIL] SDRAM status=%u phase=%s HAL=%u\r\n",
                         (unsigned)sdram_status, sdram_report.phase,
                         (unsigned)sdram_report.hal_status);
    if (sdram_status == BOARD_SDRAM_MEMORY_MISMATCH)
    {
      (void)DebugLog_Printf("[FAIL] addr=0x%08lX expected=0x%04X actual=0x%04X\r\n",
                           (unsigned long)sdram_report.failed_address,
                           (unsigned)sdram_report.expected, (unsigned)sdram_report.actual);
      BoardSDRAM_DiagnoseByteAccess(&hsdram1,
                                    (volatile uint16_t *)BOARD_SDRAM_BASE_ADDRESS,
                                    BOARD_SDRAM_SIZE_BYTES, &sdram_report);
    }
    Error_Handler();
  }
  (void)DebugLog_Printf("[PASS] SDRAM %lu bytes tested\r\n",
                       (unsigned long)sdram_report.tested_bytes);
  DebugLog_SetStage("remaining-peripheral-init");
  /* USER CODE END FMC_Init 2 */
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOI_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, H863_EN_Pin|LoRa_M0_Pin|LoRa_M1_Pin|Valve_EN_Pin
                          |Water_pump_E_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(SPI_SDC_GPIO_Port, SPI_SDC_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, SPI_CLK_Pin|SPI_MOSI_Pin|LCD_SHUT_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, SPI2_CS_Pin|FLOW_EN_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, PMU_EN1_Pin|PMU_EN2_Pin|PMU_EN4_Pin|BEEP_Pin
                          |LCD_RST_Pin|SSD2828_LCD_RESET_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(SPI_NSS_GPIO_Port, SPI_NSS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(PMU_EN3_GPIO_Port, PMU_EN3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : DN_KEY_Pin OK_KEY_Pin */
  GPIO_InitStruct.Pin = DN_KEY_Pin|OK_KEY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : Radar_OUT_Pin */
  GPIO_InitStruct.Pin = Radar_OUT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(Radar_OUT_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : H863_EN_Pin LoRa_M0_Pin LoRa_M1_Pin Valve_EN_Pin
                           Water_pump_E_Pin SPI_NSS_Pin */
  GPIO_InitStruct.Pin = H863_EN_Pin|LoRa_M0_Pin|LoRa_M1_Pin|Valve_EN_Pin
                          |Water_pump_E_Pin|SPI_NSS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : SPI_SDC_Pin SPI_CLK_Pin SPI_MOSI_Pin LCD_SHUT_Pin */
  GPIO_InitStruct.Pin = SPI_SDC_Pin|SPI_CLK_Pin|SPI_MOSI_Pin|LCD_SHUT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : SPI_MISO_Pin */
  GPIO_InitStruct.Pin = SPI_MISO_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SPI_MISO_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : FLOW_IN_Pin */
  GPIO_InitStruct.Pin = FLOW_IN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(FLOW_IN_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : SPI2_CS_Pin FLOW_EN_Pin */
  GPIO_InitStruct.Pin = SPI2_CS_Pin|FLOW_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PMU_EN1_Pin PMU_EN2_Pin PMU_EN4_Pin BEEP_Pin
                           LCD_RST_Pin SSD2828_LCD_RESET_Pin */
  GPIO_InitStruct.Pin = PMU_EN1_Pin|PMU_EN2_Pin|PMU_EN4_Pin|BEEP_Pin
                          |LCD_RST_Pin|SSD2828_LCD_RESET_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pin : PMU_EN3_Pin */
  GPIO_InitStruct.Pin = PMU_EN3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(PMU_EN3_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LCDBL_EN_Pin */
  GPIO_InitStruct.Pin = LCDBL_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(LCDBL_EN_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : ON_OFF_KEY_Pin SET_KEY_Pin UP_KEY_Pin */
  GPIO_InitStruct.Pin = ON_OFF_KEY_Pin|SET_KEY_Pin|UP_KEY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* Keep RESET low while overriding generated push-pull mode. A released
     open drain requires an external pull-up to the panel's switched 1V8. */
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = LCD_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LCD_RST_GPIO_Port, &GPIO_InitStruct);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  (void)DebugLog_Printf("[FATAL] stage=%s; startup stopped.\r\n", DebugLog_GetStage());
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
