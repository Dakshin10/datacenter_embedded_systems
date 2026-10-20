/**
 ******************************************************************************
 * @file    sd_spi.c
 * @brief   SD Card SPI1 Driver for STM32F411 / FatFs Integration
 ******************************************************************************
 */

#include "sd_spi.h"

/* SD Commands (SPI mode) */
#define CMD0    0   /* GO_IDLE_STATE */
#define CMD1    1   /* SEND_OP_COND (MMC) */
#define CMD8    8   /* SEND_IF_COND */
#define CMD9    9   /* SEND_CSD */
#define CMD10   10  /* SEND_CID */
#define CMD12   12  /* STOP_TRANSMISSION */
#define CMD16   16  /* SET_BLOCKLEN */
#define CMD17   17  /* READ_SINGLE_BLOCK */
#define CMD18   18  /* READ_MULTIPLE_BLOCK */
#define CMD24   24  /* WRITE_BLOCK */
#define CMD25   25  /* WRITE_MULTIPLE_BLOCK */
#define CMD55   55  /* APP_CMD (Lead-in for ACMD) */
#define CMD58   58  /* READ_OCR */
#define ACMD41  41  /* SD_SEND_OP_COND */

/* Internal state variables */
static uint8_t CardType = SD_TYPE_UNKNOWN;

/* Small delay helper */
static void SD_Delay(volatile uint32_t count)
{
    while (count--) {
        __NOP();
    }
}

/**
 * @brief  Asserts Chip Select (CS = LOW)
 */
void SD_CS_Assert(void)
{
    SD_CS_LOW();
}

/**
 * @brief  Deasserts Chip Select (CS = HIGH)
 */
void SD_CS_Deassert(void)
{
    SD_CS_HIGH();
}

/**
 * @brief  Sets SPI1 baud rate prescaler to /256 (~62.5kHz - 390kHz).
 *         Required during SD card power-on and initialization.
 */
void SD_SPI_SetSpeedLow(void)
{
    SD_SPI->CR1 &= ~SPI_CR1_SPE;
    SD_SPI->CR1 |= (SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0); /* Prescaler 256 */
    SD_SPI->CR1 |= SPI_CR1_SPE;
}

/**
 * @brief  Sets SPI1 baud rate prescaler to /8 (~12.5MHz at 100MHz APB2).
 *         Fast clock for data read/write transfers after card is initialized.
 */
void SD_SPI_SetSpeedHigh(void)
{
    SD_SPI->CR1 &= ~SPI_CR1_SPE;
    SD_SPI->CR1 &= ~SPI_CR1_BR;
    SD_SPI->CR1 |= SPI_CR1_BR_1; /* Prescaler 8 (~12.5MHz) */
    SD_SPI->CR1 |= SPI_CR1_SPE;
}

/**
 * @brief  Configures GPIOA pins (PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI)
 *         and initializes SPI1 peripheral in Master mode (Mode 0).
 */
