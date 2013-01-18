#ifndef __JZ_CHARSD_H__
#define __JZ_CHARSD_H__

#include <linux/cdev.h>

#include <asm/jzsoc.h>
#include <asm/jzmmc/jz_mmc_dma.h>

#define DEVNAME                 "jz_charsd"
#define NUMBER_OF_CHARSD        2
#define JZ_CHARSD_MAJOR         0                               //0--dynamic alloc,others--staticly
#define P4_START_ADDR           0x40000000
#define CHARSD_IRQ              1
#define DEFSD_IRQ               2
#define NUM_OF_BUFFERS          2
#define PALU_BUFFER_SIZE        (64 * 1024 * 1024)

#define WAITMASK  \
(MSC_STAT_CRC_RES_ERR | \
MSC_STAT_CRC_READ_ERROR | MSC_STAT_CRC_WRITE_ERROR_MASK | \
 MSC_STAT_TIME_OUT_RES | MSC_STAT_TIME_OUT_READ)

enum palu_buffer_state {
	PBUF_STATE_EMPTY = 0,
	PBUF_STATE_FULL,
	PBUF_STATE_BUSY
};

struct charsd_palu_buffer {
	unsigned char num;
	enum palu_buffer_state state;
	unsigned char *buf;
	unsigned long buf_phys;

	struct charsd_palu_buffer *next;

	unsigned int blks_2wr;                          //The number of block to Write/Read
	loff_t data_addr;                         //Address in operated card
	unsigned char data_dir;                         //Value:1--Write operation,0--Read operation

};

struct uminor {
	unsigned int minor;
	loff_t partition_start_addr;
	unsigned char used;
};

struct charsd_dev {
	struct cdev cdev;
	unsigned int blks_2wr;                          //The number of block to Write/Read
	loff_t data_addr;                         //Address in operated card
	char *buf_2wr;                                  //Address of buffer
	unsigned char data_dir;                         //Value:1--Write operation,0--Read operation
	unsigned char ishc;                             //Value:1--SDHC,0--SDSC
	unsigned char pdev_id;                          //Value:0--Inand,2--extern card
	unsigned char use_cpu;                          //Value:0--Use DMA transfer,2--only use CPU transfer
	unsigned int rca;                               //Card's RCA
	loff_t pa_start_addr;
	struct jz_mmc_host *host;
	wait_queue_head_t dma_wait_queue;
	volatile int dma_ack;
	struct uminor uminor[32];
	unsigned char error;
	unsigned char used;
	spinlock_t super_lock;
	struct semaphore sem;

	struct task_struct *thread;
	struct semaphore thread_sem;

	struct charsd_palu_buffer palu_buffers[NUM_OF_BUFFERS];
	struct charsd_palu_buffer *next_palu_buffer;
	struct charsd_palu_buffer *operatable_palu_buffer;
	wait_queue_head_t palu_wait_queue;
};

extern irqreturn_t jz_mmc_dma_rx_callback(int irq, void *devid);
extern irqreturn_t jz_mmc_dma_tx_callback(int irq, void *devid);

#endif
