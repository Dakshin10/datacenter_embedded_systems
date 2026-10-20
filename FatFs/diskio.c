#include "ff.h"
#include "diskio.h"
#include "sd_spi.h"
#include <stdint.h>

extern uint8_t SD_Init(void);
extern uint8_t SD_ReadBlock(uint32_t sector, uint8_t *buffer);
extern uint8_t SD_WriteBlock(uint32_t sector, const uint8_t *buffer);
extern uint32_t SD_GetSectorCount(void);

static DSTATUS Stat = STA_NOINIT;

DSTATUS disk_initialize(BYTE pdrv)
{
    uint8_t result;

    if (pdrv != 0)
        return STA_NOINIT;

    result = SD_Init();

    /* SD_Init() returns 0 on success */
    Stat = (result == 0) ? 0 : STA_NOINIT;

    return Stat;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0)
        return STA_NOINIT;

    return Stat;
}

DRESULT disk_read(
    BYTE pdrv,
    BYTE *buff,
    LBA_t sector,
    UINT count)
{
    UINT i;

    if (pdrv != 0)
        return RES_PARERR;

    if (Stat & STA_NOINIT)
        return RES_NOTRDY;

    if (buff == 0 || count == 0)
        return RES_PARERR;

    for (i = 0; i < count; i++)
    {
        if (!SD_ReadBlock((uint32_t)sector, buff))
            return RES_ERROR;

        sector++;
        buff += 512;
    }

    return RES_OK;
}

#if FF_FS_READONLY == 0

DRESULT disk_write(
    BYTE pdrv,
    const BYTE *buff,
    LBA_t sector,
    UINT count)
{
    UINT i;

    if (pdrv != 0)
        return RES_PARERR;

    if (Stat & STA_NOINIT)
        return RES_NOTRDY;

    if (buff == 0 || count == 0)
        return RES_PARERR;

    for (i = 0; i < count; i++)
    {
        if (!SD_WriteBlock((uint32_t)sector, buff))
            return RES_ERROR;

        sector++;
        buff += 512;
    }

    return RES_OK;
}

#endif

DRESULT disk_ioctl(
    BYTE pdrv,
    BYTE cmd,
    void *buff)
{
    uint32_t sectors;

    if (pdrv != 0)
        return RES_PARERR;

    if (Stat & STA_NOINIT)
        return RES_NOTRDY;

    if (buff == 0 && cmd != CTRL_SYNC)
        return RES_PARERR;

    switch (cmd)
    {
        case CTRL_SYNC:
            return RES_OK;

        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;

        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;
            return RES_OK;

        case GET_SECTOR_COUNT:

            sectors = SD_GetSectorCount();

            if (sectors == 0)
                return RES_ERROR;

            *(LBA_t *)buff = (LBA_t)sectors;

            return RES_OK;

        default:
            return RES_PARERR;
    }
}