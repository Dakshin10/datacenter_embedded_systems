/*
 * Project: Mini Multi-User Networked Data Center
 * MCU: STM32F411CEU6
 * OLED: SSD1306 128x64, I2C1 PB6/PB7, address 0x3C
 * ESP8266: USART2 PA2=TX, PA3=RX, 115200 baud
 * Clock assumption: HSI/PCLK1 = 16 MHz
 *
 * Wi-Fi:
 * SSID     = Dakshin
 * PASSWORD = DakshinData
 *
 * Features:
 * - OLED status display
 * - ESP8266 AT communication
 * - Connects to hotspot
 * - Reads IP and MAC address
 * - Avoids repeated join commands while connected
 */

#include "stm32f4xx.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sd_spi.h"
#include "ff.h"

#define OLED_ADDR       0x3CU
#define I2C_TIMEOUT     100000U
#define OLED_WIDTH      128U
#define OLED_PAGES      8U

#define WIFI_SSID       "Dakshin"
#define WIFI_PASSWORD   "DakshinData"

/* Web login accounts (max 15 chars each) */
#define ADMIN_USER      "admin"
#define ADMIN_PASS      "Admin@2026"
#define NORMAL_USER     "user"
#define NORMAL_PASS     "User@2026"

#define RX_SIZE         512U

static volatile uint32_t g_ms = 0U;
static char rx[RX_SIZE];
static uint16_t rx_len = 0U;

/* ---------------- SysTick ---------------- */

void SysTick_Handler(void)
{
    g_ms++;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = g_ms;
    while ((uint32_t)(g_ms - start) < ms)
        __NOP();
}

static void SysTick_Init(void)
{
    SysTick->LOAD = 16000U - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
}

/* ---------------- GPIO ---------------- */

static void GPIO_Init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN |
                    RCC_AHB1ENR_GPIOBEN;
    (void)RCC->AHB1ENR;

    /* OLED: PB6=SCL, PB7=SDA, AF4, open-drain */
    GPIOB->MODER &= ~((3U << 12U) | (3U << 14U));
    GPIOB->MODER |=  ((2U << 12U) | (2U << 14U));
    GPIOB->OTYPER |= (1U << 6U) | (1U << 7U);
    GPIOB->OSPEEDR |= (3U << 12U) | (3U << 14U);
    GPIOB->PUPDR &= ~((3U << 12U) | (3U << 14U));
    GPIOB->AFR[0] &= ~((0xFU << 24U) | (0xFU << 28U));
    GPIOB->AFR[0] |=  ((4U << 24U) | (4U << 28U));

    /* ESP8266 UART: PA2=USART2_TX, PA3=USART2_RX, AF7 */
    GPIOA->MODER &= ~((3U << 4U) | (3U << 6U));
    GPIOA->MODER |=  ((2U << 4U) | (2U << 6U));
    GPIOA->OTYPER &= ~((1U << 2U) | (1U << 3U));
    GPIOA->OSPEEDR |= (3U << 4U) | (3U << 6U);
    GPIOA->PUPDR &= ~((3U << 4U) | (3U << 6U));
    GPIOA->AFR[0] &= ~((0xFU << 8U) | (0xFU << 12U));
    GPIOA->AFR[0] |=  ((7U << 8U) | (7U << 12U));
}

/* ---------------- I2C1 ---------------- */

static void I2C1_Init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    (void)RCC->APB1ENR;

    I2C1->CR1 &= ~I2C_CR1_PE;
    RCC->APB1RSTR |= RCC_APB1RSTR_I2C1RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C1RST;

    /* PCLK1 = 16 MHz, I2C standard mode = 100 kHz */
    I2C1->CR2 = 16U;
    I2C1->CCR = 80U;
    I2C1->TRISE = 17U;
    I2C1->CR1 = I2C_CR1_PE;
}

static int I2C1_Write(uint8_t control,
                      const uint8_t *data,
                      uint16_t length)
{
    volatile uint32_t timeout;
    uint16_t i;

    timeout = I2C_TIMEOUT;
    while ((I2C1->SR2 & I2C_SR2_BUSY) != 0U)
    {
        if (--timeout == 0U)
            return -1;
    }

    I2C1->CR1 |= I2C_CR1_START;
    timeout = I2C_TIMEOUT;

    while ((I2C1->SR1 & I2C_SR1_SB) == 0U)
    {
        if (--timeout == 0U)
        {
            I2C1->CR1 |= I2C_CR1_STOP;
            return -1;
        }
    }

    I2C1->DR = (uint8_t)(OLED_ADDR << 1U);
    timeout = I2C_TIMEOUT;

    while ((I2C1->SR1 & I2C_SR1_ADDR) == 0U)
    {
        if ((I2C1->SR1 & I2C_SR1_AF) != 0U)
        {
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 |= I2C_CR1_STOP;
            return -1;
        }

        if (--timeout == 0U)
        {
            I2C1->CR1 |= I2C_CR1_STOP;
            return -1;
        }
    }

    (void)I2C1->SR1;
    (void)I2C1->SR2;

    timeout = I2C_TIMEOUT;

    while ((I2C1->SR1 & I2C_SR1_TXE) == 0U)
    {
        if (--timeout == 0U)
        {
            I2C1->CR1 |= I2C_CR1_STOP;
            return -1;
        }
    }

    I2C1->DR = control;

    for (i = 0U; i < length; i++)
    {
        timeout = I2C_TIMEOUT;

        while ((I2C1->SR1 & I2C_SR1_TXE) == 0U)
        {
            if (--timeout == 0U)
            {
                I2C1->CR1 |= I2C_CR1_STOP;
                return -1;
            }
        }

        I2C1->DR = data[i];
    }

    timeout = I2C_TIMEOUT;

    while ((I2C1->SR1 & I2C_SR1_BTF) == 0U)
    {
        if (--timeout == 0U)
        {
            I2C1->CR1 |= I2C_CR1_STOP;
            return -1;
        }
    }

    I2C1->CR1 |= I2C_CR1_STOP;
    return 0;
}

static void OLED_Command(uint8_t command)
{
    (void)I2C1_Write(0x00U, &command, 1U);
}

static void OLED_Init(void)
{
    delay_ms(100U);

    OLED_Command(0xAEU);
    OLED_Command(0xD5U);
    OLED_Command(0x80U);
    OLED_Command(0xA8U);
    OLED_Command(0x3FU);
    OLED_Command(0xD3U);
    OLED_Command(0x00U);
    OLED_Command(0x40U);
    OLED_Command(0x8DU);
    OLED_Command(0x14U);
    OLED_Command(0x20U);
    OLED_Command(0x02U);
    OLED_Command(0xA1U);
    OLED_Command(0xC8U);
    OLED_Command(0xDAU);
    OLED_Command(0x12U);
    OLED_Command(0x81U);
    OLED_Command(0x7FU);
    OLED_Command(0xD9U);
    OLED_Command(0xF1U);
    OLED_Command(0xDBU);
    OLED_Command(0x40U);
    OLED_Command(0xA4U);
    OLED_Command(0xA6U);
    OLED_Command(0xAFU);
}

/* ---------------- 5x7 font ---------------- */

static const uint8_t font[][5] =
{
    {0,0,0,0,0},
    {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},
    {0x3E,0x51,0x49,0x45,0x3E},
    {0,0x42,0x7F,0x40,0},
    {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
    {0,0x36,0x36,0,0},
    {0x08,0x08,0x08,0x08,0x08},
    {0,0x60,0x60,0,0}
};

static const uint8_t *Font_GetGlyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');

    if (c == ' ')
        return font[0];

    if (c >= 'A' && c <= 'Z')
        return font[1U + (uint8_t)(c - 'A')];

    if (c >= '0' && c <= '9')
        return font[27U + (uint8_t)(c - '0')];

    if (c == ':')
        return font[37];

    if (c == '-')
        return font[38];

    if (c == '.')
        return font[39];

    return font[0];
}

static void OLED_Clear(void)
{
    uint8_t page;
    uint8_t zeros[OLED_WIDTH] = {0};

    for (page = 0U; page < OLED_PAGES; page++)
    {
        OLED_Command((uint8_t)(0xB0U + page));
        OLED_Command(0x00U);
        OLED_Command(0x10U);
        (void)I2C1_Write(0x40U, zeros, OLED_WIDTH);
    }
}

static void OLED_PrintLine(uint8_t page, const char *text)
{
    uint8_t buffer[OLED_WIDTH] = {0};
    uint16_t len = 0U;
    uint16_t col;
    uint16_t i;

    if (page >= OLED_PAGES || text == 0)
        return;

    while (text[len] != '\0' && len < 21U)
        len++;

    col = (OLED_WIDTH - len * 6U) / 2U;

    for (i = 0U; i < len; i++)
    {
        const uint8_t *glyph = Font_GetGlyph(text[i]);
        uint8_t j;

        for (j = 0U; j < 5U; j++)
            buffer[col++] = glyph[j];

        buffer[col++] = 0U;
    }

    OLED_Command((uint8_t)(0xB0U + page));
    OLED_Command(0x00U);
    OLED_Command(0x10U);
    (void)I2C1_Write(0x40U, buffer, OLED_WIDTH);
}

static void OLED_Show3(const char *a,
                       const char *b,
                       const char *c)
{
    OLED_Clear();
    OLED_PrintLine(0U, a);
    OLED_PrintLine(2U, b);
    OLED_PrintLine(4U, c);
}

/* ---------------- USART2 / ESP8266 ---------------- */

