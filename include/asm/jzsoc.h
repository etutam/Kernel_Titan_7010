/*
 *  linux/include/asm-mips/jzsoc.h
 *
 *  Ingenic's JZXXXX SoC common include.
 *
 *  Copyright (C) 2006 - 2008 Ingenic Semiconductor Inc.
 *
 *  Author: <jlwei@ingenic.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ASM_JZSOC_H__
#define __ASM_JZSOC_H__

/*
 * SoC include
 */

#ifdef CONFIG_SOC_JZ4770
#include <mach/jz4770.h>
#endif

#define GSS_OUTPUT_LOW 		1
#define GSS_OUTPUT_HIGH 	2
#define GSS_INPUT_PULL 		3
#define GSS_INPUT_NOPULL 	4
#define GSS_IGNORE 		5
#define GSS_TABLET_END		0x999

#endif /* __ASM_JZSOC_H__ */
