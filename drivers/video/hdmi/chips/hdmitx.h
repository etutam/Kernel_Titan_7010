///*****************************************
//  Copyright (C) 2009-2014
//  ITE Tech. Inc. All Rights Reserved
//  Proprietary and Confidential
///*****************************************
//   @file   >hdmitx.h<
//   @author Jau-Chih.Tseng@ite.com.tw
//   @date   2009/02/19
//   @fileversion: CAT6611_SAMPLEINTERFACE_1.08
//******************************************/

#ifndef _HDMITX_H_
#define _HDMITX_H_

// #define SUPPORT_DEGEN
#define SUPPORT_SYNCEMB

#define SUPPORT_EDID
#define SUPPORT_HDCP
#define SUPPORT_INPUTRGB
#define SUPPORT_INPUTYUV444
#define SUPPORT_INPUTYUV422


#if defined(SUPPORT_INPUTYUV444) || defined(SUPPORT_INPUTYUV422)
#define SUPPORT_INPUTYUV
#endif

// #define DEBUG
#ifdef DEBUG
#define ErrorF 	printk
#else
#define ErrorF(args...)  
#endif

//#include <stdio.h>
//#include <stdlib.h>
//#include <stdarg.h>
// #include "mcu.h"
#include "typedef.h"
#include "cat6611_sys.h"
#include "cat6611_drv.h"
#include "edid.h"

//////////////////////////////////////////////////////////////////////
// Function Prototype
//////////////////////////////////////////////////////////////////////

BYTE HDMITX_ReadI2C_Byte(BYTE RegAddr);
SYS_STATUS HDMITX_WriteI2C_Byte(BYTE RegAddr, BYTE d);
SYS_STATUS HDMITX_ReadI2C_ByteN(BYTE RegAddr, BYTE *pData, int N);
SYS_STATUS HDMITX_WriteI2C_ByteN(SHORT RegAddr, BYTE *pData, int N);

// dump
#ifdef DEBUG
void DumpCat6611Reg() ;
#endif
// I2C

//////////////////////////////////////////////////////////////////////////////
// main.c
//////////////////////////////////////////////////////////////////////////////

#endif // _HDMITX_H_

