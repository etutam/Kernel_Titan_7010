#ifndef _JZ_TCU_PWM_H
#define _JZ_TCU_PWM_H

#include <linux/interrupt.h> 
#include <mach/chip-tcu.h>
#include <mach/chip-misc.h>

/* define for TCU CHANNEL*/
#define  JZ_TIMER_CHANNEL0  				0
#define  JZ_TIMER_CHANNEL1       		    1
#define  JZ_TIMER_CHANNEL2      		    2
#define  JZ_TIMER_CHANNEL3       		    3
#define  JZ_TIMER_CHANNEL4        	        4
#define  JZ_TIMER_CHANNEL5        	        5
#define  JZ_TIMER_CHANNEL6        	        6
#define  JZ_TIMER_CHANNEL7        	        7
#define  JZ_TIMER_MODE1   					8
#define  JZ_TIMER_MODE2   					9
#define  JZ_TIMER_VALID    					10

#define JZ_PWM_CHANNEL0                     JZ_TIMER_CHANNEL0
#define JZ_PWM_CHANNEL1                     JZ_TIMER_CHANNEL1
#define JZ_PWM_CHANNEL2                     JZ_TIMER_CHANNEL2
#define JZ_PWM_CHANNEL3                     JZ_TIMER_CHANNEL3
#define JZ_PWM_CHANNEL4                     JZ_TIMER_CHANNEL4
#define JZ_PWM_CHANNEL5                     JZ_TIMER_CHANNEL5
#define JZ_PWM_CHANNEL6                     JZ_TIMER_CHANNEL6
#define JZ_PWM_CHANNEL7                     JZ_TIMER_CHANNEL7

/*define for PWM GPIO*/
#define  JZ_GPIO_PWM0   					GPE00
#define  JZ_GPIO_PWM1   					GPE01
#define  JZ_GPIO_PWM2   					GPE02
#define  JZ_GPIO_PWM3   					GPE03
#define  JZ_GPIO_PWM4   				    GPE04
#define  JZ_GPIO_PWM5   					GPE05
#define  JZ_GPIO_PWM6   					GPD10
#define  JZ_GPIO_PWM7   					GPD11


/*define for TCU state*/
#define  JZ_TCU_NULL_STATE        	        0
#define  JZ_TCU_PWM_STATE                   1
#define  JZ_TCU_TIMER_STATE                 2


/*define for PWM init level*/
#define  JZ_PWM_LOW_INIT    				0
#define  JZ_PWM_HIGH_INIT   			    TCSR_INITL_HIGH

/*define for TIMER ioctl cmd*/
#define  JZ_TIMER_SET_START    			    0
#define  JZ_TIMER_SET_RESTART  			    1
#define  JZ_TIMER_RESTART_ONLY  		    2
#define  JZ_TIMER_SET_ONLY  				3
#define  JZ_TIMER_STOP     				    4

/*define for PWM ioctl cmd */
#define  JZ_PWM_SET_START   				0
#define  JZ_PWM_SET_RESTART  				1
#define  JZ_PWM_RESTART_ONLY  			    2
#define  JZ_PWM_SET_ONLY    				3   
#define  JZ_PWM_STOP        				4



/*define for TCU clock*/
#define  JZ_TCU_PCK_CLOCK(DIV)                  ((TCSR_PRESCALE##DIV) | (TCSR_PCK_EN))
#define  JZ_TCU_EXT_CLOCK(DIV)                  ((TCSR_PRESCALE##DIV) | (TCSR_EXT_EN))
#define  JZ_TCU_RTC_CLOCK(DIV)                  ((TCSR_PRESCALE##DIV) | (TCSR_RTC_EN))
typedef enum 
{
    PCK1 = JZ_TCU_PCK_CLOCK(1), 
    PCK4 = JZ_TCU_PCK_CLOCK(4), 
    PCK16 = JZ_TCU_PCK_CLOCK(16),
    PCK64 = JZ_TCU_PCK_CLOCK(64), 
    PCK256 = JZ_TCU_PCK_CLOCK(256),
    PCK1024 = JZ_TCU_PCK_CLOCK(1024),   

    RTC1 = JZ_TCU_RTC_CLOCK(1), 
    RTC4 = JZ_TCU_RTC_CLOCK(4), 
    RTC16 = JZ_TCU_RTC_CLOCK(16),
    RTC64 = JZ_TCU_RTC_CLOCK(64), 
    RTC256 = JZ_TCU_RTC_CLOCK(256),
    RTC1024 = JZ_TCU_RTC_CLOCK(1024),  

    EXT1 = JZ_TCU_EXT_CLOCK(1), 
    EXT4 = JZ_TCU_EXT_CLOCK(4), 
    EXT16 = JZ_TCU_EXT_CLOCK(16),
    EXT64 = JZ_TCU_EXT_CLOCK(64), 
    EXT256 = JZ_TCU_EXT_CLOCK(256),
    EXT1024 = JZ_TCU_EXT_CLOCK(1024),                                           

}jz_tcu_clock;

/*define for TCU IRQ mode */
#define  JZ_TCU_HALF_IRQ    				0
#define  JZ_TCU_FULL_IRQ  					1
#define  JZ_TCU_ALL_IRQ   					2
#define  JZ_TCU_NULL_IRQ  					3

/*define for TCU irq state*/
#define  IRQ_NOT_REQUEST                    0
#define  IRQ_HAVE_REQUEST                   1

/*define for tcu_udelay list node*/
struct tcu_udelay_node {
    unsigned int wake_value;
    struct task_struct *wake_task;
    struct list_head  list;

};


int  jz_timer_request( int timer_ch , void (*irqhandler)(void *dev_id),
                       int timer_irq_mode, void * dev_id);
int  jz_pwm_request(unsigned int pwm_gpio);

int  jz_timer_init( int timer_ch, jz_tcu_clock clock, unsigned short int half_num, 
				    unsigned short int full_num);
int  jz_pwm_init( int pwm_ch, jz_tcu_clock clock,  unsigned short int full_num, int init_level);

int jz_timer_ioctl(int timer_ch, unsigned int cmd,unsigned short int value_start);
int jz_pwm_ioctl(int pwm_ch, unsigned int cmd, int duty_ratio);

int jz_timer_read(int timer_ch);
int jz_pwm_read(int pwm_ch); 

int jz_timer_free(int timer_ch);
int jz_pwm_free(int pwm_ch); 

void jz_timer_dump_reg(int timer_ch);
void jz_pwm_dump_reg(int pwm_ch);

#endif