static void USART2_Init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;

    USART2->CR1 = 0U;
    USART2->CR2 = 0U;
    USART2->CR3 = 0U;

    /* 115200 baud when USART2 clock is 16 MHz */
    USART2->BRR = 139U;

    USART2->CR1 = USART_CR1_TE |
                  USART_CR1_RE |
                  USART_CR1_RXNEIE |
                  USART_CR1_UE;

    NVIC_SetPriority(USART2_IRQn, 1U);
    NVIC_EnableIRQ(USART2_IRQn);
}

/* RX is interrupt-driven: the USART holds only one byte, so any blocking
   work (OLED, SD card, delays) used to drop incoming requests. 4 KB covers
   ~350 ms of continuous data at 115200 baud. */
#define UART_RX_SIZE    4096U

static volatile uint8_t  uart_rx_buf[UART_RX_SIZE];
static volatile uint16_t uart_rx_head = 0U;
static volatile uint16_t uart_rx_tail = 0U;

void USART2_IRQHandler(void)
{
    uint32_t sr = USART2->SR;

    if ((sr & (USART_SR_RXNE | USART_SR_ORE)) != 0U)
    {
        /* Reading DR after SR also clears an overrun */
        uint8_t c = (uint8_t)(USART2->DR & 0xFFU);
        uint16_t next = (uint16_t)((uart_rx_head + 1U) & (UART_RX_SIZE - 1U));

        if (next != uart_rx_tail)
        {
            uart_rx_buf[uart_rx_head] = c;
            uart_rx_head = next;
        }
    }
}

/* Returns the next received byte, or -1 if none is waiting */
static int UART_ReadByte(void)
{
    uint8_t c;

    if (uart_rx_tail == uart_rx_head)
        return -1;

    c = uart_rx_buf[uart_rx_tail];
    uart_rx_tail = (uint16_t)((uart_rx_tail + 1U) & (UART_RX_SIZE - 1U));
    return (int)c;
}

static void UART_SendString(const char *s)
{
    while (*s != '\0')
    {
        while ((USART2->SR & USART_SR_TXE) == 0U)
            __NOP();

        USART2->DR = (uint8_t)*s++;
    }
}

static void UART_FlushRx(void)
{
    uart_rx_tail = uart_rx_head;
}

static void ESP_Command(const char *command,
                        uint32_t timeout_ms)
{
    uint32_t start;
    uint32_t last_rx;

    UART_FlushRx();
    rx_len = 0U;
    memset(rx, 0, sizeof(rx));

    UART_SendString(command);
    UART_SendString("\r\n");

    start = g_ms;
    last_rx = start;

    while ((uint32_t)(g_ms - start) < timeout_ms)
    {
        int b = UART_ReadByte();

        if (b >= 0)
        {
            char c = (char)b;
            last_rx = g_ms;

            if (rx_len < RX_SIZE - 1U)
            {
                rx[rx_len++] = c;
                rx[rx_len] = '\0';
            }

            if (strstr(rx, "WIFI GOT IP") != 0)
                break;
        }
        else if (rx_len > 0U &&
                 (uint32_t)(g_ms - last_rx) > 500U)
        {
            break;
        }
    }
}

static uint8_t ESP_Has(const char *s)
{
    return (strstr(rx, s) != 0) ? 1U : 0U;
}

static void CopyQuotedValue(const char *response,
                            const char *key,
                            char *out,
                            uint16_t capacity)
{
    const char *p;
    uint16_t i = 0U;

    if (capacity == 0U)
        return;

    out[0] = '\0';

    p = strstr(response, key);

    if (p == 0)
        return;

    p += strlen(key);

    if (*p == '"')
        p++;

    while (*p != '\0' &&
           *p != '"' &&
           *p != '\r' &&
           *p != '\n' &&
           i < capacity - 1U)
    {
        out[i++] = *p++;
    }

    out[i] = '\0';
}

static uint8_t ESP_IsConnected(void)
{
    ESP_Command("AT+CWJAP?", 3000U);

    if (ESP_Has("+CWJAP:") && !ESP_Has("No AP"))
        return 1U;

    return 0U;
}

static uint8_t ESP_GetIP(char *out, uint16_t capacity)
{
    char temp[RX_SIZE];

    out[0] = '\0';

    ESP_Command("AT+CIFSR", 3000U);

    memcpy(temp, rx, sizeof(temp));
    temp[sizeof(temp) - 1U] = '\0';

    CopyQuotedValue(temp, "+CIFSR:STAIP,", out, capacity);

    if (out[0] == '\0')
        CopyQuotedValue(temp, "STAIP,", out, capacity);

    if (out[0] == '\0')
        CopyQuotedValue(temp, "STAIP:", out, capacity);

    if (out[0] == '\0' ||
        strstr(out, "0.0.0.0") != 0)
        return 0U;

    return 1U;
}

static void ESP_JoinHotspot(void)
{
    char command[128];

    strcpy(command, "AT+CWJAP_CUR=\"");
    strcat(command, WIFI_SSID);
    strcat(command, "\",\"");
    strcat(command, WIFI_PASSWORD);
    strcat(command, "\"");

    OLED_Show3("CONNECTING WIFI",
               "SSID " WIFI_SSID,
               "WAIT 20 SEC");

    ESP_Command(command, 20000U);
}

/* -------- FATFS SD CARD FILE TEST -------- */

static FATFS SD_FatFs;
static FIL SD_File;

static const char SD_TestText[] = "DATA CENTER TEST";

static char SD_ReadBuffer[32];

/* Test result codes */
#define FILE_TEST_OK          0U
#define FILE_TEST_MOUNT_FAIL  1U
#define FILE_TEST_OPEN_FAIL   2U
#define FILE_TEST_WRITE_FAIL  3U
#define FILE_TEST_READ_FAIL   4U
#define FILE_TEST_MATCH_FAIL  5U

static uint8_t SD_FileTest(void)
{
    FRESULT result;
    UINT bytesWritten = 0U;
    UINT bytesRead = 0U;

    /* 1. Mount the filesystem.
       This should initialize the card through diskio.c. */
    result = f_mount(&SD_FatFs, "0:", 1);

    if (result != FR_OK)
    {
        OLED_Show3("SD FILE TEST",
                   "MOUNT FAILED",
                   "CHECK DISKIO");
        return FILE_TEST_MOUNT_FAIL;
    }

    OLED_Show3("SD FILE TEST",
               "MOUNT OK",
               "CREATING FILE");

    delay_ms(1000U);

    /* 2. Create or overwrite test.txt */
    result = f_open(&SD_File,
                    "0:/test.txt",
                    FA_CREATE_ALWAYS | FA_WRITE);

    if (result != FR_OK)
    {
        OLED_Show3("SD FILE TEST",
                   "OPEN FAILED",
                   "CHECK FATFS");
        return FILE_TEST_OPEN_FAIL;
    }

    /* 3. Write the test message */
    result = f_write(&SD_File,
                     SD_TestText,
                     (UINT)(sizeof(SD_TestText) - 1U),
                     &bytesWritten);

    /* Always close an opened file */
    {
        FRESULT closeResult = f_close(&SD_File);

        if (result != FR_OK ||
            bytesWritten != sizeof(SD_TestText) - 1U ||
            closeResult != FR_OK)
        {
            char line2[22] = "RES:0 BW:00";
            char line3[22] = "CLOSE:0";
            line2[4] = (char)('0' + (result % 10U));
            line2[9] = (char)('0' + ((bytesWritten / 10U) % 10U));
            line2[10] = (char)('0' + (bytesWritten % 10U));
            line3[6] = (char)('0' + (closeResult % 10U));
            OLED_Show3("WRITE FAILED", line2, line3);
            delay_ms(3000U);
            return FILE_TEST_WRITE_FAIL;
        }
    }

    OLED_Show3("SD FILE TEST",
               "WRITE OK",
               "READING FILE");

    delay_ms(1000U);

    /* 4. Reopen the file in read-only mode */
    result = f_open(&SD_File,
                    "0:/test.txt",
                    FA_READ);

    if (result != FR_OK)
    {
        OLED_Show3("SD FILE TEST",
                   "READ OPEN FAIL",
                   "CHECK FILE");
        return FILE_TEST_READ_FAIL;
    }

    /* 5. Read the saved message into RAM */
    memset(SD_ReadBuffer, 0, sizeof(SD_ReadBuffer));

    result = f_read(&SD_File,
                    SD_ReadBuffer,
                    (UINT)(sizeof(SD_TestText) - 1U),
                    &bytesRead);

    {
        FRESULT closeResult = f_close(&SD_File);

        if (result != FR_OK ||
            closeResult != FR_OK ||
            bytesRead != sizeof(SD_TestText) - 1U)
        {
            OLED_Show3("SD FILE TEST",
                       "READ FAILED",
                       "CHECK CARD");
            return FILE_TEST_READ_FAIL;
        }
    }

    /* 6. Compare the original and retrieved text */
    if (memcmp(SD_TestText,
               SD_ReadBuffer,
               sizeof(SD_TestText) - 1U) != 0)
    {
        OLED_Show3("SD FILE TEST",
                   "DATA MISMATCH",
                   "CHECK RESULT");
        return FILE_TEST_MATCH_FAIL;
    }

    /* 7. Test passed */
    OLED_Show3("SD FILE TEST",
               "READ WRITE OK",
               "DATA MATCH YES");

    return FILE_TEST_OK;
}

