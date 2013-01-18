/*
 *  linux/drivers/mmc/host/jz_mmc/jz_mmc_host.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Copyright (c) Ingenic Semiconductor Co., Ltd.
 */

#ifndef __JZ_MMC_HOST_H__
#define __JZ_MMC_HOST_H__

#include <asm/jzmmc/jz_mmc_platform_data.h>
#include <asm/jzsoc.h>

#define MMC_CLOCK_SLOW    200000      /* 200 kHz for initial setup */
#define MMC_CLOCK_FAST  20000000      /* 20 MHz for maximum for normal operation */
#define SD_CLOCK_FAST   25000000      /* 25 MHz for SD Cards */
#define SD_CLOCK_HIGH   50000000      /* 50 MHz for SD Cards */
#define SDIO_CLOCK_HIGH 40000000      /* 25 MHz for SDIO Cards */
#define MMC_NO_ERROR  0

#define NR_SG	1
#define NUM_DESC 128

#define MSC_1BIT_BUS 0
#define MSC_4BIT_BUS 1
#define MSC_8BIT_BUS 2

#define SZ_4K                           0x00001000

typedef struct jzsoc_dma_desc {
	volatile u32 ddadr;	/* Points to the next descriptor + flags */
	volatile u32 dsadr;	/* DSADR value for the current transfer */
	volatile u32 dtadr;	/* DTADR value for the current transfer */
	volatile u32 dcmd;	/* DCMD value for the current transfer */
} jzsoc_dma_desc;

struct jz_mmc_curr_req {
	struct mmc_request	*mrq;
	struct mmc_command	*cmd;
	struct mmc_data		*data;
	int r_type;
};

enum trans_state {
	MSC_NOT_CARD = 0,        //This MSC isn't a card controllor.
	MSC_CARD_NROP,           //This operation is not a read operation.
	MSC_CARD_NAL,            //This read operation can't bring an aligned read operation next time.
	MSC_CARD_MAL,            //This read operation may bring an aligned read operation next time.

	MSC_CARD_ALAL,           //The read operation is an aligned operation this time,
                                 //and may bring an aligned read operation next time.

	MSC_CARD_ALNAL           //The read operation is an aligned operation this time,
                                 //but can't bring an aligned read operation next time.
};

struct desc_hd {
	jz_dma_desc_8word *dma_desc;
	dma_addr_t dma_desc_phys_addr;
	struct desc_hd *next;
};

struct jz_mmc_host {
	struct mmc_host *mmc;
	spinlock_t lock;

	struct {
		int len;
		int dir;
		int rxchannel;
		int txchannel;
		int channel;
	} dma;

	unsigned int clkrt;
	unsigned int cmdat;
	unsigned int imask;
	unsigned int power_mode;
	volatile unsigned int eject;
	unsigned int oldstat;
	unsigned int pdev_id;
	unsigned int has_4bytes;
	unsigned int  has_4bytes_done;
	void __iomem *base;

	struct resource *irqres;
	struct resource *memres;
	struct resource *dmares;

	struct jz_mmc_platform_data *plat;
	struct jz_mmc_curr_req curr;
	unsigned int dma_len;
	unsigned int dma_dir;
	int dma_ts;
	int irq_is_on;
	volatile int msc_ack;
	volatile int dma_ack;
	volatile int resp_ack;
	volatile int flag_cp;
	volatile long for_sync;
	
	volatile dma_addr_t dma_start_addr;

	volatile unsigned int cpu_start_addr;
	volatile unsigned int cpu_trans_bytes;

	volatile unsigned int aligned_bytes;
	volatile unsigned int unaligned_bytes;

	int detect_retry;
	unsigned int *dma_buf;
	jz_dma_desc_8word *dma_desc;
	dma_addr_t dma_desc_phys_addr;
	
	struct work_struct gpio_jiq_work;
	struct workqueue_struct *gpio_jiq_queue;

	struct work_struct msc_jiq_work;
	struct workqueue_struct *msc_work_queue;
	wait_queue_head_t msc_wait_queue;
	wait_queue_head_t dma_wait_queue;

	unsigned int *dma_stream_buf;
	unsigned int arg_last;
	unsigned int cmdat_bak;
	unsigned int cmd_bak;
	enum trans_state trans_state;
	volatile unsigned char dma_flag;
	volatile unsigned char trans_state_last;
	unsigned int buf_gap;
	wait_queue_head_t parallel_queue;

	struct charsd_dev *devp;

	struct desc_hd decshds[NUM_DESC];
	
	unsigned int bytes_dma_did;
};

#if 0
struct jz_mmc_functions {
	void (*deinit) (struct jz_mmc_host *, struct platform_device *);
	int (*transmit_data) (struct jz_mmc_host *);
	void (*execute_cmd) (struct jz_mmc_host *, struct mmc_command *, unsigned int);
	void (*set_clock) (struct jz_mmc_host *, int);
	void (*msc_deinit) (struct jz_mmc_host *);
	int (*gpio_deinit) (struct jz_mmc_host *, struct platform_device *);
	void (*dma_deinit) (struct jz_mmc_host *);
};
#endif

#endif /* __JZ_MMC_HOST_H__ */