void SD_SPI_Init(void)
{
    /* 1. Enable GPIOA and SPI1 peripheral clocks */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->AHB1ENR; /* Delay after clock enable */

    /* 2. Configure PA4 as Chip Select (CS) Output */
    /* Set MODER: 01 (General purpose output mode) */
    SD_CS_GPIO_PORT->MODER &= ~(3U << (SD_CS_PIN * 2));
    SD_CS_GPIO_PORT->MODER |= (1U << (SD_CS_PIN * 2));
    /* Set OSPEEDR: 11 (Very high speed) */
    SD_CS_GPIO_PORT->OSPEEDR |= (3U << (SD_CS_PIN * 2));
    /* Set PUPDR: 01 (Pull-up) */
    SD_CS_GPIO_PORT->PUPDR &= ~(3U << (SD_CS_PIN * 2));
    SD_CS_GPIO_PORT->PUPDR |= (1U << (SD_CS_PIN * 2));
    /* Default state: Deasserted (HIGH) */
    SD_CS_HIGH();

    /* 3. Configure PA5 (SCK), PA6 (MISO), PA7 (MOSI) as Alternate Function AF5 */
    /* SCK: PA5 */
    GPIOA->MODER &= ~(3U << (SD_SPI_SCK_PIN * 2));
    GPIOA->MODER |= (2U << (SD_SPI_SCK_PIN * 2)); /* AF mode */
    GPIOA->OSPEEDR |= (3U << (SD_SPI_SCK_PIN * 2));
    GPIOA->PUPDR &= ~(3U << (SD_SPI_SCK_PIN * 2));
    GPIOA->AFR[0] &= ~(0xFU << (SD_SPI_SCK_PIN * 4));
    GPIOA->AFR[0] |= (5U << (SD_SPI_SCK_PIN * 4)); /* AF5 */

    /* MISO: PA6 (with pull-up for SD bus idle state) */
    GPIOA->MODER &= ~(3U << (SD_SPI_MISO_PIN * 2));
    GPIOA->MODER |= (2U << (SD_SPI_MISO_PIN * 2)); /* AF mode */
    GPIOA->OSPEEDR |= (3U << (SD_SPI_MISO_PIN * 2));
    GPIOA->PUPDR &= ~(3U << (SD_SPI_MISO_PIN * 2));
    GPIOA->PUPDR |= (1U << (SD_SPI_MISO_PIN * 2)); /* Pull-up */
    GPIOA->AFR[0] &= ~(0xFU << (SD_SPI_MISO_PIN * 4));
    GPIOA->AFR[0] |= (5U << (SD_SPI_MISO_PIN * 4)); /* AF5 */

    /* MOSI: PA7 */
    GPIOA->MODER &= ~(3U << (SD_SPI_MOSI_PIN * 2));
    GPIOA->MODER |= (2U << (SD_SPI_MOSI_PIN * 2)); /* AF mode */
    GPIOA->OSPEEDR |= (3U << (SD_SPI_MOSI_PIN * 2));
    GPIOA->PUPDR &= ~(3U << (SD_SPI_MOSI_PIN * 2));
    GPIOA->AFR[0] &= ~(0xFU << (SD_SPI_MOSI_PIN * 4));
    GPIOA->AFR[0] |= (5U << (SD_SPI_MOSI_PIN * 4)); /* AF5 */

    /* 4. Configure SPI1 in Master Mode, Mode 0 (CPOL=0, CPHA=0), Software NSS */
    SD_SPI->CR1 = 0;
    SD_SPI->CR2 = 0;
    SD_SPI->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                  (SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0); /* Low speed initially */
    SD_SPI->CR1 |= SPI_CR1_SPE; /* Enable SPI */

    /* Drain any pending data */
    (void)SD_SPI->DR;
    (void)SD_SPI->SR;
}

/**
 * @brief  Transfers a single byte over SPI1 (Full Duplex)
 */
uint8_t SD_SPI_Transfer(uint8_t data)
{
    uint32_t timeout = 500000;

    while (!(SD_SPI->SR & SPI_SR_TXE)) {
        if (--timeout == 0) return 0xFF;
    }
    SD_SPI->DR = data;

    timeout = 500000;
    while (!(SD_SPI->SR & SPI_SR_RXNE)) {
        if (--timeout == 0) return 0xFF;
    }

    return (uint8_t)(SD_SPI->DR);
}

/**
 * @brief  Waits until SD card is no longer busy (MISO lines release to 0xFF)
 */
static uint8_t SD_WaitReady(void)
{
    uint32_t timeout = 500000;
    uint8_t res;

    do {
        res = SD_SPI_Transfer(0xFF);
    } while ((res != 0xFF) && (--timeout));

    return (res == 0xFF) ? SD_OK : SD_ERR_TIMEOUT;
}

/**
 * @brief  Sends a 6-byte command packet over SPI and retrieves the R1 response.
 */