/* -------- HTTP SERVER: DATAVAULT PRIVATE CLOUD (RBAC & ATOMIC UPLOAD) -------- */

#define HTTP_REQ_SIZE       2048U   /* must hold one full +IPD packet (up to 1460 B) plus headers */
#define HTTP_RESP_SIZE      5200U
#define HTTP_BODY_SIZE      4800U
#define HTTP_HEADER_SIZE     32U
#define MAX_UPLOAD_SIZE     (2048U * 1024U)

#define ROLE_GUEST 0U
#define ROLE_USER  1U
#define ROLE_ADMIN 2U

typedef struct {
    const char *user;
    const char *pass;
    uint8_t role;
} UserAccount_t;

static const UserAccount_t g_users[] = {
    {ADMIN_USER,  ADMIN_PASS,  ROLE_ADMIN},
    {NORMAL_USER, NORMAL_PASS, ROLE_USER}
};
#define NUM_USERS (sizeof(g_users) / sizeof(g_users[0]))

typedef struct {
    uint8_t active;
    uint8_t role;
    char user[8];
    char token[9];
    uint32_t last_activity_s;
} Session_t;

static Session_t g_sessions[2];

static char g_station_ip[20] = "10.244.31.180";

static char http_request[HTTP_REQ_SIZE];
static char http_response[HTTP_RESP_SIZE];
static char http_body[HTTP_BODY_SIZE];
static char http_header[HTTP_HEADER_SIZE];

static uint16_t http_req_len = 0U;
static uint16_t http_req_expected = 0U;
static uint16_t http_chunk_read = 0U;
static uint16_t http_header_len = 0U;

static uint8_t http_link_id = 0U;
static uint8_t http_state = 0U;
static uint8_t http_prefix_pos = 0U;
static uint16_t http_discard_remaining = 0U;
static uint8_t http_accumulating = 0U;
static uint32_t http_last_rx_ms = 0U;

static uint8_t  upload_active = 0U;
static uint8_t  upload_link_id = 0U;
static uint32_t upload_expected_bytes = 0U;
static uint32_t upload_received_bytes = 0U;
static char     upload_name[100];
static uint8_t  upload_staging[256];
static uint16_t upload_staging_len = 0U;

#define HTTP_WAIT_PREFIX  0U
#define HTTP_READ_HEADER  1U
#define HTTP_READ_BODY    2U
#define HTTP_DISCARD_BODY 3U

static const char ipd_prefix[] = "+IPD,";

static const char HTML_TOP[] =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>DataVault</title><style>"
    "*{box-sizing:border-box}body{font:14px system-ui,sans-serif;margin:0;background:#000;color:#f5f5f5;padding:12px}"
    "a{color:#fff}.w{max-width:480px;margin:auto}"
    ".c{background:#111;border:1px solid #262626;border-radius:12px;padding:14px;margin-bottom:10px}"
    ".c .c{background:#0a0a0a;border-color:#1f1f1f}.h{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:10px}"
    ".l{font-size:18px;font-weight:800;letter-spacing:.5px}.s{font-size:12px;color:#8a8a8a}"
    ".b{padding:2px 9px;border-radius:99px;font-size:11px;font-weight:700;border:1px solid #444}"
    ".ba{background:#fff;color:#000}.bu{background:#333}.bg{color:#999}"
    ".btn{display:inline-block;padding:7px 12px;border:1px solid #333;border-radius:8px;font:600 13px sans-serif;text-decoration:none;color:#fff;margin:2px;cursor:pointer;text-align:center}"
    ".bp{background:#fff;color:#000;border-color:#fff}.bs{background:#1c1c1c}.bd{background:#000;border-color:#666}"
    ".t{padding:4px 9px;font-size:12px}"
    ".g{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin:6px 0}.sb{background:#161616;border:1px solid #222;border-radius:10px;padding:10px}"
    ".sb b{display:block;font-size:15px;margin-top:2px;word-break:break-all}"
    ".bar{height:8px;background:#222;border-radius:99px;overflow:hidden;margin:8px 0 4px}.bar i{display:block;height:100%;background:#fff}"
    ".dz{border:2px dashed #444;border-radius:12px;padding:20px;text-align:center;cursor:pointer;margin-top:6px}"
    "td b{word-break:break-all}table{width:100%;border-collapse:collapse}td,th{padding:9px 4px;border-bottom:1px solid #222;font-size:13px;text-align:left}"
    "input,select,textarea{width:100%;padding:10px;margin:4px 0 10px;background:#0a0a0a;border:1px solid #333;border-radius:8px;color:#fff;font-size:14px}"
    "input:focus,select:focus,textarea:focus{outline:0;border-color:#fff}"
    "pre{background:#0a0a0a;border:1px solid #222;color:#ddd;padding:10px;border-radius:8px;font-size:12px;white-space:pre-wrap;word-break:break-all}"
    ".e{background:#1a1a1a;border-left:3px solid #fff;padding:9px;border-radius:6px;margin:8px 0}"
    "</style></head><body><div class='w'><div class='c'>";

static const char HTML_END[] = "</div></div></body></html>";

static const char HTML_NAV[] =
    "<div class='g'><a class='btn bp' href='/files'>Files</a><a class='btn bs' href='/write'>Write</a>"
    "<a class='btn bs' href='/status'>Status</a><a class='btn bs' href='/'>Home</a></div>";

static void HTTP_ExtractParam(const char *req, const char *param, char *out, uint16_t capacity);
static void HTTP_UrlDecode(char *s);
static uint8_t HTTP_ValidateFilename(const char *name);

static const char *Auth_RoleName(uint8_t role)
{
    if (role == ROLE_ADMIN) return "Admin";
    if (role == ROLE_USER)  return "User";
    return "Guest";
}

static uint8_t Auth_GetRole(const char *req, char *user_out, char *token_out)
{
    char tok[10] = {0};
    uint32_t now_s = g_ms / 1000U;

    const char *p = strstr(req, "session=");
    if (!p) p = strstr(req, "auth=");
    if (p)
    {
        p += (p[0] == 's') ? 8 : 5;
        for (int i = 0; i < 8 && p[i] && p[i] != ';' && p[i] != '&' && p[i] != ' ' && p[i] != '\r'; i++)
            tok[i] = p[i];
    }
    if (token_out) memcpy(token_out, tok, 9);

    if (tok[0])
    {
        for (int i = 0; i < 2; i++)
        {
            if (g_sessions[i].active)
            {
                if ((uint32_t)(now_s - g_sessions[i].last_activity_s) > 900U)
                {
                    g_sessions[i].active = 0U;
                    continue;
                }
                if (memcmp(g_sessions[i].token, tok, 8) == 0)
                {
                    g_sessions[i].last_activity_s = now_s;
                    if (user_out) strcpy(user_out, g_sessions[i].user);
                    return g_sessions[i].role;
                }
            }
        }
    }
    if (user_out) strcpy(user_out, "Guest");
    return ROLE_GUEST;
}

static void Auth_CreateSession(const char *user, uint8_t role, char *token_out)
{
    uint32_t now_s = g_ms / 1000U;
    uint8_t slot = (g_sessions[0].active && !g_sessions[1].active) ? 1U :
                   (g_sessions[0].active && g_sessions[1].active) ?
                   ((g_sessions[0].last_activity_s < g_sessions[1].last_activity_s) ? 0U : 1U) : 0U;

    g_sessions[slot].active = 1U;
    g_sessions[slot].role = role;
    g_sessions[slot].last_activity_s = now_s;
    (void)snprintf(g_sessions[slot].user, sizeof(g_sessions[slot].user), "%s", user);

    static uint32_t seed = 0x1A2B3C4DU;
    seed = (seed ^ g_ms ^ (SysTick->VAL << 12)) * 2654435761U + 0x9E3779B9U + slot;
    uint32_t rand_val = seed;
    const char hx[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) { g_sessions[slot].token[i] = hx[rand_val & 0x0FU]; rand_val >>= 4; }
    g_sessions[slot].token[8] = '\0';

    if (token_out) memcpy(token_out, g_sessions[slot].token, 9);
}

static uint8_t AuthorizeRequest(uint8_t role, const char *endpoint)
{
    if (strncmp(endpoint, "/delete", 7) == 0)
        return (role == ROLE_ADMIN);
    if (strncmp(endpoint, "/upload", 7) == 0 || strncmp(endpoint, "/write", 6) == 0)
        return (role != ROLE_GUEST);
    return 1U;
}

static void HTML_EscapeText(const char *src, char *dst, size_t dst_len)
{
    size_t d = 0;
    if (!src || !dst || dst_len == 0) return;
    for (size_t s = 0; src[s] && d < dst_len - 6; s++)
    {
        char c = src[s];
        if (c == '<')      { memcpy(dst + d, "&lt;", 4); d += 4; }
        else if (c == '>') { memcpy(dst + d, "&gt;", 4); d += 4; }
        else if (c == '&') { memcpy(dst + d, "&amp;", 5); d += 5; }
        else if (c == '"') { memcpy(dst + d, "&quot;", 6); d += 6; }
        else if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t') { dst[d++] = c; }
    }
    dst[d] = '\0';
}

static uint8_t ESP_WaitPrompt(uint32_t timeout_ms)
{
    uint32_t start = g_ms;
    while ((uint32_t)(g_ms - start) < timeout_ms)
    {
        if (UART_ReadByte() == '>') return 1U;
    }
    return 0U;
}

