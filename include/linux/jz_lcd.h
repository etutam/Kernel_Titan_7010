#ifndef __JZ_LCD_H__
#define __JZ_LCD_H__

#define JZ_LCD_PFX "jz_lcd_panel"
#define LCD_PANEL_DEBUG

#ifdef LCD_PANEL_DEBUG
#define D(msg, fmt...)               \
	do {                         \
		printk(KERN_ERR JZ_LCD_PFX": %s(): "msg, __func__, ##fmt); \
	} while(0)
#else
#define D(msg, fmt...)               \
	do{} while(0)
#endif

/********************************************************************************/
/**
 * struct lcd_board_pin_info - defines all the pins which lcd may be in use.
 * @LCD_DITHB_PIN: 
 * @LCD_DITHB_PIN: dither function
 * @LCD_UD_PIN:
 * @LCD_LR_PIN:
 * @LCD_MODE_PIN:
 * @LCD_RESET_PIN:
 * @LCD_VCC_EN:
 * @LCD_DE_PIN:
 * @LCD_VSYNC_PIN:
 * @LCD_HSYNC_PIN:
 * @LCD_POWERON:
 * @LCD_SPCK:
 * @LCD_SPDA:
 */
struct lcd_board_pin_info {
	unsigned int LCD_DITHB_PIN;
	unsigned int LCD_UD_PIN;
	unsigned int LCD_LR_PIN;
	unsigned int LCD_MODE_PIN;
	unsigned int LCD_RESET_PIN;
	unsigned int LCD_VCC_EN;
	unsigned int LCD_DE_PIN;
	unsigned int LCD_VSYNC_PIN;
	unsigned int LCD_HSYNC_PIN;
	unsigned int LCD_POWERON;
	unsigned int LCD_STBY_PIN;
	unsigned int SPCK;
	unsigned int SPEN;
	unsigned int SPDT;
	unsigned int SPRS;
	unsigned int LCD_CSB;
	unsigned int LCD_DIMO;
	unsigned int LCD_DIMI;
};

/**
 * struct lcd_board_ops_info - board-specific operations 
 *
 */
struct lcd_board_ops_info {
	void (*lcd_board_power_on) (void);
	void (*lcd_board_power_off) (void);
};

struct lcd_board_info
{
	char *named_panel;
	struct lcd_board_pin_info *board_pin;
	struct lcd_board_ops_info *board_ops;
};

/********************************************************************************/
/**
 * struct lcd_soc_ops_info - SOC-specific operations 
 *
 */
struct lcd_soc_ops_info {
	void (*gpio_as_output) (unsigned int gpio);
	void (*gpio_as_input) (unsigned int gpio);
	void (*gpio_set_pin) (unsigned int gpio);
	int (*gpio_get_pin) (unsigned int gpio);
	void (*gpio_clear_pin) (unsigned int gpio);
	void (*gpio_enable_pull) (unsigned int gpio);
	void (*gpio_disable_pull) (unsigned int gpio);

	void (*spi_init) (struct lcd_board_info *board, unsigned int timming);
	void (*spi_read) (unsigned char reg, unsigned char *val, unsigned char readByte);
	void (*spi_read_nodummy) (unsigned char reg, unsigned char *val, unsigned char readByte);
	void (*spi_write) (unsigned char reg, unsigned char val);
};

struct lcd_soc_info
{
	struct lcd_soc_ops_info *soc_ops;
};

/********************************************************************************/
/**
 * struct lcd_panel_ops - the lcd panel specific operations
 * each panel must fulfill those api below.
 * @lcd_display_pin_init: lcd panel specific pin initialization.
 * @lcd_display_on: open lcd panel.
 * @lcd_display_off: close lcd panel.
 * @spi_write_reg: write lcd panel registers via spi.
 * @spi_read_reg: read lcd panel registers via spi.
 * @probe: if there are several panels in use, the @probe can be used to
 * 	determine if the panel exists.
 */
struct lcd_panel_ops_info {
	void (*panel_init) (void);
	void (*panel_display_on) (void);
	void (*panel_display_off) (void);
	void (*panel_special_on) (void);
	void (*panel_special_off) (void);

	int (*panel_probe) (struct lcd_soc_info *soc , struct lcd_board_info *board);
};

struct lcd_panel_attr_info {
	unsigned int bpp;	/*  */
	unsigned int vsp;	/* VSYNC polarity:0-rising,1-falling */
	unsigned int hsp;	/* HSYNC polarity:0-active high,1-active low */
	unsigned int pcp;	/* PCLK polarity:0-rising,1-falling */
	unsigned int dep;	/* DE polarity:0-active high,1-active low */
	unsigned int slcd_cfg;	/* Smart lcd configurations */
	unsigned int w;		/* Panel Width(in pixel) */
	unsigned int h;		/* Panel Height(in line) */
	unsigned int fclk;	/* frame clk */
	unsigned int hsw;	/* hsync width, in pclk */
	unsigned int vsw;	/* vsync width, in line count */
	unsigned int elw;	/* end of line, in pclk */
	unsigned int blw;	/* begin of line, in pclk */
	unsigned int efw;	/* end of frame, in line count */
	unsigned int bfw;	/* begin of frame, in line count */
	unsigned int fg0_bpp;	/* fg0 bpp */
	unsigned int fg1_bpp;	/* fg1 bpp */
	unsigned int ctrl;	/* lcd control register*/
};

struct lcd_panel_info
{
	struct list_head list;
	char   *name;
	int panel_probable;	/*is panel supportive auto probe: 0-not support, 1-support*/

	struct lcd_panel_ops_info *panel_ops;
	struct lcd_panel_attr_info *panel_attr;
};

#endif /* __JZ_LCD_H__ */
