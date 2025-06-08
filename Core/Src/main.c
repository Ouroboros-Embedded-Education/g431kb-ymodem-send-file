/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "ymodem.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "timers.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct{
	bool fileIsOpen;

	ymodem_t Ymodem;

	uint32_t fSize;
	uint32_t fReceived;

	TaskHandle_t task;
	QueueHandle_t RxQueue;
	SemaphoreHandle_t TxSemaphore;
	TimerHandle_t RstTimer;
	bool started;
}receive_file_t;

typedef struct{
	uint8_t raw[64];
	uint16_t len;
}rec_data_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart1_tx;
DMA_HandleTypeDef hdma_usart2_rx;
DMA_HandleTypeDef hdma_usart2_tx;

/* USER CODE BEGIN PV */
receive_file_t _RecFile = {0};
uint8_t data;
uint32_t recBytes = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/*
 * UART Callbacks
 */
/************************************************
 ********* 		UART CALLBACKS
 ************************************************/
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart){
	if (huart->Instance == USART1){
		BaseType_t taskWoken = pdFALSE;

		xSemaphoreGiveFromISR(_RecFile.TxSemaphore, &taskWoken);
	}
	else if (huart->Instance == USART2){

	}
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart){
	if (huart->Instance == USART1){
		BaseType_t taskWoken = pdFALSE;

		if (_RecFile.started == false){
			return;
		}
		if (xQueueSendFromISR(_RecFile.RxQueue, &data, &taskWoken) != pdTRUE){
			// lost byte
		}
		HAL_UART_Receive_DMA(huart, &data, sizeof(data));
		recBytes++;
	}
	else if (huart->Instance == USART2){

	}
}

/************************************************
 ********* 		YMODEM LIBRARY CALLBACKS
 ************************************************/
ymodem_err_e ymodem_FileCallback(ymodem_t *ymodem, ymodem_file_cb_e e, uint8_t *data, uint32_t len){
	char *FileName;
	uint32_t i;

	switch (e){
	case YMODEM_FILE_CB_NAME:
		if (_RecFile.fileIsOpen == false){
			// When we receive a File, data contains the filename
			_RecFile.fileIsOpen = true;
			_RecFile.fSize = len;
			_RecFile.fReceived = 0;
			FileName = (char*)data;
			printf(">> Receiving File request: \r\n");
			printf(">> \tFilename: %s\r\n", FileName);
			printf(">> \tLength: %lu\r\n", len);
		}
		break;

	case YMODEM_FILE_CB_DATA:
		printf(">> Received %lu bytes from Host.\r\n\t", len);
		_RecFile.fReceived += len;
		// Check if received chunk will overflow total file len
		if (_RecFile.fReceived > _RecFile.fSize){
			// fix the chunck length and ignore unused bytes
			_RecFile.fReceived -= len;
			len = _RecFile.fSize - _RecFile.fReceived;
			_RecFile.fReceived += len;
		}
		for (i=0 ; i<len ; i++){
			if (i > 0 && i%8 == 0){
				printf("\r\n\t");
			}
			printf("%02X ", data[i]);
		}
		printf("\r\n");
		break;

	case YMODEM_FILE_CB_END:
	case YMODEM_FILE_CB_ABORTED:
		if (_RecFile.fileIsOpen == true){
			printf(">> File is closed.\r\n");
			_RecFile.fileIsOpen  = false;
		}
		break;
	}

	return YMODEM_OK;
}

/************************************************
 ********* 		RESET Timer
 ************************************************/
void _RecFile_Abort_Timeout(TimerHandle_t xTimer){
	ymodem_Reset(&_RecFile.Ymodem);
}

/************************************************
 ********* 		SERIAL TRANSMIT WRAPPER
 ************************************************/
uint8_t _uart_tx(uint8_t *data, uint32_t len){
	HAL_UART_Transmit(&huart1, data, len, 100);
	return 0;
}

/************************************************
 ********* 		FILE RECEIVE TASK
 ************************************************/
void task_receive_file(void *pvParams){
	uint8_t C = 'C';
	uint8_t RecData;
	ymodem_err_e ymodemRet;
	BaseType_t queueRet;

 	printf(">> Preparing YMODEM Application.\r\n");
	_RecFile.TxSemaphore = xSemaphoreCreateBinary();
	_RecFile.RxQueue = xQueueCreate(512, sizeof(data));
	_RecFile.RstTimer = xTimerCreate("Reset Ymodem",
									 pdMS_TO_TICKS(1500),
									 pdFALSE,
									 NULL,
									 _RecFile_Abort_Timeout);
	ymodem_Init(&_RecFile.Ymodem, _uart_tx);
	_RecFile.started = true;
	HAL_UART_Receive_DMA(&huart1, &data, sizeof(data));
	printf(">> YMODEM is ready to receive Files.\r\n");
	while(1) {
		queueRet = xQueueReceive(_RecFile.RxQueue, &RecData, pdMS_TO_TICKS(1000));
		if (queueRet == pdTRUE){
			if (xTimerIsTimerActive(_RecFile.RstTimer) != pdFALSE){
				xTimerStart(_RecFile.RstTimer, 0);
			}
			else{
				xTimerReset(_RecFile.RstTimer, 0);
			}

			ymodemRet = ymodem_ReceiveByte(&_RecFile.Ymodem, RecData);
			switch (ymodemRet){
			case YMODEM_OK:

				break;
			case YMODEM_TX_PENDING:

				break;
			case YMODEM_ABORTED:
				ymodem_Reset(&_RecFile.Ymodem);
				break;
			case YMODEM_WRITE_ERR:
				ymodem_Reset(&_RecFile.Ymodem);
				break;
			case YMODEM_SIZE_ERR:
				ymodem_Reset(&_RecFile.Ymodem);
				break;
			case YMODEM_COMPLETE:
				ymodem_Reset(&_RecFile.Ymodem);
				break;
			}
		}
		else{
			_uart_tx(&C, 1);
		}
	}
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
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  HAL_Delay(200);
  printf(">> Starting YMODEM Application\r\n");

  xTaskCreate(task_receive_file, "File Receive", 512, NULL, 1, NULL);

  printf(">> Starting RTOS Scheduler.\r\n");
  vTaskStartScheduler();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV2;
  RCC_OscInitStruct.PLL.PLLN = 25;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
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
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart1) != HAL_OK)
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
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart2, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart2, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
  /* DMA1_Channel2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
  /* DMA1_Channel3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
  /* DMA1_Channel4_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
int __io_putchar (int ch){
	HAL_UART_Transmit(&huart2, (uint8_t*)&ch, 1, 10);
	return ch;
}
/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM17 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM17)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
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