static uint8_t ESP_WaitSendOK(uint32_t timeout_ms)
{
    uint32_t start = g_ms;
    uint8_t matched = 0U;
    const char expected[] = "SEND OK";

    while ((uint32_t)(g_ms - start) < timeout_ms)
    {
        int b = UART_ReadByte();

        if (b >= 0)
        {
            char c = (char)b;
            if (c == expected[matched])
            {
                matched++;
                if (matched == sizeof(expected) - 1U) return 1U;
            }
            else
            {
                matched = (c == expected[0]) ? 1U : 0U;
            }
        }
    }
    return 0U;
}

static uint8_t ESP_SendBuffer(uint8_t link_id, const uint8_t *data, uint16_t total_len)
{
    char command[32];
    uint16_t sent = 0U;

    if (!data || total_len == 0U) return 0U;

    while (sent < total_len)
    {
        uint16_t chunk = total_len - sent;
        if (chunk > 1200U) chunk = 1200U;

        (void)snprintf(command, sizeof(command), "AT+CIPSEND=%u,%u", (unsigned int)link_id, (unsigned int)chunk);
        UART_SendString(command);
        UART_SendString("\r\n");

        if (!ESP_WaitPrompt(3000U)) return 0U;

        for (uint16_t i = 0U; i < chunk; i++)
        {
            while ((USART2->SR & USART_SR_TXE) == 0U) __NOP();
            USART2->DR = data[sent + i];
        }

        if (!ESP_WaitSendOK(5000U)) return 0U;

        sent += chunk;
    }
    return 1U;
}

static uint8_t ESP_SendHttp(uint8_t link_id, const char *response)
{
    if (!response) return 0U;
    return ESP_SendBuffer(link_id, (const uint8_t *)response, (uint16_t)strlen(response));
}

static void ESP_CloseClient(uint8_t link_id)
{
    char command[24];
    (void)snprintf(command, sizeof(command), "AT+CIPCLOSE=%u\r\n", (unsigned int)link_id);
    UART_SendString(command);
}

static void HTTP_Respond(uint8_t link_id, const char *status, const char *body)
{
    if (!body) body = "";
    unsigned int body_len = (unsigned int)strlen(body);

    int n = snprintf(http_response, sizeof(http_response),
        "HTTP/1.1 %s\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
        status, body_len, body);

    if (n > 0 && (unsigned int)n < sizeof(http_response))
        ESP_SendHttp(link_id, http_response);

    delay_ms(80U);
    ESP_CloseClient(link_id);
}

static void HTTP_RespondError(uint8_t link_id, const char *status, const char *msg)
{
    snprintf(http_body, sizeof(http_body),
        "%s<div class='h'><b>%s</b><a class='btn bs' href='/'>Home</a></div>"
        "<div class='e'>%s</div>%s",
        HTML_TOP, status, msg, HTML_END);
    HTTP_Respond(link_id, status, http_body);
}

