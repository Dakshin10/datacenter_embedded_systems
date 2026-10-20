/**
 ******************************************************************************
 * @file    sd_spi.h
 * @brief   SD Card SPI1 Driver for STM32F411 / FatFs Integration
 ******************************************************************************
 */

#ifndef __SD_SPI_H
#define __SD_SPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx.h"
#include <stdint.h>

/* ============================================================================
 * Hardware Pin & Peripheral Configuration (STM32F411 SPI1)
 * ============================================================================
 * SPI1 Peripheral:
 *   SCK  -> PA5 (AF5)
 *   MISO -> PA6 (AF5, Internal Pull-up)
 *   MOSI -> PA7 (AF5)
 *
 * Chip Select (CS):
 *   CS   -> PA4 (General Purpose Output, Active LOW)
 * ============================================================================
 */
#define SD_SPI                  SPI1
#define SD_SPI_GPIO_PORT        GPIOA
#define SD_SPI_SCK_PIN          5       /* PA5 */
#define SD_SPI_MISO_PIN         6       /* PA6 */
#define SD_SPI_MOSI_PIN         7       /* PA7 */

#define SD_CS_GPIO_PORT         GPIOA
#define SD_CS_PIN               4       /* PA4 */

/* CS Control Macros (Active LOW) */
#define SD_CS_LOW()             (SD_CS_GPIO_PORT->BSRR = (1U << (SD_CS_PIN + 16)))
#define SD_CS_HIGH()            (SD_CS_GPIO_PORT->BSRR = (1U << SD_CS_PIN))

/* ============================================================================
 * Status & Error Return Codes
 * ============================================================================
 */
#define SD_OK                   0
#define SD_ERROR                1
#define SD_ERR_TIMEOUT          2
#define SD_ERR_CMD0             3
#define SD_ERR_CMD8             4
#define SD_ERR_ACMD41           5
#define SD_ERR_CMD58            6
#define SD_ERR_WRITE            7
#define SD_ERR_READ             8

/* Card Type Flags */
#define SD_TYPE_UNKNOWN         0x00
#define SD_TYPE_MMC             0x01    /* MMC Version 3 */
#define SD_TYPE_V1              0x02    /* SD Version 1.x Standard Capacity */
#define SD_TYPE_V2SC            0x04    /* SD Version 2.x Standard Capacity (Byte addr) */
#define SD_TYPE_V2HC            0x08    /* SD Version 2.x High Capacity (SDHC/SDXC, Block addr) */

/* Block size is fixed at 512 bytes for standard SD/FAT */
#define SD_BLOCK_SIZE           512

/* ============================================================================
 * FatFs Disk I/O Compatible API Prototypes
 * ============================================================================
 */
/**
 * @brief  Initializes SPI1 and SD card in SPI mode.
 * @retval 0 (SD_OK) on success, non-zero error code on failure.
 */
uint8_t  SD_Init(void);
#define  SD_SPI_Initialize()    SD_Init()

/**
 * @brief  Reads a 512-byte sector from the SD card.
 * @param  sector: Logical sector address
 * @param  buffer: Pointer to 512-byte destination buffer
 * @retval 1 on success, 0 on failure.
 */
uint8_t  SD_ReadBlock(uint32_t sector, uint8_t *buffer);

/**
 * @brief  Writes a 512-byte sector to the SD card.
 * @param  sector: Logical sector address
 * @param  buffer: Pointer to 512-byte source buffer
 * @retval 1 on success, 0 on failure.
 */
uint8_t  SD_WriteBlock(uint32_t sector, const uint8_t *buffer);

/**
 * @brief  Gets total sector count of the SD card.
 * @retval Number of 512-byte sectors, or 0 on failure.
 */
uint32_t SD_GetSectorCount(void);

/**
 * @brief  Returns detected SD card type (SD_TYPE_V2HC, SD_TYPE_V2SC, etc.).
 */
uint8_t  SD_GetCardType(void);

/* ============================================================================
 * Low-level SPI Driver Functions
 * ============================================================================
 */
void    SD_SPI_Init(void);
void    SD_SPI_SetSpeedLow(void);
void    SD_SPI_SetSpeedHigh(void);
uint8_t SD_SPI_Transfer(uint8_t data);
void    SD_CS_Assert(void);
void    SD_CS_Deassert(void);
uint8_t SD_SendCommand(uint8_t cmd, uint32_t arg, uint8_t crc);

#ifdef __cplusplus
}
#endif

#endif /* __SD_SPI_H */