uint8_t SD_SendCommand(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    uint8_t res;
    uint32_t retry = 0;

    /* Deselect & select to ensure bus synchronization */
    SD_CS_Deassert();
    SD_SPI_Transfer(0xFF);
    SD_CS_Assert();
    SD_SPI_Transfer(0xFF);

    if (cmd != CMD0 && cmd != CMD12) {
        if (SD_WaitReady() != SD_OK) {
            return 0xFF;
        }
    }

    /* Send 6-byte command packet */
    SD_SPI_Transfer(0x40 | cmd);
    SD_SPI_Transfer((uint8_t)(arg >> 24));
    SD_SPI_Transfer((uint8_t)(arg >> 16));
    SD_SPI_Transfer((uint8_t)(arg >> 8));
    SD_SPI_Transfer((uint8_t)(arg));
    SD_SPI_Transfer(crc);

    /* Skip stuff byte if CMD12 */
    if (cmd == CMD12) {
        SD_SPI_Transfer(0xFF);
    }

    /* Wait for R1 response (MSB is always 0) */
    do {
        res = SD_SPI_Transfer(0xFF);
        retry++;
    } while ((res & 0x80) && (retry < 500));

    return res;
}

/**
 * @brief  Initializes SD card in SPI mode.
 * @retval 0 on success (SD_OK), non-zero error code on failure.
 */
uint8_t SD_Init(void)
{
    uint8_t res;
    uint8_t ocr[4];
    uint32_t retry;

    CardType = SD_TYPE_UNKNOWN;

    /* 1. Initialize SPI1 peripheral and pins at low speed (<400 kHz) */
    SD_SPI_Init();
    SD_SPI_SetSpeedLow();

    /* 2. Power-on clock trains: Send >= 74 dummy clocks with CS HIGH */
    SD_CS_Deassert();
    for (uint32_t i = 0; i < 16; i++) {
        SD_SPI_Transfer(0xFF);
    }

    /* 3. Send CMD0 to enter IDLE state (SPI mode) */
    retry = 100;
    do {
        res = SD_SendCommand(CMD0, 0, 0x95);
        SD_Delay(100);
    } while ((res != 0x01) && (--retry));

    if (res != 0x01) {
        SD_CS_Deassert();
        SD_SPI_Transfer(0xFF);
        return SD_ERR_CMD0;
    }

    /* 4. Send CMD8 to check voltage range & card version (V2+) */
    res = SD_SendCommand(CMD8, 0x000001AA, 0x87);

    if (res == 0x01) {
        /* SD Version 2.0 or later: Read remaining 4 bytes of R7 */
        for (uint32_t i = 0; i < 4; i++) {
            ocr[i] = SD_SPI_Transfer(0xFF);
        }

        /* Verify check pattern 0xAA */
        if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
            /* Initialize card with ACMD41 with HCS bit (0x40000000) */
            retry = 2000;
            do {
                res = SD_SendCommand(CMD55, 0, 0x01);
                if (res <= 0x01) {
                    res = SD_SendCommand(ACMD41, 0x40000000, 0x01);
                }
                SD_Delay(200);
            } while ((res != 0x00) && (--retry));

            if (res == 0x00) {
                /* Check capacity via CMD58 (READ_OCR) */
                if (SD_SendCommand(CMD58, 0, 0x01) == 0x00) {
                    for (uint32_t i = 0; i < 4; i++) {
                        ocr[i] = SD_SPI_Transfer(0xFF);
                    }
                    /* Bit 30 of OCR indicates High Capacity (SDHC/SDXC) */
                    if (ocr[0] & 0x40) {
                        CardType = SD_TYPE_V2HC; /* Block addressing */
                    } else {
                        CardType = SD_TYPE_V2SC; /* Byte addressing */
                    }
                }
            } else {
                SD_CS_Deassert();
                SD_SPI_Transfer(0xFF);
                return SD_ERR_ACMD41;
            }
        }
    } else {
        /* SD Version 1.x or MMC: ACMD41 with arg 0 */
        retry = 2000;
        do {
            res = SD_SendCommand(CMD55, 0, 0x01);
            if (res <= 0x01) {
                res = SD_SendCommand(ACMD41, 0, 0x01);
            }
            SD_Delay(200);
        } while ((res != 0x00) && (--retry));

        if (res == 0x00) {
            CardType = SD_TYPE_V1;
        } else {
            /* Fallback to MMC CMD1 */
            retry = 2000;
            do {
                res = SD_SendCommand(CMD1, 0, 0x01);
                SD_Delay(200);
            } while ((res != 0x00) && (--retry));

            if (res == 0x00) {
                CardType = SD_TYPE_MMC;
            } else {
                SD_CS_Deassert();
                SD_SPI_Transfer(0xFF);
                return SD_ERROR;
            }
        }

        /* Force 512-byte block size for SDSC / MMC */
        if (SD_SendCommand(CMD16, SD_BLOCK_SIZE, 0x01) != 0x00) {
            SD_CS_Deassert();
            SD_SPI_Transfer(0xFF);
            return SD_ERROR;
        }
    }

    SD_CS_Deassert();
    SD_SPI_Transfer(0xFF);

    /* 5. Switch to high-speed SPI clock (~12.5MHz) */
    SD_SPI_SetSpeedHigh();

    return SD_OK;
}