static void HTTP_RespondRedirect(uint8_t link_id, const char *cookie, const char *loc)
{
    char set_cookie[80] = "";

    /* cookie == 0 keeps the current session, "" clears it */
    if (cookie)
        (void)snprintf(set_cookie, sizeof(set_cookie), "Set-Cookie: session=%s; Path=/; %s\r\n",
                       cookie, cookie[0] ? "HttpOnly; SameSite=Lax" : "Max-Age=0");

    (void)snprintf(http_response, sizeof(http_response),
        "HTTP/1.1 302 Found\r\n%sLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        set_cookie, loc);
    (void)ESP_SendHttp(link_id, http_response);
    delay_ms(80U);
    ESP_CloseClient(link_id);
}

static void HTTP_AbortUpload(uint8_t link_id, const char *msg)
{
    (void)f_close(&SD_File);
    (void)f_unlink("0:/UPLOAD.TMP");
    upload_active = 0U;
    upload_staging_len = 0U;
    http_accumulating = 0U;
    http_state = HTTP_WAIT_PREFIX;
    HTTP_RespondError(link_id, "500 Internal Error", msg);
}

static void HTTP_FinishUpload(uint8_t link_id)
{
    FRESULT close_fr = f_close(&SD_File);
    upload_active = 0U;
    upload_staging_len = 0U;
    http_accumulating = 0U;
    http_state = HTTP_WAIT_PREFIX;
    http_prefix_pos = 0U;

    if (close_fr == FR_OK)
    {
        char target_path[40];
        snprintf(target_path, sizeof(target_path), "0:/%s", upload_name);
        (void)f_unlink(target_path);
        (void)f_rename("0:/UPLOAD.TMP", target_path);

        OLED_Show3("DATAVAULT", upload_name, "UPLOAD OK");
        snprintf(http_body, sizeof(http_body),
            "{\"status\":\"ok\",\"name\":\"%s\",\"bytes\":%u}",
            upload_name, (unsigned int)upload_received_bytes);
        HTTP_Respond(link_id, "200 OK", http_body);
    }
    else
    {
        (void)f_unlink("0:/UPLOAD.TMP");
        HTTP_RespondError(link_id, "500 Internal Error", "File commit error");
    }
}

/* Parses an unsigned decimal; returns the char after it, or 0 if no digits */
static const char *ParseUint(const char *p, unsigned int *out)
{
    const char *start;
    unsigned int v = 0U;

    while (*p == ' ')
        p++;

    start = p;

    while (*p >= '0' && *p <= '9')
        v = v * 10U + (unsigned int)(*p++ - '0');

    *out = v;
    return (p == start) ? 0 : p;
}

static void HTTP_ExtractParam(const char *req, const char *param, char *out, uint16_t capacity)
{
    if (capacity == 0U) return;
    out[0] = '\0';
    const char *p = strstr(req, param);
    if (!p) return;
    p += strlen(param);
    uint16_t i = 0U;
    while (*p && *p != ' ' && *p != '&' && *p != '\r' && *p != '\n' && i < capacity - 1U)
        out[i++] = *p++;
    out[i] = '\0';
}

static int HTTP_HexToNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void HTTP_UrlDecode(char *s)
{
    if (!s) return;
    char *dst = s;
    const char *src = s;
    while (*src)
    {
        if (*src == '+') { *dst++ = ' '; src++; }
        else if (*src == '%' && src[1] && src[2])
        {
            int h1 = HTTP_HexToNibble(src[1]);
            int h2 = HTTP_HexToNibble(src[2]);
            if (h1 >= 0 && h2 >= 0) { *dst++ = (char)((h1 << 4) | h2); src += 3; }
            else { *dst++ = *src++; }
        }
        else { *dst++ = *src++; }
    }
    *dst = '\0';
}

static const char *HTTP_Body(const char *req)
{
    const char *p = strstr(req, "\r\n\r\n");
    if (p) return p + 4;
    p = strstr(req, "\n\n");
    return p ? p + 2 : req;
}

static uint8_t HTTP_GetFormValue(const char *body, const char *key, char *out, uint16_t max_len)
{
    if (!body || !key || !out || max_len == 0U) return 0U;
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (*p)
    {
        if ((p == body || *(p - 1) == '&') && strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            p += klen + 1;
            uint16_t i = 0U;
            while (*p && *p != '&' && *p != '\r' && *p != '\n' && i < max_len - 1U)
                out[i++] = *p++;
            out[i] = '\0';
            return 1U;
        }
        p++;
    }
    return 0U;
}

static uint8_t HTTP_ValidateFilename(const char *name)
{
    uint8_t len = 0U;

    /* Long names up to FF_MAX_LFN (32). Only characters that are safe in
       paths, URLs and HTML attributes without escaping are accepted. */
    if (!name || name[0] == '\0' || name[0] == '.' || name[0] == ' ')
        return 0U;

    for (; name[len] != '\0'; len++)
    {
        char c = name[len];

        if (len >= 32U)
            return 0U;

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.' || c == ' ' || c == '(' || c == ')'))
            return 0U;
    }

    /* FAT silently strips a trailing dot or space */
    return (name[len - 1U] == '.' || name[len - 1U] == ' ') ? 0U : 1U;
}

static void Storage_GetMetrics(uint32_t *total_mb, uint32_t *free_mb, const char **card_str)
{
    DWORD fre_clust = 0;
    FATFS *pfs = 0;
    uint8_t ctype = SD_GetCardType();
    uint32_t sectors = SD_GetSectorCount();

    *total_mb = 0U;
    *free_mb = 0U;
    if (card_str)
    {
        *card_str = (ctype & SD_TYPE_V2HC) ? "SDHC/SDXC" :
                    (ctype & SD_TYPE_V2SC) ? "SDSC V2" :
                    (ctype & SD_TYPE_V1)   ? "SDSC V1" : "SD/MMC";
    }

    if (sectors > 0U)
        *total_mb = sectors / 2048U;

    if (f_getfree("0:", &fre_clust, &pfs) == FR_OK && pfs != 0)
    {
        *free_mb = (uint32_t)((fre_clust * pfs->csize) / 2048U);
        if (*total_mb == 0U)
            *total_mb = (uint32_t)(((pfs->n_fatent - 2U) * pfs->csize) / 2048U);
    }
}

static void HTTP_ServeDashboard(uint8_t link_id, uint8_t role, const char *username)
{
    uint32_t total_mb = 0U, free_mb = 0U;
    Storage_GetMetrics(&total_mb, &free_mb, 0);

    const char *rb_class = (role == ROLE_ADMIN) ? "b ba" : (role == ROLE_USER) ? "b bu" : "b bg";

    snprintf(http_body, sizeof(http_body),
        "%s<div class='h'><div><span class='l'>DataVault</span><div class='s'>Private Cloud</div></div>"
        "<div><span class='%s'>%s</span> <a class='btn bs' href='%s'>%s</a></div></div>"
        "<div class='c'><div style='display:flex;justify-content:space-between'><b>Storage</b><span class='s'>FAT32</span></div>"
        "<div class='g'><div class='sb'><div class='s'>TOTAL</div><b>%u MB</b></div>"
        "<div class='sb'><div class='s'>FREE</div><b>%u MB</b></div></div></div>"
        "<div class='c'><b>Upload</b>%s"
        "<input type='file' id='fi' style='display:none' onchange='up(this.files[0])'>"
        "<div id='st' style='margin-top:4px;font-weight:700'></div></div>"
        "<div class='c'><b>Navigation</b>%s</div>"
        "<div class='s' style='text-align:center'>Signed in as <b>%s</b></div>"
        "<script>"
        /* Keep the real name; swap characters the server rejects, cap at 32 */
        "function up(f){if(!f)return;var s=document.getElementById('st'),"
        "n=f.name.replace(/[^\\w .()-]/g,'_').replace(/^[. ]+|[. ]+$/g,''),i=n.lastIndexOf('.'),e=i>0?n.slice(i):'';"
        "if(n.length>32)n=n.slice(0,32-e.length)+e;n=n||'file';if(f.size>2097152){s.innerText='Too big: max 2MB';return}"
        "s.innerText='Uploading '+n+'...';"
        "var x=new XMLHttpRequest();x.open('POST','/upload?name='+encodeURIComponent(n));"
        "x.upload.onprogress=p=>{if(p.total)s.innerText='Uploading '+n+' '+Math.round(p.loaded*100/p.total)+'%%'};"
        "x.onload=()=>{s.innerText=(x.status==200)?'Saved as '+n:'Upload failed ('+x.status+')';if(x.status==200)setTimeout(()=>location.href='/files',900)};"
        "x.onerror=()=>s.innerText='Network error';x.send(f);}"
        "var z=document.getElementById('dz');if(z){z.ondragover=e=>e.preventDefault();z.ondrop=e=>{e.preventDefault();if(e.dataTransfer.files[0])up(e.dataTransfer.files[0])};}"
        "</script>%s",
        HTML_TOP, rb_class, Auth_RoleName(role),
        (role == ROLE_GUEST) ? "/login" : "/logout", (role == ROLE_GUEST) ? "Login" : "Logout",
        (unsigned int)total_mb, (unsigned int)free_mb,
        (role == ROLE_GUEST) ? "<div class='e'>Guests are read-only. <a href='/login' style='color:#fff'>Sign in</a> to upload.</div>" :
        "<div class='dz' id='dz' onclick='document.getElementById(\"fi\").click()'><b>Drop a file here</b><div class='s'>or tap to choose (max 2MB)</div></div>",
        HTML_NAV, username, HTML_END);

    HTTP_Respond(link_id, "200 OK", http_body);
}

static void HTTP_ServeStatus(uint8_t link_id, uint8_t role)
{
    uint32_t total_mb = 0U, free_mb = 0U;
    const char *card_str = "SD";
    uint32_t uptime_s = g_ms / 1000U;
    uint32_t used_mb, used_pct;

    Storage_GetMetrics(&total_mb, &free_mb, &card_str);
    used_mb = (total_mb > free_mb) ? (total_mb - free_mb) : 0U;
    used_pct = (total_mb > 0U) ? (used_mb * 100U) / total_mb : 0U;

    /* Mirror the status page on the OLED (font: A-Z 0-9 : - . only) */
    {
        char line[22];

        OLED_Clear();
        OLED_PrintLine(0U, "SYSTEM STATUS");
        OLED_PrintLine(1U, g_station_ip);
        (void)snprintf(line, sizeof(line), "UP %uH %uM %uS",
                       (unsigned int)(uptime_s / 3600U), (unsigned int)((uptime_s / 60U) % 60U),
                       (unsigned int)(uptime_s % 60U));
        OLED_PrintLine(2U, line);
        OLED_PrintLine(3U, card_str);
        (void)snprintf(line, sizeof(line), "USED %u OF %uMB", (unsigned int)used_mb, (unsigned int)total_mb);
        OLED_PrintLine(4U, line);
        (void)snprintf(line, sizeof(line), "FREE %uMB  %u PCT", (unsigned int)free_mb, (unsigned int)used_pct);
        OLED_PrintLine(5U, line);
        (void)snprintf(line, sizeof(line), "ROLE %s", Auth_RoleName(role));
        OLED_PrintLine(6U, line);
        OLED_PrintLine(7U, "STM32F411 ONLINE");
    }

    snprintf(http_body, sizeof(http_body),
        "%s<div class='h'><div><span class='l'>System Status</span><div class='s'>Live device telemetry</div></div>"
        "<a class='btn bs t' href='/status'>Refresh</a></div>"
        "<div class='g'><div class='sb'><div class='s'>IP ADDRESS</div><b>%s</b></div>"
        "<div class='sb'><div class='s'>UPTIME</div><b>%uh %um %us</b></div>"
        "<div class='sb'><div class='s'>MCU</div><b>STM32F411</b></div>"
        "<div class='sb'><div class='s'>YOUR ROLE</div><b>%s</b></div></div>"
        "<div class='c'><div class='h' style='margin:0'><b>Storage</b><span class='s'>%s &middot; FAT32</span></div>"
        "<div class='bar'><i style='width:%u%%'></i></div>"
        "<div class='h' style='margin:0'><span class='s'>%u MB used (%u%%)</span><span class='s'>%u MB free of %u MB</span></div></div>"
        "%s%s",
        HTML_TOP, g_station_ip,
        (unsigned int)(uptime_s / 3600U), (unsigned int)((uptime_s / 60U) % 60U), (unsigned int)(uptime_s % 60U),
        Auth_RoleName(role), card_str, (unsigned int)used_pct,
        (unsigned int)used_mb, (unsigned int)used_pct, (unsigned int)free_mb, (unsigned int)total_mb,
        HTML_NAV, HTML_END);

    HTTP_Respond(link_id, "200 OK", http_body);
}

static void HTTP_ServeFiles(uint8_t link_id, uint8_t role, const char *token)
{
    DIR dir;
    FILINFO fno;
    int offset = snprintf(http_body, sizeof(http_body),
        "%s<div class='h'><div><span class='l'>Files</span><div class='s'>MicroSD card &middot; %s access</div></div>"
        "%s</div><table>",
        HTML_TOP, Auth_RoleName(role),
        (role == ROLE_GUEST) ? "<a class='btn bs t' href='/login'>Sign in</a>" : "<a class='btn bp t' href='/write'>+ New</a>");

    uint16_t file_count = 0U;
    if (f_opendir(&dir, "0:/") == FR_OK)
    {
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0])
        {
            if (fno.fattrib & (AM_HID | AM_SYS)) continue;
            file_count++;

            if (offset < (int)(sizeof(http_body) - 700U))
            {
                uint8_t is_kb = (fno.fsize >= 1024U) ? 1U : 0U;
                unsigned int size = (unsigned int)(is_kb ? (fno.fsize + 1023U) / 1024U : fno.fsize);

                if (fno.fattrib & AM_DIR)
                {
                    offset += snprintf(http_body + offset, sizeof(http_body) - (size_t)offset,
                        "<tr><td><b>%s/</b><div class='s'>Folder</div></td><td></td></tr>", fno.fname);
                    continue;
                }

                offset += snprintf(http_body + offset, sizeof(http_body) - (size_t)offset,
                    "<tr><td><b>%s</b><div class='s'>%u %s</div></td><td style='text-align:right;white-space:nowrap'>"
                    "<a class='btn bs t' href='/read?name=%s'>View</a><a class='btn bp t' href='/download?name=%s'>Get</a>",
                    fno.fname, size, is_kb ? "KB" : "bytes", fno.fname, fno.fname);

                if (role == ROLE_ADMIN)
                {
                    offset += snprintf(http_body + offset, sizeof(http_body) - (size_t)offset,
                        "<a class='btn bd t' href='/delete?name=%s&auth=%s' onclick='return confirm(\"Delete %s?\")'>Del</a>",
                        fno.fname, token ? token : "", fno.fname);
                }

                offset += snprintf(http_body + offset, sizeof(http_body) - (size_t)offset, "</td></tr>");
            }
        }
        f_closedir(&dir);
    }

    if (file_count == 0U)
    {
        offset += snprintf(http_body + offset, sizeof(http_body) - (size_t)offset,
            "<tr><td style='text-align:center;padding:24px' class='s'>No files yet. Upload one from Home.</td></tr>");
    }

    snprintf(http_body + offset, sizeof(http_body) - (size_t)offset,
        "</table><p class='s' style='text-align:center'>%u item%s</p>%s%s",
        (unsigned int)file_count, (file_count == 1U) ? "" : "s", HTML_NAV, HTML_END);

    HTTP_Respond(link_id, "200 OK", http_body);
}

