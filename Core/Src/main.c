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
#include "nor.h"
#include "lfs.h"

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

	lfs_file_t lFile;

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
SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim6;

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

nor_t NORH;
lfs_t LFSH;
struct lfs_config Lfs_Config;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM6_Init(void);
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

			lfs_file_open(&LFSH, &_RecFile.lFile, FileName, LFS_O_CREAT | LFS_O_RDWR);
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

		lfs_file_write(&LFSH, &_RecFile.lFile, data, len);
		break;

	case YMODEM_FILE_CB_END:
	case YMODEM_FILE_CB_ABORTED:
		if (_RecFile.fileIsOpen == true){
			printf(">> File is closed.\r\n");
			_RecFile.fileIsOpen  = false;

			lfs_file_close(&LFSH, &_RecFile.lFile);
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
 ********* 		INITIALIZE FLASH NOR
 ************************************************/
void _w25_assert(){
	HAL_GPIO_WritePin(NOR_CS_GPIO_Port, NOR_CS_Pin, GPIO_PIN_RESET);
}

void _w25_deassert(){
	HAL_GPIO_WritePin(NOR_CS_GPIO_Port, NOR_CS_Pin, GPIO_PIN_SET);
}

void _w25_spi_send(uint8_t* data, uint32_t len){
	if (HAL_SPI_Transmit(&hspi1, data, len, 100) != HAL_OK){
		return;
	}
}

void _w25_spi_receive(uint8_t* data, uint32_t len){
	if (HAL_SPI_Receive(&hspi1, data, len, 100) != HAL_OK){
		return;
	}
}

void _w25_delay_us(uint32_t us){
	uint32_t a;

	__HAL_TIM_SET_AUTORELOAD(&htim6, UINT16_MAX);
	HAL_TIM_Base_Start(&htim6);
	do {
		__HAL_TIM_SET_COUNTER(&htim6, 0);
		if (us >= UINT16_MAX){
			a = UINT16_MAX - 1;
		}
		else{
			a = us;
		}
		while(__HAL_TIM_GET_COUNTER(&htim6) < a);

		us -= a;
	}while (us > 0);
	HAL_TIM_Base_Stop(&htim6);

}

void W25_Init(){
	NORH.config.CsAssert = _w25_assert;
	NORH.config.CsDeassert = _w25_deassert;
	NORH.config.DelayUs = _w25_delay_us;
	NORH.config.MutexLockFxn = NULL;
	NORH.config.MutexUnlockFxn = NULL;
	NORH.config.SpiRxFxn = _w25_spi_receive;
	NORH.config.SpiTxFxn = _w25_spi_send;

	assert(NOR_Init(&NORH) == NOR_OK);
}

/************************************************
 ********* 		INITIALIZE LITTLEFS
 ************************************************/
int __read_sector(const struct lfs_config *c, lfs_block_t block,
                  lfs_off_t off, void *buffer, lfs_size_t size){
    uint32_t address = (block * c->block_size) + off;

    if (NOR_ReadBytes(&NORH, (uint8_t*)buffer, address, size) != NOR_OK){
    	return -1;
    }

	return LFS_ERR_OK;
}

int __write_sector(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, const void *buffer, lfs_size_t size){
    uint32_t address = (block * c->block_size) + off;

    if (NOR_WriteBytes(&NORH, (uint8_t*)buffer, address, size) != NOR_OK){
    	return -1;
    }

	return LFS_ERR_OK;
}

int __erase_sector(const struct lfs_config *c, lfs_block_t block){
    if (NOR_EraseSector(&NORH, block) !=  NOR_OK){
    	return -1;
    }

	return LFS_ERR_OK;
}

int __sync_sector(const struct lfs_config *c){
    return 0;
}


void LittleFs_Init(){
    enum lfs_error err;

    Lfs_Config.read = __read_sector;
    Lfs_Config.prog = __write_sector;
    Lfs_Config.erase = __erase_sector;
    Lfs_Config.sync = __sync_sector;

    Lfs_Config.block_size = NORH.info.u16SectorSize;
    Lfs_Config.block_count = NORH.info.u32SectorCount;
    Lfs_Config.lookahead_size = NORH.info.u16SectorSize/8;
    Lfs_Config.read_size = 64;
    Lfs_Config.prog_size = 64;
    Lfs_Config.cache_size = 64;
    Lfs_Config.block_cycles = 256;

    err = lfs_mount(&LFSH, &Lfs_Config);
    if (err != LFS_ERR_OK){
    	err = lfs_format(&LFSH, &Lfs_Config);
		err = lfs_mount(&LFSH, &Lfs_Config);
    }
    assert (err == LFS_ERR_OK);
}


/************************************************
 ********* 		FILE RECEIVE TASK
 ************************************************/
void task_receive_file(void *pvParams){
	uint8_t C = 'C';
	uint8_t RecData;
	ymodem_err_e ymodemRet;
	BaseType_t queueRet;
	bool readFile = false;
	char fileToRead[32] = "message.txt";

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
		if (readFile == true){
			lfs_file_t file;
			lfs_size_t rd;
			char temp[128];

			readFile = false;
			lfs_file_open(&LFSH, &file, fileToRead, LFS_O_RDONLY);
			printf(">> Reading data from file %s\r\n ", fileToRead);
			do{
				rd = lfs_file_read(&LFSH, &file, temp, 128);
				for (int i=0 ; i<rd ; i++){
					printf("%c", temp[i]);
					if (temp[i] == '\n'){
						printf("\r");
					}
				}
			}while (rd>0);
			lfs_file_close(&LFSH, &file);
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
  MX_SPI1_Init();
  MX_TIM6_Init();
  /* USER CODE BEGIN 2 */
  HAL_Delay(200);
  printf(">> Starting YMODEM Application\r\n");
  xTaskCreate(task_receive_file, "File Receive", 512, NULL, 1, NULL);

  printf(">> Starting LittleFs File System.\r\n");
  W25_Init();
  LittleFs_Init();

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
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM6_Init(void)
{

  /* USER CODE BEGIN TIM6_Init 0 */

  /* USER CODE END TIM6_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM6_Init 1 */

  /* USER CODE END TIM6_Init 1 */
  htim6.Instance = TIM6;
  htim6.Init.Prescaler = 99;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim6.Init.Period = 65535;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM6_Init 2 */

  /* USER CODE END TIM6_Init 2 */

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
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(NOR_CS_GPIO_Port, NOR_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : NOR_CS_Pin */
  GPIO_InitStruct.Pin = NOR_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(NOR_CS_GPIO_Port, &GPIO_InitStruct);

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