/**
 * @brief  Reads a 512-byte sector from the SD card.
 * @param  sector: Sector / LBA number
 * @param  buffer: Target RAM buffer (must be at least 512 bytes)
 * @retval 1 on success, 0 on failure (diskio convention).
 */
uint8_t SD_ReadBlock(uint32_t sector, uint8_t *buffer)
{
    uint8_t res;
    uint32_t addr;
    uint32_t timeout = 500000;

    if (!buffer) return 0;

    /* Address is block-addressed for SDHC/SDXC, byte-addressed for SDSC */
    if (CardType & SD_TYPE_V2HC) {
        addr = sector;
    } else {
        addr = sector << 9;
    }

    /* Send CMD17: READ_SINGLE_BLOCK */
    res = SD_SendCommand(CMD17, addr, 0x01);
    if (res != 0x00) {
        SD_CS_Deassert();
        SD_SPI_Transfer(0xFF);
        return 0;
    }

    /* Wait for Data Token (0xFE) */
    do {
        res = SD_SPI_Transfer(0xFF);
    } while ((res != 0xFE) && (--timeout));

    if (res != 0xFE) {
        SD_CS_Deassert();
        SD_SPI_Transfer(0xFF);
        return 0;
    }

    /* Read 512 bytes */
    for (uint32_t i = 0; i < SD_BLOCK_SIZE; i++) {
        buffer[i] = SD_SPI_Transfer(0xFF);
    }

    /* Read & discard 2 CRC bytes */
    SD_SPI_Transfer(0xFF);
    SD_SPI_Transfer(0xFF);

    SD_CS_Deassert();
    SD_SPI_Transfer(0xFF);

    return 1; /* Success */
}

/**
 * @brief  Writes a 512-byte sector to the SD card.
 * @param  sector: Sector / LBA number
 * @param  buffer: Source RAM buffer
 * @retval 1 on success, 0 on failure (diskio convention).
 */