static void HTTP_ServeDownload(uint8_t link_id, const char *filename)
{
    char path[40];
    if (!HTTP_ValidateFilename(filename))
    {
        HTTP_RespondError(link_id, "400 Bad Request", "Invalid filename");
        return;
    }

    snprintf(path, sizeof(path), "0:/%s", filename);
    if (f_open(&SD_File, path, FA_READ) != FR_OK)
    {
        HTTP_RespondError(link_id, "404 Not Found", "File not found");
        return;
    }

    DWORD fsz = f_size(&SD_File);
    snprintf(http_response, sizeof(http_response),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %u\r\nConnection: close\r\n\r\n",
        filename, (unsigned int)fsz);

    if (ESP_SendHttp(link_id, http_response))
    {
        while (fsz > 0U)
        {
            UINT to_read = (fsz > 1024U) ? 1024U : (UINT)fsz;
            UINT br = 0U;
            if (f_read(&SD_File, http_body, to_read, &br) != FR_OK || br == 0U)
                break;
            if (!ESP_SendBuffer(link_id, (const uint8_t *)http_body, (uint16_t)br))
                break;
            fsz -= br;
        }
    }

    (void)f_close(&SD_File);
    delay_ms(50U);
    ESP_CloseClient(link_id);
    OLED_Show3("DATAVAULT", filename, "DOWNLOAD OK");
}

