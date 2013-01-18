/*
 * Copyright (C) 2005 Mips Technologies
 * Author: Chris Dearman, chris@mips.com derived from fpu.h
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#ifndef _ASM_MXU_H
#define _ASM_MXU_H

#include <asm/cpu.h>
#include <asm/cpu-features.h>
#include <asm/hazards.h>
#include <asm/mipsregs.h>
#include <asm/jzmedia.h>

static inline void __init_mxu(void)
{
	S32I2M(xr16,3);  //enable_mxu 
}

#define __save_mxu_reg(tsk,reg,off)			 \
	do {									 \
	unsigned int reg_val = S32M2I(xr##reg);  \
	*(unsigned int *)((char *)tsk->thread.mxu.regs + off) = reg_val; \
	}while(0)

#define __save_mxu(tsk)							\
	do {										\
		__save_mxu_reg(tsk,16, 0);		\
		__save_mxu_reg(tsk, 1, 4);		\
		__save_mxu_reg(tsk, 2, 8);		\
		__save_mxu_reg(tsk, 3,12);	\
		__save_mxu_reg(tsk, 4,16);	\
		__save_mxu_reg(tsk, 5,20);	\
		__save_mxu_reg(tsk, 6,24);	\
		__save_mxu_reg(tsk, 7,28);	\
		__save_mxu_reg(tsk, 8,32);	\
		__save_mxu_reg(tsk, 9,36);	\
		__save_mxu_reg(tsk,10,40);	\
		__save_mxu_reg(tsk,11,44);	\
		__save_mxu_reg(tsk,12,48);	\
		__save_mxu_reg(tsk,13,52);	\
		__save_mxu_reg(tsk,14,56);	\
		__save_mxu_reg(tsk,15,60);	\
	} while (0)

#define __restore_mxu_reg(tsk,reg,off) \
	do {									 \
		unsigned int reg_val =									 \
			*(unsigned int *)((char *)tsk->thread.mxu.regs + off);	\
		S32I2M(xr##reg, reg_val);									\
	}while(0)

#define __restore_mxu(tsk)						\
	do {										\
		__restore_mxu_reg(tsk,16, 0);		\
		__restore_mxu_reg(tsk, 1, 4);		\
		__restore_mxu_reg(tsk, 2, 8);		\
		__restore_mxu_reg(tsk, 3,12);	\
		__restore_mxu_reg(tsk, 4,16);	\
		__restore_mxu_reg(tsk, 5,20);	\
		__restore_mxu_reg(tsk, 6,24);	\
		__restore_mxu_reg(tsk, 7,28);	\
		__restore_mxu_reg(tsk, 8,32);	\
		__restore_mxu_reg(tsk, 9,36);	\
		__restore_mxu_reg(tsk,10,40);	\
		__restore_mxu_reg(tsk,11,44);	\
		__restore_mxu_reg(tsk,12,48);	\
		__restore_mxu_reg(tsk,13,52);	\
		__restore_mxu_reg(tsk,14,56);	\
		__restore_mxu_reg(tsk,15,60);	\
	} while (0)

static inline void init_mxu(void)
{
	if(cpu_has_mxu)
		__init_mxu();
}


#define save_mxu(tsk)							\
	do {										\
		if (cpu_has_mxu)						\
			__save_mxu(tsk);					\
	} while (0)

#define restore_mxu(tsk)						\
	do {										\
		if (cpu_has_mxu)						\
			__restore_mxu(tsk);					\
	} while (0)

#define __get_mxu_regs(tsk)						\
	({											\
		if (tsk == current)						\
			__save_mxu(current);				\
												\
		tsk->thread.mxu.regs;					\
	})
#define __let_mxu_regs(tsk,regs)				\
	do{											\
		int i;									\
		for(i = 0; i < NUM_MXU_REGS;i++)		\
			tsk->thread.mxu.regs[i] = regs[i];	\
		if (tsk == current)						\
			__save_mxu(current);				\
	}while(0)

#endif /* _ASM_MXU_H */
