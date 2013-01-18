#ifndef JZ47XX_BATTERY_H
#define JZ47XX_BATTERY_H

struct battery_info {
	int max_vol;
	int min_vol;
	int capacity;
	int dc_chg_max_vol;
	int dc_chg_min_vol;
	int usb_chg_max_vol;
	int usb_chg_min_vol;
	int battery_mah;
	int dc_charg_ma;
	int usb_charg_ma;
	unsigned long update_time;
	int windage;
};

struct battery_core {
	void *pdev;
	int charg_state;
	int voltage_filter;
	int capacity_filter;
	int dc_charg_time;
	int usb_charg_time;
	struct battery_info info;
	void (*enable)(void *);
	int (*read_bat)(void);
	int (*curve)[2];
};

struct battery_args {
	int ad_value;
	int next_scan_time;
	int st_full;
	int usb;
	int dc;
};

struct storage {
	int fd;
};

struct jz47xx_detect_pin {
	int pin;
	int low_active;
};

struct jz47xx_battery_platform_data {
	struct jz47xx_detect_pin gpio_usb_dete;
	struct jz47xx_detect_pin gpio_bat_dete;
	struct jz47xx_detect_pin gpio_dc_dete;
	struct jz47xx_detect_pin gpio_charg_stat;
	struct battery_info *info;
	int irq;
};

int battery_load_info(struct battery_core *c,struct battery_info *info);
int battery_save_info(struct battery_core *c);

struct battery_core *new_battery_core(void);
void battery_calculate(struct battery_core *c,struct battery_args *args);
int init_capacity(struct battery_core *c,struct battery_args *args);
void battery_update_scan_time(struct battery_core *c,struct battery_args *args);

int jz_read_bat(void);
int read_dete_pin(struct jz47xx_detect_pin *dete);

extern int g_jz_battery_min_voltage;
#endif