static uint8_t HTTP_IsTextFile(const char *name)
{
    static const char * const exts[] = {".TXT", ".LOG", ".CSV", ".HEX", ".C", ".H", ".MD", ".DAT"};
    const char *dot = strrchr(name, '.');
    if (!dot) return 0U;
    for (uint8_t i = 0U; i < 8U; i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1U;
    return 0U;
}

static void HTTP_ServeRead(uint8_t link_id, const char *filename)
{
    char path[40];
    static char file_buf[350];
    static char escaped_buf[700];
    UINT bytesRead = 0U;

    if (!HTTP_ValidateFilename(filename))
    {
        HTTP_RespondError(link_id, "400 Bad Request", "Invalid filename");
        return;
    }

    if (!HTTP_IsTextFile(filename))
    {
        HTTP_RespondError(link_id, "415 Unsupported", "Binary preview unavailable");
        return;
    }

    snprintf(path, sizeof(path), "0:/%s", filename);
    if (f_open(&SD_File, path, FA_READ) != FR_OK)
    {
        HTTP_RespondError(link_id, "404 Not Found", "File not found");
        return;
    }

    memset(file_buf, 0, sizeof(file_buf));
    (void)f_read(&SD_File, file_buf, sizeof(file_buf) - 1U, &bytesRead);
    (void)f_close(&SD_File);

    HTML_EscapeText(file_buf, escaped_buf, sizeof(escaped_buf));

    snprintf(http_body, sizeof(http_body),
        "%s<div class='h'><b>%s</b><span class='s'>%u B read</span></div>"
        "<pre>%s</pre><div class='g'><a class='btn bp' href='/download?name=%s'>Download</a><a class='btn bs' href='/files'>Back</a></div>%s",
        HTML_TOP, filename, (unsigned int)bytesRead, escaped_buf, filename, HTML_END);

    HTTP_Respond(link_id, "200 OK", http_body);
}

static void HTTP_ServeLogin(uint8_t link_id, const char *msg)
{
    snprintf(http_body, sizeof(http_body),
        "%s<div style='text-align:center;margin:6px 0 14px'><div class='l' style='font-size:24px'>DataVault</div>"
        "<div class='s'>Sign in to your private cloud</div></div>%s"
        "<form method='POST' action='/login'>"
        "<label class='s'>Username</label>"
        "<input name='user' maxlength='15' autocomplete='username' autocapitalize='none' required autofocus>"
        "<label class='s'>Password</label>"
        "<input type='password' name='pass' maxlength='15' autocomplete='current-password' required>"
        "<button class='btn bp' style='width:100%%;padding:10px'>Sign In</button></form>"
        "<div style='text-align:center;margin-top:12px'><a class='s' href='/'>Continue as guest</a></div>%s",
        HTML_TOP, msg ? msg : "", HTML_END);

    HTTP_Respond(link_id, "200 OK", http_body);
}

static void HTTP_HandlePostLogin(uint8_t link_id, const char *request)
{
    char user[16] = {0}, pass[16] = {0};
    const char *body = HTTP_Body(request);
    HTTP_GetFormValue(body, "user", user, sizeof(user));
    HTTP_GetFormValue(body, "pass", pass, sizeof(pass));
    HTTP_UrlDecode(user);
    HTTP_UrlDecode(pass);

    for (uint8_t i = 0U; i < NUM_USERS; i++)
    {
        if (strcmp(user, g_users[i].user) == 0 && strcmp(pass, g_users[i].pass) == 0)
        {
            char token[16];
            Auth_CreateSession(g_users[i].user, g_users[i].role, token);
            OLED_Show3("DATAVAULT", user, "LOGIN OK");
            HTTP_RespondRedirect(link_id, token, "/");
            return;
        }
    }

    OLED_Show3("DATAVAULT", user[0] ? user : "LOGIN", "FAIL AUTH");
    HTTP_ServeLogin(link_id, "<div class='e'>Invalid username or password.</div>");
}

static void HTTP_HandleLogout(uint8_t link_id, const char *request)
{
    char tok[10];
    Auth_GetRole(request, 0, tok);
    for (int i = 0; i < 2; i++)
        if (g_sessions[i].active && memcmp(g_sessions[i].token, tok, 8) == 0)
            g_sessions[i].active = 0U;

    OLED_Show3("DATAVAULT", "LOGOUT", "SESSION ENDED");
    HTTP_RespondRedirect(link_id, "", "/");
}

static void HTTP_HandleDelete(uint8_t link_id, const char *request, uint8_t role)
{
    if (!AuthorizeRequest(role, "/delete"))
    {
        OLED_Show3("DELETE DENIED", "RBAC BLOCKED", "ADMIN ONLY");
        HTTP_RespondError(link_id, "403 Forbidden", "Admin only");
        return;
    }

    char name[100] = {0}, path[40];
    HTTP_ExtractParam(request, "name=", name, sizeof(name));
    HTTP_UrlDecode(name);

    if (!HTTP_ValidateFilename(name))
    {
        HTTP_RespondError(link_id, "400 Bad Request", "Invalid filename");
        return;
    }

    snprintf(path, sizeof(path), "0:/%s", name);
    if (f_unlink(path) == FR_OK)
    {
        OLED_Show3("DATAVAULT", name, "DELETED");
        HTTP_RespondRedirect(link_id, 0, "/files");
    }
    else
    {
        OLED_Show3("DELETE FAIL", name, "ERR");
        HTTP_RespondError(link_id, "500 Internal Error", "Deletion failed");
    }
}

static void HTTP_ServeWriteForm(uint8_t link_id, uint8_t role, const char *token)
{
    if (!AuthorizeRequest(role, "/write"))
    {
        HTTP_RespondError(link_id, "403 Forbidden", "Guests cannot write.");
        return;
    }

    snprintf(http_body, sizeof(http_body),
        "%s<div class='h'><b>Write / Append</b><a class='btn bs' href='/files'>Files</a></div>"
        "<form method='POST' action='/write'>"
        "<input type='hidden' name='auth' value='%s'>"
        "<input type='text' name='name' placeholder='File name, e.g. notes.txt' maxlength='32' required>"
        "<select name='mode'><option value='create'>Create / Overwrite</option><option value='append'>Append</option></select>"
        "<textarea name='content' rows='4' placeholder='Content'></textarea>"
        "<button type='submit' class='btn bp' style='width:100%%'>Save File</button></form>%s",
        HTML_TOP, token ? token : "", HTML_END);

    HTTP_Respond(link_id, "200 OK", http_body);
}

static void HTTP_HandlePostWrite(uint8_t link_id, const char *request, uint8_t role)
{
    if (!AuthorizeRequest(role, "/write"))
    {
        HTTP_RespondError(link_id, "403 Forbidden", "Write forbidden for Guest");
        return;
    }

    char name[100], mode[16], path[40];
    static char decoded_content[512];
    UINT bw = 0U;
    uint8_t is_append = 0U;

    request = HTTP_Body(request);
    if (!HTTP_GetFormValue(request, "name", name, sizeof(name)))
    {
        HTTP_RespondError(link_id, "400 Bad Request", "Missing name");
        return;
    }
    HTTP_UrlDecode(name);

    if (!HTTP_ValidateFilename(name))
    {
        HTTP_RespondError(link_id, "400 Bad Request", "Invalid filename");
        return;
    }

    if (HTTP_GetFormValue(request, "mode", mode, sizeof(mode)))
    {
        HTTP_UrlDecode(mode);
        if (strcmp(mode, "append") == 0) is_append = 1U;
    }

    decoded_content[0] = '\0';
    if (HTTP_GetFormValue(request, "content", decoded_content, sizeof(decoded_content)))
        HTTP_UrlDecode(decoded_content);

    snprintf(path, sizeof(path), "0:/%s", name);
    FRESULT fr = is_append ? f_open(&SD_File, path, FA_OPEN_EXISTING | FA_WRITE) :
                             f_open(&SD_File, path, FA_CREATE_ALWAYS | FA_WRITE);

    if (fr != FR_OK)
    {
        OLED_Show3("FILE WRITE", name, "OPEN FAIL");
        HTTP_RespondError(link_id, "500 Internal Error", "File open failed");
        return;
    }

    if (is_append)
    {
        if (f_lseek(&SD_File, f_size(&SD_File)) != FR_OK)
        {
            (void)f_close(&SD_File);
            HTTP_RespondError(link_id, "500 Internal Error", "Seek failed");
            return;
        }
    }

    UINT clen = (UINT)strlen(decoded_content);
    if (clen > 0U) fr = f_write(&SD_File, decoded_content, clen, &bw);
    FRESULT cfr = f_close(&SD_File);

    if (fr != FR_OK || cfr != FR_OK)
    {
        OLED_Show3("FILE WRITE", name, "WRITE FAIL");
        HTTP_RespondError(link_id, "500 Internal Error", "Write failed");
        return;
    }

    OLED_Show3("FILE WRITE", name, "SUCCESS");
    snprintf(http_body, sizeof(http_body),
        "%s<h3>Saved: %s</h3><p>%u B written</p>"
        "<div class='g'><a class='btn bp' href='/read?name=%s'>Read</a><a class='btn bs' href='/files'>Files</a></div>%s",
        HTML_TOP, name, (unsigned int)bw, name, HTML_END);

    HTTP_Respond(link_id, "200 OK", http_body);
}

static void __attribute__((noinline)) HTTP_HandleRequest(uint8_t link_id, const char *request)
{
    char username[8] = {0}, token[16] = {0};
    uint8_t role = Auth_GetRole(request, username, token);
    const char *p = (request[0] == 'G') ? (request + 4) : (request + 5);

    if (p[0] == '/' && (p[1] == ' ' || p[1] == '?' || p[1] == 'H'))
    {
        OLED_Show3("DATAVAULT", "DASHBOARD", Auth_RoleName(role));
        HTTP_ServeDashboard(link_id, role, username);
    }
    else if (strncmp(p, "/status", 7) == 0)
    {
        HTTP_ServeStatus(link_id, role);
    }
    else if (strncmp(p, "/files", 6) == 0)
    {
        OLED_Show3("DATAVAULT", "FILES", "LISTING");
        HTTP_ServeFiles(link_id, role, token);
    }
    else if (strncmp(p, "/login", 6) == 0)
    {
        if (request[0] == 'P') HTTP_HandlePostLogin(link_id, request);
        else HTTP_ServeLogin(link_id, 0);
    }
    else if (strncmp(p, "/logout", 7) == 0)
    {
        HTTP_HandleLogout(link_id, request);
    }
    else if (strncmp(p, "/delete", 7) == 0)
    {
        HTTP_HandleDelete(link_id, request, role);
    }
    else if (strncmp(p, "/download", 9) == 0)
    {
        char fname[100];
        HTTP_ExtractParam(request, "name=", fname, sizeof(fname));
        HTTP_UrlDecode(fname);
        OLED_Show3("DATAVAULT", "DOWNLOAD", fname[0] ? fname : "EMPTY");
        HTTP_ServeDownload(link_id, fname);
    }
    else if (strncmp(p, "/write", 6) == 0)
    {
        if (request[0] == 'P')
        {
            OLED_Show3("DATAVAULT", "WRITE", "SAVING");
            HTTP_HandlePostWrite(link_id, request, role);
        }
        else
        {
            OLED_Show3("DATAVAULT", "WRITE", "FORM");
            HTTP_ServeWriteForm(link_id, role, token);
        }
    }
    else if (strncmp(p, "/read", 5) == 0)
    {
        char fname[100];
        HTTP_ExtractParam(request, "name=", fname, sizeof(fname));
        HTTP_UrlDecode(fname);
        OLED_Show3("DATAVAULT", "READ", fname[0] ? fname : "EMPTY");
        HTTP_ServeRead(link_id, fname);
    }
    else if (strncmp(p, "/favicon", 8) == 0)
    {
        HTTP_Respond(link_id, "404 Not Found", "");
    }
    else
    {
        OLED_Show3("DATAVAULT", "404", "NOT FOUND");
        HTTP_RespondError(link_id, "404 Not Found", "Endpoint not recognized.");
    }
}

#define SERVER_OK           0U
#define SERVER_MUX_FAIL     1U
#define SERVER_LISTEN_FAIL  2U

static uint8_t ESP_ServerStart(void)
{
    /* Resetting/flashing the STM32 does not reset the ESP8266. A server left
       running from the previous boot makes AT+CIPMUX=1 return ERROR, so only
       set multi-connection mode when it is not already on. */
    ESP_Command("AT+CIPMUX?", 2000U);
    if (!ESP_Has("+CIPMUX:1"))
    {
        ESP_Command("AT+CIPMUX=1", 2000U);
        if (!ESP_Has("OK")) return SERVER_MUX_FAIL;
    }

    /* Optional: older AT firmware rejects this, which is harmless */
    ESP_Command("AT+CIPDINFO=0", 2000U);

    /* "no change" means the server is already listening */
    ESP_Command("AT+CIPSERVER=1,80", 3000U);
    return (ESP_Has("OK") || ESP_Has("no change")) ? SERVER_OK : SERVER_LISTEN_FAIL;
}

static void ESP_Restart(void)
{
    OLED_Show3("RESETTING ESP", "CLEARING OLD STATE", "PLEASE WAIT");
    ESP_Command("AT+RST", 2000U);
    delay_ms(3000U);
    ESP_Command("ATE0", 1500U);
    ESP_Command("AT+CWMODE_CUR=1", 2000U);
}

/* First line of the last ESP reply, for showing on the OLED */
static void ESP_ReplySnippet(char *out, uint16_t capacity)
{
    const char *p = rx;
    uint16_t i = 0U;

    while (*p == '\r' || *p == '\n' || *p == ' ')
        p++;

    while (*p != '\0' && *p != '\r' && *p != '\n' && i < capacity - 1U)
        out[i++] = *p++;

    out[i] = '\0';

    if (i == 0U)
        (void)snprintf(out, capacity, "NO REPLY");
}

static void Halt(const char *a, const char *b, const char *c)
{
    OLED_Show3(a, b, c);

    while (1)
        delay_ms(1000U);
}

/* Join Wi-Fi if needed and read the IP. Halts on Wi-Fi failure. */
static uint8_t Network_Connect(void)
{
    uint8_t ip_found;

    if (!ESP_IsConnected())
    {
        ESP_JoinHotspot();
        delay_ms(1000U);

        if (!ESP_IsConnected())
            Halt("WIFI FAILED", "CHECK HOTSPOT", "SSID " WIFI_SSID);
    }

    ip_found = ESP_GetIP(g_station_ip, sizeof(g_station_ip));

    OLED_Show3("WIFI CONNECTED",
               ip_found ? g_station_ip : "IP NOT FOUND",
               "STARTING SERVER");

    delay_ms(1500U);
    return ip_found;
}

static void __attribute__((noinline)) ESP_ServerPoll(void)
{
    char c;
    int b;

    if ((http_accumulating || upload_active) && (uint32_t)(g_ms - http_last_rx_ms) > 5000U)
    {
        if (upload_active)
        {
            (void)f_close(&SD_File);
            (void)f_unlink("0:/UPLOAD.TMP");
            upload_active = 0U;
            upload_staging_len = 0U;
            OLED_Show3("DATAVAULT", upload_name, "UPLOAD TIMEOUT");
        }
        http_accumulating = 0U;
        http_state = HTTP_WAIT_PREFIX;
        http_prefix_pos = 0U;
        http_req_len = 0U;
    }

    while ((b = UART_ReadByte()) >= 0)
    {
        c = (char)b;
        http_last_rx_ms = g_ms;

        if (http_state == HTTP_WAIT_PREFIX)
        {
            if (c == ipd_prefix[http_prefix_pos])
            {
                http_prefix_pos++;
                if (http_prefix_pos == sizeof(ipd_prefix) - 1U)
                {
                    http_prefix_pos = 0U;
                    http_header_len = 0U;
                    http_header[0] = '\0';
                    http_state = HTTP_READ_HEADER;
                }
            }
            else
            {
                http_prefix_pos = (c == '+') ? 1U : 0U;
            }
        }
        else if (http_state == HTTP_READ_HEADER)
        {
            if (c == ':')
            {
                unsigned int id, len;
                http_header[http_header_len] = '\0';

                const char *hp = ParseUint(http_header, &id);

                if (hp == 0 || *hp != ',' || ParseUint(hp + 1, &len) == 0 || id > 4U)
                {
                    http_state = HTTP_WAIT_PREFIX;
                    http_prefix_pos = 0U;
                    http_accumulating = 0U;
                    continue;
                }

                if (upload_active && ((uint8_t)id == upload_link_id))
                {
                    http_link_id = (uint8_t)id;
                    http_req_expected = (uint16_t)len;
                    http_chunk_read = 0U;
                    http_state = (http_req_expected == 0U) ? HTTP_WAIT_PREFIX : HTTP_READ_BODY;
                }
                else
                {
                    if (upload_active)
                    {
                        http_state = HTTP_DISCARD_BODY;
                        http_discard_remaining = (uint16_t)len;
                        continue;
                    }

                    http_link_id = (uint8_t)id;
                    http_req_expected = (uint16_t)len;
                    http_chunk_read = 0U;

                    if (!http_accumulating)
                    {
                        http_req_len = 0U;
                        http_request[0] = '\0';
                    }

                    if (http_req_len + http_req_expected >= HTTP_REQ_SIZE)
                    {
                        http_state = HTTP_DISCARD_BODY;
                        http_discard_remaining = http_req_expected;
                    }
                    else
                    {
                        http_state = (http_req_expected == 0U) ? HTTP_WAIT_PREFIX : HTTP_READ_BODY;
                    }
                }
            }
            else if (http_header_len < HTTP_HEADER_SIZE - 1U)
            {
                http_header[http_header_len++] = c;
            }
            else
            {
                http_state = HTTP_WAIT_PREFIX;
                http_prefix_pos = 0U;
                http_accumulating = 0U;
            }
        }
        else if (http_state == HTTP_READ_BODY)
        {
            if (upload_active && (http_link_id == upload_link_id))
            {
                upload_staging[upload_staging_len++] = (uint8_t)c;
                upload_received_bytes++;
                http_chunk_read++;

                if (upload_staging_len >= sizeof(upload_staging))
                {
                    UINT bw = 0U;
                    FRESULT wr_fr = f_write(&SD_File, upload_staging, upload_staging_len, &bw);
                    if (wr_fr != FR_OK || bw != upload_staging_len)
                    {
                        HTTP_AbortUpload(upload_link_id, "MicroSD write error");
                        continue;
                    }
                    upload_staging_len = 0U;
                }

                if (http_chunk_read >= http_req_expected)
                {
                    if (upload_staging_len > 0U)
                    {
                        UINT bw = 0U;
                        FRESULT wr_fr = f_write(&SD_File, upload_staging, upload_staging_len, &bw);
                        if (wr_fr != FR_OK || bw != upload_staging_len)
                        {
                            HTTP_AbortUpload(upload_link_id, "MicroSD write error");
                            continue;
                        }
                        upload_staging_len = 0U;
                    }

                    if (upload_received_bytes >= upload_expected_bytes)
                    {
                        HTTP_FinishUpload(upload_link_id);
                    }
                    else
                    {
                        http_state = HTTP_WAIT_PREFIX;
                        http_prefix_pos = 0U;
                    }
                }
            }
            else
            {
                http_request[http_req_len++] = c;
                http_chunk_read++;

                if (http_chunk_read >= http_req_expected)
                {
                    uint8_t req_complete = 0U;
                    http_request[http_req_len] = '\0';

                    const char *head_end = strstr(http_request, "\r\n\r\n");
                    if (!head_end) head_end = strstr(http_request, "\n\n");

                    if (head_end != 0)
                    {
                        if (strncmp(http_request, "POST /upload", 12U) == 0)
                        {
                            uint8_t role = Auth_GetRole(http_request, 0, 0);

                            if (!AuthorizeRequest(role, "/upload"))
                            {
                                http_accumulating = 0U;
                                http_state = HTTP_WAIT_PREFIX;
                                OLED_Show3("DATAVAULT", "UPLOAD REJECT", "GUEST BLOCKED");
                                HTTP_RespondError(http_link_id, "403 Forbidden", "Upload forbidden for Guest");
                            }
                            else
                            {
                                upload_name[0] = '\0';
                                HTTP_ExtractParam(http_request, "name=", upload_name, sizeof(upload_name));
                                HTTP_UrlDecode(upload_name);

                                if (!HTTP_ValidateFilename(upload_name))
                                {
                                    http_accumulating = 0U;
                                    http_state = HTTP_WAIT_PREFIX;
                                    HTTP_RespondError(http_link_id, "400 Bad Request", "Invalid filename");
                                }
                                else
                                {
                                    const char *cl = strstr(http_request, "Content-Length:");
                                    if (!cl) cl = strstr(http_request, "content-length:");
                                    unsigned int clen = 0U;
                                    if (cl) (void)ParseUint(cl + 15, &clen);
                                    upload_expected_bytes = clen;

                                    if (upload_expected_bytes > MAX_UPLOAD_SIZE)
                                    {
                                        http_accumulating = 0U;
                                        http_state = HTTP_WAIT_PREFIX;
                                        HTTP_RespondError(http_link_id, "413 Payload Too Large", "File exceeds 2MB limit");
                                    }
                                    else if (f_open(&SD_File, "0:/UPLOAD.TMP", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
                                    {
                                        http_accumulating = 0U;
                                        http_state = HTTP_WAIT_PREFIX;
                                        HTTP_RespondError(http_link_id, "500 Internal Error", "MicroSD open failed");
                                    }
                                    else
                                    {
                                        const char *bstart = (head_end[1] == '\n') ? (head_end + 2) : (head_end + 4);
                                        uint16_t init_b = (uint16_t)(http_request + http_req_len - bstart);

                                        upload_received_bytes = 0U;
                                        upload_staging_len = 0U;
                                        if (init_b > 0U)
                                        {
                                            UINT bw = 0U;
                                            (void)f_write(&SD_File, bstart, init_b, &bw);
                                            upload_received_bytes += bw;
                                        }

                                        upload_link_id = http_link_id;

                                        if (upload_received_bytes >= upload_expected_bytes)
                                        {
                                            HTTP_FinishUpload(upload_link_id);
                                        }
                                        else
                                        {
                                            upload_active = 1U;
                                            http_accumulating = 1U;
                                            http_state = HTTP_WAIT_PREFIX;
                                            OLED_Show3("DATAVAULT", upload_name, "UPLOADING...");
                                        }
                                    }
                                }
                            }
                            req_complete = 0U;
                        }
                        else if (strncmp(http_request, "GET ", 4U) == 0 || strncmp(http_request, "HEAD ", 5U) == 0)
                        {
                            req_complete = 1U;
                        }
                        else if (strncmp(http_request, "POST ", 5U) == 0)
                        {
                            const char *cl = strstr(http_request, "Content-Length:");
                            if (!cl) cl = strstr(http_request, "content-length:");
                            if (cl != 0)
                            {
                                unsigned int clen = 0U;
                                if (ParseUint(cl + 15, &clen) != 0)
                                {
                                    const char *bstart = (head_end[1] == '\n') ? (head_end + 2) : (head_end + 4);
                                    size_t b_recvd = (size_t)(http_request + http_req_len - bstart);
                                    req_complete = (b_recvd >= clen) ? 1U : 0U;
                                }
                                else req_complete = 1U;
                            }
                            else req_complete = 1U;
                        }
                        else req_complete = 1U;
                    }

                    if (req_complete)
                    {
                        http_accumulating = 0U;
                        http_state = HTTP_WAIT_PREFIX;
                        HTTP_HandleRequest(http_link_id, http_request);
                    }
                    else if (!upload_active)
                    {
                        http_accumulating = 1U;
                        http_state = HTTP_WAIT_PREFIX;
                    }
                }
            }
        }
        else if (http_state == HTTP_DISCARD_BODY)
        {
            if (http_discard_remaining > 0U) http_discard_remaining--;
            if (http_discard_remaining == 0U)
            {
                http_state = HTTP_WAIT_PREFIX;
                if (http_accumulating)
                {
                    HTTP_RespondError(http_link_id, "413 Payload Too Large", "Request exceeded buffer limit");
                    http_accumulating = 0U;
                }
            }
        }
    }
}
/* ---------------- MAIN ---------------- */

int main(void)
{
    uint8_t server_result;
    uint8_t ip_found;

    SysTick_Init();
    GPIO_Init();
    I2C1_Init();
    USART2_Init();

    OLED_Init();
    OLED_Clear();

    OLED_PrintLine(0U, "DATAVAULT");
    OLED_PrintLine(2U, "PRIVATE CLOUD");
    OLED_PrintLine(4U, "STM32 + FAT32");
    OLED_PrintLine(6U, "WIFI STARTING");

    delay_ms(1500U);
		
    /* -------- SD CARD FILE TEST -------- */

    OLED_Show3("SD FILE TEST",
               "PLEASE WAIT",
               "");

    delay_ms(500U);

    uint8_t file_result = SD_FileTest();

    if (file_result == FILE_TEST_OK)
    {
        OLED_Show3("SD TEST PASSED",
                   "WRITE READ OK",
                   "DATA MATCH YES");
    }
    else
    {
        OLED_Show3("SD TEST FAILED",
                   "ERROR CODE",
                   (file_result == FILE_TEST_MOUNT_FAIL) ? "MOUNT FAIL" :
                   (file_result == FILE_TEST_OPEN_FAIL)  ? "OPEN FAIL"  :
                   (file_result == FILE_TEST_WRITE_FAIL) ? "WRITE FAIL" :
                   (file_result == FILE_TEST_READ_FAIL)  ? "READ FAIL"  :
                   "DATA MISMATCH");
    }

    delay_ms(3000U);

    /* -------- END SD CARD FILE TEST -------- */

    OLED_Show3("CHECK ESP",
               "SEND AT",
               "PLEASE WAIT");

    ESP_Command("AT", 2000U);

    if (!ESP_Has("OK"))
    {
        OLED_Show3("ESP NOT READY",
                   "CHECK UART",
                   "PA2 PA3 GND");

        while (1)
            delay_ms(1000U);
    }

    ESP_Command("ATE0", 1500U);
    ESP_Command("AT+CWMODE_CUR=1", 2000U);

    /* -------- CONNECT WIFI AND START HTTP SERVER -------- */

    ip_found = Network_Connect();
    server_result = ESP_ServerStart();

    /* Second chance: reset the ESP to clear leftover server/link state */
    if (server_result != SERVER_OK)
    {
        ESP_Restart();
        ip_found = Network_Connect();
        server_result = ESP_ServerStart();
    }

    if (server_result != SERVER_OK)
    {
        char reply[22];
        ESP_ReplySnippet(reply, sizeof(reply));
        Halt("SERVER FAILED",
             (server_result == SERVER_MUX_FAIL) ? "CIPMUX REJECTED" : "CIPSERVER REJECTED",
             reply);
    }

    /* Server is ready */
    OLED_Show3("HTTP SERVER",
               "SERVER READY",
               ip_found ? g_station_ip : "IP NOT FOUND");

    /* Continuously process browser requests */
    while (1)
    {
        ESP_ServerPoll();
    }
}