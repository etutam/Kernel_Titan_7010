//wx_add for remote ctrl ;


#if 1


#ifndef _LINUX_JZ_REMOTE_H_
#define _LINUX_JZ_REMOTE_H_


 struct jz_remote
{

    unsigned int RMC_INT;			//GPIO¿Ú
    unsigned int RMC_IRQ;	
    struct timer_list timer;
};


#endif

#endif
