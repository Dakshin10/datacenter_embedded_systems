# Mini Multi-User Networked Data Center (DataVault)

An embedded private cloud storage and multi-user networked data center running bare-metal on the **STM32F411CEU6 (ARM Cortex-M4)** microcontroller. The system integrates an **ESP8266 Wi-Fi module**, **MicroSD card storage with FAT32 (FatFs)**, and an **SSD1306 128x64 OLED display** to serve a full-featured web portal directly from embedded flash memory.

---

## System Architecture

```
                      +---------------------------------------+
                      |      STM32F411CEU6 (Cortex-M4)       |
                      |  512 KB Flash  /  128 KB SRAM         |
                      +---+-------------------+-----------+---+
                          |                   |           |
               I2C1 (PB6/PB7)       USART2 (PA2/PA3)   SPI1 (PA4-PA7)
                          |                   |           |
                          v                   v           v
                  +---------------+   +---------------+   +---------------+
                  |  SSD1306 OLED |   |    ESP8266    |   |  MicroSD Card |
                  |  (128x64 I2C) |   |  Wi-Fi Module |   | (FAT32/FatFs) |
                  | Real-Time Mon |   |  HTTP Server  |   | Storage Vault |
                  +---------------+   +-------+-------+   +---------------+
                                              |
                                     Wi-Fi 802.11 b/g/n
                                              |
                                              v
                                   +---------------------+
                                   |   Web Browser /     |
                                   | Multiple User Client|
                                   +---------------------+
```

---

## Features

- **Embedded HTTP Web Server**: Multi-connection TCP server on port 80 powered by the ESP8266 AT command engine.
- **MicroSD Card Storage (FatFs)**: Full FAT32 file system support for listing, creating, appending, reading, and deleting files on the SD card.
- **Role-Based Access Control (RBAC)**:
  - **Administrator (`admin`)**: Full permissions (browse, read, write, append, and delete files).
  - **Standard User (`user`)**: Browse, read, write, and append permissions.
  - **Guest**: Read-only public access.
  - Token-based session management with automatic credential authentication.
- **Real-Time OLED System Dashboard**:
  - Displays boot diagnostics and SD card read/write verification.
  - Displays Wi-Fi connection progress and assigned station IP address.
  - Live activity notifications on client connections, logins, file write, and delete operations.
- **Built-in Power-on Self-Test (POST)**: Performs automatic read/write and data integrity verification on the SD card during startup.
- **Robust Error Handling**: HTTP error responses (400 Bad Request, 403 Forbidden, 404 Not Found, 413 Payload Too Large, 500 Internal Error) and memory bounds protection.

---

## Hardware Pinout & Wiring

### 1. MicroSD Card Module (SPI1)

| SD Pin | STM32F411 Pin | Mode / Function |
| :--- | :--- | :--- |
| **CS** | `PA4` | General Purpose Output (Active LOW) |
| **SCK** | `PA5` | Alternate Function 5 (SPI1_SCK) |
| **MISO** | `PA6` | Alternate Function 5 (SPI1_MISO, Pull-up) |
| **MOSI** | `PA7` | Alternate Function 5 (SPI1_MOSI) |
| **VCC** | `3.3V` / `5V` | Regulated DC power |
| **GND** | `GND` | Ground reference |

### 2. SSD1306 OLED Display (I2C1 - 0x3C)

| OLED Pin | STM32F411 Pin | Mode / Function |
| :--- | :--- | :--- |
| **SCL** | `PB6` | Alternate Function 4 (I2C1_SCL, Open-Drain) |
| **SDA** | `PB7` | Alternate Function 4 (I2C1_SDA, Open-Drain) |
| **VCC** | `3.3V` | DC power |
| **GND** | `GND` | Ground reference |

### 3. ESP8266 Wi-Fi Module (USART2 @ 115200 Baud)

| ESP8266 Pin | STM32F411 Pin | Mode / Function |
| :--- | :--- | :--- |
| **RX** | `PA2` | Alternate Function 7 (USART2_TX) |
| **TX** | `PA3` | Alternate Function 7 (USART2_RX) |
| **CH_PD / EN** | `3.3V` | Chip enable |
| **VCC** | `3.3V` (Dedicated >= 500mA source) | DC power |
| **GND** | `GND` | Common ground |

---

## Default Credentials

### Wi-Fi Configuration
- **SSID**: `Dakshin`
- **Password**: `DakshinData`

*(Adjustable via `#define WIFI_SSID` and `#define WIFI_PASSWORD` in `code.c`)*

### Web Authentication
| Role | Username | Password | Permissions |
| :--- | :--- | :--- | :--- |
| **Admin** | `admin` | `Admin@2026` | Read, Write, Append, Delete |
| **User** | `user` | `User@2026` | Read, Write, Append |
| **Guest** | *None* | *None* | Read Only |

---

## Project Structure

```
├── .gitignore                                 # Git ignore rules for Keil MDK & temporary files
├── README.md                                  # Comprehensive documentation
├── code.c                                     # Main firmware: HTTP server, OLED driver, RBAC, web UI
├── sd_spi.c                                   # Bare-metal STM32 SPI1 driver for SD Card
├── sd_spi.h                                   # SPI driver definitions and card initialization
├── datacenter_project.uvprojx                 # Keil uVision 5 project definition
├── datacenter_project.uvoptx                  # Keil uVision 5 debugger and target options
├── datacenter_project.hex                     # Pre-compiled Intel HEX flashable firmware
├── DebugConfig/
│   └── Target_1_STM32F411CEUx.dbgconf         # Keil debug target configuration
├── FatFs/                                     # ChaN FatFs FAT32 filesystem library
│   ├── diskio.c                               # Storage device interface glue layer
│   ├── diskio.h                               # Disk I/O definitions
│   ├── ff.c                                   # FatFs core file operations
│   ├── ff.h                                   # FatFs API declarations
│   ├── ffconf.h                               # Filesystem configuration (buffers, code page)
│   ├── ffunicode.c                            # Unicode / LFN support tables
│   └── sd_spi.h                               # SPI interface bindings
└── RTE/                                       # Keil Run-Time Environment
    ├── Device/STM32F411CEUx/
    │   ├── startup_stm32f411xe.s              # Cortex-M4 startup vector table & stack init
    │   ├── system_stm32f4xx.c                 # System initialization and clock tree setup
    │   └── STM32F401xBCDE_411xCE.dbgconf      # CMSIS-DAP debug configuration
    └── _Target_1/
        └── RTE_Components.h                   # RTE component header
```

---

## Building and Flashing

### Using Keil MDK (uVision 5)
1. Open `datacenter_project.uvprojx` in **Keil uVision 5**.
2. Select target `Target_1` (STM32F411CEUx).
3. Ensure **Arm Compiler Version 6 (ARMCLANG)** is selected with `-Oz` optimization enabled.
4. Click **Project -> Build Target** (`F7`) to compile.
5. Connect your ST-Link / CMSIS-DAP debugger to the SWD pins (`SWDIO`, `SWCLK`, `GND`, `3.3V`).
6. Click **Flash -> Download** (`F8`) to program the microcontroller.

### Using Pre-Compiled Binary with STM32CubeProgrammer
Connect your ST-Link or put the STM32F411 into DFU mode, then flash the included HEX file:
```bash
STM32_Programmer_CLI -c port=SWD -w datacenter_project.hex -v -rst
```

---

## Authors & License

Developed by **Dakshin Raj** for the Embedded Systems Project (Semester 5).
All rights reserved.