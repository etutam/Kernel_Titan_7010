/*
 * drivers/input/touchscreen/jz_mt4d_ts.h
 *
 * JZ ITO Touch Screen Driver's head file
 *
 * Copyright (c) 2005 - 2009  Ingenic Semiconductor Inc.
 *
 * Author: Emily <chlfeng@ingenic.cn>
 *         
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef __JZ_MT4D_TS_H__
#define __JZ_MT4D_TS_H__

#include <linux/ioctl.h>

/* the address of register */
#define MT4D_TS_STATUS          (0x00)
#define MT4D_TS_POSX            (0x01)
#define MT4D_TS_POSY            (0x03)
#define MT4D_TS_POSX2           (0x05)
#define MT4D_TS_POSY2           (0x07)
#define MT4D_TS_STRENGTH        (0x09)
#define MT4D_TS_ID              (0x0C)
#define MT4D_TS_XTHR            (0x0D)
#define MT4D_TS_YTHR            (0x0E)
#define MT4D_TS_OFFSET          (0x13)
#define MT4D_TS_MODE            (0x14)
#define MT4D_TS_EEPROM          (0x15)

/* MT4D's three power management mode */
#define WATCH_MODE            0x00
#define FAST_MODE             0x20
#define FREEZE_MODE           0x90


#define RBUFF_SIZE 11

struct jz_mt4d_ts_platform_data {
	int intr;
};


#define MT4DIO				0x4D

/* IOCTLs for MT4D library */                
#define MT4D_SET_MODE			_IO(MT4DIO, 0x01)
#define MT4D_CALIBRATION		_IO(MT4DIO, 0x02)

#endif  //__JZ_MT4D_TS_H__