uint8_t SD_WriteBlock(uint32_t sector, const uint8_t *buffer)
{
    uint8_t res;
    uint32_t addr;
    uint32_t timeout;

    if (!buffer) return 0;

    if (CardType & SD_TYPE_V2HC) {
        addr = sector;
    } else {
        addr = sector << 9;
    }

    /* Wait until the card is ready before sending command */
    if (SD_WaitReady() != SD_OK) {
        return 0;
    }

    /* Send CMD24: WRITE_BLOCK */
    res = SD_SendCommand(CMD24, addr, 0x01);
    if (res != 0x00) {
        SD_CS_Deassert();
        SD_SPI_Transfer(0xFF);
        return 0;
    }

    /* Send 1 dummy byte (N_WR) and Start Block Token (0xFE) */
    SD_SPI_Transfer(0xFF);
    SD_SPI_Transfer(0xFE);

    /* Write 512 data bytes */
    for (uint32_t i = 0; i < SD_BLOCK_SIZE; i++) {
        SD_SPI_Transfer(buffer[i]);
    }

    /* Send 2 dummy CRC bytes */
    SD_SPI_Transfer(0xFF);
    SD_SPI_Transfer(0xFF);

    /* Wait for and read Data Response Token
     * Card may send 0 to 8 bytes of 0xFF (N_CR) before the token.
     * Accepted format: 0xxx00101b -> (res & 0x1F) == 0x05
     */
    timeout = 10000;
    do {
        res = SD_SPI_Transfer(0xFF);
    } while ((res == 0xFF) && (--timeout));

    if ((res & 0x1F) != 0x05) {
        SD_CS_Deassert();
        SD_SPI_Transfer(0xFF);
        return 0;
    }

    /* Wait for card to enter busy state (MISO pulled low) */
    timeout = 10000;
    do {
        res = SD_SPI_Transfer(0xFF);
    } while ((res == 0xFF) && (--timeout));

    /* Wait while card finishes writing to flash (MISO held LOW while busy) */
    if (res != 0xFF) {
        timeout = 1000000;
        do {
            res = SD_SPI_Transfer(0xFF);
        } while ((res != 0xFF) && (--timeout));

        if (res != 0xFF) {
            SD_CS_Deassert();
            SD_SPI_Transfer(0xFF);
            return 0;
        }
    }

    SD_CS_Deassert();
    SD_SPI_Transfer(0xFF);

    return 1;
}

/**
 * @brief  Queries CSD register and calculates total number of 512-byte sectors.
 * @retval Sector count, or 0 on failure.
 */
uint32_t SD_GetSectorCount(void)
{
    uint8_t csd[16];
    uint8_t res;
    uint32_t timeout = 500000;

    res = SD_SendCommand(CMD9, 0, 0x01); /* CMD9: SEND_CSD */
    if (res != 0x00) {
        SD_CS_Deassert();
        SD_SPI_Transfer(0xFF);
        return 0;
    }

    /* Wait for Data Token (0xFE) */
    do {
        res = SD_SPI_Transfer(0xFF);
    } while ((res != 0xFE) && (--timeout));

    if (res != 0xFE) {
        SD_CS_Deassert();
        SD_SPI_Transfer(0xFF);
        return 0;
    }

    /* Read 16-byte CSD register */
    for (uint32_t i = 0; i < 16; i++) {
        csd[i] = SD_SPI_Transfer(0xFF);
    }

    /* Discard 2 CRC bytes */
    SD_SPI_Transfer(0xFF);
    SD_SPI_Transfer(0xFF);

    SD_CS_Deassert();
    SD_SPI_Transfer(0xFF);

    /* Parse CSD Structure */
    if ((csd[0] >> 6) == 1) {
        /* CSD Version 2.0 (SDHC / SDXC) */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                          ((uint32_t)csd[8] << 8) |
                          ((uint32_t)csd[9]);
        return (c_size + 1) * 1024;
    } else if ((csd[0] >> 6) == 0) {
        /* CSD Version 1.0 (SDSC) */
        uint32_t c_size = (((uint32_t)csd[6] & 0x03) << 10) |
                          ((uint32_t)csd[7] << 2) |
                          ((uint32_t)(csd[8] & 0xC0) >> 6);
        uint32_t c_size_mult = (((uint32_t)csd[9] & 0x03) << 1) |
                               ((uint32_t)(csd[10] & 0x80) >> 7);
        uint32_t read_bl_len = csd[5] & 0x0F;
        uint32_t block_nr = (c_size + 1) * (1 << (c_size_mult + 2));
        return (block_nr * (1 << read_bl_len)) / SD_BLOCK_SIZE;
    }

    return 0;
}

/**
 * @brief  Returns detected SD card type.
 */
uint8_t SD_GetCardType(void)
{
    return CardType;
}
