/*****************************************
 Copyright © 2001-2004
 Sigma Designs, Inc. All Rights Reserved
 Proprietary and Confidential
 *****************************************/
/**
  @file   dvi_hdmi.c
  @brief  DVI, HDMI, and CEA/EIA 861 implementation
  
  @author Jacques Mahe, Christian Wolff
  @date   2004-07-28
*/

// #define  _DEBUG

// to enable or disable the debug messages of this source file, put 1 or 0 below
#if 0
#define LOCALDBG ENABLE
#else
#define LOCALDBG DISABLE
#endif

// to enable or disable the debug messages for high-level I2C transfers, put 1 or 0 below
#if 0
#define I2CDBG ENABLE
#else
#define I2CDBG DISABLE
#endif

// To enable the displaying of all occurring interrups in the SiI9030, define the following symbol:
//#define DEBUG_HDMI_INTR 1
#undef DEBUG_HDMI_INTR

// Define this to test SRMs signed with the "facsimile" key, for debugging
//#define HDCP_USE_FACSIMILE_KEY

// Define this to clear all accessible bits in the audio channel status (possible workaound for audio dropout problem)
//#define HDMI_CLEAR_CHSTAT

#include "sample_os.h"
#define ALLOW_OS_CODE 1

#include "../rua/include/rua.h"
#include "../rua/include/rua_property.h"
#include "../dcc/include/dcc.h"
#include "../rmcore/include/rmstatustostring.h"
#include "../rmlibcw/include/rmlibcw.h"

#include "dvi_hdmi.h"

// TODO implemet own SHA1 function, this one came from the PuTTY SSH client
#include "dss_sha.h"

extern RMascii *TVFormatString[];

#define HOTPLUG_GUARD_TIME 2  // Guard delay for reporting changes after HotPlug low-to-high transition, in seconds
#define MAX_HDCP_ATTEMPTS 2 // Allow 2 chances as defined by spec.
#define MAX_I2C_ATTEMPTS 5
#define MAX_EDID_ATTEMPTS 2
#define HDCP_GUARD_TIME 800  // minimum time, in mSec, between TMDS on and HDCP auth, or between HDCP checks.

#define DDC_HDCP_DEV            0x74  // I2C device address on the DDC bus for HDCP
#define DDC_HDCP_SEC            0x76  // I2C device address for secondary link HDCP
#define DDC_EDID_DEV            0xA0  // I2C device address on the DDC bus for EDID
#define DDC_EDID_SEG            0x60  // I2C device address on the DDC bus for the EDID segment pointer
#define HDCP_CTRL_SII_ADDR          0x0F
#define HDCP_CTRL_ANX_ADDR          0xA1
#define DDC_MASTER_CMD_SII_ABORT        0x0F
#define DDC_MASTER_CMD_SII_CLEAR_FIFO   0x09
#define DDC_MASTER_CMD_SII_SEQ_RD       0x02
#define DDC_MASTER_CMD_SII_ENHANCED_RD  0x04
#define DDC_MASTER_CMD_SII_SEQ_WR       0x06
#define DDC_MASTER_CMD_ANX_ABORT        0x00
#define DDC_MASTER_CMD_ANX_CLEAR_FIFO   0x05
#define DDC_MASTER_CMD_ANX_SEQ_RD       0x01
#define DDC_MASTER_CMD_ANX_ENHANCED_RD  0x04
#define DDC_MASTER_CMD_ANX_SEQ_WR       0x02
#define DDC_MASTER_CMD_ANX_RESET        0x06
#define MDDC_FIFO_ADDR          0xF4
#define MAX_DDC_XFER 1023 // DDC size transfers
#define MAX_DDC_FIFO 16 // DDC FIFO size for transfers
#define DDC_TIMEOUT  100000 // 0.1 sec, in uSec
#define REPEATER_TIMEOUT  5000000 // 5 sec, in uSec
#define RI_128_THRESHOLD 127 // 0..127, phase in 128 frame HDCP cycle where Ri's will be compared

#define INFO_FRAME_AVI_ENABLE           0x0003
#define INFO_FRAME_SPD_ENABLE           0x000C
#define INFO_FRAME_AUD_ENABLE           0x0030
#define INFO_FRAME_MPG_ENABLE           0x00C0
#define INFO_FRAME_GEN_ENABLE           0x0300
#define INFO_FRAME_CP_ENABLE            0x0C00
#define INFO_FRAME_CP_ONCE              0x0800
#define INFO_FRAME_GEN2_ENABLE          0x3000

#define VENDOR_ID_SII 0x0001
#define VENDOR_ID_ANX 0xAAAA
// 2007/03/22 added by jj_tseng@chipadvanced.com
#define VENDOR_ID_CAT 0xCA00
//~jj_tseng@chipadvanced.com 2007/03/22

#define EDID_BLOCKS 8  // how many EDID blocks to hold in pDH
#define EDID_SIZE 128

#define SRM_SIZE 5 * 1024

// Assure pDH parameter is initialized
#define CHECK_pDH(func) do { \
	if (0) fprintf(stderr, "[HDMI] Call to " func "()\n"); \
	if (pDH == NULL) { \
		if (! manutest) fprintf(stderr, "[HDMI] Call to " func " with invalid DH structure!\n"); \
		return RM_FATALINVALIDPOINTER; \
	} \
} while (0)

// Check a certain pointer parameter, return error if NULL
#define CHECK_PTR(func, ptr) do { \
	if ((ptr) == NULL) { \
		if (! manutest) fprintf(stderr, "[HDMI] Call to " func " with invalid Data pointer!\n"); \
		return RM_FATALINVALIDPOINTER; \
	} \
} while (0)

struct DH_I2C {
	RMuint32 I2C;
	struct EMhwlibI2CDeviceParameter dev;
};

enum DH_i2cdbgstate {
	DH_i2cdbgstate_idle = 0, 
	DH_i2cdbgstate_dispatch, 
	DH_i2cdbgstate_read_addr_hi, 
	DH_i2cdbgstate_read_addr_lo, 
	DH_i2cdbgstate_blockread_addr_hi, 
	DH_i2cdbgstate_blockread_addr_lo, 
	DH_i2cdbgstate_blockread_size_hi, 
	DH_i2cdbgstate_blockread_size_lo, 
	DH_i2cdbgstate_ddcread_addr_hi, 
	DH_i2cdbgstate_ddcread_addr_lo, 
	DH_i2cdbgstate_ddcread_size_hi, 
	DH_i2cdbgstate_ddcread_size_lo, 
	DH_i2cdbgstate_edidread_block_hi, 
	DH_i2cdbgstate_edidread_block_lo, 
	DH_i2cdbgstate_edidread_size_hi, 
	DH_i2cdbgstate_edidread_size_lo, 
	DH_i2cdbgstate_write_addr_hi, 
	DH_i2cdbgstate_write_addr_lo, 
	DH_i2cdbgstate_write_data_hi, 
	DH_i2cdbgstate_write_data_lo, 
	DH_i2cdbgstate_device_hi, 
	DH_i2cdbgstate_device_lo, 
	DH_i2cdbgstate_module, 
};

// Capabilities of a chip/part
struct DH_part_caps {
	RMuint32 HDMI;       // part supports HDMI (audio etc.) (set to HDMI version * 10)
	RMuint32 HDCP;       // part supports HDCP (set to HDCP version * 10)
	RMuint32 DeepColor;  // part supports this many bits per video component (8, 10, 12, 14, 16)
	RMbool DDC;          // part provides access to the DDC I2C bus
	RMbool GPIOReset;    // part requires hard reset via a GPIO pin
	RMuint32 I2S2;       // maximum sample frequency for 2 channel I2S audio
	RMuint32 I2S8;       // maximum sample frequency for 8 channel I2S audio
	RMuint32 SPDIF;      // maximum sample frequency for S/P-DIF audio
};

struct DH_control {
	struct RUA *pRUA;
	enum DH_vendor_parts part;
	struct DH_part_caps part_caps;
	RMuint32 i2c_module;
	struct DH_I2C i2c_tx;   // Transmitter (DVI/HDMI chipset)
	struct DH_I2C i2c_tx2;  // Transmitter 2nd address
	struct DH_I2C i2c_rx;   // Receiver (DDC bus to the monitor, HDCP device)
	enum DH_device_state state;            // State of the DVI/HDMI connector
	enum DH_connection cable;              // Are we connected? (Used for Hot Plug Detect)
	RMuint8 gpio_reset;
	RMuint32 info_frame_enable;             // See INFO_FRAME_xxxx flags
	RMuint32 VideoPixelClock;
	RMuint32 AudioSampleClock;
	struct dss_key SRM_dss;
	RMuint8 SRM[SRM_SIZE];
	RMuint32 SRM_Size;
	RMbool CPDesired;
	RMbool HDMI_mode;
	RMbool CEA_Support;       // EDID says, sink is HDMI, not DVI
	RMbool CEA_QuantSupport;  // EDID says, Q entry in AVI infor frame is supported
	RMbool HDMI_audio;
	enum DCCRoute route;
	enum DH_EDID_select EDID_select;
	RMuint32 EDID_selection;  // force mode to this entry from EDID short descriptor list, when EDID_select == DH_EDID_force
	RMuint32 EDID_vfreq;  // preferred vsync frequency (50, 59, 60 etc.)
	RMuint32 EDID_hsize;  // preferred horizontal active size
	RMuint32 EDID_vsize;  // preferred vertical active size
	RMbool EDID_intl;     // preferred interlaced (TRUE) / progressive (FALSE) mode
	RMuint64 EDID_force_mask;  // try these VICs first (bit 1 = VIC 1, bit 2 = VIC 2 etc.)
	RMuint64 EDID_exclude_mask; // never use these VICs
	RMuint32 EDID_max_pixclk;
	RMuint32 EDID_min_pixclk;
	RMuint32 EDID_max_hfreq;
	RMuint32 EDID_min_hfreq;
	RMuint32 EDID_max_vfreq;
	RMuint32 EDID_min_vfreq;
	RMbool RxPresent;     // True if a Rx signal has ever been detected on this connector
	RMbool RxLost;        // True if Rx signal went down while hot plug active
	RMuint64 RepeaterTimeout;
	RMuint32 AudioHeader;
	RMuint64 HotPlugLastCheck;
	RMuint64 IntegrityLastCheck;
	RMbool Mute;
	RMuint32 TMDS_Threshold;
	enum GPIOId_type TMDS_GPIO;
	struct DH_HDMI_state HDMIState;
	RMbool HotPlugChanged;      // hardware state: HotPlug state has changed
	RMbool HotPlugState;        // hardware state: current HotPlug state
	RMbool HotPlugGuarded;
	RMuint64 HotPlugGuardTime;
	RMuint8 EDID[EDID_BLOCKS][EDID_SIZE];
	RMuint32 EDID_blocks;
	RMuint8 *pEDID_override;
	RMuint32 EDID_override_blocks;
	RMbool CheckClock;  // TRUE if input clock stability is polled by DHCheckHDMI()
	RMbool ForceCTS;
	RMuint8 audio_mode;
	RMbool GuardTMDS;  // if TRUE, prevent TMDS from being turned on until PStable
	RMbool RequestTMDS;  // if TRUE, app would like to enable TMDS after GuardTMDS = FALSE
	RMbool IgnoreStable;  // ignore p_stable between soft reset and TMDS enable
	RMbool ForceIntegrityCheck;  // if set, perform HDCP integrity chak as soon as possible
	enum EMhwlibColorSpace InputColorSpace;
	enum EMhwlibSamplingMode InputSamplingMode;
	RMuint32 InputComponentBitDepth;
	enum EMhwlibColorSpace OutputColorSpace;
	enum EMhwlibSamplingMode OutputSamplingMode;
	RMuint32 OutputComponentBitDepth;
	enum DH_i2cdbgstate i2cdbgstate;
	struct DH_I2C i2cdbg;
	RMuint8 i2cdbgaddr;
	RMuint8 i2cdbgsize;
	RMuint8 i2cdbgdata;
	RMuint32 i2cdbgdatasize;
	
	// ANX9030 state machine
	RMbool ANX_hdcp_auth_en;
	RMbool ANX_bksv_ready;
	RMbool ANX_hdcp_auth_pass;
	RMuint32 ANX_hdcp_auth_fail_counter;
	RMbool ANX_hdcp_encryption;
	RMbool ANX_send_blue_screen;
	RMbool ANX_hdcp_init_done;
	RMbool ANX_hdcp_wait_100ms_needed;
	RMbool ANX_auth_fully_pass;
	RMbool ANX_srm_checked;
	RMbool ANX_ksv_srm_pass;

    // 2007/03/23 Added by jj_tseng@chipadvanced
	RMbool CAT_hdcpreg_ready ;
    //~jj_tseng@chipadvanced.com 2007/03/23

};

struct DH_part_info {
	RMuint8 i2c_tx_pri_addr;     // Primary I2C device address of the transmitter chip
	RMuint8 i2c_tx_sec_addr;     // Secondary I2C device address of the transmitter chip
	RMuint8 i2c_2nd_offs;        // Offset from the first to the second device address, if any
	RMuint32 i2c_tx_speed_kHz;    // Speed, in kHz, for the I2C bus
	RMuint32 i2c_tx_delay_us;     // Delay, in microseconds, for the I2C bus
	RMuint32 i2c_rx_speed_kHz;    // Speed, in kHz, for the DDC I2C bus (HDCP Bcaps bit 4: 0=100kHz, 1=400kHz to dev 0x74)
	RMuint32 i2c_rx_delay_us;     // Delay, in microseconds, for the DDC I2C bus
	RMuint8 id_sub_address;      // Vendor/Device ID subaddress used for check part
	RMuint16 vendor_ID;          // 16 bit Vendor ID that should be read
	RMuint16 device_ID;          // 16 bit Device ID that should be read
	RMuint8 device_rev;          // specific revision of the device, or 0 for all
	RMascii *part_name;          // The name of the part
	struct DH_part_caps part_caps;
};

// I2C device to probe for vendor ID
// shall contain all i2c_tx_pri_addr and tx_sec_addr of part_info[], combined with their id_sub_address
static const RMuint8 i2c_probe[][2] = {
	{0x70, 0x00}, 
	{0x72, 0x00}, 
	{0x76, 0x00}, 
// 2007/03/28 added by jjtseng
	{0x98, 0x00}, 
	{0x9A, 0x00}, 
//~jjtseng 2007/03/28	
};
// definitions and capabilities for each chip
static const struct DH_part_info part_info[] = {
	{0x70, 0x72, 0, 100, 0, 100, 0, 0x00, VENDOR_ID_SII, 0x0006, 0x00, "SiI164",  { 0,  0,  8, FALSE, TRUE, 0, 0, 0}},  // SiI164 (DVI, no HDCP support)
	{0x70, 0x72, 0, 100, 0, 100, 0, 0x00, VENDOR_ID_SII, 0x0008, 0x00, "SiI170",  { 0, 13,  8, FALSE, TRUE, 0, 0, 0}},  // SiI170 (DVI, with HDCP support)
	{0x72, 0x76, 8, 100, 0, 100, 0, 0x00, VENDOR_ID_SII, 0x9253, 0x00, "SiI9030", {12, 13,  8, TRUE, FALSE, 192, 96, 96}},  // SiI9030 (HDMI, with HDCP support) - Receiver accessed through transmitter (rx i2c address is 0x00)
// 2007/03/22 Added by jj_tseng@chipadvanced
	{0x98, 0x98, 2, 100, 0, 100, 0, 0x00, VENDOR_ID_CAT, 0x1611, 0x00, "CAT6611", {12, 13,  8, TRUE, FALSE, 192, 96, 96}},  // CAT6611 (HDMI, with HDCP support) - Receiver accessed through transmitter (rx i2c address is 0x00)
//~jj_tseng@chipadvanced.com 2007/03/22
	{0x72, 0x76, 8, 100, 0, 100, 0, 0x00, VENDOR_ID_ANX, 0x9030, 0x00, "ANX9030", {12, 13,  8, TRUE, FALSE, 192, 96, 96}},  // ANX9030 (HDMI, with HDCP support) - Receiver accessed through transmitter (rx i2c address is 0x00)
	{0x72, 0x76, 8, 100, 0, 100, 0, 0x00, VENDOR_ID_SII, 0x9034, 0x00, "SiI9034", {13, 13,  8, TRUE, FALSE, 192, 96, 96}},  // SiI9034 (HDMI, with HDCP support) - Receiver accessed through transmitter (rx i2c address is 0x00)
	{0x72, 0x76, 8, 100, 0, 100, 0, 0x00, VENDOR_ID_SII, 0x9134, 0x00, "SiI9134", {13, 13, 12, TRUE, FALSE, 192, 192, 192}},  // SiI9134 (HDMI, with HDCP support) - Receiver accessed through transmitter (rx i2c address is 0x00)
};

#ifndef HDCP_USE_FACSIMILE_KEY
// The Digital Content Protection LLC public key
// This is the public key for production!
static const RMuint8 SRM_y[128] = {
	0xc7, 0x06, 0x00, 0x52, 0x6b, 0xa0, 0xb0, 0x86, 0x3a, 0x80, 0xfb, 0xe0, 0xa3, 0xac, 0xff, 0x0d, 
	0x4f, 0x0d, 0x76, 0x65, 0x8a, 0x17, 0x54, 0xa8, 0xe7, 0x65, 0x47, 0x55, 0xf1, 0x5b, 0xa7, 0x8d, 
	0x56, 0x95, 0x0e, 0x48, 0x65, 0x4f, 0x0b, 0xbd, 0xe1, 0x68, 0x04, 0xde, 0x1b, 0x54, 0x18, 0x74, 
	0xdb, 0x22, 0xe1, 0x4f, 0x03, 0x17, 0x04, 0xdb, 0x8d, 0x5c, 0xb2, 0xa4, 0x17, 0xc4, 0x56, 0x6c, 
	0x27, 0xba, 0x97, 0x3c, 0x43, 0xd8, 0x4e, 0x0d, 0xa2, 0xa7, 0x08, 0x56, 0xfe, 0x9e, 0xa4, 0x8d, 
	0x87, 0x25, 0x90, 0x38, 0xb1, 0x65, 0x53, 0xe6, 0x62, 0x43, 0x5f, 0xf7, 0xfd, 0x52, 0x06, 0xe2, 
	0x7b, 0xb7, 0xff, 0xbd, 0x88, 0x6c, 0x24, 0x10, 0x95, 0xc8, 0xdc, 0x8d, 0x66, 0xf6, 0x62, 0xcb, 
	0xd8, 0x8f, 0x9d, 0xf7, 0xe9, 0xb3, 0xfb, 0x83, 0x62, 0xa9, 0xf7, 0xfa, 0x36, 0xe5, 0x37, 0x99};
#else
// The "facsimile" public key (used to verify the attached facsimile SRMs)
// This public key MUST NOT be used in final commercial products!
static const RMuint8 SRM_y[128] = {
	0x73, 0xeb, 0x34, 0xbb, 0x29, 0x08, 0xd1, 0x18, 0x1b, 0xe2, 0x27, 0xdb, 0x64, 0x7f, 0x4f, 0xd3, 
	0xa0, 0x9d, 0xe5, 0x91, 0xb3, 0xc6, 0x67, 0xac, 0x99, 0x45, 0x0d, 0xf1, 0x30, 0x6c, 0xdd, 0x3b, 
	0xa5, 0x2a, 0xd7, 0x73, 0xca, 0xe7, 0xad, 0xc9, 0x65, 0xb4, 0xc0, 0x40, 0xed, 0x81, 0xda, 0x73, 
	0xad, 0x2f, 0x9f, 0xed, 0xea, 0x91, 0xe7, 0xb0, 0xe9, 0x1a, 0x3d, 0x30, 0x07, 0xa3, 0x4a, 0xd1, 
	0x61, 0x69, 0x7c, 0xe7, 0xbb, 0xee, 0x1c, 0x4e, 0xb2, 0x33, 0x95, 0x4f, 0xb5, 0x23, 0x68, 0xd0, 
	0x56, 0x3b, 0x05, 0xef, 0xb1, 0xf7, 0xca, 0x0c, 0x33, 0x75, 0x7b, 0x12, 0x88, 0x17, 0xd4, 0x94, 
	0x75, 0x95, 0x28, 0xce, 0x89, 0x61, 0xae, 0xb9, 0xb9, 0x5b, 0x18, 0x41, 0xa2, 0x33, 0x93, 0xa1, 
	0x6b, 0xd0, 0x72, 0xbc, 0x3e, 0x70, 0xa9, 0xdf, 0x83, 0x06, 0x8a, 0x13, 0x8a, 0xf8, 0xbb, 0x57};
#endif

// DSS Public Key - Prime Modulus
static const RMuint8 SRM_p[128] = {
	0xd3, 0xc3, 0xf5, 0xb2, 0xfd, 0x17, 0x61, 0xb7, 0x01, 0x8d, 0x75, 0xf7, 0x93, 0x43, 0x78, 0x6b, 
	0x17, 0x39, 0x5b, 0x35, 0x5a, 0x52, 0xc7, 0xb8, 0xa1, 0xa2, 0x4f, 0xc3, 0x6a, 0x70, 0x58, 0xff, 
	0x8e, 0x7f, 0xa1, 0x64, 0xf5, 0x00, 0xe0, 0xdc, 0xa0, 0xd2, 0x84, 0x82, 0x1d, 0x96, 0x9e, 0x4b, 
	0x4f, 0x34, 0xdc, 0x0c, 0xae, 0x7c, 0x76, 0x67, 0xb8, 0x44, 0xc7, 0x47, 0xd4, 0xc6, 0xb9, 0x83, 
	0xe5, 0x2b, 0xa7, 0x0e, 0x54, 0x47, 0xcf, 0x35, 0xf4, 0x04, 0xa0, 0xbc, 0xd1, 0x97, 0x4c, 0x3a, 
	0x10, 0x71, 0x55, 0x09, 0xb3, 0x72, 0x15, 0x30, 0xa7, 0x3f, 0x32, 0x07, 0xb9, 0x98, 0x20, 0x49, 
	0x5c, 0x7b, 0x9c, 0x14, 0x32, 0x75, 0x73, 0x3b, 0x02, 0x8a, 0x49, 0xfd, 0x96, 0x89, 0x19, 0x54, 
	0x2a, 0x39, 0x95, 0x1c, 0x46, 0xed, 0xc2, 0x11, 0x8c, 0x59, 0x80, 0x2b, 0xf3, 0x28, 0x75, 0x27};
// DSS Public Key - Prime Divisor
static const RMuint8 SRM_q[20] = {
	0xee, 0x8a, 0xf2, 0xce, 0x5e, 0x6d, 0xb5, 0x6a, 0xcd, 0x6d, 
	0x14, 0xe2, 0x97, 0xef, 0x3f, 0x4d, 0xf9, 0xc7, 0x08, 0xe7};
// DSS Public Key - Generator
static const RMuint8 SRM_g[128] = {
	0x92, 0xf8, 0x5d, 0x1b, 0x6a, 0x4d, 0x52, 0x13, 0x1a, 0xe4, 0x3e, 0x24, 0x45, 0xde, 0x1a, 0xb5, 
	0x02, 0xaf, 0xde, 0xac, 0xa9, 0xbe, 0xd7, 0x31, 0x5d, 0x56, 0xd7, 0x66, 0xcd, 0x27, 0x86, 0x11, 
	0x8f, 0x5d, 0xb1, 0x4a, 0xbd, 0xec, 0xa9, 0xd2, 0x51, 0x62, 0x97, 0x7d, 0xa8, 0x3e, 0xff, 0xa8, 
	0x8e, 0xed, 0xc6, 0xbf, 0xeb, 0x37, 0xe1, 0xa9, 0x0e, 0x29, 0xcd, 0x0c, 0xa0, 0x3d, 0x79, 0x9e, 
	0x92, 0xdd, 0x29, 0x45, 0xf7, 0x78, 0x58, 0x5f, 0xf7, 0xc8, 0x35, 0x64, 0x2c, 0x21, 0xba, 0x7f, 
	0xb1, 0xa0, 0xb6, 0xbe, 0x81, 0xc8, 0xa5, 0xe3, 0xc8, 0xab, 0x69, 0xb2, 0x1d, 0xa5, 0x42, 0x42, 
	0xc9, 0x8e, 0x9b, 0x8a, 0xab, 0x4a, 0x9d, 0xc2, 0x51, 0xfa, 0x7d, 0xac, 0x29, 0x21, 0x6f, 0xe8, 
	0xb9, 0x3f, 0x18, 0x5b, 0x2f, 0x67, 0x40, 0x5b, 0x69, 0x46, 0x24, 0x42, 0xc2, 0xba, 0x0b, 0xd9};

// TODO this needs to be a board dependent setting!
#if (EM86XX_MODE == EM86XX_MODEID_WITHHOST)
#define TRANSMITTER_GPIO_CLOCK GPIOId_Sys_2 // All odyssey boards use GPIO 0 and 1 for i2c, 2 and 3 for DDC line
#define TRANSMITTER_GPIO_DATA GPIOId_Sys_3
#else
#define TRANSMITTER_GPIO_CLOCK GPIOId_Sys_0 // All DVD player reference boards use same i2c lines for DDC and I2C
#define TRANSMITTER_GPIO_DATA GPIOId_Sys_1
#endif

static const RMuint8 init_SiI164[][2] = {
	{0x08, 0x36}, // use sync inputs, 24 bit bus, latch on falling edge, power down TMDS (register 0x08 needs to be first in this array)
	{0x09, 0x09}, // disable MonitorSENse output, generate int from hpd, clear MDI int
	{0x0A, 0xf0}, // enable de-skewing with +780 pSec
	{0x0C, 0x89}, // enable PLL filter
};

static const RMuint8 init_SiI170[][2] = {
	{0x08, 0x36}, // use sync inputs, 24 bit bus, latch on falling edge, power down TMDS (register 0x08 needs to be first in this array)
	{0x09, 0x09}, // disable MonitorSENse output, generate int from hpd, clear MDI int
	{0x0A, 0xf0}, // enable de-skewing with +780 pSec
	{0x0C, 0x89}, // enable PLL filter
	{HDCP_CTRL_SII_ADDR, 0x04}, // disable encryption
	{0x33, 0x30},  // use external DE signal, negative vsync and hsync polarity
};

static const RMuint8 init_SiI9030[][2] = {
	{0x08, 0x36}, // use sync inputs, 24 bit bus, latch on falling edge, power down TMDS (register 0x08 needs to be first in this array)
	{0x0C, 0x89}, // enable PLL filter
	{HDCP_CTRL_SII_ADDR, 0x04}, // disable encryption
	{0x33, 0x30},  // use external DE signal, negative vsync and hsync polarity
};

static const RMuint8 init_ANX9030[][2] = {
	{0x09, 0x03}, //Power on I2C
	{0x08, 0x00}, //72:08, set in normal op
	{0x05, 0x00}, //72:05, clear s/w reset
	{0x07, 0x40}, //power down TMDS, disable encryption
	{0x90, 0xFF}, //Power on Audio capture and Video capture module clock
	{0x45, 0x0D}, //Initial Interrupt mask
	{0x46, 0xFF}, 
	{0x47, 0x1A}, 
	{0x9A, 0x00}, //Enable auto set clock range for video PLL
	{0xCC, 0x10}, //Set registers value of Blue Screen when HDCP authentication failed--RGB mode,green field
	{0xCD, 0xEB}, 
	{0xCE, 0x10}, 
	{0x96, 0x11}, //match ATC
	{0x97, 0x11}, //match ATC
	{0x98, 0x11}, //match ATC
	{0x99, 0x11}, //TMDS amplitude and pre-emphasis
	{0x13, 0x10}, // use external DE signal, 24 bit bus, latch on falling edge
	{0x14, 0x00}, // use sync inputs, negative vsync and hsync polarity
};

// 2007/03/22 Added by jj_tseng@chipadvanced
static const RMuint8 init_CAT6611[][2] = {
    {0x04, 0x1D}, // only enable RXCLK.
    {0x61, 0x30}, // power down AFE
    {0x09, 0xFF},
    {0x0A, 0xFF},
    {0x0B, 0xFF},
    {0x61, 0x30}, // B_AFE_DRV_PWD, B_AFE_DRV_ENBIST
    {0x70, 0x00},
    {0x72, 0x00},
    {0xC0, 0x00},
    {0xC1, 0x01}, // set AVMute
    {0xC5, 0x00},
    {0xC6, 0x00},
    {0xC9, 0x00},
    {0xCA, 0x00},
    {0xCD, 0x00},
    {0xCE, 0x00},
    {0xCF, 0x00},
    {0xD0, 0x00},
    {0x04, 0x05}
} ;
//~jj_tseng@chipadvanced.com 2007/03/22

extern RMbool manutest;

struct DH_I2C *pi2c_prev_dev = NULL;
RMuint32 i2c_usage = 0;  // I2C mutex, two subsequent property calls are sometimes needed for one I2C access

static RMstatus DHReset_SiI164(struct DH_control *pDH);
static RMstatus DHEnableOutput_SiI(struct DH_control *pDH);
static RMstatus DHDisableOutput_SiI(struct DH_control *pDH);
static RMstatus DHEnableEncryption_SiI(struct DH_control *pDH);
static RMstatus DHEnableEncryption(struct DH_control *pDH);
static RMstatus DHDisableEncryption_SiI(struct DH_control *pDH);
static RMstatus DHDisableEncryption(struct DH_control *pDH);
static RMbool DHCheckSignature(struct dss_key *dss, RMuint8 *sig, RMuint32 siglen, RMuint8 *data, RMuint32 datalen);

// Location in the ANX9030 I2C address space
enum DH_InfoFrameOffsetANX {
	DH_InfoFrameOffsetANX_AVI = 0x00,  // max data len: 16
	DH_InfoFrameOffsetANX_SPD = 0x40, 
	DH_InfoFrameOffsetANX_Audio = 0x20,  // max data len: 11
	DH_InfoFrameOffsetANX_MPEG = 0x60, 
	DH_InfoFrameOffsetANX_Generic = 0x80, 
	DH_InfoFrameOffsetANX_Generic2 = 0xA0, 
};

// 2007/03/22 Added by jj_tseng@chipadvanced
#define REG_SW_RST       0x04
    #define B_ENTEST    (1<<7)
    #define B_REF_RST (1<<5)
    #define B_AREF_RST (1<<4)
    #define B_VID_RST (1<<3)
    #define B_AUD_RST (1<<2)
    #define B_HDMI_RST (1<<1)
    #define B_HDCP_RST (1<<0)

#define REG_INT_CTRL 0x05
    #define B_INTPOL_ACTL 0
    #define B_INTPOL_ACTH (1<<7)
    #define B_INT_PUSHPULL 0
    #define B_INT_OPENDRAIN (1<<6)

#define REG_INT_STAT1    0x06
    #define B_INT_AUD_OVERFLOW  (1<<7)
    #define B_INT_ROMACQ_NOACK  (1<<6)
    #define B_INT_RDDC_NOACK    (1<<5)
    #define B_INT_DDCFIFO_ERR   (1<<4)
    #define B_INT_ROMACQ_BUS_HANG   (1<<3)
    #define B_INT_DDC_BUS_HANG  (1<<2)
    #define B_INT_RX_SENSE  (1<<1)
    #define B_INT_HPD_PLUG  (1<<0)

#define REG_INT_STAT2    0x07
    #define B_INT_PKTISRC2  (1<<7)
    #define B_INT_PKTISRC1  (1<<6)
    #define B_INT_PKTACP    (1<<5)
    #define B_INT_PKTNULL  (1<<4)
    #define B_INT_PKTGENERAL   (1<<3)
    #define B_INT_KSVLIST_CHK   (1<<2)
    #define B_INT_AUTH_DONE (1<<1)
    #define B_INT_AUTH_FAIL (1<<0)

#define REG_INT_STAT3    0x08
    #define B_INT_AUD_CTS   (1<<6)
    #define B_INT_VSYNC     (1<<5)
    #define B_INT_VIDSTABLE (1<<4)
    #define B_INT_PKTMPG    (1<<3)
    #define B_INT_PKTSPD    (1<<2)
    #define B_INT_PKTAUD    (1<<1)
    #define B_INT_PKTAVI    (1<<0)

#define REG_INT_MASK1    0x09
    #define B_AUDIO_OVFLW_MASK (1<<7)
    #define B_DDC_NOACK_MASK (1<<5)
    #define B_DDC_FIFO_ERR_MASK (1<<4)
    #define B_DDC_BUS_HANG_MASK (1<<2)
    #define B_RXSEN_MASK (1<<1)
    #define B_HPD_MASK (1<<0)

#define REG_INT_MASK2    0x0A
    #define B_PKT_AVI_MASK (1<<7)
    #define B_PKT_ISRC_MASK (1<<6)
    #define B_PKT_ACP_MASK (1<<5)
    #define B_PKT_NULL_MASK (1<<4)
    #define B_PKT_GEN_MASK (1<<3)
    #define B_KSVLISTCHK_MASK (1<<2)
    #define B_AUTH_DONE_MASK (1<<1)
    #define B_AUTH_FAIL_MASK (1<<0)

#define REG_INT_MASK3    0x0B
    #define B_AUDCTS_MASK (1<<5)
    #define B_VSYNC_MASK (1<<4)
    #define B_VIDSTABLE_MASK (1<<3)
    #define B_PKT_MPG_MASK (1<<2)
    #define B_PKT_SPD_MASK (1<<1)
    #define B_PKT_AUD_MASK (1<<0)
    
#define REG_INT_CLR0      0x0C
    #define B_CLR_PKTACP    (1<<7)
    #define B_CLR_PKTNULL   (1<<6)
    #define B_CLR_PKTGENERAL    (1<<5)
    #define B_CLR_KSVLISTCHK    (1<<4)
    #define B_CLR_AUTH_DONE  (1<<3)
    #define B_CLR_AUTH_FAIL  (1<<2)
    #define B_CLR_RXSENSE   (1<<1)
    #define B_CLR_HPD       (1<<0)

#define REG_INT_CLR1       0x0D
    #define B_CLR_VSYNC (1<<7)
    #define B_CLR_VIDSTABLE (1<<6)
    #define B_CLR_PKTMPG    (1<<5)
    #define B_CLR_PKTSPD    (1<<4)
    #define B_CLR_PKTAUD    (1<<3)
    #define B_CLR_PKTAVI    (1<<2)
    #define B_CLR_PKTISRC2  (1<<1)
    #define B_CLR_PKTISRC1  (1<<0)

#define REG_SYS_STATUS     0x0E
    // readonly
    #define B_INT_ACTIVE    (1<<7)
    #define B_HPDETECT      (1<<6)
    #define B_RXSENDETECT   (1<<5)
    #define B_TXVIDSTABLE   (1<<4)
    // read/write
    #define O_CTSINTSTEP    2
    #define M_CTSINTSTEP    (3<<2)
    #define B_CLR_AUD_CTS     (1<<1)
    #define B_INTACTDONE    (1<<0)
    
// DDC

#define REG_DDC_MASTER_CTRL   0x10
    #define B_MASTERROM (1<<1)
    #define B_MASTERDDC (0<<1)
    #define B_MASTERHOST    (1<<0)
    #define B_MASTERHDCP    (0<<0)

#define REG_DDC_HEADER  0x11
#define REG_DDC_REQOFF  0x12
#define REG_DDC_REQCOUNT    0x13
#define REG_DDC_EDIDSEG 0x14
#define REG_DDC_CMD 0x15
    #define CMD_DDC_SEQ_BURSTREAD 0
    #define CMD_LINK_CHKREAD  2
    #define CMD_EDID_READ   3
    #define CMD_FIFO_CLR    9
    #define CMD_DDC_ABORT   0xF

#define REG_DDC_STATUS  0x16
    #define B_DDC_DONE  (1<<7)
    #define B_DDC_ACT   (1<<6)
    #define B_DDC_NOACK (1<<5)
    #define B_DDC_WAITBUS   (1<<4)
    #define B_DDC_ARBILOSE  (1<<3)
    #define B_DDC_FIFOFULL  (1<<2)
    #define B_DDC_FIFOEMPTY (1<<1)

#define REG_DDC_READFIFO    0x17

// HDCP
#define REG_AN_GENERATE 0x1F
    #define B_START_CIPHER_GEN  1
    #define B_STOP_CIPHER_GEN   0

#define REG_HDCP_DESIRE 0x20
    #define B_ENABLE_HDPC11 (1<<1)
    #define B_CPDESIRE  (1<<0)

#define REG_AUTHFIRE    0x21
#define REG_LISTCTRL    0x22
    #define B_LISTFAIL  (1<<1)
    #define B_LISTDONE  (1<<0)

#define REG_AKSV    0x23
#define REG_AKSV0   0x23
#define REG_AKSV1   0x24
#define REG_AKSV2   0x25
#define REG_AKSV3   0x26
#define REG_AKSV4   0x27

#define REG_AN  0x28
#define REG_AN_GEN  0x30
#define REG_ARI     0x38
#define REG_ARI0    0x38
#define REG_ARI1    0x39
#define REG_APJ     0x3A

#define REG_BKSV    0x3B
#define REG_BRI     0x40
#define REG_BRI0    0x40
#define REG_BRI1    0x41
#define REG_BPJ     0x42
#define REG_BCAP    0x43
    #define B_CAP_HDMI_REPEATER (1<<6)
    #define B_CAP_KSV_FIFO_RDY  (1<<5)
    #define B_CAP_HDMI_FAST_MODE    (1<<4)
    #define B_CAP_HDCP_1p1  (1<<1)
    #define B_CAP_FAST_REAUTH   (1<<0)
#define REG_BSTAT   0x44
#define REG_BSTAT0   0x44
#define REG_BSTAT1   0x45
    #define B_CAP_HDMI_MODE (1<<12)
    #define B_CAP_DVI_MODE (0<<12)
    #define B_MAX_CASCADE_EXCEEDED  (1<<11)
    #define M_REPEATER_DEPTH    (0x7<<8)
    #define O_REPEATER_DEPTH    8
    #define B_DOWNSTREAM_OVER   (1<<7)
    #define M_DOWNSTREAM_COUNT  0x7F

#define REG_AUTH_STAT 0x46
#define B_AUTH_DONE (1<<7)

// Input reg Format Register
#define REG_INPUT_MODE  0x70
    #define O_INCLKDLY	0
    #define M_INCLKDLY	3
    #define B_INDDR	    (1<<2)
    #define B_SYNCEMB	(1<<3)
    #define B_2X656CLK	(1<<4)
    #define M_INCOLMOD	(3<<6)
    #define B_IN_RGB    0
    #define B_IN_YUV422 (1<<6)
    #define B_IN_YUV444 (2<<6)

#define REG_TXFIFO_RST  0x71
    #define B_ENAVMUTERST	1
    #define B_TXFFRST	(1<<1)

#define REG_CSC_CTRL    0x72
    #define B_CSC_BYPASS    0
    #define B_CSC_RGB2YUV   2
    #define B_CSC_YUV2RGB   3
    #define M_CSC_SEL       3
    #define B_EN_DITHER      (1<<7)
    #define B_EN_UDFILTER      (1<<6)
    #define B_DNFREE_GO      (1<<5)
// HDMI Packet Control

#define REG_PKT_GENCTRL_CTRL 0xC6
#define REG_PKT_NULL_CTRL    0xC9
#define REG_PKT_ACP_CTRL     0xCA
#define REG_PKT_ISRC1_CTRL   0xCB
#define REG_PKT_ISRC2_CTRL   0xCC
#define REG_AVI_INFOFRM_CTRL 0xCD
#define REG_AUD_INFOFRM_CTRL 0xCE
#define REG_SPD_INFOFRM_CTRL 0xCF
#define REG_MPG_INFOFRM_CTRL 0xD0

#define DH_Switch_6611BANK(pDH, bank) \
{\
    DH_i2c_write((pDH)->pRUA, &((pDH)->i2c_tx), 0xF, (bank)&1);\
}

#define DH_Clear_CAT6611INT(pDH) \
{\
    RMuint8 reg0E_data ;\
    DH_i2c_read((pDH)->pRUA,&((pDH)->i2c_tx), 0xE, &reg0E_data ) ;\
    reg0E_data |= 1 ;\
    DH_i2c_write((pDH)->pRUA,&((pDH)->i2c_tx), 0xE, reg0E_data ) ;\
    reg0E_data &= ~1 ;\
    DH_i2c_write((pDH)->pRUA,&((pDH)->i2c_tx), 0xE, reg0E_data ) ;\
}

#define DH_Clear_CAT6611DDCFIFO(pDH) \
{\
    DH_i2c_write((pDH)->pRUA,&((pDH)->i2c_tx), 0x11, 0x01 ) ;\
    DH_i2c_write((pDH)->pRUA,&((pDH)->i2c_tx), 0x15, 0x09 ) ;\
}

#define DH_Set_CAT6611_AVMute(pDH) \
{\
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0F, 0);\
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC1, 1);\
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC6, 3);\
}

#define DH_Clear_CAT6611_AVMute(pDH) \
{\
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0F, 0);\
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC1, 0);\
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC6, 3);\
}

//~jj_tseng@chipadvanced.com 2007/03/22


// 2007/03/22 Added by jj_tseng@chipadvanced
static RMstatus DHGetInfoFrameEnable_CAT(struct DH_control *pDH, RMuint32 *info_frame_enable) ;
static RMstatus DHSetInfoFrameEnable_CAT(struct DH_control *pDH, RMuint32 mask) ;
static RMstatus DHClearInfoFrameEnable_CAT(struct DH_control *pDH, RMuint32 mask) ;
static RMstatus DHReset_CAT6611(struct DH_control *pDH) ;
static RMstatus DHInitChip_CAT6611(struct DH_control *pDH) ;
static RMstatus DHSetDDRMode_CAT(struct DH_control *pDH, RMbool ddr_mode) ;
static RMstatus DHEnableOutput_CAT(struct DH_control *pDH) ;
static RMstatus DHSetAFE_CAT6611(struct DH_control *pDH, RMbool bHighFreq);
static RMstatus DHFireAFE_CAT6611(struct DH_control *pDH);
static RMstatus DHSetIntMask_CAT6611(struct DH_control *pDH);
static RMstatus DHEnableHDMIOutput_CAT6611(struct DH_control *pDH, RMbool bHDMI) ;
static RMstatus DHAbortDDC_CAT6611(struct DH_control *pDH) ;
static RMstatus DHDDCBlockReadSegment_CAT(struct DH_control *pDH,RMuint8 i2cAddr,RMuint8 i2cSegmentPtr,RMuint32 RegAddr,RMuint8 *pData,RMuint32 NbBytes) ;
static RMstatus DHDDCBlockRead_CAT(struct DH_control *pDH,RMuint8 i2cAddr,RMuint8 RegAddr,RMuint8 *pData,RMuint8 NbBytes) ;
static RMstatus DHGetAKSV_CAT(struct DH_control *pDH, RMuint8 *pAKSV) ;
static RMstatus DHGetBKSV_CAT(struct DH_control *pDH, RMuint8 *pBKSV) ;
static RMstatus DHCollectRxHDCPStatus_CAT(struct DH_control *pDH) ;
static RMstatus DHAuthenticate_CAT(struct DH_control *pDH, RMuint8 *pBKSV, RMbool *pRepeater) ;
static RMstatus DHGetBCaps_CAT(struct DH_control *pDH, RMuint8 *pBCaps) ;
static RMstatus DHGetBStat_CAT(struct DH_control *pDH, RMuint8 *pBStat) ;
static RMstatus DHGetRepeaterKSVList_CAT(struct DH_control *pDH,RMuint8 *pKSVList, RMuint8 devcount) ;
static RMstatus DHGetVr_CAT(struct DH_control *pDH, RMuint8 *pVr) ;
static RMstatus DHResumeRepeaterAut_CAT(struct DH_control *pDH, RMbool bSuccess) ;
static RMstatus DHVerifyIntegrity_CAT(struct DH_control *pDH) ;
static RMstatus DHCheckHDMI_CAT6611(struct DH_control *pDH, struct DH_HDMI_state *pHDMIState) ;
static RMstatus DHSetAudioFormat_CAT(struct DH_control *pDH, struct DH_AudioFormat *pAudioFormat) ;
static RMstatus DHDump_CAT6611reg(struct DH_control *pDH) ;
//~jj_tseng@chipadvanced.com 2007/03/22


static RMstatus DH_update_i2c(struct RUA *pRUA, struct DH_I2C *pi2c_dev)
{
	RMstatus err;
	
	if (pi2c_dev != pi2c_prev_dev) {
		if (i2c_usage) return RM_PENDING;  // new configuratio while old one still in use, try again later
		i2c_usage = 1;  // set I2C usage counter for new configuration
		pi2c_prev_dev = pi2c_dev;
		RMDBGLOG((DISABLE, "Set new i2c config, ModuleID 0x%08lX, dev W=0x%02X, R=0x%02X, Clk=%ld, Data=%ld\n", 
			pi2c_dev->I2C, pi2c_dev->dev.DevAddr, pi2c_dev->dev.DevAddr | 0x01, 
			(RMuint32)(pi2c_dev->dev.Clock - GPIOId_Sys_0), 
			(RMuint32)(pi2c_dev->dev.Data - GPIOId_Sys_0)));
		err = RUASetProperty(pRUA, 
			pi2c_dev->I2C, 
			RMI2CPropertyID_DeviceParameter, 
			&(pi2c_dev->dev), sizeof(pi2c_dev->dev), 0);
		if (RMFAILED(err)) {
			RMDBGLOG((LOCALDBG, "Error setting RMI2CPropertyID_DeviceParameter! Error %s\n", RMstatusToString(err)));
			i2c_usage = 0;
			return err;
		}
	} else {
		i2c_usage++;  // increase I2C usage counter for current configuration
	}
	return RM_OK;
}

#define DH_I2C_VERIFY_WRITE 0

static RMstatus DH_i2c_read(struct RUA *pRUA, struct DH_I2C *pi2c_dev, RMuint8 sub_address, RMuint8 *value)
{
	struct I2C_QueryRMuint8_in_type i2c_param; 
	struct I2C_QueryRMuint8_out_type i2c_res;
	RMstatus err;
	RMuint32 r;
	
	if (value == NULL) {
		return RM_FATALINVALIDPOINTER;
	}
	
	r = MAX_I2C_ATTEMPTS;
	do {
		if (RMFAILED(err = DH_update_i2c(pRUA, pi2c_dev))) continue;
		i2c_param.SubAddr = sub_address;
		err = RUAExchangeProperty(pRUA, pi2c_dev->I2C, RMI2CPropertyID_QueryRMuint8, &i2c_param, sizeof(i2c_param), &i2c_res, sizeof(i2c_res));
		i2c_usage--;  // decrease usage counter for this I2C configuration
	} while (--r && RMFAILED(err));
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "Error setting RMI2CPropertyID_QueryRMuint8(0x%02X:0x%02X) on I2C 0x%08lX GPIO %lu/%lu! %s\n", 
			pi2c_dev->dev.DevAddr, i2c_param.SubAddr, 
			pi2c_dev->I2C, 
			(RMuint32)(pi2c_dev->dev.Clock - GPIOId_Sys_0), 
			(RMuint32)(pi2c_dev->dev.Data - GPIOId_Sys_0), 
			RMstatusToString(err)));
	}
	
	*value = i2c_res.Data;
	return err;
}

static RMstatus DH_i2c_write(struct RUA *pRUA, struct DH_I2C *pi2c_dev, RMuint8 sub_address, RMuint8 value)
{
	struct I2C_WriteRMuint8_type i2c_write;
	RMstatus err;
	RMuint32 r;
	
	RMDBGLOG((DISABLE, "==== I2C Wr 0x%02X:0x%02X 0x%02X ====\n", pi2c_dev->dev.DevAddr, sub_address, value));
	r = MAX_I2C_ATTEMPTS;
	do {
		if (RMFAILED(err = DH_update_i2c(pRUA, pi2c_dev))) continue;
		i2c_write.SubAddr = sub_address;
		i2c_write.Data = value;
		err = RUASetProperty(pRUA, pi2c_dev->I2C, RMI2CPropertyID_WriteRMuint8, &i2c_write, sizeof(i2c_write), 0);
		i2c_usage--;  // decrease usage counter for this I2C configuration
	} while (--r && RMFAILED(err));
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "Error setting RMI2CPropertyID_WriteRMuint8(0x%02X:0x%02X, 0x%02X) on I2C 0x%08lX GPIO %lu/%lu! %s\n", 
			pi2c_dev->dev.DevAddr, i2c_write.SubAddr, i2c_write.Data, 
			pi2c_dev->I2C, 
			(RMuint32)(pi2c_dev->dev.Clock - GPIOId_Sys_0), 
			(RMuint32)(pi2c_dev->dev.Data - GPIOId_Sys_0), 
			RMstatusToString(err)));
	}
#if DH_I2C_VERIFY_WRITE
	DH_i2c_read(pRUA, pi2c_dev, sub_address, &i2c_write.Data);
	if (i2c_write.Data != value) {
		RMDBGLOG((LOCALDBG, "Error verifying 0x%02lX:0x%02lX, is 0x%02X instead of 0x%02X!\n", 
			pi2c_dev->dev.DevAddr, i2c_write.SubAddr, i2c_write.Data, value));
	}
#endif  // DH_I2C_VERIFY_WRITE	
	return err;
}

static RMstatus DH_i2c_read_data(struct RUA *pRUA, struct DH_I2C *pi2c_dev, RMuint8 sub_address, RMuint8 *data, RMuint32 data_size)
{
	struct I2C_QueryData_in_type i2c_param; 
	struct I2C_QueryData_out_type i2c_res;
	RMstatus err;
	RMuint32 r;
	
	if (data == NULL) {
		return RM_FATALINVALIDPOINTER;
	}
	
	r = MAX_I2C_ATTEMPTS;
	do {
		if (RMFAILED(err = DH_update_i2c(pRUA, pi2c_dev))) continue;
		i2c_param.UseSubAddr = TRUE;
		i2c_param.SubAddr = sub_address;
		i2c_param.DataSize = data_size;
		err = RUAExchangeProperty(pRUA, pi2c_dev->I2C, RMI2CPropertyID_QueryData, &i2c_param, sizeof(i2c_param), &i2c_res, sizeof(i2c_res));
		i2c_usage--;  // decrease usage counter for this I2C configuration
	} while (--r && RMFAILED(err));
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "Error setting RMI2CPropertyID_QueryData(0x%02X:0x%02X, 0x%02X) on I2C 0x%08lX GPIO %lu/%lu! %s\n", 
			pi2c_dev->dev.DevAddr, i2c_param.SubAddr, i2c_param.DataSize, 
			pi2c_dev->I2C, 
			(RMuint32)(pi2c_dev->dev.Clock - GPIOId_Sys_0), 
			(RMuint32)(pi2c_dev->dev.Data - GPIOId_Sys_0), 
			RMstatusToString(err)));
	}
	
	RMMemcpy(data, i2c_res.Data, data_size);
	return err;
}

static RMstatus DH_i2c_read_data_segment(
	struct RUA *pRUA, 
	struct DH_I2C *pi2c_dev, 
	RMuint8 sub_address, 
	RMuint8 *data, 
	RMuint32 data_size, 
	RMuint8 segment_ptr, 
	RMuint8 segment)
{
	struct I2C_SelectSegment_type i2c_segment;
	RMstatus err;
	RMuint32 r;
	
	if (data == NULL) {
		return RM_FATALINVALIDPOINTER;
	}
	
	r = MAX_I2C_ATTEMPTS;
	do {
		if (RMFAILED(err = DH_update_i2c(pRUA, pi2c_dev))) continue;
		i2c_segment.SegmentPtr = segment_ptr;
		i2c_segment.Segment = segment;
		err = RUASetProperty(pRUA, pi2c_dev->I2C, RMI2CPropertyID_SelectSegment, &i2c_segment, sizeof(i2c_segment), 0);
		i2c_usage--;  // decrease usage counter for this I2C configuration
	} while (--r && RMFAILED(err));
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "%s: failed to set RMI2CPropertyID_SelectSegment(0x%02X:0x%02X, 0x%02X) on I2C 0x%08lX GPIO %lu/%lu! %s\n", 
			(segment > 0) ? "Error" : "Warning", 
			pi2c_dev->dev.DevAddr, i2c_segment.SegmentPtr, i2c_segment.Segment, 
			pi2c_dev->I2C, 
			(RMuint32)(pi2c_dev->dev.Clock - GPIOId_Sys_0), 
			(RMuint32)(pi2c_dev->dev.Data - GPIOId_Sys_0), 
			RMstatusToString(err)));
		if (segment > 0) return err;
	}
	
	return DH_i2c_read_data(pRUA, pi2c_dev, sub_address, data, data_size);
}

static RMstatus DH_i2c_write_data(struct RUA *pRUA, struct DH_I2C *pi2c_dev, RMuint8 sub_address, RMuint8 *data, RMuint32 data_size)
{
	struct I2C_WriteData_type i2c_write;
	RMstatus err;
	RMuint32 r;
	
	if (data == NULL) {
		return RM_FATALINVALIDPOINTER;
	}
	
	RMDBGLOG((DISABLE, "==== I2C Wr 0x%02X:0x%02X-0x%02X ====\n", pi2c_dev->dev.DevAddr, sub_address, sub_address + data_size - 1));
	r = MAX_I2C_ATTEMPTS;
	do {
		if (RMFAILED(err = DH_update_i2c(pRUA, pi2c_dev))) continue;
		i2c_write.UseSubAddr = TRUE;
		i2c_write.SubAddr = sub_address;
		i2c_write.DataSize = data_size;
		RMMemcpy(i2c_write.Data, data, data_size);
#ifdef RMFEATURE_HAS_FAULTY_I2C_BURST_WRITE
		{
			RMuint32 i;
			struct I2C_WriteRMuint8_type i2c_write;
			RMstatus e;
			for (i = 0; i < data_size; i++) {
				i2c_write.SubAddr = sub_address + i;
				i2c_write.Data = data[i];
				e = RUASetProperty(pRUA, pi2c_dev->I2C, RMI2CPropertyID_WriteRMuint8, &i2c_write, sizeof(i2c_write), 0);
				if (RMFAILED(e)) err = e;
			}
		}
#else
		err = RUASetProperty(pRUA, pi2c_dev->I2C, RMI2CPropertyID_WriteData, &i2c_write, sizeof(i2c_write), 0);
#endif
		i2c_usage--;  // decrease usage counter for this I2C configuration
	} while (--r && RMFAILED(err));
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "Error setting RMI2CPropertyID_WriteData(0x%02X:0x%02X, 0x%02X) on I2C 0x%08lX GPIO %lu/%lu! %s\n", 
			pi2c_dev->dev.DevAddr, i2c_write.SubAddr, i2c_write.DataSize, 
			pi2c_dev->I2C, 
			(RMuint32)(pi2c_dev->dev.Clock - GPIOId_Sys_0), 
			(RMuint32)(pi2c_dev->dev.Data - GPIOId_Sys_0), 
			RMstatusToString(err)));
	}
#if DH_I2C_VERIFY_WRITE
	{
		RMuint32 i;
		DH_i2c_read_data(pRUA, pi2c_dev, sub_address, i2c_write.Data, i2c_write.DataSize);
		for (i = 0; i < data_size; i++) {
			if (i2c_write.Data[i] != data[i]) {
				RMDBGLOG((LOCALDBG, "Error verifying 0x%02lX:0x%02lX, is 0x%02X instead of 0x%02X!\n", 
					pi2c_dev->dev.DevAddr, i2c_write.SubAddr + i, i2c_write.Data[i], data[i]));
			}
		}
	}
#endif  // DH_I2C_VERIFY_WRITE	
	return err;
}

static RMstatus DH_i2c_write_data_segment(
	struct RUA *pRUA, 
	struct DH_I2C *pi2c_dev, 
	RMuint8 sub_address, 
	RMuint8 *data, 
	RMuint32 data_size, 
	RMuint8 segment_ptr, 
	RMuint8 segment)
{
	struct I2C_SelectSegment_type i2c_segment;
	RMstatus err;
	RMuint32 r, index;
	
	if (data == NULL) {
		return RM_FATALINVALIDPOINTER;
	}
	
	r = MAX_I2C_ATTEMPTS;
	do {
		if (RMFAILED(err = DH_update_i2c(pRUA, pi2c_dev))) continue;
		i2c_segment.SegmentPtr = segment_ptr;
		i2c_segment.Segment = segment;
		err = RUASetProperty(pRUA, pi2c_dev->I2C, RMI2CPropertyID_SelectSegment, &i2c_segment, sizeof(i2c_segment), 0);
		i2c_usage--;  // decrease usage counter for this I2C configuration
	} while (--r && RMFAILED(err));
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "%s: failed to set RMI2CPropertyID_SelectSegment(0x%02X:0x%02X, 0x%02X) on I2C 0x%08lX GPIO %lu/%lu! %s\n", 
			(segment > 0) ? "Error" : "Warning", 
			pi2c_dev->dev.DevAddr, i2c_segment.SegmentPtr, i2c_segment.Segment, 
			pi2c_dev->I2C, 
			(RMuint32)(pi2c_dev->dev.Clock - GPIOId_Sys_0), 
			(RMuint32)(pi2c_dev->dev.Data - GPIOId_Sys_0), 
			RMstatusToString(err)));
		if (segment > 0) return err;
	}
	
	//return DH_i2c_write_data(pRUA, pi2c_dev, sub_address, data, data_size);
	for (index = 0; index < data_size; index++) {
		RMuint8 v;
		r = 10;
		do {
			err = DH_i2c_write(pRUA, pi2c_dev, sub_address + index, data[index]);
			if (RMFAILED(err)) {
				RMDBGLOG((LOCALDBG, "Error writing 0x%02X to %02X! %s\n", data[index], sub_address + index, RMstatusToString(err)));
			}
			RMMicroSecondSleep(100000);  // Delay for EEPROM
			err = DH_i2c_read(pRUA, pi2c_dev, sub_address + index, &v);
			if (RMSUCCEEDED(err) && (v == data[index])) break;
		} while (--r);
		if (RMFAILED(err) || (v != data[index])) {
			RMDBGLOG((LOCALDBG, "Error writing 0x%02X to %02X! Read back 0x%02X instead.\n", data[index], sub_address + index, v));
		}
	}
	return err;
}

static RMstatus DHSetTMDSResistor(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	struct SystemBlock_GPIO_type gpio;
	RMuint8 reg;
	if (pDH->TMDS_Threshold) {
		gpio.Bit = pDH->TMDS_GPIO;
		gpio.Data = (pDH->VideoPixelClock >= pDH->TMDS_Threshold) ? 1 : 0;
		if (RMFAILED(err = RUASetProperty(pDH->pRUA, SystemBlock, RMSystemBlockPropertyID_GPIO, &gpio, sizeof(gpio), 0))) {
			RMDBGLOG((LOCALDBG, "Error setting GPIO %u to set external swing resistor of HDMI TMDS: %s\n", pDH->TMDS_GPIO, RMstatusToString(err)));
			return err;
		}
	}
	
	switch (pDH->part) {
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x80, &reg);
		if (RMSUCCEEDED(err)) {
			if (pDH->VideoPixelClock >= 165000000) {
				reg |= 0x20;
			} else {
				reg &= ~0x20;
			}
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x80, reg);
		}
		break;
	default:
		break;
	}
	return err;
}

RMstatus DHSetTMDSMode(struct DH_control *pDH, 
	enum GPIOId_type GPIO, 
	RMuint32 PixelClockThreshold)
{
	CHECK_pDH("DHSetTMDSMode");
	pDH->TMDS_Threshold = PixelClockThreshold;
	pDH->TMDS_GPIO = GPIO;
	return RM_OK;
}

static RMstatus DHReset_SiI164(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	struct SystemBlock_GPIO_type gpio;
	
	// Set GPIO direction and Reset SiI170
	if (! manutest) fprintf(stderr, "[HDMI] Resetting DVI transmitter with GPIO pin %u.\n", pDH->gpio_reset);
	gpio.Bit = pDH->gpio_reset;
	gpio.Data = 0;
	if (RMFAILED(err = RUASetProperty(pDH->pRUA, SystemBlock, RMSystemBlockPropertyID_GPIO, &gpio, sizeof(gpio), 0))) {
		RMDBGLOG((LOCALDBG, "Error setting GPIO to reset SiI170!!! %s\n", RMstatusToString(err)));
		return err;
	}
	
	// Assert 50 uSec low time
	RMMicroSecondSleep(50);
	
	// Unreset SiI170
	gpio.Bit = pDH->gpio_reset;
	gpio.Data = 1;
	if (RMFAILED(err = RUASetProperty(pDH->pRUA, SystemBlock, RMSystemBlockPropertyID_GPIO, &gpio, sizeof(gpio), 0))) {
		RMDBGLOG((LOCALDBG, "Error setting GPIO to unreset SiI170!!! %s\n", RMstatusToString(err)));
		return err;
	}
	
	// Check if HDCP is now off
	if (pDH->part_caps.HDCP) {
		switch (pDH->part) {
		case DH_siI170: {
			RMuint8 reg;
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &reg);
			if (RMFAILED(err)) return err;
			RMDBGLOG((LOCALDBG, "Read status from transmitter: %02X\n", reg));
			if (reg & 0x01) {
				if (! manutest) fprintf(stderr, "[HDMI] FATAL! Reset of DVI transmitter failed, encryption still enabled! Set reset_GPIO in DHOpenChip()\n");
			}
		} break;
		default:
			RMDBGLOG((LOCALDBG, "Error, unknown HDCP part!\n"));
		}
	}
	
	RMMicroSecondSleep(10*1000);
	
	return err;
}

// The following functions should work with all Sigma Reference Designs

#ifdef RMFEATURE_HAS_HDMI
static RMstatus DHResetHDMICore(struct RUA *pRUA)
{
	RMstatus err = RM_OK;
	
	enum DisplayBlock_HDMIState_type hs;
	
	if (RMFAILED(err = RUAGetProperty(pRUA, 
		DisplayBlock, RMDisplayBlockPropertyID_HDMIState, 
		&hs, sizeof(hs)))) {
		RMDBGLOG((LOCALDBG, "Error getting internal HDMI state! %s\n", RMstatusToString(err)));
		return err;
	}
	if (hs == DisplayBlock_HDMIState_Running) {
		if (! manutest) fprintf(stderr, "[HDMI] Resetting internal HDMI core!\n");
		hs = DisplayBlock_HDMIState_ResetHDMI;
		if (RMFAILED(err = RUASetProperty(pRUA, 
			DisplayBlock, RMDisplayBlockPropertyID_HDMIState, 
			&hs, sizeof(hs), 0))) {
			RMDBGLOG((LOCALDBG, "Error setting internal HDMI state to ResetHDMI! %s\n", RMstatusToString(err)));
			return err;
		}
		RMMicroSecondSleep(5*1000);
		hs = DisplayBlock_HDMIState_Running;
		if (RMFAILED(err = RUASetProperty(pRUA, 
			DisplayBlock, RMDisplayBlockPropertyID_HDMIState, 
			&hs, sizeof(hs), 0))) {
			RMDBGLOG((LOCALDBG, "Error setting internal HDMI state to Running! %s\n", RMstatusToString(err)));
			return err;
		}
		RMMicroSecondSleep(5*1000);
	}
	
	return err;
}

// Reset: if TRUE, assure that a ResetHDMI to Running transition is performed
static RMstatus DHUnresetHDMICore(struct RUA *pRUA, RMbool Reset)
{
	RMstatus err = RM_OK;
	
	enum DisplayBlock_HDMIState_type hs;
	
	if (RMFAILED(err = RUAGetProperty(pRUA, 
		DisplayBlock, RMDisplayBlockPropertyID_HDMIState, 
		&hs, sizeof(hs)))) {
		RMDBGLOG((LOCALDBG, "Error getting internal HDMI state! %s\n", RMstatusToString(err)));
		return err;
	}
	
	if (Reset && (hs == DisplayBlock_HDMIState_Running)) {
		return DHResetHDMICore(pRUA);
	}
	
	if (hs == DisplayBlock_HDMIState_ResetConfig) {
		hs = DisplayBlock_HDMIState_ResetKeymemI2C;
		if (RMFAILED(err = RUASetProperty(pRUA, 
			DisplayBlock, RMDisplayBlockPropertyID_HDMIState, 
			&hs, sizeof(hs), 0))) {
			RMDBGLOG((LOCALDBG, "Error setting internal HDMI state to ResetKeymemI2C! %s\n", RMstatusToString(err)));
			return err;
		}
		RMMicroSecondSleep(5*1000);  // wait 5 ms
	}
	if (hs == DisplayBlock_HDMIState_ResetKeymemI2C) {
		hs = DisplayBlock_HDMIState_ResetHDMI;
		if (RMFAILED(err = RUASetProperty(pRUA, 
			DisplayBlock, RMDisplayBlockPropertyID_HDMIState, 
			&hs, sizeof(hs), 0))) {
			RMDBGLOG((LOCALDBG, "Error setting internal HDMI state to ResetHDMI! %s\n", RMstatusToString(err)));
			return err;
		}
		RMMicroSecondSleep(5*1000);  // wait 5 ms
	}
	if (hs == DisplayBlock_HDMIState_ResetHDMI) {
		hs = DisplayBlock_HDMIState_Running;
		if (RMFAILED(err = RUASetProperty(pRUA, 
			DisplayBlock, RMDisplayBlockPropertyID_HDMIState, 
			&hs, sizeof(hs), 0))) {
			RMDBGLOG((LOCALDBG, "Error setting internal HDMI state to Running! %s\n", RMstatusToString(err)));
			return err;
		}
	}
	
	return err;
}
#endif

RMstatus DHCheckHDCPKeyMem(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 bist;
	RMuint64 t0, t1;
	
	CHECK_pDH("DHCheckHDCPKeyMem");
	
	if (! pDH->part_caps.HDCP) {
		RMDBGLOG((LOCALDBG, "Call to DHCheckHDCPKeyMem: Error, chip does not support HDCP\n"));
		return RM_ERROR;
	}
	
	switch (pDH->part) {
	case DH_siI170:
	case DH_siI9030:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xFA, 0x03);  // calculate CRC
		if (0)  // skip first line of next case
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xFA, 0x04);  // CRC test only, no BIST, PD# does not have to be set
		if (RMFAILED(err)) return err;
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF9, &bist);
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > 200000) err = RM_TIMEOUT;  // 0.2 sec
		} while (RMSUCCEEDED(err) && ((bist & 0x01) == 0));
		if (err == RM_TIMEOUT) {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP BIST Timeout!\n");
		} else {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP BIST took %llu mSec.\n", RMCyclesElapsed64(t0, t1) / 1000);
		}
		if (bist & 0x02) {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP BIST failed! 0x%02X\n", bist);
			err = RM_ERROR;
		}
		break;
	case DH_ANX9030:
		// BIST already performed after reset
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xBE, &bist);
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > 200000) err = RM_TIMEOUT;  // 0.2 sec
		} while (RMSUCCEEDED(err) && ((bist & 0x01) == 0));
		if (err == RM_TIMEOUT) {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP BIST Timeout!\n");
		} else {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP BIST took %llu mSec.\n", RMCyclesElapsed64(t0, t1) / 1000);
		}
		if (bist & 0x02) {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP BIST failed! 0x%02X\n", bist);
			err = RM_ERROR;
		}
		break;
// 2007/03/22 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        // Do not need to check the internal embedded HDCP key mem for CAT6611
        err = RM_OK ;
        break ;
//~jj_tseng@chipadvanced.com 2007/03/22
	default:
		err = RM_ERROR;
	}
	
	return err;
}

static RMstatus DHCreate(struct RUA *pRUA, struct DH_control **ppDH)
{
	if (! manutest) fprintf(stderr, "[HDMI] ========================== creating pDH ==========================\n");
#ifdef HDCP_USE_FACSIMILE_KEY
	if (! manutest) fprintf(stderr, "[HDMI] ===  COMPILED WITH HDCP FACSIMILE KEYS -- Not for production!  ===\n");
	if (! manutest) fprintf(stderr, "[HDMI] ==================================================================\n");
#endif
	*ppDH = (struct DH_control *)RMMalloc(sizeof(struct DH_control));
	if (*ppDH == NULL) {
		if (! manutest) fprintf(stderr, "[HDMI] FATAL! Not enough memory for struct DH_control!\n");
		return RM_FATALOUTOFMEMORY;
	}
	
	RMMemset(*ppDH, 0, sizeof(struct DH_control));
	
	(*ppDH)->pRUA = pRUA;
	(*ppDH)->SRM_dss.y = bignum_from_bytes((RMuint8 *)SRM_y, 128);
	(*ppDH)->SRM_dss.p = bignum_from_bytes((RMuint8 *)SRM_p, 128);
	(*ppDH)->SRM_dss.q = bignum_from_bytes((RMuint8 *)SRM_q, 20);
	(*ppDH)->SRM_dss.g = bignum_from_bytes((RMuint8 *)SRM_g, 128);
	
	(*ppDH)->InputColorSpace = EMhwlibColorSpace_RGB_0_255;
	(*ppDH)->InputSamplingMode = EMhwlibSamplingMode_444;
	(*ppDH)->InputComponentBitDepth = 8;
	(*ppDH)->OutputColorSpace = EMhwlibColorSpace_RGB_0_255;
	(*ppDH)->OutputSamplingMode = EMhwlibSamplingMode_444;
	(*ppDH)->OutputComponentBitDepth = 8;
	
	return RM_OK;
}

static RMstatus DHReadVendorID(struct DH_control *pDH, RMuint8 addr, RMuint16 *pVendor_ID, RMuint16 *pDevice_ID, RMuint8 *pDevice_Rev)
{
	RMstatus err;
	RMuint8 id[5];
	
	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), addr, id, 5);
	if (RMSUCCEEDED(err)) {
		*pVendor_ID = id[0] | (id[1] << 8);
		*pDevice_ID = id[2] | (id[3] << 8);
		*pDevice_Rev = id[4];
		if (! manutest) fprintf(stderr, "[HDMI] Detected part at I2C device address 0x%02X: vendor 0x%04X, device %04X, rev.0x%02X%s\n", 
			pDH->i2c_tx.dev.DevAddr, *pVendor_ID, *pDevice_ID, *pDevice_Rev, 
			(*pVendor_ID == VENDOR_ID_SII) ? " (Silicon Image)" : 
			(*pVendor_ID == VENDOR_ID_ANX) ? " (Analogix)" : 
			// 2007/03/22 Modified by jj_tseng@chipadvanced
            (*pVendor_ID == VENDOR_ID_ANX) ? " (Chipadvanced Tech.)" : "");
            //~jj_tseng@chipadvanced.com 2007/03/22
	}
	return err;
}

RMstatus DHOpenChip(struct RUA *pRUA, enum DH_vendor_parts part, RMbool Reset, enum GPIOId_type reset_GPIO, 
	RMuint32 i2c_tx_ModuleNumber, enum GPIOId_type i2c_tx_GPIOClock, enum GPIOId_type i2c_tx_GPIOData, RMuint32 i2c_tx_Speed_kHz, 
	RMuint32 i2c_rx_ModuleNumber, enum GPIOId_type i2c_rx_GPIOClock, enum GPIOId_type i2c_rx_GPIOData, RMuint32 i2c_rx_Speed_kHz, 
	struct DH_control **ppDH)
{
	RMstatus err = RM_OK;
	RMuint32 i, part_num;
	RMbool found_part = FALSE;
	RMuint16 vendor_ID, device_ID;
	RMuint8 device_rev;
	
	RMDBGLOG((LOCALDBG,"DHOpenChip()\n"));
	
	if (pRUA == NULL) {
		if (! manutest) fprintf(stderr, "[HDMI] Call to DHOpenChip with invalid RUA structure!\n");
		return RM_FATALINVALIDPOINTER;
	}
	CHECK_PTR("DHOpenChip", ppDH);
	
	if (*ppDH) {
		RMDBGLOG((ENABLE, "Warning: Call to DHOpenChip() with initialized struct DH_control!\n"));
	}
	err = DHCreate(pRUA, ppDH);
	if (RMFAILED(err)) return err;
	
	(*ppDH)->i2c_module = i2c_tx_ModuleNumber;
	if ((i2c_tx_GPIOClock == GPIOId_Sys_0) && (i2c_tx_GPIOData == GPIOId_Sys_1)) {
		(*ppDH)->i2c_tx.I2C = EMHWLIB_MODULE(I2C, i2c_tx_ModuleNumber);
	} else {
		(*ppDH)->i2c_tx.I2C = EMHWLIB_MODULE(I2C, 0);
	}
	(*ppDH)->i2c_tx.dev.APIVersion = 1;
	(*ppDH)->i2c_tx.dev.Clock = i2c_tx_GPIOClock;
	(*ppDH)->i2c_tx.dev.Data = i2c_tx_GPIOData;
	(*ppDH)->i2c_tx.dev.Speed = i2c_tx_Speed_kHz ? i2c_tx_Speed_kHz : 100;
	(*ppDH)->i2c_tx.dev.Delay = 10;  // conservative setting. TODO: 0 ?
	RMDBGLOG((LOCALDBG, "Using I2C module %lu at %lu kHz to access TX on GPIO %d/%d\n", 
		EMHWLIB_MODULE_INDEX((*ppDH)->i2c_tx.I2C), 
		(*ppDH)->i2c_tx.dev.Speed, (*ppDH)->i2c_tx.dev.Clock, (*ppDH)->i2c_tx.dev.Data));
	
	(*ppDH)->gpio_reset = reset_GPIO;  // 0 is reserved for "No GPIO for RESET"
	if ((*ppDH)->i2c_module == 2) {  // SMP86xx internal HDMI core
#ifdef RMFEATURE_HAS_HDMI
		err = DHUnresetHDMICore(pRUA, Reset);
		if (RMFAILED(err)) return err;
		(*ppDH)->gpio_reset = 255;
#else
		if (! manutest) fprintf(stderr, "[HDMI] ERROR: No internal SiI9030 core on this Sigma chip!\n");
		return RM_ERROR;
#endif
	} else if ((part == DH_auto_detect) || (part == DH_siI164) || (part == DH_siI170)) {
		if ((*ppDH)->gpio_reset != 0) { // 0 is reserved for "No GPIO for RESET"
			DHReset_SiI164(*ppDH);
		}
	}
	
	if (part == DH_auto_detect) {
		for (i = 0; i < sizeof(i2c_probe) / sizeof(i2c_probe[0]); i++) {
			pi2c_prev_dev = NULL;
			(*ppDH)->i2c_tx.dev.DevAddr = i2c_probe[i][0];
			err = DHReadVendorID(*ppDH, i2c_probe[i][1], &vendor_ID, &device_ID, &device_rev);
			if (RMSUCCEEDED(err)) {
				for (part_num = 0; part_num < sizeof(part_info) / sizeof(part_info[0]); part_num++) {
					if (
						(
							(i2c_probe[i][0] == part_info[part_num].i2c_tx_pri_addr) || 
							(i2c_probe[i][0] == part_info[part_num].i2c_tx_sec_addr)
						) && 
						(i2c_probe[i][1] == part_info[part_num].id_sub_address) && 
						(vendor_ID == part_info[part_num].vendor_ID) && 
						(device_ID == part_info[part_num].device_ID) && 
						((! part_info[part_num].device_rev) || (device_rev == part_info[part_num].device_rev))
					) {
						found_part = TRUE;
						part = (enum DH_vendor_parts)part_num;
						(*ppDH)->i2c_tx.dev.Speed = i2c_tx_Speed_kHz ? i2c_tx_Speed_kHz : part_info[part_num].i2c_tx_speed_kHz;
						(*ppDH)->i2c_tx.dev.Delay = part_info[part_num].i2c_tx_delay_us;
						break;  // success, break from inner loop
					}
				}
			}
			if (found_part) {
				break;  // success, break from outer loop
			}
		}
		
		if (! found_part) {
			if (! manutest) fprintf(stderr, "[HDMI] Did not find any known DVI/HDMI parts!\n");
			err = RM_ERROR;
			goto cleanup;
		}
	} else {
		part_num = (RMuint32)part;
		(*ppDH)->i2c_tx.dev.Speed = i2c_tx_Speed_kHz ? i2c_tx_Speed_kHz : part_info[part_num].i2c_tx_speed_kHz;
		(*ppDH)->i2c_tx.dev.Delay = part_info[part_num].i2c_tx_delay_us;
		for (i = 0; i < part_info[part_num].i2c_tx_sec_addr ? 2 : 1; i++) {
			pi2c_prev_dev = NULL;
			(*ppDH)->i2c_tx.dev.DevAddr = (i == 0) ? part_info[part_num].i2c_tx_pri_addr : part_info[part_num].i2c_tx_sec_addr;
			err = DHReadVendorID(*ppDH, part_info[part_num].id_sub_address, &vendor_ID, &device_ID, &device_rev);
			if (RMSUCCEEDED(err) && 
				(vendor_ID == part_info[part_num].vendor_ID) && 
				(device_ID == part_info[part_num].device_ID) && 
				((! part_info[part_num].device_rev) || (device_rev == part_info[part_num].device_rev))
			) {
				found_part = TRUE;
				break;
			}
		}
		if (! found_part) {
			if (! manutest) fprintf(stderr, "[HDMI] Did not find the specified DVI/HDMI part!\n");
			err = RM_ERROR;
			goto cleanup;
		}
	}
	
	if (found_part) {
		(*ppDH)->part = part;
		(*ppDH)->part_caps = part_info[part].part_caps;
		if (! manutest) fprintf(stderr, "[HDMI] Found the part: %s (%d), Vendor ID is 0x%04X / 0x%04X\n", 
			part_info[part].part_name, part, vendor_ID, device_ID);
	} else {
		if (! manutest) fprintf(stderr, "[HDMI] Unknown DVI or HDMI part (%d)!!!\n", part);
		err = RM_ERROR;
		goto cleanup;
	}
	
	// common code for all known parts
	(*ppDH)->i2cdbg = (*ppDH)->i2c_tx;
	if (part_info[part].i2c_2nd_offs) {
		(*ppDH)->i2c_tx2 = (*ppDH)->i2c_tx;
		(*ppDH)->i2c_tx2.dev.DevAddr += part_info[part].i2c_2nd_offs;
	} else {
		(*ppDH)->i2c_tx2.I2C = 0;
	}
	
	if ((*ppDH)->part_caps.DDC) {
		// DDC is implemented using a FIFO in the Tx chip
		(*ppDH)->i2c_rx.I2C = 0;
	} else {
		// Direct access to DCC from 86xx chip
		if ((i2c_rx_GPIOClock == GPIOId_Sys_0) && (i2c_rx_GPIOData == GPIOId_Sys_1)) {
			(*ppDH)->i2c_rx.I2C = EMHWLIB_MODULE(I2C, i2c_rx_ModuleNumber);
		} else {
			// Pins not supported by hw i2c, fall back to sw i2c
			(*ppDH)->i2c_rx.I2C = EMHWLIB_MODULE(I2C, 0);
		}
		(*ppDH)->i2c_rx.dev.APIVersion = 1;
		(*ppDH)->i2c_rx.dev.Clock = i2c_rx_GPIOClock;
		(*ppDH)->i2c_rx.dev.Data = i2c_rx_GPIOData;
		(*ppDH)->i2c_rx.dev.Speed = i2c_rx_Speed_kHz ? i2c_rx_Speed_kHz : part_info[part].i2c_rx_speed_kHz;
		(*ppDH)->i2c_rx.dev.Delay = part_info[part].i2c_rx_delay_us;
		(*ppDH)->i2c_rx.dev.DevAddr = 0x00;
	}
	
	DHInitChip(*ppDH);
	DHGetConnection(*ppDH, &(*ppDH)->cable, NULL, NULL);
	DHCheckHDMI(*ppDH, NULL);
	
	return err;
	
cleanup:
	RMFree(*ppDH);
	*ppDH = NULL;
	return err;
}


/* These DHInit... functions are obsolete, use DHOpenChip() instead. */

RMstatus DHInitWithAutoDetectReset(struct RUA *pRUA, 
	RMuint8 pio_clock_transmitter, RMuint8 pio_data_transmitter, RMuint32 i2c_module, 
	RMuint8 gpio_reset, struct DH_control **ppDH, RMbool Reset)
{
	if (! manutest) fprintf(stderr, "[HDMI] ALERT: The function DHInitWithAutoDetect() is obsolete, use DHOpenChip() instead!\n");
	RMDBGLOG((LOCALDBG,"DHInitWithAutoDetectReset()\n")) ;
	return DHOpenChip(pRUA, DH_auto_detect, Reset, gpio_reset, 
		i2c_module, (enum GPIOId_type)(GPIOId_Sys_0 + pio_clock_transmitter), (enum GPIOId_type)(GPIOId_Sys_0 + pio_data_transmitter), 400, 
		i2c_module, TRANSMITTER_GPIO_CLOCK, TRANSMITTER_GPIO_DATA, 400, 
		ppDH);
}

RMstatus DHInitWithAutoDetect(struct RUA *pRUA, 
	RMuint8 pio_clock_transmitter, RMuint8 pio_data_transmitter, RMuint32 i2c_module, 
	RMuint8 gpio_reset, struct DH_control **ppDH)
{
	RMDBGLOG((LOCALDBG,"DHInitWithAutoDetect()\n")) ;
	return DHInitWithAutoDetectReset(pRUA, pio_clock_transmitter, pio_data_transmitter, i2c_module, gpio_reset, ppDH, FALSE);
}

RMstatus DHInitReset(struct RUA *pRUA, RMuint8 pio_clock_transmitter, RMuint8 pio_data_transmitter, 
	RMuint8 pio_clock_receiver, RMuint8 pio_data_receiver, RMuint32 i2c_module, 
	enum DH_vendor_parts part, struct DH_control **ppDH, RMbool Reset)
{
	if (! manutest) fprintf(stderr, "[HDMI] ALERT: The function DHInit() is obsolete, use DHOpenChip() instead!\n");
	RMDBGLOG((LOCALDBG,"DHInitReset()\n")) ;
	return DHOpenChip(pRUA, part, Reset, 
#if ( (EM86XX_CHIP==EM86XX_CHIPID_TANGO2) || (EM86XX_CHIP==EM86XX_CHIPID_TANGO15) )
		4, 
#else  
		0, 
#endif
		i2c_module, (enum GPIOId_type)(GPIOId_Sys_0 + pio_clock_transmitter), (enum GPIOId_type)(GPIOId_Sys_0 + pio_data_transmitter), 400, 
		i2c_module, (enum GPIOId_type)(GPIOId_Sys_0 + pio_clock_receiver), (enum GPIOId_type)(GPIOId_Sys_0 + pio_data_receiver), 400, 
		ppDH);
}

RMstatus DHInit(struct RUA *pRUA, RMuint8 pio_clock_transmitter, RMuint8 pio_data_transmitter, 
	RMuint8 pio_clock_receiver, RMuint8 pio_data_receiver, RMuint32 i2c_module, 
	enum DH_vendor_parts part, struct DH_control **ppDH)
{
	RMDBGLOG((LOCALDBG,"DHInit()\n")) ;
	return DHInitReset(pRUA, pio_clock_transmitter, pio_data_transmitter, pio_clock_receiver, pio_data_receiver, i2c_module, part, ppDH, FALSE);
}


RMstatus DHSetHDMIMode(struct DH_control *pDH, RMbool HDMI)
{
	RMstatus err;
	RMuint8 reg;
	
	CHECK_pDH("DHSetHDMIMode");
	
	if (! manutest) fprintf(stderr, "[HDMI] DHSetHDMIMode(%s)\n", HDMI ? "TRUE" : "FALSE");
	pDH->HDMI_mode = HDMI && (pDH->part_caps.HDMI);
	if (! pDH->HDMI_mode) {
		pDH->info_frame_enable = 0;
		RMDBGLOG((LOCALDBG, "info_frame_enable now 0x%04X\n", pDH->info_frame_enable));
	}
	switch (pDH->part) {
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			// set HDMI mode (DVI or HDMI)
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x2F, &reg);  // HDMI mode
			if (RMFAILED(err)) reg = 0x00;
			RMinsShiftBool(&reg, pDH->HDMI_mode, 0);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x2F, reg);  // HDMI mode
			// enable / disable audio
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x14, &reg);
			reg = (reg & 0xFE) | ((pDH->HDMI_mode && pDH->HDMI_audio) ? 0x01 : 0x00);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x14, reg);
			// disable info frames, if DVI
			if (! pDH->HDMI_mode) {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3E, 0x00);
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, 0x00);
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
			}
			break;
		case DH_ANX9030:
			// set HDMI mode (DVI or HDMI)
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &reg);  // HDMI mode
			if (RMFAILED(err)) reg = init_ANX9030[3][1];
			RMinsShiftBool(&reg, pDH->HDMI_mode, 1);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x07, reg);  // HDMI mode
			// enable / disable audio
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x51, &reg);
			reg = (reg & 0x7F) | ((pDH->HDMI_mode && pDH->HDMI_audio) ? 0x80 : 0x00);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x51, reg);
			// disable info frames, if DVI
			if (! pDH->HDMI_mode) {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xC0, 0x00);
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xC1, 0x00);
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
			}
			break;
        // 2007/03/22 Added by jj_tseng@chipadvanced
        case DH_CAT6611:
            // set HDMI mode (DVI or HDMI)
            err = DHEnableHDMIOutput_CAT6611(pDH, pDH->HDMI_mode) ;
            if( !pDH->HDMI_mode )
            {
                // disable all infoframe for DVI mode.
				DH_Switch_6611BANK(pDH, 0) ;
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC6, 0);
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC9, 0);
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xCA, 0);
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xCB, 0);
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xCC, 0);
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xCD, 0);
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xCE, 0);
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xCF, 0);
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xD0, 0);
                
            }
            break ;
        //~jj_tseng@chipadvanced.com 2007/03/22
		default:
			if (HDMI) {  // attempt to enable HDMI mode on DVI chipset
				if (! manutest) fprintf(stderr, "[HDMI] ERROR: Can not enable HDMI mode on DVI chipset!\n");
				return RM_ERROR;
			}
			break;
	}
	
	return RM_OK;
}

RMstatus DHSetEDIDMode(struct DH_control *pDH, 
	enum DH_EDID_select EDID_select, 
	RMuint32 EDID_selection,  // preferred entry in VIC table
	RMuint32 EDID_vfreq,  // preferred vsync frequency (50, 59, 60 etc.)
	RMuint32 EDID_hsize,  // preferred horizontal active size
	RMuint32 EDID_vsize,  // preferred vertical active size
	RMbool EDID_intl)     // preferred interlaced (TRUE) / progressive (FALSE) mode
{
	CHECK_pDH("DHSetEDIDMode");
	
	pDH->EDID_select = EDID_select;
	pDH->EDID_selection = EDID_selection;
	pDH->EDID_vfreq = EDID_vfreq;
	pDH->EDID_hsize = EDID_hsize;
	pDH->EDID_vsize = EDID_vsize;
	pDH->EDID_intl = EDID_intl;
	// TODO adjust video mode, if changed
	return RM_OK;
}

RMstatus DHSetEDIDForceMask(struct DH_control *pDH, 
	RMuint64 EDID_force_mask)  // try these VICs first (bit 1 = VIC 1, bit 2 = VIC 2 etc.)
{
	CHECK_pDH("DHSetEDIDForceMask");
	
	pDH->EDID_force_mask = EDID_force_mask;
	return RM_OK;
}

RMstatus DHSetEDIDExcludeMask(struct DH_control *pDH, 
	RMuint64 EDID_exclude_mask) // never use these VICs
{
	CHECK_pDH("DHSetEDIDExcludeMask");
	
	pDH->EDID_exclude_mask = EDID_exclude_mask;
	return RM_OK;
}

RMstatus DHSetEDIDFrequencyLimits(struct DH_control *pDH, 
	RMuint32 EDID_max_pixclk, 
	RMuint32 EDID_min_pixclk, 
	RMuint32 EDID_max_hfreq, 
	RMuint32 EDID_min_hfreq, 
	RMuint32 EDID_max_vfreq, 
	RMuint32 EDID_min_vfreq)
{
	CHECK_pDH("DHSetEDIDFrequencyLimits");
	
	pDH->EDID_max_pixclk = EDID_max_pixclk;
	pDH->EDID_min_pixclk = EDID_min_pixclk;
	pDH->EDID_max_hfreq = EDID_max_hfreq;
	pDH->EDID_min_hfreq = EDID_min_hfreq;
	pDH->EDID_max_vfreq = EDID_max_vfreq;
	pDH->EDID_min_vfreq = EDID_min_vfreq;
	return RM_OK;
}

static RMstatus DHGetInfoFrameEnable_SiI(struct DH_control *pDH, RMuint32 *info_frame_enable)
{
	RMstatus err;
	RMuint8 reg;
	
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x3E, &reg);
	if (RMFAILED(err)) {
		reg = pDH->info_frame_enable & 0xFF;
	} else {
		if (reg & 0x01) reg |= 0x02; else reg &= ~0x02;
		if (reg & 0x04) reg |= 0x08; else reg &= ~0x08;
		if (reg & 0x10) reg |= 0x20; else reg &= ~0x20;
		if (reg & 0x40) reg |= 0x80; else reg &= ~0x80;
	}
	*info_frame_enable = reg;
	
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, &reg);
	if (RMFAILED(err)) {
		reg = (pDH->info_frame_enable >> 8) & 0xFF;
	} else {
		if (reg & 0x01) reg |= 0x02; else reg &= ~0x02;
		if (reg & 0x04) reg |= 0x08; else reg &= ~0x08;
		if (reg & 0x10) reg |= 0x20; else reg &= ~0x20;
	}
	*info_frame_enable |= (reg << 8);
	
	return RM_OK;
}

static RMstatus DHSetInfoFrameEnable_SiI(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err, err1, err2;
	RMuint32 info_frame_enable;
	
	info_frame_enable = pDH->info_frame_enable | mask;
	pDH->info_frame_enable = info_frame_enable;
	
	err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3E, info_frame_enable & 0xFF);
	err2 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, (info_frame_enable >> 8) & 0xFF);
	if (mask & 0x00FF) err = err1;
	else if (mask & 0xFF00) err = err2;
	else err = RM_OK;
	if (RMSUCCEEDED(err)) RMDBGLOG((LOCALDBG, "info_frame_enable now 0x%04X\n", info_frame_enable));
	else RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
	
	return err;
}

static RMstatus DHClearInfoFrameEnable_SiI(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err, err1, err2;
	RMuint32 info_frame_enable;
	
	info_frame_enable = pDH->info_frame_enable & ~mask;
	pDH->info_frame_enable = info_frame_enable;
	
	err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3E, info_frame_enable & 0xFF);
	err2 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, (info_frame_enable >> 8) & 0xFF);
	if (mask & 0x00FF) err = err1;
	else if (mask & 0xFF00) err = err2;
	else err = RM_OK;
	if (RMSUCCEEDED(err)) RMDBGLOG((LOCALDBG, "info_frame_enable now 0x%04X\n", info_frame_enable));
	else RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
	
	return err;
}

static RMstatus DHWaitInfoFrameEnable_SiI(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err;
	RMuint8 ife;
	RMuint64 t0, t1;
	
	t0 = RMGetTimeInMicroSeconds();
	do {
		if (mask < 0x100) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x3E, &ife);
			ife &= (mask & 0xFF);
		} else {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, &ife);
			ife &= ((mask >> 8) & 0xFF);
		}
		t1 = RMGetTimeInMicroSeconds();
		if (RMCyclesElapsed64(t0, t1) > 200000) err = RM_TIMEOUT;  // 0.2 sec
	} while (RMSUCCEEDED(err) && ife);
	
	return err;
}

// 2007/03/22 Added by jj_tseng@chipadvanced
static RMstatus DHGetInfoFrameEnable_CAT(struct DH_control *pDH, RMuint32 *info_frame_enable)
{
    *info_frame_enable = pDH->info_frame_enable ;
    
    RMDBGLOG((LOCALDBG,"DHGetInfoFrameEnable_CAT(%08lX)\n",*info_frame_enable)) ;

    return RM_OK;
}

static RMstatus DHSetInfoFrameEnable_CAT(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err, err1 ;
	RMuint32 info_frame_enable;

	RMDBGLOG((LOCALDBG,"static RMstatus DHSetInfoFrameEnable_CAT(struct DH_control *pDH, RMuint32 %08lX)\n",mask)) ;
	
	info_frame_enable = pDH->info_frame_enable | mask;
	pDH->info_frame_enable = info_frame_enable ;

	DH_Switch_6611BANK(pDH, 0) ;
    err = RM_OK ;
    if( mask & INFO_FRAME_AVI_ENABLE )
    {
		RMDBGPRINT((LOCALDBG,"INFO_FRAM_AVI_ENABLE\n")) ;
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_AVI_INFOFRM_CTRL, 3);
        if( RMFAILED(err1)) err = err1 ;
    }


    if( mask & INFO_FRAME_SPD_ENABLE )
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_SPD_INFOFRM_CTRL, 3);
        if( RMFAILED(err1)) err = err1 ;
    }

    if( mask & INFO_FRAME_AUD_ENABLE )
    {
		RMDBGPRINT((LOCALDBG,"INFO_FRAM_AUD_ENABLE\n")) ;
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_AUD_INFOFRM_CTRL, 3);
        if( RMFAILED(err1)) err = err1 ;
    }

    if( mask & INFO_FRAME_MPG_ENABLE )
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_MPG_INFOFRM_CTRL, 3);
        if( RMFAILED(err1)) err = err1 ;
    }

    if( (mask & INFO_FRAME_CP_ENABLE)==INFO_FRAME_CP_ENABLE )
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_PKT_GENCTRL_CTRL, 3);
        if( RMFAILED(err1)) err = err1 ;
    }

    if( (mask & INFO_FRAME_CP_ONCE )==INFO_FRAME_CP_ONCE)
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_PKT_GENCTRL_CTRL, 1);
        if( RMFAILED(err1)) err = err1 ;
    }

	RMDBGLOG((LOCALDBG,"DHSetInfoFrameEnable_CAT(): pDH->info_frame_enable=%08lX \n",pDH->info_frame_enable)) ;
	// DHDump_CAT6611reg(pDH) ;

	if (RMSUCCEEDED(err)) RMDBGLOG((LOCALDBG, "info_frame_enable now 0x%04X\n", info_frame_enable));
	else RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
	
	return err;
}

static RMstatus DHClearInfoFrameEnable_CAT(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err = RM_OK , err1 ;
	RMuint32 info_frame_enable;
	
	info_frame_enable = pDH->info_frame_enable & ~mask;
	pDH->info_frame_enable = info_frame_enable;
	
    if( mask & INFO_FRAME_AVI_ENABLE )
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_AVI_INFOFRM_CTRL, 0);
        if( RMFAILED(err1)) err = err1 ;
    }


    if( mask & INFO_FRAME_SPD_ENABLE )
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_SPD_INFOFRM_CTRL, 0);
        if( RMFAILED(err1)) err = err1 ;
    }

    if( mask & INFO_FRAME_AUD_ENABLE )
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_AUD_INFOFRM_CTRL, 0);
        if( RMFAILED(err1)) err = err1 ;
    }

    if( mask & INFO_FRAME_MPG_ENABLE )
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_MPG_INFOFRM_CTRL, 0);
        if( RMFAILED(err1)) err = err1 ;
    }

    if( (mask & INFO_FRAME_CP_ENABLE)==INFO_FRAME_CP_ENABLE )
    {
        err1 = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), REG_PKT_GENCTRL_CTRL, 0);
        if( RMFAILED(err1)) err = err1 ;
    }

	if (RMSUCCEEDED(err)) RMDBGLOG((LOCALDBG, "info_frame_enable now 0x%04X\n", info_frame_enable));
	else RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
	RMDBGLOG((LOCALDBG,"DHClearInfoFrameEnable_CAT(): pDH->info_frame_enable=%08lX = %08lX\n",pDH->info_frame_enable)) ;
	
	return err;
}

// static RMstatus DHWaitInfoFrameEnable_CAT(struct DH_control *pDH, RMuint32 mask)
// {
// 	return RM_OK ; // for CAT, no need to wait info frame
// }
//~jj_tseng@chipadvanced.com 2007/03/22

static RMstatus DHGetInfoFrameEnable_ANX(struct DH_control *pDH, RMuint32 *info_frame_enable)
{
	RMstatus err;
	RMuint8 reg[2];
	RMuint32 ife;
	
	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), 0xC0, reg, 2);
	if (RMFAILED(err)) {
		ife = pDH->info_frame_enable;
	} else {
		ife = 0;
		if (reg[0] & 0x02) ife |= 0xC000;  // ACR
		if (reg[0] & 0x08) ife |= 0x0C00;  // CP
		if (reg[0] & 0x20) ife |= 0x0003;  // AVI
		if (reg[0] & 0x80) ife |= 0x000C;  // SPD
		if (reg[1] & 0x02) ife |= 0x0030;  // Audio
		if (reg[1] & 0x08) ife |= 0x00C0;  // MPEG
		if (reg[1] & 0x20) ife |= 0x0300;  // Gen
		if (reg[1] & 0x80) ife |= 0x3000;  // Gen2
	}
	*info_frame_enable = ife;
	
	return RM_OK;
}

static RMstatus DHSetInfoFrameEnable_ANX(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err;
	RMuint32 info_frame_enable;
	RMuint8 reg[2] = {0, 0};
	
	info_frame_enable = pDH->info_frame_enable | mask;
	pDH->info_frame_enable = info_frame_enable;
	
	if (info_frame_enable & 0x8000) reg[0] |= 0x01;
	if (info_frame_enable & 0x4000) reg[0] |= 0x02;
	if (info_frame_enable & 0x0800) reg[0] |= 0x04;
	if (info_frame_enable & 0x0400) reg[0] |= 0x08;
	if (info_frame_enable & 0x0002) reg[0] |= 0x10;
	if (info_frame_enable & 0x0001) reg[0] |= 0x20;
	if (info_frame_enable & 0x0008) reg[0] |= 0x40;
	if (info_frame_enable & 0x0004) reg[0] |= 0x80;
	if (info_frame_enable & 0x0020) reg[1] |= 0x01;
	if (info_frame_enable & 0x0010) reg[1] |= 0x02;
	if (info_frame_enable & 0x0080) reg[1] |= 0x04;
	if (info_frame_enable & 0x0040) reg[1] |= 0x08;
	if (info_frame_enable & 0x0200) reg[1] |= 0x10;
	if (info_frame_enable & 0x0100) reg[1] |= 0x20;
	if (info_frame_enable & 0x2000) reg[1] |= 0x40;
	if (info_frame_enable & 0x1000) reg[1] |= 0x80;
	err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), 0xC0, reg, 2);
	if (RMSUCCEEDED(err)) RMDBGLOG((LOCALDBG, "info_frame_enable now 0x%04X\n", info_frame_enable));
	else RMDBGLOG((ENABLE, "Failed to write info_frame_enable!\n"));
	
	return err;
}

static RMstatus DHClearInfoFrameEnable_ANX(struct DH_control *pDH, RMuint32 mask)
{
	pDH->info_frame_enable &= ~mask;
	
	return DHSetInfoFrameEnable_ANX(pDH, 0x0000);
}

static RMstatus DHWaitInfoFrameEnable_ANX(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err;
	RMuint8 ife;
	RMuint64 t0, t1;
	RMuint32 mask_anx = 0;
	
	if (mask & 0x8000) mask_anx |= 0x0001;
	if (mask & 0x4000) mask_anx |= 0x0002;
	if (mask & 0x0800) mask_anx |= 0x0004;
	if (mask & 0x0400) mask_anx |= 0x0008;
	if (mask & 0x0002) mask_anx |= 0x0010;
	if (mask & 0x0001) mask_anx |= 0x0020;
	if (mask & 0x0008) mask_anx |= 0x0040;
	if (mask & 0x0004) mask_anx |= 0x0080;
	if (mask & 0x0020) mask_anx |= 0x0100;
	if (mask & 0x0010) mask_anx |= 0x0200;
	if (mask & 0x0080) mask_anx |= 0x0400;
	if (mask & 0x0040) mask_anx |= 0x0800;
	if (mask & 0x0200) mask_anx |= 0x1000;
	if (mask & 0x0100) mask_anx |= 0x2000;
	if (mask & 0x2000) mask_anx |= 0x4000;
	if (mask & 0x1000) mask_anx |= 0x8000;
	
	t0 = RMGetTimeInMicroSeconds();
	do {
		if (mask_anx < 0x100) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0xC0, &ife);
			ife &= (mask_anx & 0xFF);
		} else {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0xC1, &ife);
			ife &= ((mask_anx >> 8) & 0xFF);
		}
		t1 = RMGetTimeInMicroSeconds();
		if (RMCyclesElapsed64(t0, t1) > 200000) err = RM_TIMEOUT;  // 0.2 sec
	} while (RMSUCCEEDED(err) && ife);
	
	return err;
}

static RMstatus DHGetInfoFrameEnable(struct DH_control *pDH, RMuint32 *info_frame_enable)
{
	RMstatus err;
	
	RMDBGLOG((LOCALDBG, "DHGetInfoFrameEnable()\n")) ;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_ERROR;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DHGetInfoFrameEnable_SiI(pDH, info_frame_enable);
		break;
// 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		err = DHGetInfoFrameEnable_CAT(pDH, info_frame_enable);
		break;
//~jj_tseng@chipadvanced.com 2007/03/22
	case DH_ANX9030:
		err = DHGetInfoFrameEnable_ANX(pDH, info_frame_enable);
		break;
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

static RMstatus DHSetInfoFrameEnable(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err;
	
	RMDBGLOG((LOCALDBG, "DHSetInfoFrameEnable()\n")) ;

	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_OK;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DHSetInfoFrameEnable_SiI(pDH, mask);
		break;
// 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		err = DHSetInfoFrameEnable_CAT(pDH, mask);
		break;
//~jj_tseng@chipadvanced.com 2007/03/22
	case DH_ANX9030:
		err = DHSetInfoFrameEnable_ANX(pDH, mask);
		break;
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

static RMstatus DHClearInfoFrameEnable(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err;

	RMDBGLOG((LOCALDBG, "DHClearInfoFrameEnable()\n")) ;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_OK;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DHClearInfoFrameEnable_SiI(pDH, mask);
		break;
// 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		err = DHClearInfoFrameEnable_CAT(pDH, mask);
		break;
//~jj_tseng@chipadvanced.com 2007/03/22
	case DH_ANX9030:
		err = DHClearInfoFrameEnable_ANX(pDH, mask);
		break;
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

static RMstatus DHWaitInfoFrameEnable(struct DH_control *pDH, RMuint32 mask)
{
	RMstatus err;

	RMDBGLOG((LOCALDBG, "DHWaitInfoFrameEnable()\n")) ;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_OK;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DHWaitInfoFrameEnable_SiI(pDH, mask);
		break;
	case DH_ANX9030:
		err = DHWaitInfoFrameEnable_ANX(pDH, mask);
		break;
// 2007/03/22 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        err = RM_OK ;
        break ;
//~jj_tseng@chipadvanced.com 2007/03/22
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

RMstatus DHGetState(struct DH_control *pDH, 
	enum DH_device_state *pDevState, 
	enum DH_connection *pConnection)
{
	CHECK_pDH("DHGetState");

	RMDBGLOG((DISABLE, "DHGetState(pDevSTate=%x,pConnection=%x)\n",pDH->state,pDH->cable)) ;
	
	if (pDevState != NULL) *pDevState = pDH->state;
	if (pConnection != NULL) *pConnection = pDH->cable;
	return RM_OK;
}

static RMstatus DHInitChip_SiI164(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 index;
	for (index = 0; index < sizeof(init_SiI164) / sizeof(init_SiI164[0]); index++) {
		RMstatus e;
		if (RMFAILED(e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), init_SiI164[index][0], init_SiI164[index][1]))) {
			err = e;
			if (! manutest) fprintf(stderr, "[HDMI] Failed to enable DVI output: write 0x%02X to 0x%02X:0x%02X failed, %s\n", init_SiI164[index][1], pDH->i2c_tx.dev.DevAddr, init_SiI164[index][0], RMstatusToString(err));
		}
	}
	
	return err;
}

static RMstatus DHInitChip_SiI170(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 index;
	for (index = 0; index < sizeof(init_SiI170) / sizeof(init_SiI170[0]); index++) {
		RMstatus e;
		if (RMFAILED(e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), init_SiI170[index][0], init_SiI170[index][1]))) {
			err = e;
			if (! manutest) fprintf(stderr, "[HDMI] Failed to enable DVI output: write 0x%02X to 0x%02X:0x%02X failed, %s\n", init_SiI170[index][1], pDH->i2c_tx.dev.DevAddr, init_SiI170[index][0], RMstatusToString(err));
		}
	}
	
	return err;
}

static RMstatus DHInitChip_SiI9030(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 index;
	for (index = 0; index < sizeof(init_SiI9030) / sizeof(init_SiI9030[0]); index++) {
		RMstatus e;
		if (RMFAILED(e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), init_SiI9030[index][0], init_SiI9030[index][1]))) {
			err = e;
			if (! manutest) fprintf(stderr, "[HDMI] Failed to enable DVI output: write 0x%02X to 0x%02X:0x%02X failed, %s\n", init_SiI9030[index][1], pDH->i2c_tx.dev.DevAddr, init_SiI9030[index][0], RMstatusToString(err));
		}
	}
	
	return err;
}

static RMstatus DHInitChip_ANX9030(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 index;
	for (index = 0; index < sizeof(init_ANX9030) / sizeof(init_ANX9030[0]); index++) {
		RMstatus e;
		if (RMFAILED(e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), init_ANX9030[index][0], init_ANX9030[index][1]))) {
			err = e;
			if (! manutest) fprintf(stderr, "[HDMI] Failed to enable HDMI output: write 0x%02X to 0x%02X:0x%02X failed, %s\n", init_ANX9030[index][1], pDH->i2c_tx.dev.DevAddr, init_ANX9030[index][0], RMstatusToString(err));
		}
	}
	
	return err;
}

// 2007/03/22 Added by jj_tseng@chipadvanced
static RMstatus DHInitChip_CAT6611(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 index;

	RMDBGLOG((LOCALDBG, "DHInitChip_CAT6611()\n")) ;

	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, 0x3D) ;
	RMMicroSecondSleep(100) ; // delay 100 us
	
	for (index = 0; index < sizeof(init_CAT6611) / sizeof(init_CAT6611[0]); index++) {
		RMstatus e;
		if (RMFAILED(e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), init_CAT6611[index][0], init_CAT6611[index][1]))) {
			err = e;
			if (! manutest) 
			{
			    fprintf(stderr, "[HDMI] Failed to enable HDMI output: write 0x%02X to 0x%02X:0x%02X failed, %s\n", 
			        init_CAT6611[index][1], pDH->i2c_tx.dev.DevAddr, init_CAT6611[index][0], RMstatusToString(err));
			}
		}
	}
	pDH->CAT_hdcpreg_ready = FALSE ;
	DHDump_CAT6611reg(pDH) ;
	return err;
}

static RMstatus DHReset_CAT6611(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	DH_Switch_6611BANK(pDH, 0) ;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, 0x5) ; // Software Reset RCLK and resume.
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x61, 0x30) ; // power down AFE driving and reset AFE driving.
	DHDump_CAT6611reg(pDH) ;
    return err ;    
}
//~jj_tseng@chipadvanced.com 2007/03/22

RMstatus DHInitChip(struct DH_control *pDH)
{
	RMstatus e, err = RM_OK;

    RMDBGLOG((LOCALDBG,"DHInitChip()\n")) ;	
	CHECK_pDH("DHInitChip");
	
	switch (pDH->part) {
	case DH_siI164:
		err = DHInitChip_SiI164(pDH);
		break;
	case DH_siI170:
		err = DHInitChip_SiI170(pDH);
		break;
	case DH_siI9030:
		e = DHInitChip_SiI9030(pDH); if (RMFAILED(e)) err = e;
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x24, RI_128_THRESHOLD); if (RMFAILED(e)) err = e;  // RI_128_COMP
#ifdef DEBUG_HDMI_INTR
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x75, 0xFF); if (RMFAILED(e)) err = e;  // enable all intr
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x76, 0x7E); if (RMFAILED(e)) err = e;  // enable all but VSYNC_REC intr
#else
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x75, 0x64); if (RMFAILED(e)) err = e;  // enable HPD, RSEN, RI_128 intr
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x76, 0x22); if (RMFAILED(e)) err = e;  // enable ENC_DIS, TCLK_STBL intr
#endif
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3E, 0x00); if (RMFAILED(e)) err = e;  // disable info frames
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, 0x00); if (RMFAILED(e)) err = e;  // disable info frames
		break;
	case DH_siI9034:
	case DH_siI9134:
		e = DHInitChip_SiI9030(pDH); if (RMFAILED(e)) err = e;
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x24, RI_128_THRESHOLD); if (RMFAILED(e)) err = e;  // RI_128_COMP
#ifdef DEBUG_HDMI_INTR
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x75, 0xFF); if (RMFAILED(e)) err = e;  // enable all intr
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x76, 0xFE); if (RMFAILED(e)) err = e;  // enable all but VSYNC_REC intr
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x77, 0xF0); if (RMFAILED(e)) err = e;  // enable all but DDC intr
#else
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x75, 0x64); if (RMFAILED(e)) err = e;  // enable HPD, RSEN, RI_128 intr
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x76, 0x22); if (RMFAILED(e)) err = e;  // enable ENC_DIS, TCLK_STBL intr
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x77, 0xB0); if (RMFAILED(e)) err = e;  // enable Ri error intr
#endif
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3E, 0x00); if (RMFAILED(e)) err = e;  // disable info frames
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, 0x00); if (RMFAILED(e)) err = e;  // disable info frames
		break;
	case DH_ANX9030:
		e = DHInitChip_ANX9030(pDH); if (RMFAILED(e)) err = e;
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xC0, 0x00); if (RMFAILED(e)) err = e;  // disable info frames
		e = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xC1, 0x00); if (RMFAILED(e)) err = e;  // disable info frames
		break;
// 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		err = DHInitChip_CAT6611(pDH); 
		break ;
//~jj_tseng@chipadvanced.com 2007/03/22
		
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	RMDBGLOG((LOCALDBG, "Performing SoftReset of HDMI chip\n"));
	DHSoftReset(pDH);
	pDH->state = DH_disabled;
	RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: DISABLED/NOT ENCRYPTED\n"));
	
	return err;
}

static RMstatus DHSetDDRMode_SiI(struct DH_control *pDH, RMbool ddr_mode)
{
	RMstatus err;
	RMuint8 reg;
	
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &reg);
	if (RMFAILED(err)) reg = init_SiI164[0][1];
	if (pDH->i2c_module == 2) {
		RMinsShiftBool(&reg, TRUE, 2);  // internal 9030: always 24 bit single edge
	} else {
		RMinsShiftBool(&reg, ! ddr_mode, 2);
	}
	return DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg);
}

static RMstatus DHSetDDRMode_ANX(struct DH_control *pDH, RMbool ddr_mode)
{
	RMstatus err;
	RMuint8 reg;
	
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x13, &reg);
	if (RMFAILED(err)) reg = init_ANX9030[16][1];
	RMinsShiftBool(&reg, ! ddr_mode, 3);
	return DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x13, reg);
}

// 2007/03/22 Added by jj_tseng@chipadvanced
static RMstatus DHSetDDRMode_CAT(struct DH_control *pDH, RMbool ddr_mode)
{
	RMstatus err;
	RMuint8 reg;
	
	DH_Switch_6611BANK(pDH, 0) ;
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x70, &reg);
	if(RMFAILED(err))
	{
	    reg = 0 ;
	}
	reg |= (ddr_mode)? 4 : 0 ;
	
	return DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x70, reg);
}
//~jj_tseng@chipadvanced.com 2007/03/22

RMstatus DHSetDDRMode(struct DH_control *pDH, RMbool ddr_mode)
{
	RMstatus err;
	
	CHECK_pDH("DHSetDDRMode");
    RMDBGLOG((LOCALDBG,"DHSetDDRMode(%s)\n",ddr_mode?"DDR":"SDR")) ;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DHSetDDRMode_SiI(pDH, ddr_mode);
		break;
	case DH_ANX9030:
		err = DHSetDDRMode_ANX(pDH, ddr_mode);
		break;
// 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		err = DHSetDDRMode_CAT(pDH, ddr_mode);
		break;
//~jj_tseng@chipadvanced.com 2007/03/22
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Failed to set DDR mode on DVI/HDMI output, %s\n", RMstatusToString(err)));
	}
	
	return err;
}

static RMstatus DHEnableOutput_SiI(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 reg;
	
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &reg);
	if (RMFAILED(err)) reg = init_SiI164[0][1];
	RMinsShiftBool(&reg, TRUE, 0);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Failed to enable DVI/HDMI output, %s\n", RMstatusToString(err)));
	}
	pDH->IgnoreStable = FALSE;
	
	return err;
}

static RMstatus DHEnableOutput_ANX(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 reg;
	
	// power up chip
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &reg);
	if (RMFAILED(err)) reg = init_ANX9030[3][1];
	RMinsShiftBool(&reg, TRUE, 0);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x07, reg);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Failed to enable DVI/HDMI output, %s\n", RMstatusToString(err)));
	}
	// enable TMDS clock
    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x99, &reg);
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x99, reg | 0x80);
    
	pDH->IgnoreStable = FALSE;
	
	return err;
}

// 2007/03/22 Added by jj_tseng@chipadvanced
static RMstatus DHSetAFE_CAT6611(struct DH_control *pDH, RMbool bHighFreq)
{
    RMstatus err = RM_OK ;
    
    if( !pDH ) 
    {
        return RM_INVALID_PARAMETER ;
    }
    RMDBGLOG((LOCALDBG,"DHSetAFE_CAT6611(%s)\n",bHighFreq?"High":"Low")) ;

	DH_Switch_6611BANK(pDH, 0) ;
    if( bHighFreq )
    {
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x62, 0x88) ; // if 1080p, AFE should set as 0x88, however, need to check how to identify the mode.
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x63, 0x01) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x64, 0x56) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x65, 0x00) ;
    }
    else
    {    
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x62, 0x18) ; // if 1080p, AFE should set as 0x88, however, need to check how to identify the mode.
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x63, 0x01) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x64, 0x1E) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x65, 0x00) ;
    }
        
    return err; 
}

static RMstatus DHFireAFE_CAT6611(struct DH_control *pDH)
{
    RMstatus err = RM_OK ;
	RMuint64 t0, t1;
    RMuint8 reg ;
        
    RMDBGLOG((LOCALDBG,"DHFireAFE_CAT6611()\n")) ;

    if( !pDH ) 
    {
        return RM_INVALID_PARAMETER ;
    }
	DH_Switch_6611BANK(pDH, 0) ;
	t0 = RMGetTimeInMicroSeconds();
	
	do {
	    DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x0E, &reg) ;
		t1 = RMGetTimeInMicroSeconds();
		if (RMCyclesElapsed64(t0, t1) > 200 * 1000)
		{
		    RMDBGLOG((LOCALDBG,"Wait for video stable timeout\n")) ;
		    err = RM_TIMEOUT;  // 0.2 sec
		    break ;
		}
		RMMicroSecondSleep(500) ; // avoid too busy register read.
	}while(!(reg & B_TXVIDSTABLE)) ;
	
	DH_Switch_6611BANK(pDH, 0) ;
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x61, 0x4) ; // if 1080p, AFE should set as 0x88, however, need to check how to identify the mode.

    return err; 
}

static RMstatus DHSetIntMask_CAT6611(struct DH_control *pDH)
{
    RMstatus err = RM_OK;
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x09, 0) ; // Detect HPD/RxSense/DDC update status
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0A, 0xF8) ; // KSVListChkMask | AuthDonMask | AuthFailMask
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0B, 0xF7) ; // VidStable
    return err ;
}

static RMstatus DHEnableHDMIOutput_CAT6611(struct DH_control *pDH, RMbool bHDMI)
{
    RMstatus err ; 
    RMuint8 reg ;
    
    RMDBGLOG((LOCALDBG,"DHEnableHDMIOutput_CAT6611(%s)\n",bHDMI?"HDMI":"DVI")) ;
	if(!bHDMI)
	{
    	DH_Switch_6611BANK(pDH, 1) ;
    	DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x58, &reg) ;
    	reg &= ~0x60 ; // output color mode = RGB444
    	DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x58, reg) ;
	}
	
	DH_Switch_6611BANK(pDH, 0) ;

    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC0, (bHDMI)?1:0);
    
    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x72, &reg) ;
    reg &= ~ 0x03 ; // set default color converting is bypass.
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x72, reg) ;
    // if DVI mode, the output color mode should be RGB444, and,
    // if input is not RGB, the external call should be called to set the intput mode.
    
    return err ;
}

static RMstatus DHAbortDDC_CAT6611(struct DH_control *pDH)
{
	RMuint8 reg;
	int count ;

    RMDBGLOG((ENABLE,"DHAbortDDC_CAT6611()\n")) ;

    DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x16, &reg) ;
    if((reg & 0xA8) ==0x80)
    {
        RMDBGLOG((ENABLE,"reg0x16 = 0x%02X, no need to abort.\n",reg)) ;
        return RM_OK ;
    }

    DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x10, &reg) ;
    if( (reg & 1 ) == 0 )
    {
        RMDBGLOG((ENABLE,"DDC master is HDCP Tx core.\n",reg)) ;


        DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &reg) ;
        reg |= 1 ; // hdcp reset
        DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, reg) ;
    }
    DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x10, 1) ;
    DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x15, 0xF) ; // abort DDC command


    for( count = 0 ; count < 100 ; count++ )
    {
    	RMMicroSecondSleep(20*1000);
        DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x16, &reg) ;
        // RMDBGLOG((ENABLE,"DDC master is HDCP Tx core. %dth reg16=%x\n",count,reg)) ;
        if( (reg & 0xA8) == 0x80 )
        {
            DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x15, &reg) ;
            //if( (reg & 0xC0) == 0xC0 )
            {
                return RM_OK ;
            }
            //else
            //{
            //    RMDBGLOG((ENABLE,"reg15 value %d is not available.\n",reg)) ;
            //}
        }
    }

    return RM_ERROR ;
}

static RMstatus DHEnableOutput_CAT(struct DH_control *pDH)
{

    RMstatus err = RM_OK;
	// RMuint8 reg;

    RMDBGLOG((LOCALDBG,"DHEnableOutput_CAT():")) ;
    RMDBGLOG((LOCALDBG,"VideoPixelClock = %ld, %s\n",pDH->VideoPixelClock,pDH->HDMI_mode?"HDMI":"DVI")) ;
    
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, 0x1D) ; // enable video on
    // AFE clocl control registers setting.
	
	err = DHSetAFE_CAT6611(pDH, (pDH->VideoPixelClock>=80000000)) ;

    // Set interrupt mask to get status from interrupt status registers
    err = DHSetIntMask_CAT6611(pDH) ;

    DHSetHDMIMode(pDH, pDH->HDMI_mode) ;
    
    // enable Video out
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, 0x05) ; // enable video on
	
    err = DHFireAFE_CAT6611(pDH) ;
	DHAbortDDC_CAT6611(pDH) ; // clear DDC

    // err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF1, &reg) ;
	// reg |= 1 ;
    // err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF1, reg) ;
    // err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF4, 0xA0) ;

	RMDBGLOG((ENABLE, "Enable CAT6611 Output, %s\n", RMstatusToString(err)));
	RMDBGLOG((ENABLE,"DHEnable_CAT6611 setted registers.\n")) ;
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, 0x01) ; // enable video on
	DHDump_CAT6611reg(pDH) ;
	pDH->IgnoreStable = FALSE;
	pDH->CAT_hdcpreg_ready = FALSE ; // when reset enable, need to reget HDCP data.
	return err;
}
//~jj_tseng@chipadvanced.com 2007/03/22

RMstatus DHEnableOutput(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	
    RMDBGLOG((LOCALDBG,"DHEnableOutput\n")) ;
	CHECK_pDH("DHEnableOutput");
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHEnableOutput with unitialize device!"));
		return RM_ERROR;
	}
	
	if (pDH->state != DH_disabled) {
		RMDBGLOG((LOCALDBG, "Call to DHEnableOutput with already enabled output."));
		return RM_OK;
	} else if (pDH->GuardTMDS) {
		RMDBGLOG((LOCALDBG, "Deferred TMDS enable due to unstable clock.\n"));
		pDH->RequestTMDS = TRUE;
		return RM_OK;
	} else switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		// make sure to send Mute
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, 0x00);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xDF, 0x01);  // set AVMute flag
		// no break;
	case DH_siI164:
	case DH_siI170:
		err = DHEnableOutput_SiI(pDH);
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		break;
	case DH_ANX9030:
		// make sure to send Mute
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xC0, 0x00);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xCC, 0x01);  // set AVMute flag
		err = DHEnableOutput_ANX(pDH);
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		break;
    // 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		// make sure to send Mute
		DH_Set_CAT6611_AVMute(pDH) ; 
		err = DHEnableOutput_CAT(pDH);
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		break;
    //~jj_tseng@chipadvanced.com 2007/03/22
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	if (pDH->part_caps.HDMI) {
		RMuint32 enable = pDH->info_frame_enable;
		
		// Send one CP only, no other InfoFrames, and wait until it has been sent
		pDH->info_frame_enable = 0x0000;
		DHSetInfoFrameEnable(pDH, INFO_FRAME_CP_ONCE);
		DHWaitInfoFrameEnable(pDH, INFO_FRAME_CP_ONCE);
		// enable repeated CPs and all previous info frames
		DHSetInfoFrameEnable(pDH, enable | INFO_FRAME_CP_ENABLE);
	}
	
	if (pDH->CPDesired) {
		// set start time for HDCP init delay
		pDH->IntegrityLastCheck = RMGetTimeInMicroSeconds();
	} else {
		// un-mute
		RMDBGLOG((LOCALDBG, "Enabling UNENCRYPTED output, as requested by application\n"));
		if (RMFAILED(DHMuteOutput(pDH, FALSE))) {  // unmute
			RMDBGLOG((ENABLE, "Failed to unmute output!\n"));
		}
	}
	
	return err;
}

RMstatus DHReEnableOutput(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	
	RMDBGLOG((LOCALDBG,"DHReEnableOutput()\n")) ;
	
	CHECK_pDH("DHReEnableOutput");
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHReEnableOutput with unitialize device!"));
		return RM_ERROR;
	}
	
	if (pDH->state != DH_disabled) {
		RMDBGLOG((LOCALDBG, "Call to DHReEnableOutput with already enabled output."));
		return RM_OK;
	} else if (pDH->GuardTMDS) {
		RMDBGLOG((LOCALDBG, "Deferred TMDS enable due to unstable clock.\n"));
		pDH->RequestTMDS = TRUE;
		return RM_OK;
	} else switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		// make sure to send Mute
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, 0x00);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xDF, 0x01);  // set AVMute flag
		// Enable TMDS
		err = DHEnableOutput_SiI(pDH);
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		// re-enable audio, if any
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x14, pDH->audio_mode);
		break;
	case DH_ANX9030:
		// make sure to send Mute
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xC0, 0x00);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xCC, 0x01);  // set AVMute flag
		// Enable TMDS
		err = DHEnableOutput_ANX(pDH);
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		// re-enable audio, if any
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x51, pDH->audio_mode);
		break;
    // 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		// make sure to send Mute
		DH_Set_CAT6611_AVMute(pDH) ; 
		// Enable TMDS
		err = DHEnableOutput_CAT(pDH);
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		// re-enable audio, if any
		{
		    // to do, how to re-enable audio , need to think.
		}
		
		break;
    //~jj_tseng@chipadvanced.com 2007/03/22
	default:
		err = DHEnableOutput(pDH);
	}
	
	if (pDH->part_caps.HDMI) {
		RMuint32 enable = pDH->info_frame_enable;
		
		// Send one CP only, no other InfoFrames, and wait until it has been sent
		pDH->info_frame_enable = 0x0000;
		DHSetInfoFrameEnable(pDH, INFO_FRAME_CP_ONCE);
		DHWaitInfoFrameEnable(pDH, INFO_FRAME_CP_ONCE);
		// enable repeated CPs and all previous info frames
		DHSetInfoFrameEnable(pDH, enable | INFO_FRAME_CP_ENABLE);
	}
	
	if (pDH->CPDesired) {
		// set start time for HDCP init delay
		pDH->IntegrityLastCheck = RMGetTimeInMicroSeconds();
	} else {
		// un-mute
		RMDBGLOG((LOCALDBG, "Enabling UNENCRYPTED output, as requested by application\n"));
		if (RMFAILED(DHMuteOutput(pDH, FALSE))) {  // unmute
			RMDBGLOG((ENABLE, "Failed to unmute output!\n"));
		}
	}
	
	return err;
}

static RMstatus DHDisableOutput_SiI(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 reg;
	
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &reg);
	if (RMFAILED(err)) reg = init_SiI164[0][1];
	RMinsShiftBool(&reg, FALSE, 0);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Failed to disable SII64 DVI output, %s\n", RMstatusToString(err)));
	}
	
	return err;
}

static RMstatus DHDisableOutput_ANX(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 reg;
	
	// disable TMDS clock
    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x99, &reg);
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x99, reg & (~0x80));
	// power down chip
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &reg);
	if (RMFAILED(err)) reg = init_ANX9030[3][1];
	RMinsShiftBool(&reg, FALSE, 0);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x07, reg);
	
	return err;
}

// 2007/03/22 Added by jj_tseng@chipadvanced
static RMstatus DHDisableOutput_CAT(struct DH_control *pDH)
{
	RMstatus err = RM_OK ;
//	RMuint8 reg;
	
	RMDBGLOG((LOCALDBG,"DHDisableOutput_CAT()\n")) ;
	// disable TMDS clock
    err = DHReset_CAT6611(pDH) ;
	return err;
}
//~jj_tseng@chipadvanced.com 2007/03/22

RMstatus DHDisableOutput(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint32 enable;
	
	CHECK_pDH("DHDisableOutput");
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHDisableOutput with unitialize device!\n"));
		return RM_ERROR;
	}
	
	if (pDH->state == DH_disabled) {
		RMDBGLOG((LOCALDBG, "Call to DHDisableOutput with already disabled output.\n"));
		return RM_OK;
	}
	
	RMDBGLOG((LOCALDBG, "Disabling DVI/HDMI outputs\n"));
	
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		// Perform organized shut-down of HDMI output:
		// Mute
		DHMuteOutput(pDH, TRUE);
		
		// Disable all but GCP info frames, don't clear pDH->info_frame_enable
		enable = pDH->info_frame_enable;
		pDH->info_frame_enable = 0x0000;
		DHSetInfoFrameEnable(pDH, INFO_FRAME_CP_ENABLE);
		pDH->info_frame_enable = enable;
		
		// Disable audio output
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x14, pDH->audio_mode & 0xFE);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, 0x02);  // flush audio FIFO
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, 0x00);
		if (pDH->state == DH_enabled_encrypted) {
			// Disable encryption
			DHDisableEncryption_SiI(pDH);
			// Wait for ENC_ON bit, up to 50 msec
			{
				RMuint8 data;
				RMuint64 t0, t1;
				
				t0 = RMGetTimeInMicroSeconds();
				do {
					err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &data);
					if (RMFAILED(err)) data = 0x40;  // time out on error
					t1 = RMGetTimeInMicroSeconds();
				} while ((RMCyclesElapsed64(t0, t1) < 50000) && (data & 0x40));
				if (data & 0x40) {
					RMDBGLOG((ENABLE, "Encryption still ON after 50 msec!\n"));
				} else {
					if (! manutest) fprintf(stderr, "[HDMI] HDCP is now disabled.\n");
				}
			}
		} else {
			// Wait 50 mSec until audio fifo is flushed
			RMMicroSecondSleep(50 * 1000);
		}
		// Disable remaining GCP info frames
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, 0x00);
		// no break;
	case DH_siI164:
	case DH_siI170:
		// Turn off TMDS
		err = DHDisableOutput_SiI(pDH);
		break;
	case DH_ANX9030:
		// TODO  shut down HDMI cleanly before turning off TMDS
		err = DHDisableOutput_ANX(pDH);
		break;
    // 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		// TODO  shut down HDMI cleanly before turning off TMDS
		err = DHDisableOutput_CAT(pDH);
		break;
    //~jj_tseng@chipadvanced.com 2007/03/22
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	pDH->state = DH_disabled;
	RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: DISABLED/NOT ENCRYPTED\n"));
	return err;
}

RMstatus DHSoftReset(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 reg;
	
	RMDBGLOG((LOCALDBG,"DHSoftReset()\n")) ;
	CHECK_pDH("DHSoftReset");
	
	if (pDH->state == DH_enabled_repeater_wait) {
		RMDBGLOG((ENABLE, "Delay SoftReset until Repeater is authenticated\n"));
		return RM_OK;  // Don't interrupt repeater handshake
	}
	
	switch(pDH->part) {
	case DH_siI164:
	case DH_siI170:
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x05, &reg);
		if (RMFAILED(err)) reg = 0;
		reg |= 1;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, reg);
		reg &= ~1;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, reg);
		// intr bits are set after soft reset, clear them all to avoid trouble
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x71, 0xFF);
		if (pDH->part == DH_siI9030) {
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x72, 0x7F);
		} else {
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x72, 0xFF);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x73, 0xFF);
		}
		pDH->IgnoreStable = TRUE;
		// Re-apply info frame enable, enable state of info frames is lost during soft reset
		DHGetInfoFrameEnable(pDH, &(pDH->info_frame_enable));
		DHSetInfoFrameEnable(pDH, 0x0000);
		break;
	case DH_ANX9030:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x05, &reg);
		if (RMFAILED(err)) reg = 0;
		reg |= 1;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, reg);
		reg &= ~1;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, reg);
		pDH->IgnoreStable = TRUE;
		// Re-apply info frame enable, enable state of info frames is lost during soft reset
		DHGetInfoFrameEnable(pDH, &(pDH->info_frame_enable));
		DHSetInfoFrameEnable(pDH, 0x0000);
		break;
    // 2007/03/22 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		pDH->IgnoreStable = TRUE;
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &reg);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, reg | 0xC); // reset the audio and video circuit.
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, reg); // resume reset reg.
		if( !(reg & 8) )
		{
		    // if the original AFE is ready, Fire AFE
		    DHFireAFE_CAT6611(pDH) ;
		}
        
		// Re-apply info frame enable, enable state of info frames is lost during soft reset
		DHGetInfoFrameEnable(pDH, &(pDH->info_frame_enable));
		DHSetInfoFrameEnable(pDH, 0x0000);
		break;
    //~jj_tseng@chipadvanced.com 2007/03/22
	default:
		RMDBGLOG((ENABLE, "Unknown part, can not perform software reset!\n"));
		err = RM_ERROR;
	}
	return err;
}

// Hard reset of the chip
RMstatus DHReset(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	
	CHECK_pDH("DHReset");
	
	RMDBGLOG((LOCALDBG,"DHReset()\n")) ;
	
	if (! pDH->part_caps.GPIOReset) {
		return RM_OK;
	}
	
	if (pDH->gpio_reset) {
		// Re-set
		if (pDH->gpio_reset != 255) {
			err = DHReset_SiI164(pDH);
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "ERROR! Reset of transmitter failed!\n"));
				return err;
			}
		} else if (pDH->part == DH_siI170) {
			if (! manutest) {
				fprintf(stderr, "[HDMI] FATAL! Reset of transmitter failed, wrong GPIO!\n");
				fprintf(stderr, "[HDMI] Don't force internal transmitter to be SiI170 chip, or set reset_GPIO in DHOpenChip()\n");
			}
			return RM_ERROR;
		}
		
		// Re-init
		DHInitChip(pDH);
		// Re-enable
		err = DHEnableOutput(pDH);
	} else if (pDH->part_caps.HDCP) {
		if (! manutest) {
			fprintf(stderr, "[HDMI] FATAL! Reset of HDCP transmitter failed, no GPIO set!\n");
			fprintf(stderr, "[HDMI] Set reset_GPIO in DHOpenChip()!\n");
		}
		return RM_ERROR;
	}
	
	return err;
}

static RMstatus DHDDCBlockReadSegment_direct(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 i2cSegmentPtr, 
	RMuint32 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMstatus err = RM_OK;
	RMuint8 segment, offset;
	RMuint32 i, n, chunksize;
	RMuint8 *ptr;
	
	if (pDH->i2c_rx.dev.DevAddr != i2cAddr) {
		pi2c_prev_dev = NULL; // Force setting new i2c settings in next read/write in i2c
	}
	pDH->i2c_rx.dev.DevAddr = i2cAddr;
	
	RMDBGLOG((I2CDBG, "Reading segmented data from device 0x%02X/0x%02X, 0x%04X bytes from 0x%04X\n", 
		i2cAddr, i2cSegmentPtr, NbBytes, RegAddr));
	ptr = pData;
	n = NbBytes;
	while (n) {
		segment = RegAddr >> 8;
		offset = RegAddr & 0xFF;
		chunksize = 0x100 - offset;
		if (chunksize > n) chunksize = n;
		RMDBGLOG((I2CDBG, "  reading segment %d , 0x%02X bytes from 0x%02X\n", segment, chunksize, offset));
		err = DH_i2c_read_data_segment(pDH->pRUA, &(pDH->i2c_rx), offset, ptr, chunksize, i2cSegmentPtr, segment);
		if (RMFAILED(err) && (segment <= 1)) {  // segment access failed, try normal read on 0xA0/0xA1 or 0xA2/0xA3 for segment 0 and 1
			if (segment == 1) {
				pi2c_prev_dev = NULL;
				(pDH->i2c_rx).dev.DevAddr = i2cAddr + 2;
			}
			err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_rx), offset, ptr, chunksize);
			if (segment == 1) {
				pi2c_prev_dev = NULL;
				(pDH->i2c_rx).dev.DevAddr = i2cAddr;
			}
		}
		if (RMFAILED(err)) {
			RMDBGLOG((ENABLE, "Failed to read data from segment %d:0x%02X\n", segment, offset));
			break;
		}
		ptr += chunksize;
		n -= chunksize;
		RegAddr += chunksize;
	}
	
	if (RMSUCCEEDED(err)) {
		for (i = 0; i < NbBytes - n; i++) {
			if (i % 8 == 0) RMDBGPRINT((I2CDBG, "Data[%02X]: ", i));
			RMDBGPRINT((I2CDBG, "%02X ", pData[i]));
			if (i % 8 == 7) RMDBGPRINT((I2CDBG, "\n"));
		}
		RMDBGPRINT((I2CDBG, "\n"));
	}
	
	return err;
}

static RMstatus DHAutoRiCheck_SiI(
	struct DH_control *pDH, 
	RMbool RiEnable)
{
	RMstatus err;
	RMuint8 reg;
	
	// Ri mismatch feature of 9x34
	switch (pDH->part) {
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x27, &reg);
		if (RMSUCCEEDED(err)) {
			reg = RiEnable ? (reg | 0x01) : (reg & ~0x01);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x27, reg);
		}
		break;
	default:
		break;
	}
	
	return RM_OK;
}

static RMstatus DHDDCCheckDisable_SiI(
	struct DH_control *pDH, 
	RMbool *pRiEnable)
{
	RMstatus err;
	RMuint8 reg;
	
	*pRiEnable = FALSE;
	
	// Ri mismatch feature of 9x34 active?
	switch (pDH->part) {
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x27, &reg);
		if (RMSUCCEEDED(err)) {
			*pRiEnable = (reg & 0x01) ? TRUE : FALSE;
		}
		if (*pRiEnable) {
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x27, reg & ~0x01);
			do {
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x26, &reg);
			} while (RMSUCCEEDED(err) && (reg & 0x01));
		}
		break;
	default:
		break;
	}
	
	return RM_OK;
}

// i2c read via the siI9030
static RMstatus DHDDCBlockReadSegment_SiI(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 i2cSegmentPtr, 
	RMuint32 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint8 index;
	RMstatus err = RM_OK;
	RMuint8 segment, offset;
	RMuint32 chunksize;
	RMuint8 ddc_status;
	RMuint64 t0, t1;
	RMbool ri_en;
	
	RMDBGLOG((I2CDBG, "BlockReadSegment siI9030, %ld bytes from 0x%04X:0x%04X\n", NbBytes, i2cAddr, RegAddr));
	
	if (i2cSegmentPtr != DDC_EDID_SEG) {
		RMDBGLOG((ENABLE, "Call to DHDDCBlockReadSegment with invalid segment pointer\n"));
		return RM_INVALID_PARAMETER;
	}
	
	RMDBGLOG((I2CDBG, "Reading segmented data from device 0x%02X/0x%02X, 0x%04X bytes from 0x%04X\n", 
		i2cAddr, i2cSegmentPtr, NbBytes, RegAddr));
	DHDDCCheckDisable_SiI(pDH, &ri_en);
	while (NbBytes) {
		segment = RegAddr >> 8;
		offset = RegAddr & 0xFF;
		chunksize = 0x100 - offset;
		if (chunksize > NbBytes) chunksize = NbBytes;
		if (chunksize > MAX_DDC_FIFO) chunksize = MAX_DDC_FIFO;
		RMDBGLOG((I2CDBG, "  reading segment %d , 0x%02X bytes from 0x%02X\n", segment, chunksize, offset));
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xED, i2cAddr);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xEE, segment);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xEF, offset);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF0, chunksize & 0xFF); // Byte count
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF1, chunksize >> 8);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_CLEAR_FIFO);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ENHANCED_RD);
		if (RMFAILED(err)) break;
		for (index = 0; index < chunksize; index++) {
			t0 = RMGetTimeInMicroSeconds();
			do {
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF2, &ddc_status);
				t1 = RMGetTimeInMicroSeconds();
				if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) err = RM_TIMEOUT;
			} while (RMSUCCEEDED(err) && (ddc_status & 0x04));  // fifo_empty?
			if (RMFAILED(err)) {
				DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
				break;
			}
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF4, pData++);
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Could not read DDC block in DHDDCBlockReadSegment! %s\n", RMstatusToString(err)));
				DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
				break;
			}
		}
		NbBytes -= chunksize;
		RegAddr += chunksize;
		if (ddc_status & 0x10) {
			RMDBGLOG((ENABLE, "DDC problem: transfer still in progress after byte %u\n", RegAddr));
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
		}
	}
	if (ri_en) DHAutoRiCheck_SiI(pDH, ri_en);
	
	return err;
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHDDCBlockReadSegment_CAT(
	struct DH_control *pDH,
	RMuint8 i2cAddr,
	RMuint8 i2cSegmentPtr,
	RMuint32 RegAddr,
	RMuint8 *pData,
	RMuint32 NbBytes)
{
	RMstatus err = RM_OK;

    RMuint8 data ;
	RMuint8 *pBuff = pData ;
    RMuint8 segment, offset ;
    RMuint32 RemainedSize, ReadSize ;
    RMuint32 i ;

	RMDBGLOG((LOCALDBG, "DHDDCBlockReadSegment_CAT, %ld bytes from 0x%04X:0x%04X\n", NbBytes, i2cAddr, RegAddr));
	if (pDH == NULL) {
		RMDBGLOG((LOCALDBG, "Call to DHDDCBlockReadSegment with invalid DH structure\n"));
		return RM_INVALID_PARAMETER;
	}

	if (pData == NULL) {
		RMDBGLOG((LOCALDBG, "Call to DHDDCBlockReadSegment with invalid buffer\n"));
		return RM_INVALID_PARAMETER;
	}

	if (i2cSegmentPtr != 0x60) {
		RMDBGLOG((LOCALDBG, "Call to DHDDCBlockReadSegment with invalid segment pointer\n"));
		return RM_INVALID_PARAMETER;
	}

	RMDBGLOG((LOCALDBG, "Reading segmented data from device 0x%02X/0x%02X, 0x%04X bytes from 0x%04X\n",
		i2cAddr, i2cSegmentPtr, NbBytes, RegAddr));
    err = DHAbortDDC_CAT6611(pDH) ; 
    RemainedSize = NbBytes ;

    while(RemainedSize>0)
    {
        offset = RegAddr & 0xFF ;
        segment = (RegAddr>>8)&0xFF ;
        ReadSize = (RemainedSize>32)?32:RemainedSize ;

		DH_Clear_CAT6611DDCFIFO(pDH);
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x10, 1); // Read Rx DDC/EDID, with PC/host.
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x11, i2cAddr); // I2c PC DDC slave address
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x12, offset); // register address
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x14, segment); // EDID segment
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x13, ReadSize ); // Request byte.
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x15, 3 ); // command, EDID read

    	while(1)
    	{
    		RMMicroSecondSleep(10*1000); // Needs a minimum delay. We may need to increase it
    		err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x16, &data ) ;

    		if( data & (1<<7))
    		{
    		    // DDC done.
    		    break ;
    		}

    		if( data & ((1<<3)|(1<<5)))
    		{
    		    RMDBGLOG((LOCALDBG, "BlockReadSegment CAT6611 fail, ddc status = %02X\n",data));
    		    return RM_ERROR ;
    		}
    	}

    	for( i = 0 ;i < ReadSize ; i++)
    	{
    		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x17, pBuff++); // read EDID from RDDC_ReadFIFO
    		if (RMFAILED(err)) {
    			RMDBGLOG((LOCALDBG, "Could not read DDC block in DHDDCBlockReadSegment! %s\n", RMstatusToString(err)));
    			return err;
    		}
    	}


        RemainedSize -= ReadSize ;
        RegAddr += ReadSize ;
    }
    
    #if 0
    ///////////////////////////////////////////////////////////////////////////
    // Dump EDID data by Readback
    ///////////////////////////////////////////////////////////////////////////
	for( i = 0 ; i < NbBytes ; i++ )
	{
		RMDBGPRINT((LOCALDBG,"%02X ",pData[i])) ;
		if(((i+1)%16) == 0)
		{
			RMDBGPRINT((LOCALDBG,"\n")) ;
		}
	}
	#endif // 0
	
	return RM_OK;
}
//~jj_tseng@chipadvanced.com 2007/03/23

// i2c read via the ANX9030
static RMstatus DHDDCBlockReadSegment_ANX(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 i2cSegmentPtr, 
	RMuint32 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint8 index, reg;
	RMstatus err = RM_OK;
	RMuint8 segment, offset;
	RMuint32 chunksize;
	RMuint8 ddc_status;
	RMuint64 t0, t1;
	
	RMDBGLOG((I2CDBG, "BlockReadSegment ANX9030, %ld bytes from 0x%04X:0x%04X\n", NbBytes, i2cAddr, RegAddr));
	
	if (i2cSegmentPtr != DDC_EDID_SEG) {
		RMDBGLOG((ENABLE, "Call to DHDDCBlockReadSegment with invalid segment pointer\n"));
		return RM_INVALID_PARAMETER;
	}
	
	RMDBGLOG((I2CDBG, "Reading segmented data from device 0x%02X/0x%02X, 0x%04X bytes from 0x%04X\n", 
		i2cAddr, i2cSegmentPtr, NbBytes, RegAddr));
	
	// Reset DDC
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &reg);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg | 0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg & ~0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_RESET);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_CLEAR_FIFO);
	
	while (NbBytes) {
		segment = RegAddr >> 8;
		offset = RegAddr & 0xFF;
		chunksize = 0x100 - offset;
		if (chunksize > NbBytes) chunksize = NbBytes;
		if (chunksize > MAX_DDC_FIFO) chunksize = MAX_DDC_FIFO;
		RMDBGLOG((I2CDBG, "  reading segment %d , 0x%02X bytes from 0x%02X\n", segment, chunksize, offset));
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x80, i2cAddr);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x81, segment);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x82, offset);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x84, chunksize & 0xFF); // Byte count
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x85, chunksize >> 8);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_CLEAR_FIFO);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ENHANCED_RD);
		if (RMFAILED(err)) return err;
		for (index = 0; index < chunksize; index++) {
			t0 = RMGetTimeInMicroSeconds();
			do {
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x86, &ddc_status);
				t1 = RMGetTimeInMicroSeconds();
				if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) err = RM_TIMEOUT;
			} while (RMSUCCEEDED(err) && (ddc_status & 0x10));  // fifo_empty?
			if (RMFAILED(err)) {
				DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
				return err;
			}
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x87, pData++);
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Could not read DDC block in DHDDCBlockReadSegment! %s\n", RMstatusToString(err)));
				DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
				return err;
			}
		}
		NbBytes -= chunksize;
		RegAddr += chunksize;
		if (ddc_status & 0x01) {
			RMDBGLOG((ENABLE, "DDC problem: transfer still in progress after byte %u\n", RegAddr));
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
		}
	}
	
	return err;
}

RMstatus DHDDCBlockReadSegment(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 i2cSegmentPtr, 
	RMuint32 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMstatus err = RM_ERROR;
	
	CHECK_pDH("DHDDCBlockReadSegment");
	CHECK_PTR("DHDDCBlockReadSegment", pData);
	
	RMDBGLOG((I2CDBG, "BlockReadSegment, %ld bytes from 0x%04X:0x%04X\n", NbBytes, i2cAddr, RegAddr));
	if (! pDH->part_caps.DDC) {
		err = DHDDCBlockReadSegment_direct(pDH, i2cAddr, i2cSegmentPtr, RegAddr, pData, NbBytes);
	} else {
		switch(pDH->part) {
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			err = DHDDCBlockReadSegment_SiI(pDH, i2cAddr, i2cSegmentPtr, RegAddr, pData, NbBytes);
			break;
		case DH_ANX9030:
			err = DHDDCBlockReadSegment_ANX(pDH, i2cAddr, i2cSegmentPtr, RegAddr, pData, NbBytes);
			break;
        // 2007/03/23 Added by jj_tseng@chipadvanced
		case DH_CAT6611:
			err = DHDDCBlockReadSegment_CAT(pDH, i2cAddr, i2cSegmentPtr, RegAddr, pData, NbBytes);
			break;
        //~jj_tseng@chipadvanced.com 2007/03/23
		default:
			RMDBGLOG((ENABLE, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		}
	}
	return err;
}

static RMstatus DHDDCBlockWriteSegment_direct(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 i2cSegmentPtr, 
	RMuint32 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMstatus err = RM_OK;
	RMuint8 segment, offset;
	RMuint32 i, n, chunksize;
	RMuint8 *ptr;
	
	if (pDH->i2c_rx.dev.DevAddr != i2cAddr) {
		pi2c_prev_dev = NULL; // Force setting new i2c settings in next read/write in i2c
	}
	pDH->i2c_rx.dev.DevAddr = i2cAddr;
	
	RMDBGLOG((I2CDBG, "Writing segmented data to device 0x%02X/0x%02X, 0x%04X bytes to 0x%04X\n", 
		i2cAddr, i2cSegmentPtr, NbBytes, RegAddr));
	ptr = pData;
	n = NbBytes;
	while (n) {
		segment = RegAddr >> 8;
		offset = RegAddr & 0xFF;
		chunksize = 0x100 - offset;
		if (chunksize > n) chunksize = n;
		RMDBGLOG((I2CDBG, "  writing segment %d , 0x%02X bytes to 0x%02X\n", segment, chunksize, offset));
		err = DH_i2c_write_data_segment(pDH->pRUA, &(pDH->i2c_rx), offset, ptr, chunksize, i2cSegmentPtr, segment);
		if (RMFAILED(err) && (segment <= 1)) {  // segment access failed, try normal read on 0xA0/0xA1 or 0xA2/0xA3 for segment 0 and 1
			if (segment == 1) {
				pi2c_prev_dev = NULL;
				pDH->i2c_rx.dev.DevAddr = i2cAddr + 2;
			}
			//err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_rx), offset, ptr, chunksize);
			for (i = 0; i < chunksize; i++) {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_rx), offset + i, ptr[i]);
				if (RMFAILED(err)) {
					RMDBGLOG((ENABLE, "Error writing 0x%02X to %02X! %s\n", ptr[i], offset + i, RMstatusToString(err)));
				}
				RMMicroSecondSleep(100000);  // wait for EEPROM
			}
			if (segment == 1) {
				pi2c_prev_dev = NULL;
				pDH->i2c_rx.dev.DevAddr = i2cAddr;
			}
		}
		if (RMFAILED(err)) {
			RMDBGLOG((ENABLE, "Failed to write data to segment %d:0x%02X\n", segment, offset));
			break;
		}
		ptr += chunksize;
		n -= chunksize;
		RegAddr += chunksize;
	}
	
	if (RMSUCCEEDED(err)) {
		for (i = 0; i < NbBytes - n; i++) {
			if (i % 8 == 0) RMDBGPRINT((I2CDBG, "Data[%02X]: ", i));
			RMDBGPRINT((I2CDBG, "%02X ", pData[i]));
			if (i % 8 == 7) RMDBGPRINT((I2CDBG, "\n"));
		}
		RMDBGPRINT((I2CDBG, "\n"));
	}
	
	return err;
}

// i2c read via the siI9030
static RMstatus DHDDCBlockWriteSegment_SiI(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 i2cSegmentPtr, 
	RMuint32 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint8 index;
	RMstatus err = RM_OK;
	RMuint32 segment, offset;
	RMuint32 chunksize;
	RMuint8 ddc_status;
	RMuint64 t0, t1;
	RMbool ri_en;
	
	RMDBGLOG((I2CDBG, "BlockWriteSegment siI9030, %ld bytes to 0x%04X:0x%04X\n", NbBytes, i2cAddr, RegAddr));
	
	if (i2cSegmentPtr != DDC_EDID_SEG) {
		RMDBGLOG((ENABLE, "Call to DHDDCBlockWriteSegment with invalid segment pointer\n"));
		return RM_INVALID_PARAMETER;
	}
	
	RMDBGLOG((I2CDBG, "Writing segmented data to device 0x%02X/0x%02X, 0x%04X bytes to 0x%04X\n", 
		i2cAddr, i2cSegmentPtr, NbBytes, RegAddr));
	DHDDCCheckDisable_SiI(pDH, &ri_en);
	while (NbBytes) {
		segment = RegAddr >> 8;
		offset = RegAddr & 0xFF;
		chunksize = 0x100 - offset;
		if (chunksize > NbBytes) chunksize = NbBytes;
		if (chunksize > MAX_DDC_FIFO) chunksize = MAX_DDC_FIFO;
		RMDBGLOG((I2CDBG, "  writing segment %d , 0x%02X bytes to 0x%02X\n", segment, chunksize, offset));
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF2, &ddc_status);
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) err = RM_TIMEOUT;
		} while (RMSUCCEEDED(err) && (ddc_status & 0x10));  //in_progress?
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xED, i2cAddr);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xEE, segment);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xEF, offset);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF0, chunksize & 0xFF); // Byte count
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF1, chunksize >> 8);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_CLEAR_FIFO);
		if (RMFAILED(err)) break;
		for (index = 0; index < chunksize; index++) {
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF4, *(pData++));
			if (RMFAILED(err)) break;
		}
		if (RMFAILED(err)) {
			RMDBGLOG((ENABLE, "Could not write DDC block in DHDDCBlockWriteSegment! %s\n", RMstatusToString(err)));
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
			break;
		}
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_SEQ_WR);
		if (RMFAILED(err)) {
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
			break;
		}
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF2, &ddc_status);
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) err = RM_TIMEOUT;
		} while (RMSUCCEEDED(err) && (ddc_status & 0x10) && !(ddc_status & 0x20));  //in_progress?
		if (!(ddc_status & 0x10) && (ddc_status & 0x20)) {
			RMDBGLOG((ENABLE, "ACK error while writing DDC data!\n"));
			err = RM_ERROR;
		}
		if (RMFAILED(err)) {
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
			break;
		}
		NbBytes -= chunksize;
		RegAddr += chunksize;
	}
	if (ri_en) DHAutoRiCheck_SiI(pDH, ri_en);
	
	return err;
}

// i2c read via the ANX9030
static RMstatus DHDDCBlockWriteSegment_ANX(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 i2cSegmentPtr, 
	RMuint32 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint8 index, reg;
	RMstatus err = RM_OK;
	RMuint32 segment, offset;
	RMuint32 chunksize;
	RMuint8 ddc_status;
	RMuint64 t0, t1;
	
	RMDBGLOG((I2CDBG, "BlockWriteSegment ANX9030, %ld bytes to 0x%04X:0x%04X\n", NbBytes, i2cAddr, RegAddr));
	
	if (i2cSegmentPtr != DDC_EDID_SEG) {
		RMDBGLOG((ENABLE, "Call to DHDDCBlockWriteSegment with invalid segment pointer\n"));
		return RM_INVALID_PARAMETER;
	}
	
	RMDBGLOG((I2CDBG, "Writing segmented data to device 0x%02X/0x%02X, 0x%04X bytes to 0x%04X\n", 
		i2cAddr, i2cSegmentPtr, NbBytes, RegAddr));
	
	// Reset DDC
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &reg);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg | 0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg & ~0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_RESET);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_CLEAR_FIFO);
	
	while (NbBytes) {
		segment = RegAddr >> 8;
		offset = RegAddr & 0xFF;
		chunksize = 0x100 - offset;
		if (chunksize > NbBytes) chunksize = NbBytes;
		if (chunksize > MAX_DDC_FIFO) chunksize = MAX_DDC_FIFO;
		RMDBGLOG((I2CDBG, "  writing segment %d , 0x%02X bytes to 0x%02X\n", segment, chunksize, offset));
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x86, &ddc_status);
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) err = RM_TIMEOUT;
		} while (RMSUCCEEDED(err) && (ddc_status & 0x01));  //in_progress?
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x80, i2cAddr);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x81, segment);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x82, offset);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x84, chunksize & 0xFF); // Byte count
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x85, chunksize >> 8);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_CLEAR_FIFO);
		if (RMFAILED(err)) return err;
		for (index = 0; index < chunksize; index++) {
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x87, *(pData++));
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Could not write DDC block in DHDDCBlockWriteSegment! %s\n", RMstatusToString(err)));
				return err;
			}
		}
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_SEQ_WR);
		if (RMFAILED(err)) return err;
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x86, &ddc_status);
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) err = RM_TIMEOUT;
		} while (RMSUCCEEDED(err) && (ddc_status & 0x01) && !(ddc_status & 0x08));  //in_progress?
		if (!(ddc_status & 0x01) && (ddc_status & 0x08)) {
			RMDBGLOG((ENABLE, "ACK error while writing DDC data!\n"));
			err = RM_ERROR;
		}
		if (RMFAILED(err)) {
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
			return err;
		}
		NbBytes -= chunksize;
		RegAddr += chunksize;
	}
	
	return err;
}

RMstatus DHDDCBlockWriteSegment(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 i2cSegmentPtr, 
	RMuint32 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMstatus err = RM_ERROR;
	
	CHECK_pDH("DHDDCBlockWriteSegment");
	CHECK_PTR("DHDDCBlockWriteSegment", pData);
	
	RMDBGLOG((I2CDBG, "BlockWriteSegment, %ld bytes to 0x%04X:0x%04X\n", NbBytes, i2cAddr, RegAddr));
	if (! pDH->part_caps.DDC) {
		err = DHDDCBlockWriteSegment_direct(pDH, i2cAddr, i2cSegmentPtr, RegAddr, pData, NbBytes);
	} else {
		RMbool burst_write = FALSE;  // TRUE: write all data at once, FALSE: wait 100 mSec inbetween bytes
		RMuint32 i;
		RMuint8 d;
		RMstatus e;
		
		switch(pDH->part) {
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			if (burst_write) {
				err = DHDDCBlockWriteSegment_SiI(pDH, i2cAddr, i2cSegmentPtr, RegAddr, pData, NbBytes);
			} else {
				err = RM_OK;
				for (i = 0; i < NbBytes; i++) {
					d = pData[i];
					e = DHDDCBlockWriteSegment_SiI(pDH, i2cAddr, i2cSegmentPtr, RegAddr + i, &d, 1);
					if (RMFAILED(e)) err = e;
					RMMicroSecondSleep(100000);  // wait for EEPROM
				}
			}
			break;
		case DH_ANX9030:
			if (burst_write) {
				err = DHDDCBlockWriteSegment_ANX(pDH, i2cAddr, i2cSegmentPtr, RegAddr, pData, NbBytes);
			} else {
				err = RM_OK;
				for (i = 0; i < NbBytes; i++) {
					d = pData[i];
					e = DHDDCBlockWriteSegment_ANX(pDH, i2cAddr, i2cSegmentPtr, RegAddr + i, &d, 1);
					if (RMFAILED(e)) err = e;
					RMMicroSecondSleep(100000);  // wait for EEPROM
				}
			}
			break;
        // 2007/03/23 Added by jj_tseng@chipadvanced
        case DH_CAT6611:
			RMDBGLOG((ENABLE, "CAT6611 does not provide EDID write function!!!\n"));
            break ;
        //~jj_tseng@chipadvanced.com 2007/03/23
			
		default:
			RMDBGLOG((ENABLE, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		}
	}
	return err;
}

// direct i2c read
static RMstatus DHDDCBlockRead_direct(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint32 index;
	RMstatus err;
	
	RMDBGLOG((I2CDBG, "BlockRead siI164, %ld bytes from 0x%02X:0x%02X\n", NbBytes, i2cAddr, RegAddr));
	if (pDH->i2c_rx.dev.DevAddr != i2cAddr) {
		pi2c_prev_dev = NULL; // Force setting new i2c settings in next read/write in i2c
	}
	pDH->i2c_rx.dev.DevAddr = i2cAddr;
	
	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_rx), RegAddr, pData, NbBytes);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Error reading %d bytes from %02X:%02X! %s\n", NbBytes, i2cAddr + 1, RegAddr, RMstatusToString(err)));
	} else {
		for (index = 0; index < NbBytes; index++) {
			if (index % 8 == 0) RMDBGPRINT((I2CDBG, "Data[%02X]: ", index));
			RMDBGPRINT((I2CDBG, "%02X ", pData[index]));
			if (index % 8 == 7) RMDBGPRINT((I2CDBG, "\n"));
		}
		RMDBGPRINT((I2CDBG, "\n"));
	}
	
	return err;
}

// i2c read via the siI9030
static RMstatus DHDDCBlockRead_SiI(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint8 index;
	RMstatus err = RM_OK;
	RMuint8 xfer_size;
	RMuint8 ddc_status;
	RMbool ri_en;
	
	RMDBGLOG((I2CDBG, "BlockRead siI9030, %ld bytes from 0x%02X:0x%02X\n", NbBytes, i2cAddr, RegAddr));
	
	if (NbBytes > 1023) {
		RMDBGLOG((ENABLE, "Transfer too large for SiI9030: %lu bytes (1023 max.)\n", NbBytes));
		return RM_INVALID_PARAMETER;
	}
	
	DHDDCCheckDisable_SiI(pDH, &ri_en);
	do {
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xED, i2cAddr);  // Device
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xEE, 0x00);  // Segment
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xEF, RegAddr);  // Offset
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF0, NbBytes & 0xFF); // Byte count
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF1, NbBytes >> 8);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_CLEAR_FIFO);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_SEQ_RD);
		if (RMFAILED(err)) break;
		
		while (NbBytes) {
			RMuint64 t0, t1;
			t0 = RMGetTimeInMicroSeconds();
			do {
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF2, &ddc_status);
				if (RMFAILED(err)) ddc_status = 0x10;  // dummy status, try again
				t1 = RMGetTimeInMicroSeconds();
				if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) {
					RMDBGLOG((ENABLE, "Timeout waiting for DDC transfer status!\n"));
					DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
					err = RM_TIMEOUT;
					break;
				}
			} while ((ddc_status & 0x18) == 0x10);  // until fifo full or transfer finished
			if (RMFAILED(err)) break;
			xfer_size = (ddc_status & 0x10) ? MAX_DDC_FIFO : NbBytes;
			for (index = 0; index < xfer_size; index++) {
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF4, pData++);
				if (RMFAILED(err)) break;
			}
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Could not read byte of DDC block in DHDDCBlockRead! %s\n", RMstatusToString(err)));
				DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
				break;
			}
			NbBytes -= xfer_size;
		}
	} while (0);
	if (ri_en) DHAutoRiCheck_SiI(pDH, ri_en);
	
	return err;
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHDDCBlockRead_CAT(
    struct DH_control *pDH,
    RMuint8 i2cAddr,
    RMuint8 RegAddr,
    RMuint8 *pData,
    RMuint8 NbBytes)
{
	// RMuint8 index;
	RMstatus err = RM_OK;

    RMuint8 data ;
    RMuint8 segment, offset, DDCCommand ;
    RMuint32 RemainedSize, ReadSize ;
    RMuint8 *pBuff = pData ;
    RMuint32 i;


	RMDBGLOG((ENABLE,"DHDDCBlockRead_CAT(0x%x,0x%x,%d bytes)\n",i2cAddr,RegAddr,NbBytes));
    err = DHAbortDDC_CAT6611(pDH) ; 
    RemainedSize = NbBytes ;
    DDCCommand = (i2cAddr == 0xA0)?0x03:0x00 ;
    
    while(RemainedSize>0)
    {
        offset = RegAddr & 0xFF ;
        segment = (RegAddr>>8)&0xFF ;
        ReadSize = (RemainedSize>32)?32:RemainedSize ;


		DH_Clear_CAT6611DDCFIFO(pDH);
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x10, 1); // Read Rx DDC/EDID, with PC/host.
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x11, i2cAddr); // I2c PC DDC slave address
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x12, offset); // register address
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x14, segment); // EDID segment
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x13, ReadSize ); // Request byte.
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x15, DDCCommand ); // command, EDID read

    	while(1)
    	{
    		RMMicroSecondSleep(10*1000); // Needs a minimum delay. We may need to increase it
    		err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x16, &data ) ;

    		if( data & (1<<7))
    		{
    		    // DDC done.
    		    break ;
    		}

    		if( data & ((1<<3)|(1<<5)))
    		{
    		    RMDBGLOG((ENABLE, "BlockRead_CAT6611 fail, ddc status = %02X\n",data));
    		    return RM_ERROR ;
    		}
    	}

    	for( i = 0 ;i < ReadSize ; i++)
    	{
    		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x17, pBuff++); // read EDID from RDDC_ReadFIFO
    		if (RMFAILED(err)) {
    			RMDBGLOG((ENABLE, "Could not read DDC block in DHDDCBlockReadSegment! %s\n", RMstatusToString(err)));
    			return err;
    		}
    	}

        RemainedSize -= ReadSize ;
        RegAddr += ReadSize ;
    }

	DH_Clear_CAT6611DDCFIFO(pDH); 
    #if 0 
    ///////////////////////////////////////////////////////////
    // Dump EDID on Block Read
    ///////////////////////////////////////////////////////////
	for( i  = 0 ; i < NbBytes; i++)
	{
		if( (i+1)%16 )
		RMDBGPRINT((ENABLE,"%02X ",pData[i]));
		else
		RMDBGPRINT((ENABLE,"%02X\n",pData[i]));

	}
	DHDump_CAT6611reg(pDH);
	#endif

	return RM_OK;
}
//~jj_tseng@chipadvanced.com 2007/03/23

static RMstatus DHDDCBlockRead_ANX(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint8 index, reg;
	RMstatus err = RM_OK;
	RMuint8 xfer_size;
	RMuint8 ddc_status;
	
	RMDBGLOG((I2CDBG, "BlockRead ANX9030, %ld bytes from 0x%02X:0x%02X\n", NbBytes, i2cAddr, RegAddr));
	
	if (NbBytes > 1023) {
		RMDBGLOG((ENABLE, "Transfer too large for SiI9030: %lu bytes (1023 max.)\n", NbBytes));
		return RM_INVALID_PARAMETER;
	}
	
	// Reset DDC
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &reg);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg | 0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg & ~0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_RESET);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_CLEAR_FIFO);
	
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x80, i2cAddr);  // Device
	if (RMFAILED(err)) return err;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x81, 0x00);  // Segment
	if (RMFAILED(err)) return err;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x82, RegAddr);  // Offset
	if (RMFAILED(err)) return err;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x84, NbBytes & 0xFF); // Byte count
	if (RMFAILED(err)) return err;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x85, NbBytes >> 8);
	if (RMFAILED(err)) return err;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_CLEAR_FIFO);
	if (RMFAILED(err)) return err;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_SEQ_RD);
	if (RMFAILED(err)) return err;
	
	while (NbBytes) {
		RMuint64 t0, t1;
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x86, &ddc_status);
			if (RMFAILED(err)) ddc_status = 0x01;  // dummy status, try again
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) {
				RMDBGLOG((ENABLE, "Timeout waiting for DDC transfer status!\n"));
				DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
				return RM_TIMEOUT;
			}
		} while ((ddc_status & 0xA1) == 0x01);  // until error or fifo full or transfer finished
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x88, &xfer_size);
		if (RMFAILED(err)) {
			RMDBGLOG((ENABLE, "Could not read FIFO size of DDC block in DHDDCBlockRead! %s\n", RMstatusToString(err)));
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
			return err;
		}
		if (xfer_size == 0) {
			RMDBGLOG((ENABLE, "DDC Read FIFO is empty!\n"));
			return RM_ERROR;
		}
		for (index = 0; index < xfer_size; index++) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x87, pData++);
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Could not read byte of DDC block in DHDDCBlockRead! %s\n", RMstatusToString(err)));
				DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
				return err;
			}
		}
		NbBytes -= xfer_size;
	}
	
	return err;
}

// read data from the DCC in one burst
RMstatus DHDDCBlockRead(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes) 
{
	RMstatus err = RM_ERROR;
	
	CHECK_pDH("DHDDCBlockRead");
	CHECK_PTR("DHDDCBlockRead", pData);
	
	RMDBGLOG((I2CDBG, "BlockRead, %ld bytes from 0x%02X:0x%02X\n", NbBytes, i2cAddr, RegAddr));
	if (! pDH->part_caps.DDC) {
		err = DHDDCBlockRead_direct(pDH, i2cAddr, RegAddr, pData, NbBytes);
	} else {
		switch(pDH->part) {
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			err = DHDDCBlockRead_SiI(pDH, i2cAddr, RegAddr, pData, NbBytes);
			break;
		case DH_ANX9030:
			err = DHDDCBlockRead_ANX(pDH, i2cAddr, RegAddr, pData, NbBytes);
			break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
		case DH_CAT6611:
			err = DHDDCBlockRead_CAT(pDH, i2cAddr, RegAddr, pData, NbBytes);
			break;
    //~jj_tseng@chipadvanced.com 2007/03/23
		default:
			RMDBGLOG((ENABLE, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		}
	}
	return err;
}

// direct i2c write
static RMstatus DHDDCBlockWrite_direct(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint32 index;
	RMstatus err = RM_OK;
	
	RMDBGLOG((I2CDBG, "BlockWrite siI164, %ld bytes to 0x%02X:0x%02X\n", NbBytes, i2cAddr, RegAddr));
	if (pDH->i2c_rx.dev.DevAddr != i2cAddr) {
		pi2c_prev_dev = NULL; // Force setting new i2c settings in next read/write in i2c
	}
	pDH->i2c_rx.dev.DevAddr = i2cAddr;
	
//	err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_rx), RegAddr, pData, NbBytes);
//	if (RMFAILED(err)) {
//		RMDBGLOG((ENABLE, "Error writing %d bytes to %02X:%02X! %s\n", NbBytes, i2cAddr, RegAddr, RMstatusToString(err)));
//	}
	for (index = 0; index < NbBytes; index++) {
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_rx), RegAddr + index, pData[index]);
		if (RMFAILED(err)) {
			RMDBGLOG((ENABLE, "Error writing 0x%02X to %02X:%02X! %s\n", pData[index], i2cAddr, RegAddr + index, RMstatusToString(err)));
		}
		RMMicroSecondSleep(100000);
	}
	
	if (RMSUCCEEDED(err)) {
		for (index = 0; index < NbBytes; index++) {
			if (index % 8 == 0) RMDBGPRINT((I2CDBG, "Data[%02X]: ", index));
			RMDBGPRINT((I2CDBG, "%02X ", pData[index]));
			if (index % 8 == 7) RMDBGPRINT((I2CDBG, "\n"));
		}
		RMDBGPRINT((I2CDBG, "\n"));
	}
	
	return err;
}

static RMstatus DHDDCBlockWrite_SiI(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint8 index;
	RMstatus err = RM_OK;
	RMuint8 xfer_num;
	RMuint8 nb_xfers = NbBytes / MAX_DDC_FIFO + 1;
	RMuint8 xfer_size;
	RMuint8 ddc_status;
	RMuint64 t0, t1;
	RMbool ri_en;
	
	DHDDCCheckDisable_SiI(pDH, &ri_en);
	for (xfer_num = 0; xfer_num < nb_xfers; xfer_num++) {
		xfer_size = RMmin(NbBytes, MAX_DDC_FIFO);
		if (xfer_size == 0) break;  // done
		
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xED, i2cAddr);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xEE, 0x00);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xEF, RegAddr);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF0, xfer_size & 0xFF); // Byte count
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF1, xfer_size >> 8);
		if (RMFAILED(err)) break;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_CLEAR_FIFO);
		if (RMFAILED(err)) break;
		for (index = 0; index < xfer_size; index++) {
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF4, *pData++);
			if (RMFAILED(err)) break;
		}
		if (RMFAILED(err)) {
			RMDBGLOG((ENABLE, "Could not write DDC block in DHDDCBlockWrite_SiI9030! %s\n", RMstatusToString(err)));
			break;
		}
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_SEQ_WR);
		if (RMFAILED(err)) break;
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xF2, &ddc_status);
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) err = RM_TIMEOUT;
		} while (RMSUCCEEDED(err) && (ddc_status & 0x10) && !(ddc_status & 0x20));  //in_progress?
		if (!(ddc_status & 0x10) && (ddc_status & 0x20)) {
			RMDBGLOG((ENABLE, "ACK error while writing DDC data!\n"));
			err = RM_ERROR;
		}
		if (RMFAILED(err)) {
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xF3, DDC_MASTER_CMD_SII_ABORT);
			break;
		}
		NbBytes -= xfer_size;
		RegAddr += xfer_size;
	}
	if (ri_en) DHAutoRiCheck_SiI(pDH, ri_en);
	
	return err;
}

static RMstatus DHDDCBlockWrite_ANX(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes)
{
	RMuint8 index, reg;
	RMstatus err = RM_OK;
	RMuint8 xfer_num;
	RMuint8 nb_xfers = NbBytes / MAX_DDC_FIFO + 1;
	RMuint8 xfer_size;
	RMuint8 ddc_status;
	RMuint64 t0, t1;
	
	// Reset DDC
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &reg);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg | 0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x08, reg & ~0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_RESET);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_CLEAR_FIFO);
	
	for (xfer_num = 0; xfer_num < nb_xfers; xfer_num++) {
		xfer_size = RMmin(NbBytes, MAX_DDC_FIFO);
		if (xfer_size == 0) break;  // done
		
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x80, i2cAddr);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x81, 0x00);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x82, RegAddr);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x84, xfer_size & 0xFF); // Byte count
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x85, xfer_size >> 8);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_CLEAR_FIFO);
		if (RMFAILED(err)) return err;
		for (index = 0; index < xfer_size; index++) {
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x87, *pData++);
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Could not write DDC block in DHDDCBlockWrite_SiI9030! %s\n", RMstatusToString(err)));
				return err;
			}
		}
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_SEQ_WR);
		if (RMFAILED(err)) return err;
		t0 = RMGetTimeInMicroSeconds();
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x86, &ddc_status);
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > DDC_TIMEOUT) err = RM_TIMEOUT;
		} while (RMSUCCEEDED(err) && (ddc_status & 0x01) && !(ddc_status & 0x08));  //in_progress?
		if (!(ddc_status & 0x01) && (ddc_status & 0x08)) {
			RMDBGLOG((ENABLE, "ACK error while writing DDC data!\n"));
			err = RM_ERROR;
		}
		if (RMFAILED(err)) {
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x83, DDC_MASTER_CMD_ANX_ABORT);
			return err;
		}
		NbBytes -= xfer_size;
		RegAddr += xfer_size;
	}
	
	return err;
}

RMstatus DHDDCBlockWrite(
	struct DH_control *pDH, 
	RMuint8 i2cAddr, 
	RMuint8 RegAddr, 
	RMuint8 *pData, 
	RMuint32 NbBytes) 
{
	RMstatus err = RM_ERROR;
	
	CHECK_pDH("DHDDCBlockWrite");
	CHECK_PTR("DHDDCBlockWrite", pData);
	
	RMDBGLOG((I2CDBG, "BlockWrite, %ld bytes to 0x%02X:0x%02X\n", NbBytes, i2cAddr, RegAddr));
	if (! pDH->part_caps.DDC) {
		err = DHDDCBlockWrite_direct(pDH, i2cAddr, RegAddr, pData, NbBytes);
	} else {
		switch(pDH->part) {
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			err = DHDDCBlockWrite_SiI(pDH, i2cAddr, RegAddr, pData, NbBytes);
			break;
		case DH_ANX9030:
			err = DHDDCBlockWrite_ANX(pDH, i2cAddr, RegAddr, pData, NbBytes);
			break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
			RMDBGLOG((ENABLE, "CAT6611 do not support EDID write.\n"));
    //~jj_tseng@chipadvanced.com 2007/03/23
            break ;			
		default:
			RMDBGLOG((ENABLE, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		}
	}
	return err;
}

RMstatus DH_dump_i2c(struct DH_control *pDH) 
{
	RMuint8 data[256];
	RMuint32 i;
	RMstatus err;
		
	CHECK_pDH("DH_dump_i2c");
	
	fprintf(stderr, "[HDMI] --- Dump all I2C busses ---\n");
	
	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x00, data, 256);
	fprintf(stderr, "[HDMI] Dump of tx I2C device: Dev=0x%02X, ModID=0x%08lX, ClkGPIO=%u, DataGPIO=%u, %s\n", 
		pDH->i2c_tx.dev.DevAddr, pDH->i2c_tx.I2C, 
		(RMuint8)(pDH->i2c_tx.dev.Clock - GPIOId_Sys_0), 
		(RMuint8)(pDH->i2c_tx.dev.Data - GPIOId_Sys_0), 
		RMstatusToString(err));
	for (i = 0; i < 256; i++) {
		if (i % 8 == 0) fprintf(stderr, "[HDMI] Data[%02lX]: ", i);
		fprintf(stderr, "%02X ", data[i]);
		if (i % 8 == 7) fprintf(stderr, "\n");
	}
	
	if (pDH->i2c_tx2.I2C) {
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), 0x00, data, 256);
		fprintf(stderr, "[HDMI] Dump of tx2 I2C device: Dev=0x%02X, ModID=0x%08lX, ClkGPIO=%u, DataGPIO=%u, %s\n", 
			pDH->i2c_tx2.dev.DevAddr, pDH->i2c_tx2.I2C, 
			(RMuint8)(pDH->i2c_tx2.dev.Clock - GPIOId_Sys_0), 
			(RMuint8)(pDH->i2c_tx2.dev.Data - GPIOId_Sys_0), 
			RMstatusToString(err));
		for (i = 0; i < 256; i++) {
			if (i % 8 == 0) fprintf(stderr, "[HDMI] Data[%02lX]: ", i);
			fprintf(stderr, "%02X ", data[i]);
			if (i % 8 == 7) fprintf(stderr, "\n");
		}
		fprintf(stderr, "\n");
	}
	
	return RM_OK;
}

static RMstatus DH_probe_i2c(struct DH_control *pDH, struct DH_I2C *pi2c_dev)
{
	RMstatus err;
	RMuint32 i;
	struct EMhwlibI2CDeviceParameter i2c;
	struct I2C_QueryRMuint8_in_type i2c_param;
	struct I2C_QueryRMuint8_out_type i2c_res;
	
	fprintf(stderr, "--- Probing all I2C devices ---\n");
	i2c = pi2c_dev->dev;
	for (i = 0; i < 0x80; i++) {
		i2c.DevAddr = i << 1;
		err = RUASetProperty(pDH->pRUA, pi2c_dev->I2C, RMI2CPropertyID_DeviceParameter, &i2c, sizeof(i2c), 0);
		i2c_param.SubAddr = 0;
		err = RUAExchangeProperty(pDH->pRUA, pi2c_dev->I2C, RMI2CPropertyID_QueryRMuint8, 
			&i2c_param, sizeof(i2c_param), 
			&i2c_res, sizeof(i2c_res));
		if (RMSUCCEEDED(err)) fprintf(stderr, "Success reading from 0x%02X:0x%02X = 0x%02X\n", i2c.DevAddr, i2c_param.SubAddr, i2c_res.Data);
	}
	fprintf(stderr, "--- Probing done. ---\n");
	pi2c_prev_dev = NULL;
	return RM_OK;
}

static RMstatus DH_burst_verify_i2c(struct DH_control *pDH)
{
	#define DH_I2C_BURST_SIZE 0x1F
	#define DH_I2C_BURST_ADDR 0xA0
	RMstatus err;
	RMuint32 i, n;
	RMuint8 data_orig[DH_I2C_BURST_SIZE], data_wr[DH_I2C_BURST_SIZE], data_rd[DH_I2C_BURST_SIZE];
	
	fprintf(stderr, "Reading original data\n");
	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), DH_I2C_BURST_ADDR, data_orig, DH_I2C_BURST_SIZE);
	if (RMFAILED(err)) return err;
	
	for (n = 0; n < 5; n++) {
		fprintf(stderr, "Run %lu: Generating random content\n", n + 1);
		for (i = 0; i < DH_I2C_BURST_SIZE; i++) {
			data_wr[i] = (RMuint8)RMIntRandom(0x100);
		}
		fprintf(stderr, "Writing 0x%02X bytes\n", DH_I2C_BURST_SIZE);
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), DH_I2C_BURST_ADDR, data_wr, DH_I2C_BURST_SIZE);
		if (RMFAILED(err)) break;
		fprintf(stderr, "Reading back data\n");
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), DH_I2C_BURST_ADDR, data_rd, DH_I2C_BURST_SIZE);
		if (RMFAILED(err)) break;
		fprintf(stderr, "Comparing data\n");
		for (i = 0; i < DH_I2C_BURST_SIZE; i++) {
			if (data_rd[i] != data_wr[i]) {
				fprintf(stderr, "Run %lu: Error in byte %lu: wr=0x%02X,0x%02X, rd=0x%02X\n", n + 1, i, i ? data_wr[i - 1] : DH_I2C_BURST_ADDR, data_wr[i], data_rd[i]);
			}
		}
	}
	fprintf(stderr, "Restoring original data\n");
	err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), DH_I2C_BURST_ADDR, data_orig, DH_I2C_BURST_SIZE);
	
	return err;
}

static RMuint32 CountBits(RMuint8 *Data, RMuint32 Size)
{
	RMuint32 i, j, n = 0;
	for (i = 0; i < Size; i++) {
		for (j = 0; j < 8; j++) {
			if (Data[i] & (1 << j)) n++;
		}
	}
	return n;
}

static RMstatus DHGetAKSV_SiI(struct DH_control *pDH, RMuint8 *pAKSV)
{
	return DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x1D, pAKSV, 5);
}

static RMstatus DHGetAKSV_ANX(struct DH_control *pDH, RMuint8 *pAKSV)
{
	return DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0xA5, pAKSV, 5);
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHGetAKSV_CAT(struct DH_control *pDH, RMuint8 *pAKSV)
{
	return DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x23, pAKSV, 5);
}
//~jj_tseng@chipadvanced.com 2007/03/23


RMstatus DHGetAKSV(struct DH_control *pDH, RMuint8 *pAKSV)
{
	RMstatus err = RM_ERROR;
	
	CHECK_pDH("DHGetAKSV");
	CHECK_PTR("DHGetAKSV", pAKSV);
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHGetAKSV with uninitialized device!\n"));
		return RM_ERROR;
	}
	
	RMDBGLOG((LOCALDBG, "read aksv value from transmitter\n"));
	switch(pDH->part) {
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DHGetAKSV_SiI(pDH, pAKSV);
		break;
	case DH_ANX9030:
		err = DHGetAKSV_ANX(pDH, pAKSV);
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		err = DHGetAKSV_CAT(pDH, pAKSV);
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		RMDBGLOG((ENABLE, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
	}
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Can not read AKSV!\n"));
		return err;
	}
	if (! manutest) fprintf(stderr, "[HDMI] AKSV (Tx HDCP key): %02X %02X %02X %02X %02X\n", pAKSV[0], pAKSV[1], pAKSV[2], pAKSV[3], pAKSV[4]);
	if (CountBits(pAKSV, 5) != 20) {
		if (! manutest) fprintf(stderr, "[HDMI] Transmiter key invalid!\n");
		err = RM_ERROR;
	}
	
	return err;
}

static RMstatus DHGetBKSV_ANX(struct DH_control *pDH, RMuint8 *pBKSV)
{
	return DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0xB2, pBKSV, 5);
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHCollectRxHDCPStatus_CAT(struct DH_control *pDH)
{
    RMstatus err = RM_OK ;
    RMuint8 reg ;
    
	DH_Clear_CAT6611DDCFIFO(pDH);
	
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x10, 1); // Read Rx DDC/EDID, with PC/host.
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x11, 0x74); // 0x74 for get HDCP registers
	
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x12, 0); // 0 for BKSV 
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x13, 5); // Request byte.
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x15, 0); // command, burst DDC read

	while(1)
	{
		RMMicroSecondSleep(10*1000); // Needs a minimum delay. We may need to increase it
		err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x16, &reg ) ;

		if( reg & (1<<7))
		{
		    // DDC done.
		    break ;
		}

		if( reg & ((1<<3)|(1<<5)))
		{
		    RMDBGLOG((ENABLE, "BlockReadSegment CAT6611 fail, ddc status = %02X\n",reg));
		    return RM_ERROR ;
		}
	}
	// BKSV is ready in reg3B~reg3F
	
	DH_Clear_CAT6611DDCFIFO(pDH);
	
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x10, 1); // Read Rx DDC/EDID, with PC/host.
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x11, 0x74); // 0x74 for get HDCP registers
	
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x12, 0x40); // 0x40 for BCaps
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x13, 3); // Request byte 3 is for BCaps, BStatus (2 bytes).
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x15, 0); // command, burst DDC read

	while(1)
	{
		RMMicroSecondSleep(10*1000); // Needs a minimum delay. We may need to increase it
		err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x16, &reg ) ;

		if( reg & (1<<7))
		{
		    // DDC done.
		    break ;
		}

		if( reg & ((1<<3)|(1<<5)))
		{
		    RMDBGLOG((ENABLE, "BlockReadSegment CAT6611 fail, ddc status = %02X\n",reg));
		    return RM_ERROR ;
		}
	}
	// BCaps is ready in reg43
	// BStatus is ready in reg44 and reg45.
	
	pDH->CAT_hdcpreg_ready = TRUE ;
    return err ;
}

static RMstatus DHGetBKSV_CAT(struct DH_control *pDH, RMuint8 *pBKSV)
{
//	RMuint8 reg ;
	RMstatus err;


	// read bksv from receiver
    RMDBGLOG((ENABLE,"DHGetBKSV_CAT6611()\n"));
    
    if( !pDH->CAT_hdcpreg_ready )
    {
        err = DHCollectRxHDCPStatus_CAT(pDH) ;
        if( RMFAILED(err) )
        {
        	RMDBGLOG((ENABLE, "read bksv value fail from sink.\n"));
        	return RM_ERROR ;
        }
    }    
    
	RMDBGLOG((ENABLE, "read bksv value from receiver via CAT6611\n"));

	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x3B, pBKSV, 5);

	if (! manutest) fprintf(stderr, "[HDMI] BKSV (Rx HDCP key): %02X %02X %02X %02X %02X\n", pBKSV[0], pBKSV[1], pBKSV[2], pBKSV[3], pBKSV[4]);
	if (CountBits(pBKSV, 5) != 20) {
		if (! manutest) fprintf(stderr, "[HDMI] Receiver key invalid!\n");
		return RM_ERROR;
	}
	
	return err;
}
//~jj_tseng@chipadvanced.com 2007/03/23


static RMstatus DHGetBKSV(struct DH_control *pDH, RMuint8 *pBKSV)
{
	RMstatus err = RM_ERROR;
	
	CHECK_pDH("DHGetBKSV");
	CHECK_PTR("DHGetBKSV", pBKSV);
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHGetBKSV with uninitialized device!\n"));
		return RM_ERROR;
	}
	
	RMDBGLOG((LOCALDBG, "read bksv value from receiver\n"));
	switch(pDH->part) {
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
	case DH_ANX9030:
		err = DHDDCBlockRead(pDH, DDC_HDCP_DEV, 0x00, pBKSV, 5);
		break;
//	case DH_ANX9030:
		err = DHGetBKSV_ANX(pDH, pBKSV);
		break;
		
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        err = DHGetBKSV_CAT(pDH, pBKSV) ;
        break ;
    //~jj_tseng@chipadvanced.com 2007/03/23
		
	default:
		RMDBGLOG((ENABLE, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
	}
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Can not read BKSV!\n"));
		return err;
	}
	if (! manutest) fprintf(stderr, "[HDMI] BKSV (Rx HDCP key): %02X %02X %02X %02X %02X\n", pBKSV[0], pBKSV[1], pBKSV[2], pBKSV[3], pBKSV[4]);
	if (CountBits(pBKSV, 5) != 20) {
		if (! manutest) fprintf(stderr, "[HDMI] Receiver key invalid!\n");
		return RM_ERROR;
	}
	
	return err;
}

static RMstatus DHEnableEncryption_SiI(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 data;
	
	// set ENC_EN bit
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &data);
	if (RMFAILED(err)) data = 0x00;  // TODO: might break repeater status
	RMDBGLOG((LOCALDBG, "Read status from transmitter: %02X\n", data));
	data |= 0x0D;
	RMDBGLOG((LOCALDBG, "Write status to transmitter: %02X\n", data));
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, data);
	
	switch(pDH->part) {
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x27, &data);
		if (RMFAILED(err)) data = 0x00;
		data |= 0x03;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x27, data);
		break;
	default:
		break;
	}
	
	return RM_OK;
}

static RMstatus DHEnableEncryption_ANX(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 data;
	
	// set HDCP_ENC_EN bit
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, &data);
	if (RMFAILED(err)) data = 0x00;  // TODO: might break repeater status
	RMDBGLOG((LOCALDBG, "Read status from transmitter: %02X\n", data));
	data |= 0x04;  // HDCP_ENC_EN
	RMDBGLOG((LOCALDBG, "Write status to transmitter: %02X\n", data));
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, data);
	pDH->ANX_hdcp_encryption = TRUE;
	
	return RM_OK;
}

static RMstatus DHEnableEncryption(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	
	RMDBGLOG((LOCALDBG,"DHEnableEncryption()\n")) ;
	if (! pDH->part_caps.HDCP) {
		RMDBGLOG((LOCALDBG, "Call to DHEnableEncryption, but chip does not support HDCP\n"));
		return RM_ERROR;
	} else if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHEnableEncryption with uninitialized device!\n"));
		return RM_ERROR;
	}
	else if (pDH->state != DH_enabled_authenticated) {
		RMDBGLOG((LOCALDBG, "Call to DHEnableEncryption with unauthenticated device!\n"));
		return RM_ERROR;
	}
	
	switch(pDH->part) {
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DHEnableEncryption_SiI(pDH);
		break;
	case DH_ANX9030:
		err = DHEnableEncryption_ANX(pDH);
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        // if authenticated, CAT6611 automatically encrypted output data.
        err = RM_OK ;
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23
		
	default:
		RMDBGLOG((LOCALDBG, "Part (%d) does not support HDCP\n", pDH->part));
		err = RM_ERROR;
	}
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "Cannot enable encryption on Chip\n"));
		return err;
	}
	
	pDH->state = DH_enabled_encrypted;
	RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/ENCRYPTED\n"));
	
	return err;
}


static RMstatus DHDisableEncryption_SiI(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 data;
	
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &data);
	if (RMFAILED(err)) data = 0x00;  // TODO: might break repeater status
	RMDBGLOG((LOCALDBG, "Read status from transmitter: %02X\n", data));
	data = (data & 0xF6) | 0x04;
	RMDBGLOG((LOCALDBG, "Write status to transmitter: %02X\n", data));
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, data);
	//if (RMFAILED(err)) return err;  // some bits are read only, verify might fail
	
	switch(pDH->part) {
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x27, &data);
		if (RMFAILED(err)) data = 0x00;
		data &= ~0x03;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x27, data);
		break;
	default:
		break;
	}
	
	return RM_OK;
}

static RMstatus DHDisableEncryption_ANX(struct DH_control *pDH)
{
	RMstatus err;
	RMuint8 data;
	
	// stop hardware HDCP authentication process and encryption
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, &data);
	if (RMFAILED(err)) data = 0x00;  // TODO: might break repeater status
	RMDBGLOG((LOCALDBG, "Read status from transmitter: %02X\n", data));
	data &= ~0x0C;  // HARD_AUTH_EN, HDCP_ENC_EN
	RMDBGLOG((LOCALDBG, "Write status to transmitter: %02X\n", data));
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, data);
	//if (RMFAILED(err)) return err;  // some bits are read only, verify might fail
	pDH->ANX_hdcp_encryption = FALSE;
	
	return RM_OK;
}


static RMstatus DHDisableEncryption(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 reg ;
	RMDBGLOG((LOCALDBG,"DHDisableEncryption()\n")) ;
	if (! pDH->part_caps.HDCP) {
		RMDBGLOG((LOCALDBG, "Call to DHDisableEncryption, but chip does not support HDCP\n"));
		return RM_ERROR;
	} else if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHDisableEncryption with uninitialized device!\n"));
		return RM_ERROR;
	}
	
	if (pDH->state == DH_enabled_encrypted) {
		switch(pDH->part) {
		case DH_siI170:
			err = DHReset(pDH);
			DHMuteOutput(pDH, FALSE);
			break;
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			err = DHDisableEncryption_SiI(pDH);
			break;
		case DH_ANX9030:
			err = DHDisableEncryption_ANX(pDH);
			break;
        // 2007/03/23 Added by jj_tseng@chipadvanced
        case DH_CAT6611:
        	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &reg);
        	reg |= 1 ; // reset hdcp circuit, disable hdcp.
        	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, reg);
            break ;			
        //~jj_tseng@chipadvanced.com 2007/03/23
		default:
			RMDBGLOG((LOCALDBG, "Part (%d) does not support HDCP\n", pDH->part));
			err = RM_ERROR;
		}
		if (RMFAILED(err)) {
			RMDBGLOG((LOCALDBG, "Cannot disable encryption on chip\n"));
			return err;
		}
		pDH->state = DH_enabled_authenticated;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/AUTHENTICATED\n"));
	} else if (pDH->state == DH_enabled_repeater_wait) {
		pDH->state = DH_enabled_authenticated;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/AUTHENTICATED\n"));
	} else if (pDH->state != DH_disabled) {
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
	}
	return err;
}

static RMstatus DHVerifyIntegrity_SiI170(struct DH_control *pDH, RMbool check_hdcp)
{
	RMuint8 Data[2];
	RMuint16 riRx = 0, riTx = 0;
	RMstatus err = RM_OK;
	RMint8 i;
	RMbool ri_ready, hdcp_enabled;
	
	for (i = 0; i < MAX_HDCP_ATTEMPTS; i++) {
		if (i) RMMicroSecondSleep(50 * 1000);  // delay 2nd try into next video frame
		
		// read key status
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &Data[0]);
		if (RMFAILED(err)) continue;
		if (i) RMDBGLOG((LOCALDBG, "HDCP RETRY: check status: 0x%02X - BKSV %s, Encryption is %s, %s, transmitter ri is %s\n", Data[0], 
			(Data[0] & 0x20) ? "ERROR" : "OK", 
			(Data[0] & 0x40) ? "ON" : "OFF", 
			(Data[0] & 0x01) ? "ENABLED" : "DISABLED", 
			(Data[0] & 0x02) ? "READY" : "NOT READY"));
		ri_ready = (Data[0] & 0x02);
		hdcp_enabled = (Data[0] & 0x40);
		if (check_hdcp && (! hdcp_enabled)) {
			RMDBGLOG((ENABLE, "DHVerifyIntegrity_SiI170() HDCP is OFF!\n"));
			return RM_ERROR; // HDCP is off
		}
		if (! ri_ready) {
			RMDBGLOG((LOCALDBG, "HDCP Ri key not ready!\n"));
			continue;
		}
		
		// read the ri value from the receiver
		err = DHDDCBlockRead(pDH, DDC_HDCP_DEV, 0x08, Data, 2);
		if (RMFAILED(err)) continue;
		riRx = ((RMuint16) Data[1] << 8) | Data[0];
		if (i) RMDBGLOG((DISABLE, "receiver ri = %02X%02X\n", Data[1], Data[0]));
		
		// read the ri value from the transmitter
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x22, Data, 2);
		if (RMFAILED(err)) continue;
		riTx = ((RMuint16)Data[1] << 8) | Data[0];
		if (i) RMDBGLOG((DISABLE, "transmitter ri = %02X%02X\n", Data[1], Data[0]));
		
		if (riRx == riTx) return RM_OK;
		if (i) {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP key mismatch: 0x%04X != 0x%04X\n", riRx, riTx);
		}
	}
	
	RMDBGLOG((ENABLE, "DHVerifyIntegrity_SiI170() ri's don't match! (0x%04X != 0x%04X)\n", riRx, riTx));
	return RM_ERROR;
}

static RMstatus DHVerifyIntegrity_SiI9030(struct DH_control *pDH, RMbool check_hdcp)
{
	RMuint8 Data[2], i_count, reg;
	RMuint16 riRx = 0, riTx = 0, riTx127 = 0;
	RMstatus err = RM_OK;
	RMint32 i;
	RMbool ri_ready, hdcp_enabled, ri_en;
	
	// Ri mismatch feature of 9034/9134
	switch (pDH->part) {
		case DH_siI9034:
		case DH_siI9134:
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x27, &reg);
			ri_en = (reg & 0x01) ? TRUE : FALSE;
			break;
		default:
			ri_en = FALSE;
	}
	
	if (ri_en) {
		RMDBGLOG((DISABLE, "SiI9x34: HDCP Ri integrity checked automatically\n"));
		
		// read the ri value that was read from the receiver
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x29, Data, 2);
		riRx = ((RMuint16)Data[1] << 8) | Data[0];
		if (! riRx) return RM_OK;
		RMDBGLOG((LOCALDBG, "HDCP: receiver Ri = %02X%02X\n", Data[1], Data[0]));
		
		// read the ri value from the transmitter
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x22, Data, 2);
		riTx = ((RMuint16)Data[1] << 8) | Data[0];
		RMDBGLOG((LOCALDBG, "HDCP: transmitter Ri = %02X%02X\n", Data[1], Data[0]));
		
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x25, &i_count);
		if (! manutest) fprintf(stderr, "[HDMI] HDCP key error: RiRX=0x%04X RiTX=0x%04X, I-Count=%u\n", riRx, riTx, i_count);
		return RM_ERROR;
	}
	
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x25, &i_count);
	if (RMSUCCEEDED(err) && (i_count == 127)) {  // improved detection of HDCP loss
		// read key status
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &Data[0]);
		if (RMSUCCEEDED(err)) {
			ri_ready = (Data[0] & 0x02);
			hdcp_enabled = (Data[0] & 0x40);
			if (check_hdcp && (! hdcp_enabled)) {
				RMDBGLOG((ENABLE, "DHVerifyIntegrity_SiI9030() HDCP is OFF!\n"));
				return RM_ERROR; // HDCP is off
			}
			if (ri_ready) {
				// read the ri value from the transmitter
				err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x22, Data, 2);
				if (RMSUCCEEDED(err)) {
					riTx127 = ((RMuint16)Data[1] << 8) | Data[0];
					//RMDBGLOG((LOCALDBG, "HDCP: previous transmitter ri = %02X%02X\n", Data[1], Data[0]));
				}
			}
			do {  // wait for next vsync
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x25, &i_count);
			} while (RMSUCCEEDED(err) && (i_count == 127));
			RMMicroSecondSleep(1 * 1000);  // delay 1 mSec
		}
	}
	
	for (i = 0; i < MAX_HDCP_ATTEMPTS; i++) {
		if (i) RMMicroSecondSleep(50 * 1000);  // delay 2nd try into next video frame
		
		// read key status
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &Data[0]);
		if (RMFAILED(err)) continue;
		if (i) RMDBGLOG((LOCALDBG, "HDCP RETRY: check status: 0x%02X - BKSV %s, Encryption is %s, %s, transmitter ri is %s\n", Data[0], 
			(Data[0] & 0x20) ? "ERROR" : "OK", 
			(Data[0] & 0x40) ? "ON" : "OFF", 
			(Data[0] & 0x01) ? "ENABLED" : "DISABLED", 
			(Data[0] & 0x02) ? "READY" : "NOT READY"));
		ri_ready = (Data[0] & 0x02);
		hdcp_enabled = (Data[0] & 0x40);
		if (check_hdcp && (! hdcp_enabled)) {
			RMDBGLOG((ENABLE, "DHVerifyIntegrity_SiI9030() HDCP is OFF!\n"));
			return RM_ERROR; // HDCP is off
		}
		if (! ri_ready) {
			RMDBGLOG((LOCALDBG, "HDCP Ri key not ready!\n"));
			continue;
		}
		
		// read the ri value from the receiver
		err = DHDDCBlockRead(pDH, DDC_HDCP_DEV, 0x08, Data, 2);
		if (RMFAILED(err)) continue;
		riRx = ((RMuint16)Data[1] << 8) | Data[0];
		if (i) RMDBGLOG((LOCALDBG, "HDCP RETRY: receiver ri = %02X%02X\n", Data[1], Data[0]));
		
		// read the ri value from the transmitter
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x22, Data, 2);
		if (RMFAILED(err)) continue;
		riTx = ((RMuint16)Data[1] << 8) | Data[0];
		if (i) RMDBGLOG((LOCALDBG, "HDCP RETRY: transmitter ri = %02X%02X\n", Data[1], Data[0]));
		
		if (riTx127 && (riTx127 == riRx)) {
			if (! manutest) fprintf(stderr, 
				"\n\n"
				"=============================================================================\n"
				"=                                                                           =\n"
				"=                                                                           =\n"
				"= [HDMI] HDCP key sync error! HDMI Receiver has detected extra CTL3 signal. =\n"
				"=                                                                           =\n"
				"=                                                                           =\n"
				"=============================================================================\n"
				"\n\n"
			);
			return RM_ERROR;
		}
		
		if (riRx == riTx) return RM_OK;
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x25, &i_count);
		if (i) {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP key mismatch: 0x%04X != 0x%04X, I-Count=%u\n", riRx, riTx, i_count);
		}
		//if ((Data[0] >= RI_128_THRESHOLD) && (Data[0] <= RI_128_THRESHOLD + 5)) return RM_ERROR;  // fail after 1st try if synchronized integrity check
	}
	
	RMDBGLOG((ENABLE, "DHVerifyIntegrity_SiI9030() ri's don't match! (0x%04X != 0x%04X)\n", riRx, riTx));
	return RM_ERROR;
}

// Start Authentication and return random AN value
static RMstatus DHReadAN_SiI(struct DH_control *pDH, RMuint8 *pAN, RMbool *pRepeater)
{
	RMstatus err = RM_OK;
	RMuint8 reg, tmds_ctrl4, Bcaps;
	
	// CP Reset
	if (pDH->part == DH_siI170) {
		RMuint32 tries;
		
		err = DHReset(pDH); /* reset and enable SiI170 */
		if (RMFAILED(err)) return err;
		
		tries = 5;
		do {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x09, &reg);
			if (reg & 0x04) break;
			if (! --tries) {
				RMDBGLOG((LOCALDBG, "Error: receiver not present!\n"));
				return RM_ERROR;
			}
			RMMicroSecondSleep(1000*1000);
		} while (1);
	}
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &reg);
	if (RMFAILED(err)) { RMDBGLOG((ENABLE, "I2C read error\n")); return err; }
	if (pDH->part != DH_siI170) {
		reg &= ~0x04;  // set
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, reg);
		reg |= 0x04;  // release
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, reg);
	}
	
	// receiver is repeater?
	err = DHDDCBlockRead(pDH, DDC_HDCP_DEV, 0x40, &Bcaps, 1);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "I2C DDC read error\n")); 
		return err;
	}
	*pRepeater = (Bcaps & 0x40) ? TRUE : FALSE;
	RMDBGLOG((ENABLE, "*************** HDCP RX is %s **************\n", *pRepeater ? "Repeater" : "Receiver"));
	
	// write repeater status to Transmitter
	if (*pRepeater) reg |=  0x10;
	else            reg &= ~0x10;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, reg);
	switch(pDH->part) {
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xCC, &tmds_ctrl4);
		if (RMSUCCEEDED(err)) {
			if (*pRepeater) tmds_ctrl4 |=  0x08;
			else            tmds_ctrl4 &= ~0x08;
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xCC, tmds_ctrl4);
		}
		break;
	default:
		break;
	}
	
	// generate random AN value in Transmitter
	reg &= ~0x09;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, reg);
	RMMicroSecondSleep(10*1000);
	reg |= 0x08;
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, reg);
	
	// read Transmitter AN random value
	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x15, pAN, 8);
	if (RMFAILED(err)) { RMDBGLOG((ENABLE, "I2C read error\n")); return err; }
	
	return err;
}

static RMstatus DHWriteBKSV_SiI(struct DH_control *pDH, RMuint8 *pBKSV)
{
	RMstatus err;
	RMuint8 reg;
	RMuint32 tries;
	
	// write BKSV to Transmitter
	err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x10, pBKSV, 5);
	if (RMFAILED(err)) { RMDBGLOG((ENABLE, "I2C write error\n")); return err; }
	
	// check for BKSV error and Ri Ready
	tries = 5;
	do {
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &reg);
		if (RMFAILED(err)) return err;
		RMDBGLOG((LOCALDBG, "Read status from transmitter: %02X\n", reg));
		if (reg & 0x02) break;
		if (! --tries) {
			RMDBGLOG((LOCALDBG, "Error: RI on transmitter not ready!\n"));
			return RM_ERROR;
		}
		// wait 100ms before retry
		RMMicroSecondSleep(100*1000);
	} while (1);
	if (reg & 0x20) {
		RMDBGLOG((LOCALDBG, "Error: B_ERR set\n"));
		return RM_ERROR;
	}
	
	return err;
}

static RMstatus DHAuthenticate_SiI(struct DH_control *pDH, RMuint8 *pBKSV, RMbool *pRepeater)
{
	RMstatus err = RM_OK;
	RMuint8 Data[8];
	
	// Start Auth and read random value from transmitter
	err = DHReadAN_SiI(pDH, Data, pRepeater);
	if (RMFAILED(err)) { RMDBGLOG((ENABLE, "AN read error\n")); return err; }
	RMDBGLOG((LOCALDBG, "AN: %02X %02X %02X %02X %02X %02X %02X %02X\n",
		  Data[0], Data[1], Data[2], Data[3], Data[4], Data[5], Data[6], Data[7]));
	
	// write AN to Receiver
	err = DHDDCBlockWrite(pDH, DDC_HDCP_DEV, 0x18, Data, 8);
	if (RMFAILED(err)) { RMDBGLOG((ENABLE, "I2C DDC write error\n")); return err; }
	
	// read AKSV from Transmitter
	err = DHGetAKSV(pDH, Data);
	if (RMFAILED(err)) { RMDBGLOG((ENABLE, "I2C read or AKSV error\n")); return err; }
	
	// write AKSV to Receiver
	err = DHDDCBlockWrite(pDH, DDC_HDCP_DEV, 0x10, Data, 5);
	if (RMFAILED(err)) { RMDBGLOG((ENABLE, "I2C DDC write error\n")); return err; }
	
	// wait
	if (pDH->part == DH_siI170) RMMicroSecondSleep(10*1000);
	
	// write BKSV to Transmitter
	err = DHWriteBKSV_SiI(pDH, pBKSV);
	if (RMFAILED(err)) { RMDBGLOG((ENABLE, "BKSV error\n")); return err; }
	
	if (pDH->part == DH_siI170) {
		err = DHVerifyIntegrity_SiI170(pDH, FALSE);
	} else {
		err = DHVerifyIntegrity_SiI9030(pDH, FALSE);
	}
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Error: Can not verify key integrity\n"));
		return err;
	}
	
	return RM_OK;
}

static RMstatus DHAuthenticate_ANX(struct DH_control *pDH, RMuint8 *pBKSV, RMbool *pRepeater)
{
	RMstatus err = RM_OK;
	RMuint8 reg, B[3];  // Bcaps, Bstatus
	
	// TODO
	
	// receiver status
	err = DHDDCBlockRead(pDH, DDC_HDCP_DEV, 0x40, B, 3);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "I2C DDC read error\n")); 
		return err;
	}
	if (B[2] & 0x10) {  // HDMI mode?
	} else {
	}
	
	// HDCP 1.1 status
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA2, &reg);
	RMinsShiftBool(&reg, B[0] & 0x02, 0);
	RMinsShiftBool(&reg, B[0] & 0x02, 1);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xA2, reg);
	
	// Reset HDCP
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x05, &reg);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, reg | 0x08);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, reg & ~0x08);
	
	// Enable HDCP mode
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &reg);
	if (RMFAILED(err)) reg = init_ANX9030[3][1];
	RMinsShiftBool(&reg, TRUE, 2);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x07, reg);
	
	// Restart HDCP authentication
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, &reg);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, reg | 0x28);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, reg & ~0x20);
	
	pDH->ANX_hdcp_auth_en = FALSE;
	pDH->ANX_bksv_ready = FALSE;
	pDH->ANX_hdcp_auth_pass = FALSE;
	pDH->ANX_hdcp_auth_fail_counter = 0;
	pDH->ANX_hdcp_encryption = FALSE;
	pDH->ANX_send_blue_screen = FALSE;
	pDH->ANX_hdcp_init_done = FALSE;
	pDH->ANX_hdcp_wait_100ms_needed = TRUE;
	pDH->ANX_auth_fully_pass = FALSE;
	
	// receiver is repeater?
	*pRepeater = (B[0] & 0x40) ? TRUE : FALSE;
	RMDBGLOG((ENABLE, "*************** HDCP RX is %s **************\n", *pRepeater ? "Repeater" : "Receiver"));
	
	pDH->ANX_hdcp_init_done = TRUE;
	
	// start hardware HDCP authentication process
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, &reg);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, reg | 0x08);  // HARD_AUTH_EN
	pDH->ANX_hdcp_auth_en = TRUE;
	
	return RM_OK;
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHGetBCaps_CAT(struct DH_control *pDH, RMuint8 *pBCaps)
{
    RMstatus err = RM_OK ;
	RMDBGLOG((LOCALDBG,"DHGetBCaps_CAT() pBCaps =")) ;

    if( !pDH->CAT_hdcpreg_ready )
    {
        err = DHCollectRxHDCPStatus_CAT(pDH) ;
        if( RMFAILED(err) )
        {
        	RMDBGLOG((ENABLE, "read bksv value fail from sink.\n"));
        	return RM_ERROR ;
        }
    }    

	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x43, pBCaps);
    RMDBGPRINT((LOCALDBG,"%02X\n", *pBCaps)) ;
    return err ; 
}

static RMstatus DHGetBStat_CAT(struct DH_control *pDH, RMuint8 *pBStat)
{
    RMstatus err = RM_OK ;
    
    RMDBGLOG((LOCALDBG,"DHGetBStat_CAT(): pBStatus = ")) ;
    if( !pDH->CAT_hdcpreg_ready )
    {
        err = DHCollectRxHDCPStatus_CAT(pDH) ;
        if( RMFAILED(err) )
        {
        	RMDBGLOG((ENABLE, "read bksv value fail from sink.\n"));
        	return RM_ERROR ;
        }
    }    
	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x44, pBStat, 2);
    RMDBGPRINT((LOCALDBG,"%02X%02X\n", pBStat[1], pBStat[0])) ;
    
    return err ; 
}


static RMstatus DHAuthenticate_CAT(struct DH_control *pDH, RMuint8 *pBKSV, RMbool *pRepeater)
{
	RMstatus err = RM_OK;

	RMuint8 Data[8];
	RMuint8 BCaps ;
	RMuint32 count ;
	// RMint8 i;
    // RMbool repeater = FALSE ;

    RMDBGLOG((ENABLE,"DHAuthenticate_CAT6611()\n"));

	DH_Switch_6611BANK(pDH,0) ;
	
	DH_Set_CAT6611_AVMute(pDH) ;
	
    err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x04, &Data[0]) ;
	Data[0] &= ~3 ;
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x04, Data[0]) ; // enable HDCP


    DHGetBCaps_CAT(pDH,&BCaps) ;

    // Write CP_Desired and HDCP 1.1 Feature
    if(BCaps & (1<<1))
	{
        err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x20, 0x03) ; // enable HDCP 1.1, enable HDCP
    }
    else
    {
        err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x20, 0x01) ; // enable HDCP only
    }
    
    if( BCaps & B_CAP_HDMI_REPEATER )
    {
        *pRepeater = TRUE ;
    }
    else
    {
        *pRepeater = FALSE ;
    }
    

    // Generate An
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x1F, 0x1) ;
    RMMicroSecondSleep(20*1000) ; // delay 20 ms to stop Cipher random run.
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x1F, 0x0) ;
    
	err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x30, Data, 8);

	RMDBGLOG((ENABLE, "Get An %02X %02X %02X %02X %02X %02X %02X %02X\n", Data[0], Data[1], Data[2], Data[3], Data[4], Data[5], Data[6], Data[7]));
	err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x28, Data, 8); // write An

	// DDC from host, and to HDCP core.
	err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x10, 0x0) ;
	// Enable HDCP relative interrupt by reg0A[1:0] = '00'
	
    err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x0A, &Data[0]) ;
    Data[0] &= ~3 ;
	if( *pRepeater )
	{
	    Data[0] &= ~4 ; // enable KSV list check interrupt.
	}
	else
	{
	    Data[0] |= 4 ; // disable KSV list check interrupt.
	}
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x0A, Data[0]) ; // enable Auth Done and Auth Fail interrupt.

    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x21, 1) ; // Write reg21 0, Fire Authentication

	for( count = 0 ; count < 5 ; count++ ){
        RMMicroSecondSleep(200*1000) ; // delay 200 ms

        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &Data[0]); // read interrupt
    	RMDBGLOG((ENABLE, "Authenticated int status is %02X\n", Data[0]));

    	DHDump_CAT6611reg(pDH);

        if(Data[0] & 1)
        {
            // authentication fail, disable hdcp.
            err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x04, &Data[0]) ;
            Data[0] |= 1 ;
            err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x04, Data[0]) ;

            err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x0C, (1<<2)) ;
            DH_Clear_CAT6611INT(pDH) ;
            return RM_ERROR ;
        }

        if(Data[0] & (1<<1))
        {
            // clear HDCP done interrupt.
            err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x0C, (1<<3)) ;
            DH_Clear_CAT6611INT(pDH) ;

            err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x0A, &Data[0]) ;
            Data[0] &= ~3 ;
            Data[0] |= 2 ; // disable HDCP authentication done interrupt.
            err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x0A, Data[0]) ;
   			DH_Clear_CAT6611_AVMute(pDH);
            return RM_OK ;
        }
	}

	return RM_OK;
}
//~jj_tseng@chipadvanced.com 2007/03/23

RMstatus DHAuthenticate(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 BKSV[5];
	RMbool revoked, repeater;
	
	CHECK_pDH("DHAuthenticate");
	
	RMDBGLOG((LOCALDBG,"DHAuthenticate()\n")) ;
	if (! pDH->part_caps.HDCP) {
		RMDBGLOG((LOCALDBG, "Call to DHAuthenticate, but chip does not support HDCP\n"));
		return RM_ERROR;
	} else if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHAuthenticate with uninitialized device!\n"));
		return RM_ERROR;
	}
	else if (pDH->state == DH_disabled) {
		RMDBGLOG((LOCALDBG, "Call to DHAuthenticate with disabled device!\n"));
		return RM_ERROR;
	}
	else if (pDH->state != DH_enabled) {
		RMDBGLOG((LOCALDBG, "Warning: Call to DHAuthenticate with already authenticated device!\n"));
	}
	
	// check HDCP Device: read BKSV from Receiver
	err = DHGetBKSV(pDH, BKSV);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "I2C DDC read or BKSV error\n")); 
		return err;
	}
	err = DHCheckKeyRevocation(pDH->SRM, BKSV, 1, &revoked);
	if (RMSUCCEEDED(err) && revoked) {
		if (! manutest) fprintf(stderr, "[HDMI] HDCP Key error: Invalid Display or Display Key has been revoked!\n");
		return RM_DRM_INVALID_KEY;
	}
	
	switch(pDH->part) {
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DHAuthenticate_SiI(pDH, BKSV, &repeater);
		break;
	case DH_ANX9030:
		err = DHAuthenticate_ANX(pDH, BKSV, &repeater);
		break;
	case DH_CAT6611:
		err = DHAuthenticate_CAT(pDH, BKSV, &repeater);
		break;
	default:
		RMDBGLOG((LOCALDBG, "Part (%d) does not support HDCP\n", pDH->part));
		err = RM_ERROR;
	}
	
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Cannot authenticate chip\n"));
		return err;
	}
	
	if (repeater) {
		pDH->RepeaterTimeout = RMGetTimeInMicroSeconds();
		pDH->state = DH_enabled_repeater_wait;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/REPEATER WAIT\n"));
	} else {
		pDH->state = DH_enabled_authenticated;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/AUTHENTICATED\n"));
	}
	
	return err;
}

static Bignum get160(RMuint8 **data, RMuint32 *datalen)
{
	Bignum b;
	
	b = bignum_from_bytes(*data, 20);
	*data += 20;
	*datalen -= 20;
	
	return b;
}

static RMbool dss_verifysig(
	struct dss_key *dss, 
	RMuint8 *sig, RMuint32 siglen,
	RMuint8 *data, RMuint32 datalen)
{
	RMuint8 *p;
	RMuint32 slen;
	RMuint8 hash[20];
	Bignum r, s, w, gu1p, yu2p, gu1yu2p, u1, u2, sha, v;
	RMbool ret;
	
	if (! dss->p)
		return FALSE;
	r = get160(&sig, &siglen);
	s = get160(&sig, &siglen);
	if (!r || !s)
		return FALSE;
	// Step 1. w <- s^-1 mod q.
	w = modinv(s, dss->q);
	// Step 2. u1 <- SHA(message) * w mod q.
	RMSHA_Simple(data, datalen, hash);
	p = hash;
	slen = 20;
	sha = get160(&p, &slen);
	u1 = modmul(sha, w, dss->q);
	// Step 3. u2 <- r * w mod q.
	u2 = modmul(r, w, dss->q);
	// Step 4. v <- (g^u1 * y^u2 mod p) mod q.
	gu1p = modpow(dss->g, u1, dss->p);
	yu2p = modpow(dss->y, u2, dss->p);
	gu1yu2p = modmul(gu1p, yu2p, dss->p);
	v = modmul(gu1yu2p, One, dss->q);
	// Step 5. v should now be equal to r.
	ret = ! bignum_cmp(v, r);
	
	freebn(w);
	freebn(sha);
	freebn(gu1p);
	freebn(yu2p);
	freebn(gu1yu2p);
	freebn(v);
	freebn(r);
	freebn(s);
	
	return ret;
}

// checks the SRM data block signature
static RMbool DHCheckSignature(struct dss_key *dss, RMuint8 *sig, RMuint32 siglen, RMuint8 *data, RMuint32 datalen)
{
	if ((datalen < 8) || (siglen != 40)) {
		if (! manutest) fprintf(stderr, "[HDMI] Invalid SRM Block: %lu bytes data + %lu bytes signature\n", datalen, siglen);
		return RM_ERROR;
	}
	
	RMDBGLOG((LOCALDBG, "DHCheckSignature(%p, %lu, %p, %lu)\n", sig, siglen, data, datalen));	
	{
		RMuint32 i;
		for (i = 0; i < datalen; i++) {
			if (i % 8 == 0) RMDBGPRINT((LOCALDBG, "  SRM Data[%02X]: ", i));
			RMDBGPRINT((LOCALDBG, "%02X ", data[i]));
			if (i % 8 == 7) RMDBGPRINT((LOCALDBG, "\n"));
		}
		RMDBGPRINT((LOCALDBG, "\n"));
		for (i = 0; i < siglen; i++) {
			if (i % 8 == 0) RMDBGPRINT((LOCALDBG, "  SRM Sig.[%02X]: ", i));
			RMDBGPRINT((LOCALDBG, "%02X ", sig[i]));
			if (i % 8 == 7) RMDBGPRINT((LOCALDBG, "\n"));
		}
		RMDBGPRINT((LOCALDBG, "\n"));
	}
	
	// check DSS SHA1 signature
	return dss_verifysig(dss, sig, siglen, data, datalen);
}

static RMstatus DHParseSRMHeader(RMuint8 *pSRM, 
	RMuint32 *pSRMVer, 
	RMuint32 *pVRLStart, 
	RMuint32 *pVRLSize
) {
	RMstatus err = RM_OK;
	RMuint32 VecRevocListLength;
	
	// parse info from SRM
	if ((pSRM[0] & 0xF0) != 0x80) {
		RMDBGLOG((ENABLE, "No SRM ID found!\n"));
		return RM_ERROR;
	}
	
	if (pSRMVer) *pSRMVer = (pSRM[2] << 8) | pSRM[3];
	
	VecRevocListLength = (pSRM[5] << 16) | (pSRM[6] << 8) | pSRM[7];
	if (VecRevocListLength < 43) {
		RMDBGLOG((ENABLE, "SRM file format error!\n"));
		err = RM_ERROR;
	}
	if (pVRLStart) *pVRLStart = 8;
	if (pVRLSize) *pVRLSize = VecRevocListLength - 43;
	
	return err;
}

// Pointer to current SRM in app memory
// This is the SRM which is used by the HDMI code to check the display keys
RMstatus DHSetSRM(struct DH_control *pDH, RMuint8 *pSRM)
{
	RMstatus err;
	RMuint32 VRLStart, VRLSize;
	
	CHECK_pDH("DHSetSRM");
	CHECK_PTR("DHSetSRM", pSRM);
	
	// Check SRM again, just to be on the safe side
	err = DHParseSRMHeader(pSRM, NULL, &VRLStart, &VRLSize);
	if (RMFAILED(err)) {
		if (! manutest) fprintf(stderr, "[HDMI] SRM data format error!\n");
		return RM_ERROR;
	}
	if (! DHCheckSignature(&(pDH->SRM_dss), &pSRM[VRLStart + VRLSize], 40, pSRM, VRLStart + VRLSize)) {
		if (! manutest) fprintf(stderr, "[HDMI] SRM Signature does not match!\n");
		return RM_ERROR;
	}
	
	// Take new SRM
	pDH->SRM_Size = VRLStart + VRLSize + 40;
	if (pDH->SRM_Size > SRM_SIZE) {
		if (! manutest) fprintf(stderr, "[HDMI] SRM is too big, max size is %u bytes!\n", SRM_SIZE);
		pDH->SRM_Size = 0;
		return RM_ERROR;
	}
	RMMemcpy(pDH->SRM, pSRM, pDH->SRM_Size);
	
	// If HDCP is enabled, re-check with new SRM in place
	if ((pDH->state == DH_enabled_repeater_wait) || (pDH->state == DH_enabled_authenticated) || (pDH->state == DH_enabled_encrypted)) {
		RMuint64 t_now = RMGetTimeInMicroSeconds();
		RMDBGLOG((LOCALDBG, "HDCP Integrity Re-Check with new SRM after %lu mSec\n", 
			RMCyclesElapsed64(pDH->IntegrityLastCheck, t_now) / 1000));
		pDH->IntegrityLastCheck = t_now;
		err = DHVerifyIntegrity(pDH);
	}
	
	return RM_OK;
}

// Check DCP LLC signature of SRM and check if it is more recent than the one set by DHSetSRM()
// If *pUpdate is returned TRUE, the app needs to store the SRM from the disc in persistent memory.
// Suggested sequence: 
//   When app is started, app reads SRM from persistent memory, calls DHSetSRM()
//   Whenever a disc is mounted, app reads SRM from disc, calls DHValidateSRM()
//   If DHValidateSRM() sets *pUpdate to TRUE upon return, app writes disc SRM to persistent memory.
RMstatus DHValidateSRM(struct DH_control *pDH, RMuint8 *pSRM, RMbool *pUpdate)
{
	RMstatus err = RM_OK;
	RMuint32 SRMVer, SRMVerCurr, VRLStart, VRLSize;
	
	CHECK_PTR("DHValidateSRM", pUpdate);
	*pUpdate = FALSE;
	
	CHECK_pDH("DHValidateSRM");
	CHECK_PTR("DHValidateSRM", pSRM);
	
	// Parse SRM header
	err = DHParseSRMHeader(pSRM, &SRMVer, &VRLStart, &VRLSize);
	if (RMFAILED(err)) {
		if (! manutest) fprintf(stderr, "[HDMI] SRM data format error!\n");
		return RM_ERROR;
	}
	
	// Check SRM signature
	if (! DHCheckSignature(&(pDH->SRM_dss), &pSRM[VRLStart + VRLSize], 40, pSRM, VRLStart + VRLSize)) {
		if (! manutest) fprintf(stderr, "[HDMI] SRM Signature does not match!\n");
		return RM_ERROR;
	}
	
	// Check if new SRM supercedes current SRM
	err = DHParseSRMHeader(pDH->SRM, &SRMVerCurr, NULL, NULL);
	if (RMSUCCEEDED(err)) {
		if (SRMVer > SRMVerCurr) {
			if (! manutest) fprintf(stderr, "[HDMI] New SRM (V.%lu) is more recent than current SRM (V.%lu): replace!\n", SRMVer, SRMVerCurr);
			*pUpdate = TRUE;
		}
	} else {
		if (! manutest) fprintf(stderr, "[HDMI] Current SRM is invalid: replace with new SRM (V.%lu)!\n", SRMVer);
		*pUpdate = TRUE;
	}
	
	if (*pUpdate) {
		err = DHSetSRM(pDH, pSRM);
	}
	
	return err;
}

// check a number of 5 byte BKSVs against the SRMs
RMstatus DHCheckKeyRevocation(RMuint8 *pSRM, RMuint8 *BKSV, RMuint32 nBKSV, RMbool *revoked)
{
	RMstatus err = RM_OK;
	RMuint32 VRLStart, VRLSize, NumKeys;
	RMuint32 v, n, p, i, j, k, match;
	
	CHECK_PTR("DHCheckKeyRevocation", revoked);
	*revoked = FALSE;
	
	CHECK_PTR("DHCheckKeyRevocation", pSRM);
	CHECK_PTR("DHCheckKeyRevocation", BKSV);
	
	// parse info from SRM
	err = DHParseSRMHeader(pSRM, NULL, &VRLStart, &VRLSize);
	if (RMFAILED(err)) {
		if (! manutest) fprintf(stderr, "[HDMI] SRM data format error!\n");
		return RM_ERROR;
	}
	
	n = v = 0;
	p = VRLStart;
	while (p < VRLStart + VRLSize) {
		NumKeys = pSRM[p++] & 0x7F;
		if (p + NumKeys * 5 > VRLStart + VRLSize) {
			if (! manutest) fprintf(stderr, "[HDMI] SRM VRL Block #%lu data format error!\n", v);
			return RM_ERROR;
		}
		// Match BKSVs against revocation keys
		for (i = 0; i < NumKeys; i++) {
			RMDBGLOG((LOCALDBG, "KSV%02d: %02X %02X %02X %02X %02X\n", n, pSRM[p + 4], pSRM[p + 3], pSRM[p + 2], pSRM[p + 1], pSRM[p + 0]));
			if (CountBits(&pSRM[p + i * 5], 5) != 20) {
				RMDBGLOG((LOCALDBG, "Revokation key #%lu invalid!\n", n));
				continue;
			}
			for (k = 0; k < nBKSV; k++) {
				match = 0;
				for (j = 0; j < 5; j++) {
					if (pSRM[p + 5 - j] == BKSV[k * 5 + j]) match++;
				}
				if (match == 5) {
					if (! manutest) fprintf(stderr, "[HDMI] Display key #%lu [%02X %02X %02X %02X %02X] revoked by SRM key #%lu\n", 
						k, BKSV[k * 5 + 0], BKSV[k * 5 + 1], BKSV[k * 5 + 2], BKSV[k * 5 + 3], BKSV[k * 5 + 4], n);
					*revoked = TRUE;
				}
			}
			n++;
			p += 5;
		}
		v++;
	}
	RMDBGLOG((LOCALDBG, "Checked %lu display key%s against %lu SRM key%s\n", 
		nBKSV, (nBKSV == 1) ? "" : "s", 
		n, (n == 1) ? "" : "s"));
	
	return err;
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHGetRepeaterKSVList_CAT(struct DH_control *pDH,RMuint8 *pKSVList, RMuint8 devcount) 
{
    RMstatus err ;
    RMuint8 reg ;
    RMuint8 i ;
    
    RMDBGLOG((LOCALDBG,"DHGetRepeaterKSVList_CAT()\n")) ;
    if( devcount > 6 )
    {
        RMDBGLOG((LOCALDBG,"DHGetKSVList_CAT %d > maximum CAT6611 supported sink number 6.\n, failed.\n", devcount)) ;
        return RM_ERROR ;
    }
    
	DH_Clear_CAT6611DDCFIFO(pDH);
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x10, 1); // Read Rx DDC/EDID, with PC/host.
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x11, 0x74); // I2c PC DDC slave address
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x12, 0x43); // register address , 
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x13, devcount * 5 ); // Request byte.
	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x15, 0 ); // command, EDID read
    
    // to do ...
    // Should wait the interrupt or ?
    
    // when
	while(1)
	{
		RMMicroSecondSleep(10*1000); // Needs a minimum delay. We may need to increase it
		err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x16, &reg ) ;

		if( reg & (1<<7))
		{
		    // DDC done.
		    break ;
		}

		if( reg & ((1<<3)|(1<<5)))
		{
		    RMDBGLOG((ENABLE, "BlockReadSegment CAT6611 fail, ddc status = %02X\n",reg));
		    return RM_ERROR ;
		}
	}
	
	for( i = 0 ; i < devcount * 5 ; i++ )
    {
	    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x17, pKSVList+i ) ;
	}
	DH_Clear_CAT6611DDCFIFO(pDH);
    return err ;
}

static RMstatus DHGetVr_CAT(struct DH_control *pDH, RMuint8 *pVr)
{
    RMstatus err ;
    RMuint8 i,data ;
    
    if( !pDH || !pVr )
    {
        return RM_INVALID_PARAMETER ;
    }
    
    RMDBGLOG((LOCALDBG,"DHGetVr_CAT():\n")) ;
    while(1)
	{
        RMMicroSecondSleep(1*1000) ; // delay 1 ms
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x16, &data); // read DDC status
        if( data & 0x28 )
        {
            RMDBGLOG((ENABLE, "DDC status error, %02x\n", data));
            return RM_ERROR;
        }
        if( data & (1<<7))
        {
            RMDBGLOG((ENABLE, "DDC status done, %02x\n", data));
            break ;
        }
    } // wait for DDC idle.

    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x10, 0x1) ;
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x11, 0x74) ;
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x12, 0x20) ; // get 20 byte from HDCP reg20 
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x13, 20) ;
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x15, 0x0) ;

    while(1)
	{
        RMMicroSecondSleep(1*1000) ; // delay 1 ms
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x16, &data); // read DDC status
        if( data & 0x28 )
        {
            RMDBGLOG((ENABLE, "DDC status error, %02x\n", data));
            return RM_ERROR;
        }
        if( data & (1<<7))
        {
            RMDBGLOG((ENABLE, "DDC status done, %02x\n", data));
            break ;
        }
    } ;

    for( i = 0 ; i < 5 ; i++ )
    {
        err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x50, i) ; // select Vr[i][31..0]
        err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx),0x51,&pVr[i*4]) ; // Vr[i][7:0]
        err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx),0x52,&pVr[i*4+1]) ; // Vr[i][15:8]
        err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx),0x53,&pVr[i*4+2]) ; // Vr[i][23:16]
        err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx),0x54,&pVr[i*4+3]) ; // Vr[i][31:24]
    }
    
    return err ;
    
}

static RMstatus 
DHResumeRepeaterAut_CAT(struct DH_control *pDH, RMbool bSuccess)
{
    RMstatus err ;
    RMuint8 uc ;
    
    if(!pDH)
    { 
        return RM_INVALID_PARAMETER ;
    }
    
    while(1)
    {
        err = DH_i2c_read(pDH->pRUA,&(pDH->i2c_tx), 0x16, &uc ) ;
        
        if( uc & ((1<<5)|(1<<4)|(1<<3)))
        {
            err = DHAbortDDC_CAT6611(pDH) ; 
            break ;
        }
        
        if( uc & (1<<7))
        {
            break ;
        }
        
    }
    
    DH_Clear_CAT6611DDCFIFO(pDH) ;
    
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x10, 0 ) ;
    err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0x22, 1 | (bSuccess)?0:(1<<1)) ;
    return err ;
}


//~jj_tseng@chipadvanced.com 2007/03/23

// retreive the BKSVs from all devices attached to the repeater, then check against SRM
RMstatus DHCheckRepeaterKSV(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMuint8 b[3], Data[640 + 64], V[20], Vr[20];
	RMuint32 i, n, device_count, chksum;
	RMbool revoked, verified;
	RMSHA_State s;
	RMuint16 Bstatus;
	
	CHECK_pDH("DHCheckRepeaterKSV");
	
	//RMDBGLOG(("DHCheckRepeaterKSV\n")) ;
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHDisableEncryption with uninitialized device!\n"));
		return RM_ERROR;
	}
	
    // 2007/03/23 Modified by jj_tseng@chipadvanced
    if( pDH->part == DH_CAT6611 )
    {
        err = DHGetBCaps_CAT(pDH, b) ;
        err = DHGetBStat_CAT(pDH, &b[1]) ;
    }
    else
    {
    	err = DHDDCBlockRead(pDH, DDC_HDCP_DEV, 0x40, b, 3);  // Bcaps/Bstatus
    }
    //~jj_tseng@chipadvanced.com 2007/03/23

	if (RMFAILED(err)) {
		if (! manutest) fprintf(stderr, "[HDMI] Can not read Bcaps/Bstatus from HDCP Repeater!\n");
		return err;
	}
	if (! (b[0] & 0x20)) {  // KSV FIFO Ready?
		RMDBGLOG((LOCALDBG, "Repeater key is not ready yet, try again later.\n"));
		return RM_OK;
	}
	Bstatus = b[1] | (b[2] << 8);
	RMDBGLOG((LOCALDBG, "Repeater Bstatus: 0x%04lX\n", Bstatus));
	if (Bstatus & 0x0880) {
		if (! manutest) fprintf(stderr, "[HDMI] HDCP Repeater error: %s%s%s!\n", 
			(Bstatus & 0x0080) ? "too many devices" : "", 
			((Bstatus & 0x0880) == 0x0880) ? " and " : "", 
			(Bstatus & 0x0800) ? "cascade depth exceeded" : "");
		return RM_ERROR;
	}
	
	device_count = Bstatus & 0x7F;
	
	// read BKSV for each device connected to Repeater
	
	// 2007/03/23 Modified by jj_tseng@chipadvanced
	if(pDH->part == DH_CAT6611)
	{
	    if( device_count > 6 )
	    {
	        return RM_ERROR ;
	    }
	    err = DHGetRepeaterKSVList_CAT(pDH, Data, device_count)  ;
	}
	else
	{
    	err = DHDDCBlockRead(pDH, DDC_HDCP_DEV, 0x43, Data, device_count * 5);
	}
    //~jj_tseng@chipadvanced.com 2007/03/23

	for (i = 0; i < device_count; i++) {
		RMDBGLOG((LOCALDBG, "Repeated BKSV[%lu]: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n", i, 
			Data[i * 5 + 0], Data[i * 5 + 1], Data[i * 5 + 2], Data[i * 5 + 3], Data[i * 5 + 4]));
	}
	
	// add more info to Data for V calculation
	n = device_count * 5;
	Data[n++] = Bstatus & 0xFF;
	Data[n++] = Bstatus >> 8;
	switch (pDH->part) {
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		// TODO need actual HDCP M0 value from authentication, not available from SiI9030!
		Data[n++] = 0x8F;  // (not the right) M0 value
		Data[n++] = 0xE7;
		Data[n++] = 0xBB;
		Data[n++] = 0x38;
		Data[n++] = 0xCE;
		Data[n++] = 0x3D;
		Data[n++] = 0x2D;
		Data[n++] = 0x37;
		break;
	case DH_ANX9030:
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0xD0, &(Data[n]), 8);
		n += 8;
		break;
    // 2007/03/23 Modified by jj_tseng@chipadvanced
	case DH_CAT6611:
		// TODO need actual HDCP M0 value from authentication, not available from SiI9030!
		Data[n++] = 0x8F;  // (not the right) M0 value
		Data[n++] = 0xE7;
		Data[n++] = 0xBB;
		Data[n++] = 0x38;
		Data[n++] = 0xCE;
		Data[n++] = 0x3D;
		Data[n++] = 0x2D;
		Data[n++] = 0x37;
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		RMDBGLOG((LOCALDBG, "Part (%d) does not support HDCP\n", pDH->part));
		n += 8;
		err = RM_ERROR;
	}
	Data[n++] = 0x80;  // end marker?
	i = n;
	n = ((n + 63) / 64) * 64;  // data ends on next 64 byte border
	for (; i < n; i++) Data[i] = 0x00;  // clear remaining Data
	// TODO how to calc checksum? Not described in HDCP 1.1 spec
	chksum = 0;
	for (i = 0; i < n; i++) chksum += Data[i];
	//Data[n - 1] = (chksum >> 8) & 0xFF;
	//Data[n - 2] = chksum & 0xFF;
	
	// read V' from Repeater
	if(pDH->part == DH_CAT6611 )
	{
    	err = DHGetVr_CAT(pDH, Vr) ;
	}
	else
	{
    	err = DHDDCBlockRead(pDH, DDC_HDCP_DEV, 0x20, Vr, 20);
	}
	
	for (i = 0; i < 5; i++) {
		RMDBGLOG((LOCALDBG, "Repeater V'H%lu: 0x%02X 0x%02X 0x%02X 0x%02X\n", i, 
			Vr[i * 4 + 0], Vr[i * 4 + 1], Vr[i * 4 + 2], Vr[i * 4 + 3]));
	}
	
	// calculate own V vector, compare to V'
	verified = TRUE;
	RMSHA_Init(&s);
	for (i = 0; i < n; i += 64) {
		RMSHA_Bytes(&s, &Data[i], 64);
	}
	RMSHA_Final(&s, V);
	for (i = 0; i < 5; i++) {
		RMDBGLOG((LOCALDBG, "Transmt. V H%lu: 0x%02X 0x%02X 0x%02X 0x%02X\n", i, 
			V[i * 4 + 0], V[i * 4 + 1], V[i * 4 + 2], V[i * 4 + 3]));
		if (
			(V[i * 4 + 0] != Vr[i * 4 + 0]) || 
			(V[i * 4 + 1] != Vr[i * 4 + 1]) || 
			(V[i * 4 + 2] != Vr[i * 4 + 2]) || 
			(V[i * 4 + 3] != Vr[i * 4 + 3])
		) {
			// TODO: verified = FALSE;
		}
	}
	if (verified) {
		RMDBGLOG((LOCALDBG, "BKSV Signature is correct!\n"));
	} else {
		if (! manutest) fprintf(stderr, "[HDMI] HDCP BKSV Signature does not match!\n");
		DHMuteOutput(pDH, TRUE);
		return DHDisableEncryption(pDH);
	}
	
	err = DHCheckKeyRevocation(pDH->SRM, Data, device_count, &revoked);
	if (RMSUCCEEDED(err) && revoked) {
		if (! manutest) fprintf(stderr, "[HDMI] HDCP Key error: Invalid Display!\n");
		return RM_DRM_INVALID_KEY;
	}
	
	if( pDH->part == DH_CAT6611 )
    {
        err = DHResumeRepeaterAut_CAT(pDH, TRUE) ;
    }
	pDH->state = DH_enabled_authenticated;
	RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/AUTHENTICATED\n"));
	
	return RM_OK;
}

RMstatus DHCheckRevocationList(struct DH_control *pDH, RMuint8 *pSRM, RMbool *revoked)
{
	RMstatus err = RM_OK;
	RMuint8 BKSV[5];
	
	CHECK_PTR("DHCheckRevocationList", revoked);
	*revoked = FALSE;
	
	CHECK_pDH("DHCheckRevocationList");
	CHECK_PTR("DHCheckRevocationList", pSRM);
	
	if (! pDH->part_caps.HDCP) {
		RMDBGLOG((LOCALDBG, "Call to DHCheckRevocationList, but chip does not support HDCP\n"));
		return RM_ERROR;
	} else if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHCheckRevocationList with uninitialized device!\n"));
		return RM_ERROR;
	}
	
	// Get BKSV from HDMI receiver
	if (RMFAILED(err = DHGetBKSV(pDH, BKSV))) {
		RMDBGLOG((LOCALDBG, "Cannot get receiver key\n"));
		return err;
	}
	RMDBGLOG((DISABLE, "DHCheckRevocationList() BKSV: %02X %02X %02X %02X %02X\n", BKSV[0], BKSV[1], BKSV[2], BKSV[3], BKSV[4]));
	
	err = DHCheckKeyRevocation(pSRM, BKSV, 1, revoked);
	if (RMSUCCEEDED(err) && *revoked) {
		RMDBGLOG((ENABLE, "HDCP Key error: Invalid Display!\n"));
		return RM_DRM_INVALID_KEY;
	}
	
	return RM_OK;
}

RMstatus DHGetConnection(struct DH_control *pDH, enum DH_connection *cable, RMbool *Rx, RMbool *HPD)
{
	RMstatus err = RM_OK;
	RMuint8 reg;
	RMbool hpd = FALSE, rx = FALSE;
	
	CHECK_pDH("DHGetConnection");
	CHECK_PTR("DHGetConnection", cable);
	
	RMDBGLOG((LOCALDBG,"DHGetConnection(pDH,")) ;
	
	
	switch(pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x09, &reg); // Read HTPLG | RSEN (check monitor is connected and/or switched on)
		hpd = (reg & 0x02) ? TRUE : FALSE;
		rx = (reg & 0x04) ? TRUE : FALSE;
		break;
	case DH_ANX9030:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x06, &reg); // Read HTPLG | RSEN (check monitor is connected and/or switched on)
		hpd = (reg & 0x08) ? TRUE : FALSE;
		rx = (reg & 0x01) ? TRUE : FALSE;
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0E, &reg) ; // Read HTPLG | RSEN
        hpd = (reg & 0x40) ? TRUE : FALSE ;
        rx = (reg & 0x20) ? TRUE : FALSE ;
	RMDBGLOG((ENABLE, "system status = %02X, hpd = %d, rx = %d\n", reg, hpd, rx)) ;
        if( !hpd || !rx )
        {
            // if not plugged, clear the hdcpreg flag.
            pDH->CAT_hdcpreg_ready = FALSE ;
        }
        break ;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		err = RM_ERROR;
	}
	
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "Can not read HotPlug status!\n"));
		return err;
	}
	if (pDH->HotPlugGuarded) hpd = FALSE;  // hold HPD artificially low
	if (HPD) *HPD = hpd;
	if (Rx) *Rx = rx;

	RMDBGLOG((ENABLE,"*HPD = %d, *Rx = %d, hpd = %d, rx = %d\n",*HPD, *Rx, hpd, rx  )) ;
	RMDBGLOG((ENABLE, "Receiver present = %c, HotPlug active = %c\n", rx ? 'y' : 'n', hpd ? 'y' : 'n'));
	if (rx) pDH->RxPresent = TRUE;
	if (hpd && (*cable == DH_disconnected) && (! pDH->RxLost)) {
		RMDBGLOG((LOCALDBG, "HotPlug active%s.\n", rx ? " and Receiver present" : ""));
		pDH->RxPresent = rx;
		*cable = DH_connected;
	} else if ((! hpd) && ((*cable != DH_disconnected) || pDH->RxLost)) {
		RMDBGLOG((LOCALDBG, "Lost HotPlug%s\n", (pDH->RxPresent && (! pDH->RxLost) && (! rx)) ? " and Receiver" : ""));
		pDH->RxLost = FALSE;
		*cable = DH_disconnected;
	}
	if (hpd && pDH->RxPresent) {  // monitor Rx state while HPD active
		if ((! rx) && (! pDH->RxLost) && (*cable != DH_disconnected)) {
			RMDBGLOG((LOCALDBG, "Lost Receiver\n"));
			pDH->RxLost = TRUE;
			*cable = DH_disconnected;
		} else if (rx && pDH->RxLost) {
			pDH->RxLost = FALSE;
			if (*cable == DH_disconnected) {
				RMDBGLOG((LOCALDBG, "Receiver present"));
				*cable = DH_connected;
			}
		}
	}
	RMDBGPRINT((LOCALDBG,"cable = %d, Rx = %s, HPD = %s\n",*cable,(*Rx)?"TRUE":"FALSE",(*HPD)?"PLUG":"Unplug")) ;
	
	return RM_OK;
}

// Read current status from the hardware and update pDH members.
// This function is for secondary access (e.g. by the audio code)
// to the chip and can be called instead of DHVerifyIntegrity()
RMstatus DHUpdateState(struct DH_control *pDH, 
	enum DH_device_state *pState, 
	enum DH_connection *pCable, 
	RMbool *pRx, RMbool *pHPD, 
	RMbool *pUpdateEDID)
{
	RMstatus err = RM_OK;
	RMuint8 reg;
	RMbool hpd = FALSE, rx = FALSE, tmds = FALSE, hdcp = FALSE;
	
	if (pUpdateEDID) *pUpdateEDID = FALSE;
	
	CHECK_pDH("DHUpdateState");
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHUpdateState with uninitialized chip\n"));
		return RM_ERROR;
	}
	
	RMDBGLOG((LOCALDBG,"DHUpdateState()")) ;
	hpd = FALSE;
	rx = FALSE;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &reg);
		if (RMFAILED(err)) break;
		tmds = (reg & 0x01) ? TRUE : FALSE;
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x09, &reg); // Read HTPLG | RSEN (check monitor is connected and/or switched on)
		if (RMFAILED(err)) break;
		hpd = (reg & 0x02) ? TRUE : FALSE;
		rx = (reg & 0x04) ? TRUE : FALSE;
		hdcp = FALSE;
		if (pDH->part_caps.HDCP && (pDH->state != DH_disabled)) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &reg);
			if (RMFAILED(err)) break;
			hdcp = (reg & 0x40) ? TRUE : FALSE;
		}
		break;
	case DH_ANX9030:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &reg);
		if (RMFAILED(err)) break;
		tmds = (reg & 0x01) ? TRUE : FALSE;
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x06, &reg); // Read HTPLG | RSEN (check monitor is connected and/or switched on)
		if (RMFAILED(err)) break;
		hpd = (reg & 0x08) ? TRUE : FALSE;
		rx = (reg & 0x01) ? TRUE : FALSE;
		hdcp = FALSE;
		if (pDH->part_caps.HDCP && (pDH->state != DH_disabled)) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA0, &reg);
			if (RMFAILED(err)) break;
			hdcp = (reg & 0x04) ? TRUE : FALSE;
		}
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &reg);
		if (RMFAILED(err)) break;
		tmds = (reg & 0x08) ? FALSE : TRUE; 
		hdcp = FALSE;
		
		if (pDH->part_caps.HDCP && (pDH->state != DH_disabled)) {
			hdcp = (reg & 0x01) ? FALSE : TRUE ; // judge the hdcp by if the hdcp reset setted.
		}
		
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0E, &reg); // Read HTPLG | RSEN (check monitor is connected and/or switched on)
		if (RMFAILED(err)) break;
		
		hpd = (reg & 0x40) ? TRUE : FALSE;
		rx = (reg & 0x20) ? TRUE : FALSE;
        if( !hpd || !rx )
        {
            // if not plugged, clear the hdcpreg flag.
            pDH->CAT_hdcpreg_ready = FALSE ;
        }
        break ;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		err = RM_ERROR;
	}
	
	if (RMFAILED(err)) {
		RMDBGLOG((LOCALDBG, "Can not read status!\n"));
		return err;
	}
	
	if (pDH->state == DH_disabled) {
		if (tmds) {
			pDH->state = DH_enabled;
			RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		}
	} else {
		if (! tmds) {
			pDH->state = DH_disabled;
			RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: DISABLED/NOT ENCRYPTED\n"));
		}
	}
	
	if (pDH->state == DH_enabled) {
		if (hdcp) {
			pDH->state = DH_enabled_encrypted;
			RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/ENCRYPTED\n"));
		}
	} else {
		if (! hdcp) {
			pDH->state = DH_enabled;
			RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		}
	}
	
	if (pDH->HotPlugGuarded) hpd = FALSE;  // hold HPD artificially low
	if (rx) pDH->RxPresent = TRUE;
	if (hpd && (pDH->cable == DH_disconnected) && (! pDH->RxLost)) {
		RMDBGLOG((LOCALDBG, "HotPlug active%s.\n", rx ? " and Receiver present" : ""));
		pDH->RxPresent = rx;
		pDH->cable = DH_connected;
		if (pUpdateEDID) *pUpdateEDID = TRUE;
	} else if ((! hpd) && ((pDH->cable != DH_disconnected) || pDH->RxLost)) {
		RMDBGLOG((LOCALDBG, "Lost HotPlug%s\n", (pDH->RxPresent && (! pDH->RxLost) && (! rx)) ? " and Receiver" : ""));
		pDH->RxLost = FALSE;
		pDH->cable = DH_disconnected;
	}
	if (hpd && pDH->RxPresent) {  // monitor Rx state while HPD active
		if ((! rx) && (! pDH->RxLost) && (pDH->cable != DH_disconnected)) {
			RMDBGLOG((LOCALDBG, "Lost Receiver\n"));
			pDH->RxLost = TRUE;
			pDH->cable = DH_disconnected;
		} else if (rx && pDH->RxLost) {
			pDH->RxLost = FALSE;
			if (pDH->cable == DH_disconnected) {
				RMDBGLOG((LOCALDBG, "Receiver present"));
				pDH->cable = DH_connected;
				if (pUpdateEDID) *pUpdateEDID = TRUE;
			}
		}
	}
	RMDBGPRINT((LOCALDBG,"State = %d, Cable = %d, Rx = %s, HPD = %s, UpdateEDID = %s\n",*pState,*pCable,*pRx,*pHPD,*pUpdateEDID)) ;
    RMDBGPRINT((LOCALDBG,"pDH->RxLost = %s, pDH->RxPresent = %s, pDH->cable = %d\n",pDH->RxLost, pDH->RxPresent, pDH->cable)) ;
    
	return RM_OK;
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHVerifyIntegrity_CAT(struct DH_control *pDH)
{
    RMstatus err = RM_OK ;
    RMuint8 reg ;
    
    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &reg ) ;
    if( reg & 1 )
    {
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0C, (1<<2) ) ; // clear authentication fail interrupt.
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0D, 0) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0E, &reg );
        reg &= ~3 ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0E, reg );
        DH_Clear_CAT6611INT(pDH) ;
        return RM_ERROR ;
    }
    
    return err ;
}
//~jj_tseng@chipadvanced.com 2007/03/23

RMstatus DHVerifyIntegrity(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	enum DH_device_state old_state;
	
	CHECK_pDH("DHVerifyIntegrity");
	RMDBGLOG((LOCALDBG,"DHVerifyIntegrity()")) ;
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((ENABLE, "Call to DHVerifyIntegrity with uninitialized device!\n"));
		return RM_ERROR;
	}
	
	// disabled output?
	if ((pDH->state == DH_disabled) || (pDH->cable == DH_disconnected)) {
		// TMDS is off, nothing to do
		return RM_OK;
	}
	
	old_state = pDH->state;
	
	// re-authenticate?
	if ((pDH->state != DH_enabled_repeater_wait) && (pDH->state != DH_enabled_authenticated) && (pDH->state != DH_enabled_encrypted)) {
		RMDBGLOG((LOCALDBG, "DHVerifyIntegrity() ---AUTHENTICATE---\n"));
		err = DHAuthenticate(pDH);
		if (RMFAILED(err)) {
			if (! manutest) fprintf(stderr, "[HDMI] DHVerifyIntegrity() HDCP Authentication failed!\n");
			if (err == RM_DRM_INVALID_KEY) {
				DHDisableOutput(pDH);
			} else {
				DHMuteOutput(pDH, TRUE);
			}
			return err;
		}
	}
	
	// for repeater: check KSVs, transition to DH_enabled_authenticated
	if (pDH->state == DH_enabled_repeater_wait) {
		RMuint64 t1;
		RMDBGLOG((LOCALDBG, "DHVerifyIntegrity() ---CHECK REPEATER KSV---\n"));
		t1 = RMGetTimeInMicroSeconds();
		if (RMCyclesElapsed64(pDH->RepeaterTimeout, t1) > REPEATER_TIMEOUT) {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP Repeater Timeout! Authentication re-try.\n");
			err = RM_TIMEOUT;
		} else {
			err = DHCheckRepeaterKSV(pDH);
		}
		if (RMFAILED(err)) {
			if (! manutest) fprintf(stderr, "[HDMI] DHVerifyIntegrity() HDCP Repeater validation failed!\n");
			if (err == RM_DRM_INVALID_KEY) {
				DHDisableOutput(pDH);
			} else {
				DHMuteOutput(pDH, TRUE);
				pDH->state = DH_enabled;  // force re-authentication
				RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
			}
			return err;
		}
	}
	
	// authenticated: enable encryption and check SRMs
	if (pDH->state == DH_enabled_authenticated) {
		RMDBGLOG((LOCALDBG, "DHVerifyIntegrity() ---ENABLE HDCP---\n"));
		err = DHEnableEncryption(pDH);
		if (RMFAILED(err)) {
			if (! manutest) fprintf(stderr, "[HDMI] Failed to enable HDCP encryption!\n");
			return err;
		}
		if (pDH->state == DH_enabled_encrypted) {
			if (pDH->state != old_state) {
				RMDBGLOG((LOCALDBG, "HDCP encrytion established, OK to play protected content!\n"));
			}
			DHMuteOutput(pDH, FALSE);  // unmute
			
			// wait for ENC_ON bit, up to 50 msec
			{
				RMuint8 reg;
				RMuint64 t0, t1;
				RMbool hdcp = FALSE;
				
				t0 = RMGetTimeInMicroSeconds();
				do {
					switch (pDH->part) {
					case DH_siI164:
					case DH_siI170:
					case DH_siI9030:
					case DH_siI9034:
					case DH_siI9134:
						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &reg);
						if (RMFAILED(err)) break;
						hdcp = (reg & 0x40) ? TRUE : FALSE;
						break;
					case DH_ANX9030:
						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA0, &reg);
						if (RMFAILED(err)) break;
						hdcp = (reg & 0x04) ? TRUE : FALSE;
hdcp = TRUE;
						break;
                    case DH_CAT6611:
						hdcp = TRUE ;
						err = RM_OK ;
                        break ;						
					default:
						err = RM_ERROR;
					}
					if (RMFAILED(err)) {
						RMDBGLOG((LOCALDBG, "Can not read status!\n"));
						hdcp = FALSE;
					}
					t1 = RMGetTimeInMicroSeconds();
				} while ((RMCyclesElapsed64(t0, t1) < 50000) && ! hdcp);
				if (! hdcp) {
					RMDBGLOG((ENABLE, "Encryption still not ON after 50 msec!\n"));
					err = RM_ERROR;
				} else {
					if (! manutest) fprintf(stderr, "[HDMI] HDCP is now enabled.\n");
					err = RM_OK;
				}
			}
		}
	} else 
	
	// maintain HDCP integrity, once encryption is enabled, but not immediately after enabling
	if (pDH->state == DH_enabled_encrypted) {
		switch(pDH->part) {
		case DH_siI170:
			err = DHVerifyIntegrity_SiI170(pDH, TRUE);
			break;
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			err = DHVerifyIntegrity_SiI9030(pDH, TRUE);
			break;
		case DH_ANX9030:
			err = RM_OK;
			// TODOANX
			break;
        // 2007/03/23 Added by jj_tseng@chipadvanced
        case DH_CAT6611:
            err = DHVerifyIntegrity_CAT(pDH) ;
            break ;
        //~jj_tseng@chipadvanced.com 2007/03/23
        			
		default:
			RMDBGLOG((LOCALDBG, "Part (%d) does not support HDCP\n", pDH->part));
			err = RM_ERROR;
		}
	}
	RMDBGPRINT((LOCALDBG,"err = %s\n",RMSUCCEEDED(err)?"SUCCESS":"FAIL")) ;
	
	if (RMFAILED(err) || ((pDH->state != DH_enabled_encrypted) && (pDH->state != DH_enabled_repeater_wait))) {
		if (pDH->state != old_state) {
			if (! manutest) fprintf(stderr, "[HDMI] HDCP encrytion failed, can NOT play protected content!\n");
		}
		DHMuteOutput(pDH, TRUE);
		DHDisableEncryption(pDH);
		// Force authentication next time
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
		err = RM_ERROR;
	}
	
	return err;
}

// enable HDCP or, if that fails, mute output
RMstatus DHRequestHDCP(struct DH_control *pDH) 
{
	CHECK_pDH("DHRequestHDCP");
	
	if (! manutest) fprintf(stderr, "[HDMI] DHRequestHDCP()\n");
	if (! pDH->CPDesired) {
		pDH->CPDesired = TRUE;
		
		if ((pDH->state != DH_disabled) && (pDH->state != DH_enabled_encrypted)) {
			RMuint64 t_now = RMGetTimeInMicroSeconds();
			if ((pDH->state == DH_enabled) || (RMCyclesElapsed64(pDH->IntegrityLastCheck, t_now) > HDCP_GUARD_TIME * 1000)) {
				RMDBGLOG((LOCALDBG, "HDCP Initial Integrity Check\n"));
				pDH->IntegrityLastCheck = t_now;
				return DHVerifyIntegrity(pDH);
			} else {
				return DHMuteOutput(pDH, TRUE);
			}
		}
	}
	return RM_OK;
}

// disable HDCP and unmute output
RMstatus DHCancelHDCP(struct DH_control *pDH) 
{
	CHECK_pDH("DHCancelHDCP");
	
	if (! manutest) fprintf(stderr, "[HDMI] DHCancelHDCP()\n");
	if (pDH->CPDesired) {
		pDH->CPDesired = FALSE;
		if (pDH->state != DH_disabled) {
			if (pDH->state != DH_enabled) {
				DHDisableEncryption(pDH);
				if (pDH->state == DH_enabled_authenticated) {
					pDH->state = DH_enabled;
					RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
				}
			}
			RMDBGLOG((LOCALDBG, "Enabling UNENCRYPTED output, as requested by application\n"));
			DHMuteOutput(pDH, FALSE);
		}
	}
	return RM_OK;
}

// Read EDID into pDH cache
// returns RM_PENDING when EDID is same as previous one
RMstatus DHReadEDID(struct DH_control *pDH)
{
	RMstatus err;
	struct EDID_Data *pEDID = (struct EDID_Data *)(pDH->EDID[0]);
	RMbool same_EDID = TRUE;
	RMuint32 prev_EDID_blocks;
	RMuint8 prev_EDID[EDID_SIZE];
	struct CEA861BDataBlockCollection DBC;
	
	CHECK_pDH("DHReadEDID");
	
	RMDBGLOG((LOCALDBG, "Reading EDID from display\n"));
	
	prev_EDID_blocks = pDH->EDID_blocks;
	pDH->EDID_blocks = 0;
	do {
		if (pDH->EDID_blocks < prev_EDID_blocks) {
			RMMemcpy(prev_EDID, pDH->EDID[pDH->EDID_blocks], EDID_SIZE);
		}
		err = DHLoadEDIDBlock(pDH, pDH->EDID_blocks, pDH->EDID[pDH->EDID_blocks], EDID_SIZE);
		if (RMFAILED(err)) break;
		if (! pDH->EDID_blocks) {
			if (
				(pEDID->EDID_Header[0] != 0x00) || 
				(pEDID->EDID_Header[1] != 0xFF) || 
				(pEDID->EDID_Header[2] != 0xFF) || 
				(pEDID->EDID_Header[3] != 0xFF) || 
				(pEDID->EDID_Header[4] != 0xFF) || 
				(pEDID->EDID_Header[5] != 0xFF) || 
				(pEDID->EDID_Header[6] != 0xFF) || 
				(pEDID->EDID_Header[7] != 0x00)
			) break;
		}
		if (pDH->EDID_blocks < prev_EDID_blocks) {
			RMuint32 i;
			for (i = 0; i < EDID_SIZE; i++) {
				if (pDH->EDID[pDH->EDID_blocks][i] != prev_EDID[i]) same_EDID = FALSE;
			}
		}
		pDH->EDID_blocks ++;
		if (pDH->EDID_blocks >= EDID_BLOCKS) break;  // no more space
		if (pDH->EDID_blocks > pEDID->EDID_Extension) break;  // no more extension blocks
	} while (1);
	if (pDH->EDID_blocks != prev_EDID_blocks) same_EDID = FALSE;
	if (pDH->EDID_blocks) {
		RMDBGLOG((LOCALDBG, "New EDID is %s previous one.\n", same_EDID ? "same as" : "dfferent from"));
		if (! same_EDID) {
			RMuint32 i;
			pDH->CEA_Support = FALSE;
			pDH->CEA_QuantSupport = FALSE;
			for (i = 1; i < pDH->EDID_blocks; i++) {
				err = DHGetCEADataBlockCollection(pDH->EDID[i], EDID_SIZE, &DBC);
				if (RMSUCCEEDED(err)) {
					pDH->CEA_Support = pDH->CEA_Support || DBC.HDMI_sink;
					if (DBC.VideoValid) {
						pDH->CEA_QuantSupport = pDH->CEA_QuantSupport || (DBC.VideoCapability & TV_SUPPORT_VIDEO_QUANTISATION);
					}
				}
			}
		}
	} else {
		RMDBGLOG((LOCALDBG, "No valid EDID found!\n"));
	}
	
	return pDH->EDID_blocks ? (same_EDID ? RM_PENDING : RM_OK) : RM_ERROR;
}

// App. can call this function to check for a pending interrupt on the SiI9030
RMstatus DHCheckInterrupt(struct DH_control *pDH, RMbool *pInterrupt)
{
	RMstatus err = RM_OK;
	RMuint8 intr;
	
	CHECK_pDH("DHCheckInterrupt");
	CHECK_PTR("DHCheckInterrupt", pInterrupt);
	
	
	*pInterrupt = TRUE;
	// Check for notification of changes in interrupt bits
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		if (! pDH->ForceIntegrityCheck) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &intr);
			if (RMSUCCEEDED(err) && (intr & 0x01)) {  // TMDS is powered up, ... 
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x70, &intr);
				if (RMSUCCEEDED(err) && ! (intr & 0x01)) {  // ... but no interrupt is pending
					*pInterrupt = FALSE;
				}
			}
		}
		break;
	case DH_ANX9030:
		if (! pDH->ForceIntegrityCheck) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &intr);
			if (RMSUCCEEDED(err) && (intr & 0x01)) {  // TMDS is powered up, ... 
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x40, &intr);
				if (RMSUCCEEDED(err) && ! (intr & 0x01)) {  // ... but no interrupt is pending
					*pInterrupt = FALSE;
				}
			}
		}
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		if (! pDH->ForceIntegrityCheck) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0E, &intr);
			if (RMSUCCEEDED(err) && (!(intr & 0x80))){  // TMDS is powered up, ... 
				*pInterrupt = FALSE;
			}
		}
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		break;
	}

    RMDBGLOG((LOCALDBG,"DHCheckInterrupt(Interrupt = %s)\n",(*pInterrupt)?"TRUE":"FALSE")) ;
	
	return err;
}

/** 
  This function performs all neccessary checks for HDMI/DVI and HDCP
  
  All members of the parameter struct will be filled, unless the pointer is NULL.
  
  If Hot Plug became active since the last call, the EDID is read into an internal
  cache. If Hot Plug becomes inactive, the TMDS is turned off and the EDID cache is 
  invalidated. HotPlugChanged and HotPlugState are set accordingly.
  
  ReceiverSignal is the only member of the struct that needs to be defined.
  Set it to TRUE if it is certain, this hardware supports the Rx signal.
  If set to FALSE, the Rx signal availablity will be determined, but if the
  first call will be made with a plugged in and switched off display, 
  this display will be detected as active. ReceiverSignal, ReceiverChanged
  and ReceiverState will be set according to the current state.
  
  A display is active when HotPlugState is TRUE, and 
  ReceiverSignal equals ReceiverState.
  
  HDCPLost will be set when the HDCP connection will become unavailable.
  HDCPState reflects wether the HDCP encrytion is in progress or not.
  
  When the input clock into the HDMI chip becomes stable, a software reset
  of the internal state machines will be performed. InputClockChanged and
  InputClockStable are set accordingly.
  
  TMDSState reflects wether the output signal of the chip is enabled or not.
*/

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHCheckHDMI_CAT6611(struct DH_control *pDH, struct DH_HDMI_state *pHDMIState)
{
	RMstatus err;
	// RMuint8 reg, intr, intr1, intr2, intr3, sys_rst, sys_stat, hdcp_ctrl, hdcp_stat;
	RMuint8 intr, intr1, intr2, intr3, sys_rst, sys_stat, hdcp_ctrl;
	
	RMbool use_intr = FALSE, use_intr_hpd = FALSE, use_intr_rx = FALSE, use_intr_hdcp = FALSE;
	RMbool sys_stat_hpd, sys_stat_rx, sys_stat_vidstable;
	RMbool ri_128 = FALSE;
	RMbool prev_HotPlug;
	
	CHECK_pDH("DHCheckHDMI_CAT6611");
	
	// RMDBGLOG((ENABLE,"DHCheckHDMI_CAT6611()\n")) ;
	
	// Notification by app or DHGetConnection() that Rx signal is supported
	if (pDH->RxPresent || (pHDMIState && pHDMIState->ReceiverSignal)) {
		pDH->HDMIState.ReceiverSignal = TRUE;
	}
	
	// Housekeeping
	prev_HotPlug = pDH->HotPlugState;
	pDH->HotPlugChanged = FALSE;	
	pDH->HDMIState.HotPlugChanged = FALSE; // reg06[0]
	pDH->HDMIState.ReceiverChanged = FALSE; // reg06[1]
	pDH->HDMIState.HDCPLost = FALSE;        // reg06[2][4], reg07[0] 
	pDH->HDMIState.InputClockChanged = FALSE; // reg08[4]

	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0E, &sys_stat);
	if (RMSUCCEEDED(err)) {
		sys_stat_hpd = (sys_stat & 0x40) ? TRUE : FALSE;
		sys_stat_rx = (sys_stat & 0x20) ? TRUE : FALSE;
	} else {
		RMDBGLOG((DISABLE, "Failed to read SYS_STAT register!\n"));
		return err;
	}
	
	// Check output state
	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &sys_rst);
	if (RMSUCCEEDED(err)) {
		pDH->HDMIState.TMDSState = (sys_rst & 0x20) ? FALSE:TRUE;
	} else {
		RMDBGLOG((DISABLE, "Failed to read SYS_RST register!\n"));
	}

	if( sys_stat_hpd || sys_stat_rx )
	{
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &sys_rst);
		sys_rst &= 0xC7 ;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, sys_rst);
	}
	sys_stat_vidstable = (sys_stat & 0x10) ? TRUE : FALSE;
	
	// Update pDH->state
	if ((pDH->state == DH_disabled) && pDH->HDMIState.TMDSState) 
	{
		// pDH->state = DH_enabled;
		err = DHSetIntMask_CAT6611(pDH) ;
		// RMDBGLOG((DISABLE, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
	} 
	else if ((pDH->state != DH_disabled) && ! pDH->HDMIState.TMDSState) 
	{
		pDH->state = DH_disabled;
		RMDBGLOG((DISABLE, "TMDS/HDCP State is now: DISABLED/NOT ENCRYPTED\n"));
	}
	
	// Check for notification of changes in interrupt bits
	if (pDH->HDMIState.TMDSState)
	{  
	    // TMDS is on, we can use the INTR bits
		use_intr = TRUE;
		use_intr_hpd = TRUE;
		use_intr_rx = pDH->HDMIState.ReceiverSignal ;
		
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0E, &intr);
		
		if (RMFAILED(err)) {
			RMDBGLOG((DISABLE, "Failed to read INTR register!\n"));
			use_intr = FALSE;
			use_intr_hpd = FALSE;
		} 
		else if (intr & 0x80) // interrupted
		{
		    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x06, &intr1) ;
			intr1 &= ~0x10 ;
			if (RMFAILED(err)) {
				RMDBGLOG((DISABLE, "Failed to read INTR1 register!\n"));
				use_intr = FALSE;
				use_intr_hpd = FALSE;
				use_intr_rx = FALSE; 
				intr1 = 0 ;
			} 
		    
		    if( intr1 & 1 )
		    {
            	pDH->HotPlugChanged = TRUE ;	
            	pDH->HDMIState.HotPlugChanged = TRUE ; // reg06[0]
				RMDBGLOG((LOCALDBG,"Hot Plug Changed!\n")) ;
		    }

			if (pDH->HDMIState.ReceiverSignal) 
			{
				pDH->HDMIState.ReceiverChanged = (intr1 & 2) ? TRUE : FALSE;
				if( intr1 & 2 )
				{
					RMDBGLOG((LOCALDBG,"ReceiverChanged!\n")) ;
				}
			}

		    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &intr2) ;
			if (RMFAILED(err)) {
				RMDBGLOG((DISABLE, "Failed to read INTR1 register!\n"));
				use_intr = FALSE;
				use_intr_hdcp = FALSE;
				intr2 = 0 ;
			} 
		    
		    if( (intr1 & (1<<2))||(intr2 & 1))
		    {

				RMDBGLOG((LOCALDBG,"intr1 = %02x, intr2 = %02X\n",intr1,intr2)) ;
		        // DDC Error. reg6[4][2]
		        // Authenticate fail. reg7[0]
			        DHAbortDDC_CAT6611(pDH) ; // abort DDC.
			        pDH->CAT_hdcpreg_ready = FALSE ; // flush the flag fro DDC fail.
			        pDH->HDMIState.HDCPLost = TRUE ;
			        use_intr_hdcp = TRUE ;
		    }
		    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &intr3) ;
		    if( intr3 & (1<<4))
		    {
		        pDH->HDMIState.InputClockChanged = TRUE ;
		    }
		}

		if( intr1 || intr2 || intr3 )
		{
			// RMDBGLOG((LOCALDBG,"DHCheckHDMI(): intr1 = %02X, intr2 = %02X, intr3 = %02X\n", intr1,intr2,intr3)) ;
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0C, 0xFF) ;
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0D, 0xFF) ;
   			DH_Clear_CAT6611INT(pDH) ;
		}
	}
					
	
	if (! use_intr_hpd) {  // has not been set already
		pDH->HotPlugChanged = (pDH->HotPlugState != sys_stat_hpd);
	}
	
	if (! use_intr_rx) {
		pDH->HDMIState.ReceiverChanged = (pDH->HDMIState.ReceiverState != sys_stat_rx);
	}
	
	{
		RMbool p_stable = (pDH->IgnoreStable || sys_stat_vidstable) ? TRUE : FALSE;
		pDH->HDMIState.InputClockChanged = (pDH->HDMIState.InputClockStable != p_stable);
		if( pDH->HDMIState.InputClockChanged )
		{
			RMDBGLOG((LOCALDBG,"pDH->HDMIState.InputClockStable = %s->%s\n",pDH->HDMIState.InputClockStable ?"TRUE":"FALSE",p_stable?"TRUE":"FALSE")) ;
		}
		pDH->HDMIState.InputClockStable = p_stable;
	} 
	
	pDH->HotPlugState = sys_stat_hpd;
	pDH->HDMIState.ReceiverState = sys_stat_rx;
	if (pDH->HDMIState.ReceiverState) pDH->HDMIState.ReceiverSignal = TRUE;
	
	// Read EDID
	if (pDH->HotPlugChanged) {
		RMDBGLOG((LOCALDBG,"HotPlugChanged = TRUE\n")) ;
		if (pDH->HotPlugState) {
			RMDBGLOG((LOCALDBG, "HotPlug active, reading EDID\n"));
			err = DHReadEDID(pDH);
			if (err == RM_PENDING) {  // same display, no action required by app. unless re-plug
				if (prev_HotPlug == pDH->HotPlugState) {
					RMDBGLOG((DISABLE, "Same EDID without HPD change, no action required!\n"));
					pDH->HotPlugChanged = FALSE;
					if (pDH->HotPlugGuarded) {  // update guard time
						pDH->HotPlugGuardTime = RMGetTimeInMicroSeconds();
					}
				}
			}
		} else if (pDH->HDMIState.TMDSState) {
			// Turn off TMDS, if still enabled
			RMDBGLOG((DISABLE, "HotPlug lost, disabling TMDS\n"));
			DHDisableOutput(pDH);
			pDH->HDMIState.TMDSState = FALSE;
		}
	}
	
	// HotPlug delay
	if (pDH->HotPlugChanged) {
		if (pDH->HotPlugState && pDH->CheckClock) {  // delay reporting HPD low-to-high, except during first call
			RMDBGLOG((DISABLE, "HotPlug on, delaying report.\n"));
			pDH->HotPlugGuardTime = RMGetTimeInMicroSeconds();
			pDH->HotPlugGuarded = TRUE;
		} else {  // report HPD high-to-low immediately
			RMDBGLOG((DISABLE, "HotPlug o%s, reporting now.\n", pDH->HotPlugState ? "n" : "ff"));
			pDH->HDMIState.HotPlugState = pDH->HotPlugState;
			pDH->HDMIState.HotPlugChanged = TRUE;
			pDH->HotPlugGuarded = FALSE;
		}
	}
	if (pDH->HotPlugGuarded && pDH->HotPlugState) {
		// HPD still on after 5 seconds? report to app.
		RMuint64 t_now = RMGetTimeInMicroSeconds();
		if (RMCyclesElapsed64(pDH->HotPlugGuardTime, t_now) >= HOTPLUG_GUARD_TIME * 1000 * 1000) {
			if (! manutest) fprintf(stderr, "[HDMI]    ***   HotPlug Guard Time ended.\n");
			pDH->HDMIState.HotPlugState = TRUE;
			pDH->HDMIState.HotPlugChanged = TRUE;
			pDH->HotPlugGuarded = FALSE;
		}
	}
	
	// Report changes
	if (pDH->HDMIState.HotPlugChanged) {
		if (! manutest) fprintf(stderr, "[HDMI]    ***   HotPlug changed, is now %s\n", pDH->HDMIState.HotPlugState ? "ON" : "OFF");
        DHDump_CAT6611reg(pDH) ;
	}
	if (pDH->HDMIState.ReceiverChanged) {
		if (! manutest) fprintf(stderr, "[HDMI]    ***   Rx changed, is now %s\n", pDH->HDMIState.ReceiverState ? "ON" : "OFF");
        DHDump_CAT6611reg(pDH) ;
	}
	if (pDH->HDMIState.InputClockChanged) {
		if (! manutest) fprintf(stderr, "[HDMI]    ***   Clock changed, is now %s\n", pDH->HDMIState.InputClockStable ? "STABLE" : "UNSTABLE");
        DHDump_CAT6611reg(pDH) ;
	}
	
	// React to Hot Plug / Rx changes
	if (pDH->HDMIState.HotPlugChanged || pDH->HDMIState.ReceiverChanged) {
		RMbool on = pDH->HDMIState.HotPlugState && (pDH->HDMIState.ReceiverState == pDH->HDMIState.ReceiverSignal);
		
		if (on && (pDH->cable == DH_disconnected)) {
			if (! manutest) fprintf(stderr, "[HDMI] Receiver is now active\n");
		} else if (! on && (pDH->cable != DH_disconnected)) {
			if (! manutest) fprintf(stderr, "[HDMI] Receiver is now unavailable\n");
		}
		
		// Update pDH->cable
		pDH->cable = on ? DH_connected : DH_disconnected;
		
		// Turn off TMDS, if still enabled
		if ((pDH->cable == DH_disconnected) && pDH->HDMIState.TMDSState) {
			RMDBGLOG((ENABLE, "HotPlug or Receiver lost, disabling TMDS\n"));
			DHDisableOutput(pDH);
			pDH->HDMIState.TMDSState = FALSE;
		}
	}
	
	// React to clock changes
	if (pDH->HDMIState.InputClockChanged) {
		if (pDH->HDMIState.InputClockStable) {
			RMMicroSecondSleep(100 * 1000);
			RMDBGLOG((LOCALDBG, "Performing SoftReset of HDMI chip\n"));
			DHSoftReset(pDH);
			RMMicroSecondSleep(100 * 1000);
			pDH->GuardTMDS = FALSE;
			if (pDH->RequestTMDS) {
				RMDBGLOG((LOCALDBG, "Deferred TMDS enabling, clock is now stable.\n"));
				err = DHEnableOutput(pDH);
				pDH->RequestTMDS = FALSE;
				if (pDH->CPDesired) pDH->ForceIntegrityCheck = TRUE;
				// Read EDID again, in case we missed a HPD pulse while TMDS was off
				RMDBGLOG((LOCALDBG, "TMDS active, reading EDID\n"));
				err = DHReadEDID(pDH);
				if (RMSUCCEEDED(err)) {  // different EDID, signal to app.
					RMDBGLOG((LOCALDBG, "EDID has changed while TMDS was off! Generating HotPlug event.\n"));
					pDH->HDMIState.HotPlugChanged = TRUE;
				}
			} else if (! pDH->CPDesired) {
				err = DHMuteOutput(pDH, FALSE);
			}
		} else {
			RMDBGLOG((LOCALDBG, "Clock has become unstable, deferring TMDS enable.\n"));
			pDH->GuardTMDS = TRUE;	
		}
	}
	
	if (pDH->RequestTMDS) {
		if (pDH->HDMIState.InputClockStable) {
			RMDBGLOG((DISABLE, "Clock is stable and TMDS is not yet enabled!\n"));
		} else {
			RMDBGLOG((DISABLE, "Clock is still unstable while waiting to enable TMDS!\n"));
		}
	}
	
	// Read current HDCP state
	if (pDH->part_caps.HDCP) {
		RMuint64 t_now = RMGetTimeInMicroSeconds();
		RMuint32 elapsed_ms = RMCyclesElapsed64(pDH->IntegrityLastCheck, t_now) / 1000;

		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x46, &hdcp_ctrl);
		if (RMSUCCEEDED(err)) {
			if (! use_intr_hdcp) {  // has not been set already
				pDH->HDMIState.HDCPLost = (pDH->HDMIState.HDCPState && !(hdcp_ctrl & 0x80));
			}
			pDH->HDMIState.HDCPState = (hdcp_ctrl & 0x80) ? TRUE : FALSE;
		}
		else
        {
			RMDBGLOG((DISABLE, "Failed to read HDCP_CTRL register!\n"));
			return RM_ERROR;
		}
		// Update encryption in pDH->state
		if ((pDH->state == DH_enabled) && pDH->HDMIState.HDCPState) {
			pDH->state = DH_enabled_encrypted;
			RMDBGLOG((DISABLE, "TMDS/HDCP State is now: DISABLED/ENCRYPTED\n"));
		} else if ((pDH->state == DH_enabled_encrypted) && ! pDH->HDMIState.HDCPState) {
			pDH->state = pDH->HDMIState.TMDSState ? DH_enabled : DH_disabled;
			RMDBGLOG((DISABLE, "TMDS/HDCP State is now: %sABLED/NOT ENCRYPTED\n", pDH->HDMIState.TMDSState ? "EN" : "DIS"));
		}
		
		// Report changes
		if (pDH->HDMIState.HDCPLost) {
			if (! manutest) fprintf(stderr, "[HDMI]    ***   HDCP has been lost, is now %s\n", pDH->HDMIState.HDCPState ? "ACTIVE" : "DISABLED");
			// Force immediate re-authentication
			if (pDH->CPDesired) {
				pDH->ForceIntegrityCheck = TRUE;
			}
		}
		
		// Execute integrity check every 2 seconds, unless RI_128 intr is available
		if ((! use_intr) || (! pDH->HDMIState.HDCPState)) {
			if (elapsed_ms > 2000) ri_128 = TRUE;
		}
		
		// Check HDCP integrity at least every 2 seconds or every 128 frames
		if ((pDH->cable != DH_disconnected) && 
		    (pDH->state != DH_disabled) && 
		    pDH->CPDesired && 
		    (pDH->ForceIntegrityCheck ) 
		    && (elapsed_ms >= HDCP_GUARD_TIME)) 
		{
			pDH->IntegrityLastCheck = t_now;
			pDH->ForceIntegrityCheck = FALSE;
			err = DHVerifyIntegrity(pDH);  // Verify HDCP is active
			if (RMFAILED(err)) {
				if (err != RM_DRM_INVALID_KEY) {
					t_now = RMGetTimeInMicroSeconds();
					RMDBGLOG((DISABLE, "HDCP Integrity failed, Re-Check after %lu mSec\n", 
						RMCyclesElapsed64(pDH->IntegrityLastCheck, t_now) / 1000));
					pDH->IntegrityLastCheck = t_now;
					err = DHVerifyIntegrity(pDH);  // Verify HDCP is active
				}
				if (RMFAILED(err)) {
					RMDBGLOG((DISABLE, "HDCP Integrity failed!!!\n"));
				}
			}
		}
	} else {  // no HDCP on this chip
		pDH->HDMIState.HDCPLost = FALSE;
		pDH->HDMIState.HDCPState = FALSE;
	}
	
	// Copy flag for DHGetConnection()
	if (pDH->HDMIState.ReceiverSignal) pDH->RxPresent = TRUE;
	
	// Return updated HDMIState struct to App
	if (pHDMIState) *pHDMIState = pDH->HDMIState;
	
	// DHCheckHDMI() is being called by App, don't reset chip on SetPixelClock anymore.
	if (pHDMIState) pDH->CheckClock = TRUE;

    RMDBGPRINT((DISABLE,"pHDMIState->HotPlugChanged = %s\n",pHDMIState->HotPlugChanged?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->HotPlugState = %s\n",pHDMIState->HotPlugState?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->ReceiverSignal = %s\n",pHDMIState->ReceiverSignal?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->ReceiverChanged = %s\n",pHDMIState->ReceiverChanged?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->ReceiverState = %s\n",pHDMIState->ReceiverState?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->HDCPLost = %s\n",pHDMIState->HDCPLost?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->HDCPState = %s\n",pHDMIState->HDCPState?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->InputClockChanged = %s\n",pHDMIState->InputClockChanged?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->InputClockStable = %s\n",pHDMIState->InputClockStable?"TRUE":"FALSE")) ;
    RMDBGPRINT((DISABLE,"pHDMIState->TMDSState = %s\n",pHDMIState->TMDSState?"TRUE":"FALSE")) ;

	
	//DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x06, &intr1) ;
	//DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &intr2) ;
	//DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &intr3) ;
    //RMDBGLOG((LOCALDBG,"intr1 = %02X, intr2= %02X, intr3 = %02X\n",intr1,intr2,intr3)) ;
	
	return RM_OK;
}
//~jj_tseng@chipadvanced.com 2007/03/23

RMstatus DHCheckHDMI(struct DH_control *pDH, 
	struct DH_HDMI_state *pHDMIState)
{
	RMstatus err;
	RMuint8 reg, intr, intr1, intr2, intr3, sys_ctrl1, sys_stat, hdcp_ctrl, hdcp_stat;
	RMbool use_intr = FALSE, use_intr_hpd = FALSE, use_intr_rx = FALSE, use_intr_hdcp = FALSE;
	RMbool sys_stat_hpd, sys_stat_rx, sys_stat_pclk;
	RMbool ri_128 = FALSE;
	RMbool prev_HotPlug;
	
	CHECK_pDH("DHCheckHDMI");
	
	// Notification by app or DHGetConnection() that Rx signal is supported
	if (pDH->RxPresent || (pHDMIState && pHDMIState->ReceiverSignal)) {
		pDH->HDMIState.ReceiverSignal = TRUE;
	}
	
	if( pDH->part == DH_CAT6611 )
	{
	    return DHCheckHDMI_CAT6611(pDH, pHDMIState) ;

	}
	
	// Housekeeping
	prev_HotPlug = pDH->HotPlugState;
	pDH->HotPlugChanged = FALSE;
	pDH->HDMIState.HotPlugChanged = FALSE;
	pDH->HDMIState.ReceiverChanged = FALSE;
	pDH->HDMIState.HDCPLost = FALSE;
	pDH->HDMIState.InputClockChanged = FALSE;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		// Read current link state
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x09, &sys_stat);
		if (RMSUCCEEDED(err)) {
			sys_stat_hpd = (sys_stat & 0x02) ? TRUE : FALSE;
			sys_stat_rx = (sys_stat & 0x04) ? TRUE : FALSE;
			sys_stat_pclk = (sys_stat & 0x01) ? TRUE : FALSE;
		} else {
			RMDBGLOG((ENABLE, "Failed to read SYS_STAT register!\n"));
			return err;
		}
		
		// Check output state
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x08, &sys_ctrl1);
		if (RMSUCCEEDED(err)) {
			pDH->HDMIState.TMDSState = (sys_ctrl1 & 0x01) ? TRUE : FALSE;
		} else {
			RMDBGLOG((ENABLE, "Failed to read SYS_CTRL register!\n"));
		}
		break;
	case DH_ANX9030:
		// Read current link state
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x06, &sys_stat);
		if (RMSUCCEEDED(err)) {
			sys_stat_hpd = (sys_stat & 0x08) ? TRUE : FALSE;
			sys_stat_rx = (sys_stat & 0x01) ? TRUE : FALSE;
			sys_stat_pclk = (sys_stat & 0x02) ? TRUE : FALSE;
		} else {
			RMDBGLOG((ENABLE, "Failed to read SYS_STAT register!\n"));
			return err;
		}
		
		// Check output state
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x07, &sys_ctrl1);
		if (RMSUCCEEDED(err)) {
			pDH->HDMIState.TMDSState = (sys_ctrl1 & 0x01) ? TRUE : FALSE;
		} else {
			RMDBGLOG((ENABLE, "Failed to read SYS_CTRL register!\n"));
		}
		break;
	default:
		return RM_ERROR;
	}
	
	// Update pDH->state
	if ((pDH->state == DH_disabled) && pDH->HDMIState.TMDSState) {
		pDH->state = DH_enabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/NOT ENCRYPTED\n"));
	} else if ((pDH->state != DH_disabled) && ! pDH->HDMIState.TMDSState) {
		pDH->state = DH_disabled;
		RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: DISABLED/NOT ENCRYPTED\n"));
	}
	
	// Check for notification of changes in interrupt bits
	if (pDH->HDMIState.TMDSState) {  // TMDS is on, we can use the INTR bits
		switch (pDH->part) {
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			use_intr = TRUE;
			use_intr_hpd = TRUE;
			use_intr_rx = pDH->HDMIState.ReceiverSignal;
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x70, &intr);
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Failed to read INTR register!\n"));
				use_intr = FALSE;
				use_intr_hpd = FALSE;
			} else if (intr & 0x01) {
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x71, &intr1);
				if (RMFAILED(err)) {
					RMDBGLOG((ENABLE, "Failed to read INTR1 register!\n"));
					use_intr = FALSE;
					use_intr_hpd = FALSE;
				} else if (intr1) {
					//if (intr1 & 0x64) 
					//	RMDBGLOG((LOCALDBG, "INTR1: 0x%02X\n", intr1));
					err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x71, intr1);  // clear intr1
					if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear INTR1 register!\n"));
					pDH->HotPlugChanged = (intr1 & 0x40) ? TRUE : FALSE;
					if (pDH->HDMIState.ReceiverSignal) {
						pDH->HDMIState.ReceiverChanged = (intr1 & 0x20) ? TRUE : FALSE;
					}
					ri_128 = (intr1 & 0x04) ? TRUE : FALSE;
#ifdef DEBUG_HDMI_INTR
					if (intr1 & 0x01) RMDBGLOG((ENABLE, "*** INTR1.0 UNDER_RUN\n"));
					if (intr1 & 0x02) RMDBGLOG((ENABLE, "*** INTR1.1 OVER_RUN\n"));
					if (intr1 & 0x04) RMDBGLOG((ENABLE, "*** INTR1.2 RI_128\n"));
					if (intr1 & 0x08) RMDBGLOG((ENABLE, "*** INTR1.3 BIPHASE_ERR\n"));
					if (intr1 & 0x10) RMDBGLOG((ENABLE, "*** INTR1.4 DROP_SAMPLE\n"));
					if (intr1 & 0x20) RMDBGLOG((ENABLE, "*** INTR1.5 RSEN\n"));
					if (intr1 & 0x40) RMDBGLOG((ENABLE, "*** INTR1.6 HPD\n"));
					if (intr1 & 0x80) RMDBGLOG((ENABLE, "*** INTR1.7 SOFT\n"));
#endif
				}
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x72, &intr2);
				if (RMFAILED(err)) {
					RMDBGLOG((ENABLE, "Failed to read INTR2 register!\n"));
					use_intr = FALSE;
				} else if (intr2) {
					//if (intr2 & 0x22) 
					//	RMDBGLOG((LOCALDBG, "INTR2: 0x%02X\n", intr2));
					err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x72, intr2);  // clear intr2
					if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear INTR2 register!\n"));
					if (intr2 & 0x20) {
						RMDBGLOG((ENABLE, "SiI9x3x: HDCP failed, intr: 0x%02lX 0x%02lX 0x%02lX\n", intr1, intr2, intr3));
						pDH->HDMIState.HDCPLost = TRUE;
					}
					//pDH->HDMIState.HDCPLost = (intr2 & 0x20) ? TRUE : FALSE;
					//pDH->HDMIState.HDCPLost = FALSE;
					pDH->HDMIState.InputClockChanged = (intr2 & 0x02) ? TRUE : FALSE;
#ifdef DEBUG_HDMI_INTR
					//if (intr2 & 0x01) RMDBGLOG((ENABLE, "*** INTR2.0 VSYNC_REC\n"));
					if (intr2 & 0x02) RMDBGLOG((ENABLE, "*** INTR2.1 TCLK_STBL\n"));
					if (intr2 & 0x04) RMDBGLOG((ENABLE, "*** INTR2.2 ACR_OVR\n"));
					if (intr2 & 0x08) {
						RMuint32 cts;
						RMuint8 reg;
						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x09, &reg); cts = reg;
						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x0A, &reg); cts |= ((reg << 8) & 0xFF00);
						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x0B, &reg); cts |= ((reg << 16) & 0xF0000);
						RMDBGLOG((ENABLE, "*** INTR2.3 CTS_CHG, CTS = 0x%05lX (%lu)\n", cts, cts));
					}
					if (intr2 & 0x10) RMDBGLOG((ENABLE, "*** INTR2.4 PREAM_ERR\n"));
					if (intr2 & 0x20) RMDBGLOG((ENABLE, "*** INTR2.5 ENC_DIS\n"));
					if (intr2 & 0x40) RMDBGLOG((ENABLE, "*** INTR2.6 SPDIF_PAR\n"));
					if (intr2 & 0x80) RMDBGLOG((ENABLE, "*** INTR2.7 BCAP_DONE\n"));
#endif
				}
				if (pDH->part != DH_siI9030) {
					err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x73, &intr3);
					if (RMFAILED(err)) {
						RMDBGLOG((ENABLE, "Failed to read INTR2 register!\n"));
						use_intr = FALSE;
					} else if (intr3) {
						//if (intr3 & 0x22) 
						//	RMDBGLOG((LOCALDBG, "INTR3: 0x%02X\n", intr3));
						err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x73, intr3);  // clear intr3
						if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear INTR3 register!\n"));
						use_intr_hdcp = TRUE;
						if (intr3 & 0xB0) {
							RMDBGLOG((ENABLE, "SiI9x34: HDCP Ri checked failed, intr: 0x%02lX 0x%02lX 0x%02lX\n", intr1, intr2, intr3));
							pDH->HDMIState.HDCPLost = TRUE;
						}
#ifdef DEBUG_HDMI_INTR
						//if (intr3 & 0x01) RMDBGLOG((ENABLE, "*** INTR3.0 DDC_FIFO_EMPTY\n"));
						//if (intr3 & 0x02) RMDBGLOG((ENABLE, "*** INTR3.1 DDC_FIFO_FULL\n"));
						//if (intr3 & 0x04) RMDBGLOG((ENABLE, "*** INTR3.2 DDC_FIFO_HALF\n"));
						//if (intr3 & 0x08) RMDBGLOG((ENABLE, "*** INTR3.3 DDC_CMD_DONE\n"));
						if (intr3 & 0x10) RMDBGLOG((ENABLE, "*** INTR3.4 RI_MISMATCH_1\n"));
						if (intr3 & 0x20) RMDBGLOG((ENABLE, "*** INTR3.5 RI_MISMATCH_2\n"));
						if (intr3 & 0x40) RMDBGLOG((ENABLE, "*** INTR3.6 RI_ERR\n"));
						if (intr3 & 0x80) RMDBGLOG((ENABLE, "*** INTR3.7 RI_NOT_READY\n"));
#endif
					}
				}
			}
			break;
		case DH_ANX9030:
			use_intr = TRUE;
			use_intr_hpd = TRUE;
			use_intr_rx = pDH->HDMIState.ReceiverSignal;
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x40, &intr);
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Failed to read INTR register!\n"));
				use_intr = FALSE;
				use_intr_hpd = FALSE;
			} else if (intr & 0x01) {
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x42, &intr1);
				if (RMFAILED(err)) {
					RMDBGLOG((ENABLE, "Failed to read INTR1 register!\n"));
					use_intr = FALSE;
					use_intr_hpd = FALSE;
				} else if (intr1) {
					err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x42, intr1);  // clear intr1
					if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear INTR1 register!\n"));
					pDH->HotPlugChanged = (intr1 & 0x04) ? TRUE : FALSE;
					pDH->HDMIState.InputClockChanged = (intr1 & 0x01) ? TRUE : FALSE;
#ifdef DEBUG_HDMI_INTR
					if (intr1 & 0x01) RMDBGLOG((ENABLE, "*** INTR1.0 VID_CLK_CHG\n"));
					if (intr1 & 0x02) RMDBGLOG((ENABLE, "*** INTR1.1 AUD_CTS_OVRWR\n"));
					if (intr1 & 0x04) RMDBGLOG((ENABLE, "*** INTR1.2 HOTPLUG_CHG\n"));
					if (intr1 & 0x08) RMDBGLOG((ENABLE, "*** INTR1.3 SW_INT\n"));
					//if (intr1 & 0x10) RMDBGLOG((ENABLE, "*** INTR1.4 SPDIF_ERR\n"));
					if (intr1 & 0x20) RMDBGLOG((ENABLE, "*** INTR1.5 AFIFO_OVER\n"));
					if (intr1 & 0x40) RMDBGLOG((ENABLE, "*** INTR1.6 AFIFO_UNDER\n"));
					if (intr1 & 0x80) RMDBGLOG((ENABLE, "*** INTR1.7 AUD_CTS_CHG\n"));
#endif
				}
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x43, &intr2);
				if (RMFAILED(err)) {
					RMDBGLOG((ENABLE, "Failed to read INTR2 register!\n"));
					use_intr = FALSE;
				} else if (intr2) {
					err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x43, intr2);  // clear intr2
					if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear INTR2 register!\n"));
					if (intr2 & 0x80) {
						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, &hdcp_ctrl);
						if (! (hdcp_ctrl & 0x04)) pDH->HDMIState.HDCPLost = TRUE;
					}
					ri_128 = (intr2 & 0x60) ? TRUE : FALSE;
					if (intr2 & 0x10) {  // BKSV ready
						pDH->ANX_bksv_ready = TRUE;
					}
					if (intr2 & 0x02) {  // auth changed
//						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA0, &reg);
//						if (reg & 0x02) {
//							pDH->ANX_hdcp_auth_pass = TRUE;
//						} else {
//							DHMuteOutput(pDH, TRUE);
//							RMDBGLOG((ENABLE, "ANX9030_Authentication failed_by_Auth_change\n"));
//							pDH->ANX_hdcp_auth_pass = FALSE;
//							pDH->ANX_hdcp_wait_100ms_needed = TRUE;
//							pDH->ANX_auth_fully_pass = FALSE;
//							DHDisableEncryption_ANX(pDH);
//						}
					}
					if (intr2 & 0x01) {  // auth done
						DHEnableEncryption_ANX(pDH);
//						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA0, &reg);
//						if (reg & 0x02) {
							RMDBGLOG((ENABLE, "ANX9030_Authentication pass, 0xA0=0x%02X\n", reg));
							pDH->ANX_hdcp_auth_pass = TRUE;
							pDH->ANX_hdcp_auth_fail_counter = 0;
//						} else {
//							RMDBGLOG((ENABLE, "ANX9030_Authentication failed, 0xA0=0x%02X\n", reg));
//							pDH->ANX_hdcp_wait_100ms_needed = TRUE;
//							pDH->ANX_auth_fully_pass = FALSE;
//							pDH->ANX_hdcp_auth_pass = FALSE;
//							pDH->ANX_hdcp_auth_fail_counter ++;
//							if (pDH->ANX_hdcp_auth_fail_counter >= 10) {
//								pDH->ANX_hdcp_auth_fail_counter = 0;
//								pDH->ANX_bksv_ready = FALSE;
//								DHDisableEncryption_ANX(pDH);
//							}
//						}
					}
#ifdef DEBUG_HDMI_INTR
					if (intr2 & 0x01) RMDBGLOG((ENABLE, "*** INTR2.0 AUTH_DONE\n"));
					if (intr2 & 0x02) RMDBGLOG((ENABLE, "*** INTR2.1 AUTH_STATE_CHG\n"));
					if (intr2 & 0x04) RMDBGLOG((ENABLE, "*** INTR2.2 SHA_DONE\n"));
					if (intr2 & 0x08) RMDBGLOG((ENABLE, "*** INTR2.3 PLL_LOCK_CHG\n"));
					if (intr2 & 0x10) RMDBGLOG((ENABLE, "*** INTR2.4 BKSV_RDY\n"));
					if (intr2 & 0x20) RMDBGLOG((ENABLE, "*** INTR2.5 HDCP_ENHC_CHK\n"));
					if (intr2 & 0x40) RMDBGLOG((ENABLE, "*** INTR2.6 HDCP_LINK_CHK\n"));
					if (intr2 & 0x80) RMDBGLOG((ENABLE, "*** INTR2.7 ENC_EN_CHG\n"));
#endif
				}
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x44, &intr3);
				if (RMFAILED(err)) {
					RMDBGLOG((ENABLE, "Failed to read INTR2 register!\n"));
					use_intr = FALSE;
				} else if (intr3) {
					err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x44, intr3);  // clear intr3
					if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear INTR3 register!\n"));
					if (pDH->HDMIState.ReceiverSignal) {
						pDH->HDMIState.ReceiverChanged = (intr3 & 0x02) ? TRUE : FALSE;
					}
#ifdef DEBUG_HDMI_INTR
					//if (intr3 & 0x01) RMDBGLOG((ENABLE, "*** INTR3.0 SPDIF_UNSTBL\n"));
					if (intr3 & 0x02) RMDBGLOG((ENABLE, "*** INTR3.1 RX_SEN_CHG\n"));
					if (intr3 & 0x04) RMDBGLOG((ENABLE, "*** INTR3.2 VSYNC_DET\n"));
					if (intr3 & 0x08) RMDBGLOG((ENABLE, "*** INTR3.3 DDC_NO_ACK\n"));
					if (intr3 & 0x10) RMDBGLOG((ENABLE, "*** INTR3.4 DDC_ACC_ERR\n"));
					if (intr3 & 0x20) RMDBGLOG((ENABLE, "*** INTR3.5 AUD_CLK_CHG\n"));
					if (intr3 & 0x40) RMDBGLOG((ENABLE, "*** INTR3.6 VID_FORMAT_CHG\n"));
					//if (intr3 & 0x80) RMDBGLOG((ENABLE, "*** INTR3.7 SPDIF_BI_PHASE_ERR\n"));
#endif
				}
			}
			break;
		default:
			break;
		}
	} else if (pDH->part == DH_siI170) {
		//use_intr_hpd = TRUE;
		//if (sys_stat_rx) {
		//	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x09, sys_stat);
		//	pDH->HotPlugChanged = TRUE;
		//}
	}
	
	if (! use_intr_hpd) {  // has not been set already
		pDH->HotPlugChanged = (pDH->HotPlugState != sys_stat_hpd);
	}
	if (! use_intr_rx) {
		pDH->HDMIState.ReceiverChanged = (pDH->HDMIState.ReceiverState != sys_stat_rx);
	}
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
	case DH_ANX9030: {
		RMbool p_stable = (pDH->IgnoreStable || sys_stat_pclk) ? TRUE : FALSE;
		//if (! use_intr) {  // has not been set already
			pDH->HDMIState.InputClockChanged = (pDH->HDMIState.InputClockStable != p_stable);
		//}
		pDH->HDMIState.InputClockStable = p_stable;
		} break;
	default:
		pDH->HDMIState.InputClockChanged = FALSE;
		pDH->HDMIState.InputClockStable = TRUE;
		break;
	}
	pDH->HotPlugState = sys_stat_hpd;
	pDH->HDMIState.ReceiverState = sys_stat_rx;
	if (pDH->HDMIState.ReceiverState) pDH->HDMIState.ReceiverSignal = TRUE;
	
	// Read EDID
	if (pDH->HotPlugChanged) {
		if (pDH->HotPlugState) {
			RMDBGLOG((LOCALDBG, "HotPlug active, reading EDID\n"));
			err = DHReadEDID(pDH);
			if (err == RM_PENDING) {  // same display, no action required by app. unless re-plug
				if (prev_HotPlug == pDH->HotPlugState) {
					RMDBGLOG((LOCALDBG, "Same EDID without HPD change, no action required!\n"));
					pDH->HotPlugChanged = FALSE;
					if (pDH->HotPlugGuarded) {  // update guard time
						pDH->HotPlugGuardTime = RMGetTimeInMicroSeconds();
					}
				}
			}
		} else if (pDH->HDMIState.TMDSState) {
			// Turn off TMDS, if still enabled
			RMDBGLOG((LOCALDBG, "HotPlug lost, disabling TMDS\n"));
			DHDisableOutput(pDH);
			pDH->HDMIState.TMDSState = FALSE;
		}
	}
	
	// HotPlug delay
	if (pDH->HotPlugChanged) {
		if (pDH->HotPlugState && pDH->CheckClock) {  // delay reporting HPD low-to-high, except during first call
			RMDBGLOG((LOCALDBG, "HotPlug on, delaying report.\n"));
			pDH->HotPlugGuardTime = RMGetTimeInMicroSeconds();
			pDH->HotPlugGuarded = TRUE;
		} else {  // report HPD high-to-low immediately
			RMDBGLOG((LOCALDBG, "HotPlug o%s, reporting now.\n", pDH->HotPlugState ? "n" : "ff"));
			pDH->HDMIState.HotPlugState = pDH->HotPlugState;
			pDH->HDMIState.HotPlugChanged = TRUE;
			pDH->HotPlugGuarded = FALSE;
		}
	}
	if (pDH->HotPlugGuarded && pDH->HotPlugState) {
		// HPD still on after 5 seconds? report to app.
		RMuint64 t_now = RMGetTimeInMicroSeconds();
		if (RMCyclesElapsed64(pDH->HotPlugGuardTime, t_now) >= HOTPLUG_GUARD_TIME * 1000 * 1000) {
			if (! manutest) fprintf(stderr, "[HDMI]    ***   HotPlug Guard Time ended.\n");
			pDH->HDMIState.HotPlugState = TRUE;
			pDH->HDMIState.HotPlugChanged = TRUE;
			pDH->HotPlugGuarded = FALSE;
		}
	}
	
	// Report changes
	if (pDH->HDMIState.HotPlugChanged) {
		if (! manutest) fprintf(stderr, "[HDMI]    ***   HotPlug changed, is now %s\n", pDH->HDMIState.HotPlugState ? "ON" : "OFF");
	}
	if (pDH->HDMIState.ReceiverChanged) {
		if (! manutest) fprintf(stderr, "[HDMI]    ***   Rx changed, is now %s\n", pDH->HDMIState.ReceiverState ? "ON" : "OFF");
	}
	if (pDH->HDMIState.InputClockChanged) {
		if (! manutest) fprintf(stderr, "[HDMI]    ***   Clock changed, is now %s\n", pDH->HDMIState.InputClockStable ? "STABLE" : "UNSTABLE");
	}
	
	// React to Hot Plug / Rx changes
	if (pDH->HDMIState.HotPlugChanged || pDH->HDMIState.ReceiverChanged) {
		RMbool on = pDH->HDMIState.HotPlugState && (pDH->HDMIState.ReceiverState == pDH->HDMIState.ReceiverSignal);
		
		if (on && (pDH->cable == DH_disconnected)) {
			if (! manutest) fprintf(stderr, "[HDMI] Receiver is now active\n");
		} else if (! on && (pDH->cable != DH_disconnected)) {
			if (! manutest) fprintf(stderr, "[HDMI] Receiver is now unavailable\n");
		}
		
		// Update pDH->cable
		pDH->cable = on ? DH_connected : DH_disconnected;
		
		// Turn off TMDS, if still enabled
		if ((pDH->cable == DH_disconnected) && pDH->HDMIState.TMDSState) {
			RMDBGLOG((LOCALDBG, "HotPlug or Receiver lost, disabling TMDS\n"));
			DHDisableOutput(pDH);
			pDH->HDMIState.TMDSState = FALSE;
		}
	}
	
	// React to clock changes
	if (pDH->HDMIState.InputClockChanged) {
		if (pDH->HDMIState.InputClockStable) {
			RMMicroSecondSleep(100 * 1000);
			RMDBGLOG((LOCALDBG, "Performing SoftReset of HDMI chip\n"));
			DHSoftReset(pDH);
			RMMicroSecondSleep(100 * 1000);
			pDH->GuardTMDS = FALSE;
			if (pDH->RequestTMDS) {
				RMDBGLOG((LOCALDBG, "Deferred TMDS enabling, clock is now stable.\n"));
				err = DHEnableOutput(pDH);
				pDH->RequestTMDS = FALSE;
				if (pDH->CPDesired) pDH->ForceIntegrityCheck = TRUE;
				// Read EDID again, in case we missed a HPD pulse while TMDS was off
				RMDBGLOG((LOCALDBG, "TMDS active, reading EDID\n"));
				err = DHReadEDID(pDH);
				if (RMSUCCEEDED(err)) {  // different EDID, signal to app.
					RMDBGLOG((LOCALDBG, "EDID has changed while TMDS was off! Generating HotPlug event.\n"));
					pDH->HDMIState.HotPlugChanged = TRUE;
				}
			} else if (! pDH->CPDesired) {
				err = DHMuteOutput(pDH, FALSE);
			}
		} else {
			RMDBGLOG((LOCALDBG, "Clock has become unstable, deferring TMDS enable.\n"));
			pDH->GuardTMDS = TRUE;	
		}
	}
	
	if (pDH->RequestTMDS) {
		if (pDH->HDMIState.InputClockStable) {
			RMDBGLOG((LOCALDBG, "Clock is stable and TMDS is not yet enabled!\n"));
		} else {
			RMDBGLOG((LOCALDBG, "Clock is still unstable while waiting to enable TMDS!\n"));
		}
	}
	
	// Read current HDCP state
	if (pDH->part_caps.HDCP) {
		RMuint64 t_now = RMGetTimeInMicroSeconds();
		RMuint32 elapsed_ms = RMCyclesElapsed64(pDH->IntegrityLastCheck, t_now) / 1000;
		switch (pDH->part) {
		case DH_siI170:
		case DH_siI9030:
		case DH_siI9034:
		case DH_siI9134:
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_SII_ADDR, &hdcp_ctrl);
			if (RMSUCCEEDED(err)) {
				if (! use_intr_hdcp) {  // has not been set already
					pDH->HDMIState.HDCPLost = (pDH->HDMIState.HDCPState && ! ((hdcp_ctrl & 0x40) && (hdcp_ctrl & 0x01)));
				}
				pDH->HDMIState.HDCPState = ((hdcp_ctrl & 0x40) && (hdcp_ctrl & 0x01)) ? TRUE : FALSE;
			}
			break;
		case DH_ANX9030:
			err = RM_OK;
			if (pDH->CPDesired) {
				if (! pDH->ANX_bksv_ready) break;
				if (pDH->ANX_hdcp_wait_100ms_needed) {
					// disable audio
					err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x51, &reg);
					err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x51, reg & ~0x80);
					RMMicroSecondSleep(100 * 1000);
					pDH->ANX_hdcp_wait_100ms_needed = FALSE;
				}
				if (pDH->ANX_hdcp_auth_pass) {
					if (! pDH->ANX_srm_checked) {
						pDH->ANX_srm_checked = TRUE;
						//if (! ANX9030_Check_KSV_SRM()) {
						//	ANX9030_Blue_Screen_Enable();
						//	ANX9030_Clear_AVMute();
						//	pDH->ANX_ksv_srm_pass = FALSE;
						//	
						//	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA1, &c);
						//	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xA1, (c & (~0x08)));
						//} else
							pDH->ANX_ksv_srm_pass = TRUE;
					}
					if (! pDH->ANX_ksv_srm_pass && pDH->ANX_srm_checked) {
						RMDBGLOG((ENABLE, "KSV SRM fail."));
						//return;
					}
					err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA1, &reg);
					err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xA1, reg & 0xfc);
					if (! pDH->ANX_hdcp_encryption) {
						DHEnableEncryption_ANX(pDH);
						// enable audio
						err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x51, &reg);
						reg |= 0x80;
						err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x51, reg);
						pDH->ANX_auth_fully_pass = TRUE;
					}
				}
			}
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA0, &hdcp_stat);
//hdcp_stat |= 0x04;
			if (RMFAILED(err)) break;
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), HDCP_CTRL_ANX_ADDR, &hdcp_ctrl);
			if (RMSUCCEEDED(err)) {
				//if (! use_intr) {  // has not been set already
					pDH->HDMIState.HDCPLost = (pDH->HDMIState.HDCPState && ! ((hdcp_ctrl & 0x04) && (hdcp_stat & 0x04)));
				//}
				pDH->HDMIState.HDCPState = ((hdcp_ctrl & 0x04) && (hdcp_stat & 0x04)) ? TRUE : FALSE;
			}
			break;
		default:
			err = RM_ERROR;
		}
		if (RMFAILED(err)) {
			RMDBGLOG((ENABLE, "Failed to read HDCP_CTRL register!\n"));
			return RM_ERROR;
		}
		// Update encryption in pDH->state
		if ((pDH->state == DH_enabled) && pDH->HDMIState.HDCPState) {
			pDH->state = DH_enabled_encrypted;
			RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: ENABLED/ENCRYPTED\n"));
		} else if ((pDH->state == DH_enabled_encrypted) && ! pDH->HDMIState.HDCPState) {
			pDH->state = pDH->HDMIState.TMDSState ? DH_enabled : DH_disabled;
			RMDBGLOG((LOCALDBG, "TMDS/HDCP State is now: %sABLED/NOT ENCRYPTED\n", pDH->HDMIState.TMDSState ? "EN" : "DIS"));
		}
		
		// Report changes
		if (pDH->HDMIState.HDCPLost) {
			if (! manutest) fprintf(stderr, "[HDMI]    ***   HDCP has been lost, is now %s\n", pDH->HDMIState.HDCPState ? "ACTIVE" : "DISABLED");
			// Force immediate re-authentication
			if (pDH->CPDesired) {
				pDH->ForceIntegrityCheck = TRUE;
			}
		}
		
		// Execute integrity check every 2 seconds, unless RI_128 intr is available
		if ((! use_intr) || (! pDH->HDMIState.HDCPState)) {
			if (elapsed_ms > 2000) ri_128 = TRUE;
		}
		
		// Check HDCP integrity at least every 2 seconds or every 128 frames
		if ((pDH->cable != DH_disconnected) && (pDH->state != DH_disabled) && pDH->CPDesired && (pDH->ForceIntegrityCheck || ri_128) && (elapsed_ms >= HDCP_GUARD_TIME)) {
			RMuint8 i_cnt = RI_128_THRESHOLD;
			switch (pDH->part) {
			case DH_siI9030:
			case DH_siI9034:
			case DH_siI9134:
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x25, &i_cnt);
				break;
			case DH_ANX9030:
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0xA3, &i_cnt);
				break;
			default:
				break;
			}
			RMDBGPRINT((LOCALDBG, "HDCP Integrity Check after %lu mSec, SYS_CTRL:%02X, HDCP_CTRL:%02X, I_CNT:%02X\n", 
				elapsed_ms, sys_ctrl1, hdcp_ctrl, i_cnt));
			pDH->IntegrityLastCheck = t_now;
			pDH->ForceIntegrityCheck = FALSE;
			err = DHVerifyIntegrity(pDH);  // Verify HDCP is active
			if (RMFAILED(err)) {
				if (err != RM_DRM_INVALID_KEY) {
					t_now = RMGetTimeInMicroSeconds();
					RMDBGLOG((LOCALDBG, "HDCP Integrity failed, Re-Check after %lu mSec\n", 
						RMCyclesElapsed64(pDH->IntegrityLastCheck, t_now) / 1000));
					pDH->IntegrityLastCheck = t_now;
					err = DHVerifyIntegrity(pDH);  // Verify HDCP is active
				}
				if (RMFAILED(err)) {
					RMDBGLOG((ENABLE, "HDCP Integrity failed!!!\n"));
				}
			}
		}
	} else {  // no HDCP on this chip
		pDH->HDMIState.HDCPLost = FALSE;
		pDH->HDMIState.HDCPState = FALSE;
	}
	
	// Copy flag for DHGetConnection()
	if (pDH->HDMIState.ReceiverSignal) pDH->RxPresent = TRUE;
	
	// Return updated HDMIState struct to App
	if (pHDMIState) *pHDMIState = pDH->HDMIState;
	
	// DHCheckHDMI() is being called by App, don't reset chip on SetPixelClock anymore.
	if (pHDMIState) pDH->CheckClock = TRUE;
	
	return RM_OK;
}

// THIS FUNCTION IS OBSOLETE, USE DHCheckHDMI() INSTEAD!
// Note: DHIntegrityCheck could be called by an application taking care of 
// DVI/HDMI HDCP protection. 
// A better way for an application would be to implement an equivalent 
// function in itself, this function can be used as a reference.
RMstatus DHIntegrityCheck(struct DH_control *pDH, 
	RMbool *ReceiverPresent, 
	RMbool *HotPlugDetected, 
	RMbool *UpdateVideoMode)
{
	RMstatus err = RM_OK;
	enum DH_connection cable;
	RMuint64 t_now;
	
	CHECK_pDH("DHIntegrityCheck");
	CHECK_PTR("DHIntegrityCheck", UpdateVideoMode);
	
	*UpdateVideoMode = FALSE;
	t_now = RMGetTimeInMicroSeconds();
	if (RMCyclesElapsed64(pDH->HotPlugLastCheck, t_now) < 50 * 1000) return RM_TIMEOUT;  // less than 1/20th sec. since last call, skip
	pDH->HotPlugLastCheck = t_now;
	
	cable = pDH->cable;
	err = DHGetConnection(pDH, &cable, ReceiverPresent, HotPlugDetected);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "DHGetConnection() failed!\n"));
		return err;
	}
	
	/* receiver or hot plug state has changed */
	if ((cable == DH_disconnected) && (pDH->cable != DH_disconnected)) {
		RMDBGLOG((LOCALDBG, "DVI is disconnected\n"));
		pDH->cable = cable;
		
		// Turn off HDCP, info frames, and data output
		DHDisableHDMIMode(pDH);
		DHDisableOutput(pDH);
	} else if ((cable == DH_connected) && (pDH->cable == DH_disconnected)) {
		RMDBGLOG((LOCALDBG, "DVI is connected\n"));
		
		// Signal to Application, if new EDID negotiation is needed
		*UpdateVideoMode = TRUE;
		
		// Force Reset
		DHReset(pDH);
		
		// Mute output while HDCP is negotiated
		if (pDH->CPDesired) {
			DHMuteOutput(pDH, TRUE);
		}
		
		// Enable chip
		DHEnableOutput(pDH);
		
		pDH->cable = (pDH->HDMI_mode && pDH->part_caps.HDMI) ? DH_connection_HDMI : DH_connection_DVI;
	}
	
	if (
		(! *UpdateVideoMode) && (pDH->cable != DH_disconnected) && (pDH->state != DH_disabled) && pDH->CPDesired && 
		(RMCyclesElapsed64(pDH->IntegrityLastCheck, t_now) > HDCP_GUARD_TIME * 1000)  // not more often than every half second
	) {
		pDH->IntegrityLastCheck = t_now;
		// Verify HDCP is active
		err = DHVerifyIntegrity(pDH);
		if (RMFAILED(err)) {
			RMDBGLOG((ENABLE, "Integrity failed!!!\n"));
			return err;
		}
	}
	
	return RM_OK;
}


struct PNPVendorID {
	RMascii ID[4];
	RMascii Vendor[28];
};

static struct PNPVendorID PNPVendorID[] = {
	{"ADI", "ADI"}, 
	{"AOC", "AOC"}, 
	{"API", "Acer"}, 
	{"AST", "AST"}, 
	{"CPQ", "Compaq"}, 
	{"CTX", "CTX"}, 
	{"DEC", "Digital Equipment"}, 
	{"DEL", "Dell Computers"}, 
	{"DPC", "Delta Electronics"}, 
	{"DWE", "Daewoo"}, 
	{"ECS", "EliteGroup"}, 
	{"EIZ", "Eizo"}, 
	{"GSM", "LG Electronics"}, 
	{"HEI", "Hyundai"}, 
	{"HIT", "Hitachi"}, 
	{"HSL", "Hansol Electronics"}, 
	{"HTC", "Hitachi"}, 
	{"HWP", "Hewlett-Packard"}, 
	{"IBM", "IBM"}, 
	{"ICL", "Fujitsu ICL"}, 
	{"IVM", "Idek Iiyama"}, 
	{"KFC", "KFC Computek"}, 
	{"LTN", "Lite-On"}, 
	{"MAG", "MAG InnoVision"}, 
	{"MAX", "Maxdata Computer"}, 
	{"MEI", "Panasonic"}, 
	{"MEL", "Mitsubishi"}, 
	{"MIR", "Miro"}, 
	{"MTC", "Mitac"}, 
	{"NAN", "Nanao"}, 
	{"NEC", "NEC"}, 
	{"NOK", "Nokia"}, 
	{"OQI", "OptiQuest"}, 
	{"PGS", "Princeton Graphics"}, 
	{"PHL", "Philips"}, 
	{"PIO", "Pioneer"}, 
	{"REL", "Relisys"}, 
	{"SAM", "Samsung"}, 
	{"SDI", "Samtron"}, 
	{"SGM", "Sagem"}, 
	{"SMI", "Smile"}, 
	{"SNY", "Sony"}, 
	{"SPT", "Sceptre"}, 
	{"SRC", "Shamrock"}, 
	{"STP", "Sceptre"}, 
	{"TAT", "Tatung"}, 
	{"TRL", "TRL (Royal)"}, 
	{"UNM", "Unisys"}, 
	{"VSC", "ViewSonic"}, 
	{"WTC", "Wen Technology"}, 
	{"ZCM", "Zenith Data Systems"}, 
	{"XER", "Xerox"}, 
};

void DHPrintEDIDVendor(struct EDID_Data *edid) 
{
	RMascii ID[4], *Name;
	RMuint32 i;
	
	ID[0] = 'A' + ((edid->EDID_ManufacturerName[0] >> 2) & 0x1F) - 1;
	ID[1] = 'A' + ((edid->EDID_ManufacturerName[0] & 0x03) << 3) + ((edid->EDID_ManufacturerName[1] >> 5) & 0x07) - 1;
	ID[2] = 'A' + (edid->EDID_ManufacturerName[1] & 0x1F) - 1;
	ID[3] = '\0';
	Name = ID;
	
	for (i = 0; i < sizeof(PNPVendorID) / sizeof(struct PNPVendorID); i++) {
		if ((PNPVendorID[i].ID[0] == ID[0]) && (PNPVendorID[i].ID[1] == ID[1]) && (PNPVendorID[i].ID[2] == ID[2])) {
			Name = PNPVendorID[i].Vendor;
		}
	}
	
	RMDBGPRINT((ENABLE, "%s", Name));
}

// Override display's EDID with one specified by application
RMstatus DHSetEDID(struct DH_control *pDH, RMuint8 *pEDID, RMuint32 EDID_blocks)
{
	CHECK_pDH("DHSetEDID");
	
	pDH->pEDID_override = pEDID;
	pDH->EDID_override_blocks = EDID_blocks;
	pDH->EDID_blocks = 0;  // invalidate EDID cache
	
	return RM_OK;
}

RMstatus DHLoadEDIDVersion1(struct DH_control *pDH, struct EDID_Data *pData)
{
	RMstatus err = RM_OK;
	RMuint8 index, checksum;
	RMuint8* pParseData = (RMuint8 *) pData;
	
	CHECK_pDH("DHLoadEDIDVersion1");
	CHECK_PTR("DHLoadEDIDVersion1", pData);
	
	if (sizeof(struct EDID_Data) != EDID_SIZE) { // Didn't find compile options to check this.
		RMDBGLOG((ENABLE, "Error in DHLoadEDIDVersion1 (compiler issue), EDID_Data should be 128 bytes....\n"));
		return RM_INVALID_PARAMETER;
	}
	
	// Use cached block?
	if (pDH->EDID_blocks) {
		RMDBGLOG((LOCALDBG, "Using cached EDID block 0\n"));
		RMMemcpy(pData, pDH->EDID[0], EDID_SIZE);
		return RM_OK;
	}
	
	if (pDH->pEDID_override && pDH->EDID_override_blocks) {
		if (! manutest) fprintf(stderr, "[HDMI] Using Application-specified EDID data!\n");
		RMMemcpy(pData, pDH->pEDID_override, EDID_SIZE);
		err = RM_OK;
	} else {
		err = DHDDCBlockReadSegment(pDH, DDC_EDID_DEV, DDC_EDID_SEG, 0, (RMuint8 *)pData, sizeof(struct EDID_Data));
	}
	if (RMFAILED(err)) {
		if (! manutest) fprintf(stderr, "[HDMI] DHLoadEDIDVersion1() failed to read EDID block 0\n");
		return err;
	}
	
	for (index = 0; index < EDID_SIZE; index++) {
		if (index % 8 == 0) RMDBGPRINT((LOCALDBG, "EDID[0][%02X]: ", index));
		RMDBGPRINT((LOCALDBG, "%02X ", pParseData[index]));
		if (index % 8 == 7) RMDBGPRINT((LOCALDBG, "\n"));
	}
	
	// EDID should start with 0x00 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0x00
	if (
		(pData->EDID_Header[0] != 0x00) || 
		(pData->EDID_Header[1] != 0xFF) || 
		(pData->EDID_Header[2] != 0xFF) || 
		(pData->EDID_Header[3] != 0xFF) || 
		(pData->EDID_Header[4] != 0xFF) || 
		(pData->EDID_Header[5] != 0xFF) || 
		(pData->EDID_Header[6] != 0xFF) || 
		(pData->EDID_Header[7] != 0x00)
	) {
		if (! manutest) fprintf(stderr, "[HDMI] DHLoadEDIDVersion1 detected invalid header (%02X %02X %02X %02X %02X %02X %02X %02X)\n", 
			pData->EDID_Header[0], pData->EDID_Header[1], pData->EDID_Header[2], pData->EDID_Header[3],
			pData->EDID_Header[4], pData->EDID_Header[5], pData->EDID_Header[6], pData->EDID_Header[7]);
		err = RM_ERROR;
	}
	
	// Verify checksum
	checksum = 0;
	for (index = 0; index < EDID_SIZE; index++) {
		if ((index == EDID_SIZE - 1) && (((256 - (checksum & 0xFF)) & 0xFF) != *pParseData)) {
			RMDBGLOG((ENABLE, "Checksum should be 0x%02X, is 0x%02X\n", (256 - (checksum & 0xFF)) & 0xFF, *pParseData));
		}
		checksum += *pParseData++;
	}
	if (checksum != 0) { // 8 bit sum off all 128 bytes should be 0
		if (! manutest) fprintf(stderr, "[HDMI] DHLoadEDIDVersion1 detected invalid CheckSum = 0x%02X instead of 0\n", checksum);
		err = RM_ERROR;
	}
	
	return err;
}

RMstatus DHLoadEDIDBlock(struct DH_control *pDH, RMuint8 block_number, RMuint8 *pData, RMuint8 data_size)
{
	RMstatus err = RM_OK;
	RMuint8 index, checksum;
	
	CHECK_pDH("DHLoadEDIDBlock");
	CHECK_PTR("DHLoadEDIDBlock", pData);
	
	if (data_size < EDID_SIZE) {
		RMDBGLOG((ENABLE, "Call to DHLoadEDIDBlock with invalid buffer size : %d, should be %d\n", data_size, EDID_SIZE));
		return RM_INVALID_PARAMETER;
	}
	
	// Use cached block?
	if (pDH->EDID_blocks > block_number) {
		RMDBGLOG((LOCALDBG, "Using cached EDID block %lu\n", block_number));
		RMMemcpy(pData, pDH->EDID[block_number], EDID_SIZE);
		return RM_OK;
	}
	
	RMDBGLOG((LOCALDBG, "Call to DHLoadEDIDBlock\n"));
	if (pDH->pEDID_override && (pDH->EDID_override_blocks > block_number)) {
		if (! manutest) fprintf(stderr, "[HDMI] Using Application-specified EDID data!\n");
		RMMemcpy(pData, &(pDH->pEDID_override[block_number * EDID_SIZE]), EDID_SIZE);
		err = RM_OK;
	} else {
		err = DHDDCBlockReadSegment(pDH, DDC_EDID_DEV, DDC_EDID_SEG, block_number * EDID_SIZE, pData, EDID_SIZE);
	}
	if (RMFAILED(err)) {
		if (! manutest) fprintf(stderr, "[HDMI] DHLoadEDIDBlock() failed to read EDID block %d\n", block_number);
		return err;
	}
	
	for (index = 0; index < EDID_SIZE; index++) {
		if (index % 8 == 0) RMDBGPRINT((LOCALDBG, "EDID[%d][%02X]: ", block_number, index));
		RMDBGPRINT((LOCALDBG, "%02X ", pData[index]));
		if (index % 8 == 7) RMDBGPRINT((LOCALDBG, "\n"));
	}
	
	// Verify checksum
	checksum = 0;
	for (index = 0; index < EDID_SIZE; index++) {
		if ((index == EDID_SIZE - 1) && (((256 - (checksum & 0xFF)) & 0xFF) != *pData)) {
			RMDBGLOG((ENABLE, "Checksum should be 0x%02X, is 0x%02X\n", (256 - (checksum & 0xFF)) & 0xFF, *pData));
		}
		checksum += *pData++;
	}
	if (checksum != 0) { // 8 bit sum off all 128 bytes should be 0
		if (! manutest) fprintf(stderr, "[HDMI] DHLoadEDIDBlock #%d detected invalid CheckSum = %02X instead of 0\n", block_number, checksum);
		err = RM_ERROR;
	}
	
	return err;
}

RMstatus DHWriteEDIDBlock(struct DH_control *pDH, RMuint8 block_number, RMuint8 *pData, RMuint8 data_size)
{
	RMstatus err = RM_OK;
	RMuint8 index, checksum, *ptr;
	
	CHECK_pDH("DHWriteEDIDBlock");
	CHECK_PTR("DHWriteEDIDBlock", pData);
	if (data_size < EDID_SIZE) {
		RMDBGLOG((ENABLE, "Call to DHWriteEDIDBlock with invalid buffer size : %d, should be %d\n", data_size, EDID_SIZE));
		return RM_INVALID_PARAMETER;
	}
	
	RMDBGLOG((LOCALDBG, "Call to DHWriteEDIDBlock\n"));
	
	// Verify checksum
	checksum = 0;
	ptr = pData;
	for (index = 0; index < EDID_SIZE; index++) {
		if ((index == EDID_SIZE - 1) && (((256 - (checksum & 0xFF)) & 0xFF) != *ptr)) {
			RMDBGLOG((ENABLE, "Checksum should be 0x%02X, is 0x%02X\n", (256 - (checksum & 0xFF)) & 0xFF, *ptr));
		}
		checksum += *ptr++;
	}
	if (checksum != 0) { // 8 bit sum off all 128 bytes should be 0
		RMDBGLOG((ENABLE, "DHWriteEDIDBlock #%d detected invalid CheckSum = %02X instead of 0\n", block_number, checksum));
		return RM_ERROR;
	}
	
	err =  DHDDCBlockWriteSegment(pDH, DDC_EDID_DEV, DDC_EDID_SEG, block_number * 128, (RMuint8 *)pData, sizeof(struct EDID_Data));
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "DHWriteEDIDBlock() failed to write EDID block %d\n", block_number));
		return err;
	}
	
	return err;
}

RMstatus DHGetCEADataBlockCollection(RMuint8 *pedid, RMuint8 data_size, struct CEA861BDataBlockCollection *pDBC)
{
	RMuint32 dbc_index, dbc_end;
	RMuint32 i, n, tag;
	
	CHECK_PTR("DHGetCEADataBlockCollection", pedid);
	CHECK_PTR("DHGetCEADataBlockCollection", pDBC);
	
	RMMemset(pDBC, 0, sizeof(struct CEA861BDataBlockCollection));
	
	if ((pedid[0] != 0x2) || (pedid[1] != 0x3)) { // CEA Rev 3 extension present?
		RMDBGLOG((ENABLE, "Not a CEA EDID timing extension!\n"));
		return RM_ERROR;
	}
	
	pDBC->BasicTVSupport = pedid[3];
	pDBC->BasicTVValid = TRUE;
	dbc_index = 4;  // First byte of CEA 861B Data Block Colection
	dbc_end = pedid[2];  // First byte after CEA 861B Data Block Colection
	
	while (dbc_index < dbc_end) {
		tag = pedid[dbc_index] >> 5;
		n = pedid[dbc_index] & 0x1F;
		RMDBGLOG((LOCALDBG, "Call to DHGetCEADataBlockCollection (TAG = %02X - Byte = %02lX - size = %d)\n",
			tag, dbc_index, n));
		dbc_index++;
		
		switch (tag) {
		case VIDEO_DATA_BLOCK_SH_TAGS:
			for (i = 0; i < n; i++) {
				if (pDBC->NbShortVideoDescriptors < MAX_VIDEO_DESCRIPTORS) {
					pDBC->ShortVideoDescriptors[pDBC->NbShortVideoDescriptors++] = pedid[dbc_index + i];
				}
			}
			break;
		case AUDIO_DATA_BLOCK_SH_TAGS:
			for (i = 0; i < n - 2; i += 3) {
				if (pDBC->NbShortAudioDescriptors < MAX_AUDIO_DESCRIPTORS) {
					RMuint8 param;
					pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].AudioFormatCode = (pedid[dbc_index + i] >> 3) & 0xF;
					pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].MaxNumberOfChannels = (pedid[dbc_index + i] & 0x7) + 1;
					pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].FrequencyMask = pedid[dbc_index + i + 1] & 0x7F;
					param = pedid[dbc_index + i + 2];
					if (pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].AudioFormatCode == DH_AudioCodingType_PCM) {
						pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].u.BitMask = param & 0x7;
					} else if (pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].AudioFormatCode <= DH_AudioCodingType_ATRAC) {
						pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].u.MaximumBitrate = ((RMuint32)param) * 8000;
					} else if (pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].AudioFormatCode <= DH_AudioCodingType_MLP) {
						pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].u.Option = param;
					}
					RMDBGLOG((LOCALDBG, "DHGetCEADataBlockCollection: Audio format: %d, Max channels %d, Freq Mask: %02X\n",
						pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].AudioFormatCode, 
						pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].MaxNumberOfChannels, 
						pDBC->ShortAudioDescriptors[pDBC->NbShortAudioDescriptors].FrequencyMask));
					pDBC->NbShortAudioDescriptors++;
				}
			}
			break;
		case SPEAKER_ALLOCATION_TAGS:
			if (n == 3) {
				pDBC->SpeakerConfigurationCode = pedid[dbc_index];
				pDBC->SpeakerValid = TRUE;
			} else {
				RMDBGLOG((ENABLE, "Unknown Speaker Configuration!\n"));
			}
			break;
		case VENDOR_SPECIFIC_TAG:
			// HDMI IEEE RegID
			if (
				(pedid[dbc_index + 0] == 0x03) && 
				(pedid[dbc_index + 1] == 0x0C) && 
				(pedid[dbc_index + 2] == 0x00) 
			) {
				pDBC->HDMI_sink = TRUE;
				pDBC->HDMI_PhysAddr = ((pedid[dbc_index + 3] << 8) | pedid[dbc_index + 4]);
				if (n > 5) {
					pDBC->SinkCapability = pedid[dbc_index + 5];
					pDBC->Supports_AI = (pDBC->SinkCapability & SINK_SUPPORT_AI) ? TRUE : FALSE;  // kept for backwards compatibility
					pDBC->CEASinkValid = TRUE;
				}
				if (n > 6) {
					pDBC->MaxTMDSClock = pedid[dbc_index + 6];
					pDBC->CEATMDSValid = TRUE;
				}
				if (n > 7) {
					pDBC->LatencyPresent = pedid[dbc_index + 7];
					pDBC->CEALatencyValid = TRUE;
					if (n >  8) pDBC->VideoLatency = pedid[dbc_index + 8];
					if (n >  9) pDBC->AudioLatency = pedid[dbc_index + 9];
					if (n > 10) pDBC->InterlacedVideoLatency = pedid[dbc_index + 10];
					if (n > 11) pDBC->InterlacedAudioLatency = pedid[dbc_index + 11];
				}
			}
			break;
		case EXTENDED_DATA_BLOCK_TAG:
			switch (pedid[dbc_index]) {
			case EXTENDED_TAG_COLORIMETRY:  // Colorimetry Data Block
				pDBC->ColorimetrySupport = pedid[dbc_index + 1];
				pDBC->MetaDataProfile = pedid[dbc_index + 1];
				pDBC->ColorimetryValid = TRUE;
				break;
			case EXTENDED_TAG_VIDEO_CAPABILITY:  // Video Capability Data Block
				pDBC->VideoCapability = pedid[dbc_index + 1];
				pDBC->VideoValid = TRUE;
				break;
			}
			break;
		default:
			RMDBGLOG((ENABLE, "Reserved or invalid tag in CEA Data Block Collection!\n"));
			break;
		}
		dbc_index += n;
	}
	return RM_OK;
}

struct VideoFormat {
	enum EMhwlibTVStandard Standard;
	struct DH_VideoFormatInfo Info;
};

// IMPORTANT! change EDID mask system if more than 63 VICs
struct VideoFormat VideoFormats[] = {
	{ EMhwlibTVStandard_HDMI_640x480p59,   { DH_ar_4x3,  0, DH_no_repetition,       1, EMhwlibColorSpace_RGB_0_255 } },
	{ EMhwlibTVStandard_HDMI_640x480p60,   { DH_ar_4x3,  0, DH_no_repetition,       1, EMhwlibColorSpace_RGB_0_255 } },
	{ EMhwlibTVStandard_HDMI_480p59,       { DH_ar_4x3,  1, DH_no_repetition,       2, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480p60,       { DH_ar_4x3,  1, DH_no_repetition,       2, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480p59,       { DH_ar_16x9, 1, DH_no_repetition,       3, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480p60,       { DH_ar_16x9, 1, DH_no_repetition,       3, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_720p59,       { DH_ar_16x9, 0, DH_no_repetition,       4, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_720p60,       { DH_ar_16x9, 0, DH_no_repetition,       4, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_1080i59,      { DH_ar_16x9, 0, DH_no_repetition,       5, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_1080i60,      { DH_ar_16x9, 0, DH_no_repetition,       5, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_480i59,       { DH_ar_4x3,  1, DH_pixel_sent_twice,    6, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i60,       { DH_ar_4x3,  1, DH_pixel_sent_twice,    6, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i59,       { DH_ar_16x9, 1, DH_pixel_sent_twice,    7, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i60,       { DH_ar_16x9, 1, DH_pixel_sent_twice,    7, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_720x240p59,   { DH_ar_4x3,  1, DH_pixel_sent_twice,    8, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_720x240p60,   { DH_ar_4x3,  1, DH_pixel_sent_twice,    8, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_720x240p59,   { DH_ar_16x9, 1, DH_pixel_sent_twice,    9, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_720x240p60,   { DH_ar_16x9, 1, DH_pixel_sent_twice,    9, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x480i59,  { DH_ar_4x3,  1, DH_no_repetition,      10, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x480i60,  { DH_ar_4x3,  1, DH_no_repetition,      10, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x480i59,  { DH_ar_16x9, 1, DH_no_repetition,      11, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x480i60,  { DH_ar_16x9, 1, DH_no_repetition,      11, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x240p59,  { DH_ar_4x3,  1, DH_no_repetition,      12, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x240p60,  { DH_ar_4x3,  1, DH_no_repetition,      12, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x240p59,  { DH_ar_16x9, 1, DH_no_repetition,      13, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x240p60,  { DH_ar_16x9, 1, DH_no_repetition,      13, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_1440x480p59,  { DH_ar_4x3,  1, DH_no_repetition,      14, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_1440x480p60,  { DH_ar_4x3,  1, DH_no_repetition,      14, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_1440x480p59,  { DH_ar_16x9, 1, DH_no_repetition,      15, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_1440x480p60,  { DH_ar_16x9, 1, DH_no_repetition,      15, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_1080p59,      { DH_ar_16x9, 0, DH_no_repetition,      16, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_1080p60,      { DH_ar_16x9, 0, DH_no_repetition,      16, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_576p50,       { DH_ar_4x3,  1, DH_no_repetition,      17, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_576p50,       { DH_ar_16x9, 1, DH_no_repetition,      18, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_720p50,       { DH_ar_16x9, 0, DH_no_repetition,      19, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_1080i50,      { DH_ar_16x9, 0, DH_no_repetition,      20, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_576i50,       { DH_ar_4x3,  1, DH_pixel_sent_twice,   21, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_576i50,       { DH_ar_16x9, 1, DH_pixel_sent_twice,   22, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_720x288p50,   { DH_ar_4x3,  1, DH_pixel_sent_twice,   23, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_720x288p50,   { DH_ar_16x9, 1, DH_pixel_sent_twice,   24, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x576i50,  { DH_ar_4x3,  1, DH_no_repetition,      25, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x576i50,  { DH_ar_16x9, 1, DH_no_repetition,      26, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x288p50,  { DH_ar_4x3,  1, DH_no_repetition,      27, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_2880x288p50,  { DH_ar_16x9, 1, DH_no_repetition,      28, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_1440x576p50,  { DH_ar_4x3,  1, DH_no_repetition,      29, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_1440x576p50,  { DH_ar_16x9, 1, DH_no_repetition,      30, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_1080p50,      { DH_ar_16x9, 0, DH_no_repetition,      31, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_1080p23,      { DH_ar_16x9, 0, DH_no_repetition,      32, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_1080p24,      { DH_ar_16x9, 0, DH_no_repetition,      32, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_1080p25,      { DH_ar_16x9, 0, DH_no_repetition,      33, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_1080p29,      { DH_ar_16x9, 0, DH_no_repetition,      34, EMhwlibColorSpace_YUV_709 } },
	{ EMhwlibTVStandard_HDMI_1080p30,      { DH_ar_16x9, 0, DH_no_repetition,      34, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_2880x480p59,  { DH_ar_4x3,  1, DH_no_repetition,      35, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_2880x480p60,  { DH_ar_4x3,  1, DH_no_repetition,      35, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_2880x480p59,  { DH_ar_16x9, 1, DH_no_repetition,      36, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_2880x480p60,  { DH_ar_16x9, 1, DH_no_repetition,      36, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_2880x576p50,  { DH_ar_4x3,  1, DH_no_repetition,      37, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_2880x576p50,  { DH_ar_16x9, 1, DH_no_repetition,      38, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_1080i50_1250, { DH_ar_16x9, 0, DH_no_repetition,      39, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_1080i100,     { DH_ar_16x9, 0, DH_no_repetition,      40, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_720p100,      { DH_ar_16x9, 0, DH_no_repetition,      41, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_576p100,      { DH_ar_4x3,  1, DH_no_repetition,      42, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_576p100,      { DH_ar_16x9, 1, DH_no_repetition,      43, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_576i100,      { DH_ar_4x3,  1, DH_pixel_sent_twice,   44, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_576i100,      { DH_ar_16x9, 1, DH_pixel_sent_twice,   45, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_1080i119,     { DH_ar_16x9, 0, DH_no_repetition,      46, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_1080i120,     { DH_ar_16x9, 0, DH_no_repetition,      46, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_720p119,      { DH_ar_16x9, 0, DH_no_repetition,      47, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_720p120,      { DH_ar_16x9, 0, DH_no_repetition,      47, EMhwlibColorSpace_YUV_709 } }, 
	{ EMhwlibTVStandard_HDMI_480p119,      { DH_ar_4x3,  1, DH_no_repetition,      48, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480p120,      { DH_ar_4x3,  1, DH_no_repetition,      48, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480p119,      { DH_ar_16x9, 1, DH_no_repetition,      49, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480p120,      { DH_ar_16x9, 1, DH_no_repetition,      49, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480i119,      { DH_ar_4x3,  1, DH_pixel_sent_twice,   50, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i120,      { DH_ar_4x3,  1, DH_pixel_sent_twice,   50, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i119,      { DH_ar_16x9, 1, DH_pixel_sent_twice,   51, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i120,      { DH_ar_16x9, 1, DH_pixel_sent_twice,   51, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_576p200,      { DH_ar_4x3,  1, DH_no_repetition,      52, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_576p200,      { DH_ar_16x9, 1, DH_no_repetition,      53, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_576i200,      { DH_ar_4x3,  1, DH_pixel_sent_twice,   54, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_576i200,      { DH_ar_16x9, 1, DH_pixel_sent_twice,   55, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480p239,      { DH_ar_4x3,  1, DH_no_repetition,      56, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480p240,      { DH_ar_4x3,  1, DH_no_repetition,      56, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480p239,      { DH_ar_16x9, 1, DH_no_repetition,      57, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480p240,      { DH_ar_16x9, 1, DH_no_repetition,      57, EMhwlibColorSpace_YUV_601 } }, 
	{ EMhwlibTVStandard_HDMI_480i239,      { DH_ar_4x3,  1, DH_pixel_sent_twice,   58, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i240,      { DH_ar_4x3,  1, DH_pixel_sent_twice,   58, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i239,      { DH_ar_16x9, 1, DH_pixel_sent_twice,   59, EMhwlibColorSpace_YUV_601 } },
	{ EMhwlibTVStandard_HDMI_480i240,      { DH_ar_16x9, 1, DH_pixel_sent_twice,   59, EMhwlibColorSpace_YUV_601 } },
};

RMstatus DHGetCEADetailedTimingDescriptor(RMuint8 *pedid_dtc, struct CEADetailedTimingDescriptor *pDTD)
{
	CHECK_PTR("DHGetCEADetailedTimingDescriptor", pedid_dtc);
	CHECK_PTR("DHGetCEADetailedTimingDescriptor", pDTD);
	
	RMMemset(pDTD, 0, sizeof(struct CEADetailedTimingDescriptor));
	
	pDTD->PixelClock = ((((RMuint32) pedid_dtc[1]) << 8) + (RMuint32) pedid_dtc[0]) * 10000;
	if ((pDTD->PixelClock == 74170000) || (pDTD->PixelClock == 74180000)) pDTD->PixelClock = 74175824;
	if (pDTD->PixelClock == 148350000) pDTD->PixelClock = 148351648;
	pDTD->NbHorizActivePixels = ((((RMuint16) pedid_dtc[4]) << 4) & 0x0F00) | pedid_dtc[2];
	pDTD->NbHorizBlankingPixels = ((((RMuint16) pedid_dtc[4]) << 8) & 0x0F00) | pedid_dtc[3];
	pDTD->NbVertActiveLines = ((((RMuint16) pedid_dtc[7]) << 4) & 0x0F00) | pedid_dtc[5];
	pDTD->NbVertBlankingLines = ((((RMuint16) pedid_dtc[7]) << 8) & 0x0F00) | pedid_dtc[6];
	pDTD->HorizSyncOffset = ((((RMuint16) pedid_dtc[11]) << 2) & 0x0300) | pedid_dtc[8];
	pDTD->HorizSyncPulseWidth = ((((RMuint16) pedid_dtc[11]) << 4) & 0x0300) | pedid_dtc[9];
	pDTD->VertSyncOffset = ((((RMuint16) pedid_dtc[11]) << 2) & 0x0030) | ((pedid_dtc[10] >> 4) & 0xF);
	pDTD->VertSyncPulseWidth = ((((RMuint16) pedid_dtc[11]) << 4) & 0x0030) | (pedid_dtc[10] & 0xF);
	pDTD->HorizImageSize_mm = ((((RMuint16) pedid_dtc[14]) << 4) & 0x0F00) | pedid_dtc[12];
	pDTD->VertImageSize_mm = ((((RMuint16) pedid_dtc[14]) << 8) & 0x0F00) | pedid_dtc[13];
	pDTD->HorizBorder = pedid_dtc[15];
	pDTD->VertBorder = pedid_dtc[16];
	pDTD->Interlaced = pedid_dtc[17] >> 7;
	pDTD->StereoModeDecode = ((pedid_dtc[17] >> 4) & 0x6) | (pedid_dtc[17] & 0x1);
	pDTD->SyncSignalDescription = (pedid_dtc[17] >> 3) & 0x3;
	pDTD->SyncSignalInfo = (pedid_dtc[17] >> 1) & 0x3; // Meaning depends on pDTD->SyncSignalDescription value
	return RM_OK;
}

// returns TRUE if 'b' is within a +/- 10 range of 'a', sets 'x' to FALSE if no exact match
static RMbool compare_within_range(RMuint32 a, RMuint32 b, RMbool *x) 
{
	if (a != b) *x = FALSE;
	return ((a < b) ? b - a : a - b) <= 10;
}

RMstatus DHGetEmhwlibTVStandardFromCEADetailedTimingDescriptor(
	struct RUA *pRUA, 
	struct CEADetailedTimingDescriptor *pDTD,
	RMuint32 AspX, RMuint32 AspY, 
	enum EMhwlibTVStandard *pTVStandard, 
	RMuint32 *pVIC)
{
	RMuint32 i;
	struct EMhwlibTVFormatDigital fmt_d;
	enum EMhwlibTVStandard standard;
	RMbool exact;
	
	CHECK_PTR("DHGetEmhwlibTVStandardFromCEADetailedTimingDescriptor", pRUA);
	CHECK_PTR("DHGetEmhwlibTVStandardFromCEADetailedTimingDescriptor", pDTD);
	CHECK_PTR("DHGetEmhwlibTVStandardFromCEADetailedTimingDescriptor", pTVStandard);
	CHECK_PTR("DHGetEmhwlibTVStandardFromCEADetailedTimingDescriptor", pVIC);
	
	*pVIC = 0;
	
	// Match HDMI mode
	for (i = 0; i < sizeof(VideoFormats) / sizeof(VideoFormats[0]); i ++) {
		RUAExchangeProperty(pRUA, DisplayBlock, 
			RMDisplayBlockPropertyID_TVFormatDigital, 
			&(VideoFormats[i].Standard), sizeof(VideoFormats[i].Standard), 
			&fmt_d, sizeof(fmt_d));
		exact = TRUE;
		if (! compare_within_range(fmt_d.PixelClock, pDTD->PixelClock, &exact)) continue;
		if (! compare_within_range(fmt_d.ActiveWidth, pDTD->NbHorizActivePixels, &exact)) continue;
		if (! compare_within_range(fmt_d.ActiveHeight, pDTD->NbVertActiveLines, &exact)) continue;
		if (! compare_within_range(fmt_d.HTotalSize, pDTD->NbHorizActivePixels + pDTD->NbHorizBlankingPixels, &exact)) continue;
		if (! compare_within_range(fmt_d.Progressive ? fmt_d.VTotalSize : (fmt_d.VTotalSize / 2), pDTD->NbVertActiveLines + pDTD->NbVertBlankingLines, &exact)) continue;
		if (! compare_within_range(fmt_d.HSyncWidth, pDTD->HorizSyncPulseWidth, &exact)) continue;
		if (! compare_within_range(fmt_d.VSyncWidth, pDTD->VertSyncPulseWidth * 2, &exact)) continue;
		if (fmt_d.Progressive == pDTD->Interlaced) continue;
		
		if (VideoFormats[i].Info.multiple_aspect_ratios) {
			if ((AspX == 4) && (AspY == 3) && (VideoFormats[i].Info.aspect_ratio != DH_ar_4x3)) continue;
			if ((AspX == 16) && (AspY == 9) && (VideoFormats[i].Info.aspect_ratio != DH_ar_16x9)) continue;
		}
		RMDBGLOG((LOCALDBG, "extracted %sstandard: %s, VIC %ld\n", exact ? "exact " : "approx. ", TVFormatString[VideoFormats[i].Standard - 1], VideoFormats[i].Info.VIC));
		
		if (exact) *pTVStandard = VideoFormats[i].Standard;
		*pVIC = VideoFormats[i].Info.VIC;
		return RM_OK;
	}
	
	// Match VESA mode
	for (standard = EMhwlibTVStandard_VESA_640x350x85; standard <= EMhwlibTVStandard_VESA_1920x1080x60i; standard++) {
		RUAExchangeProperty(pRUA, DisplayBlock, 
			RMDisplayBlockPropertyID_TVFormatDigital, 
			&standard, sizeof(standard), 
			&fmt_d, sizeof(fmt_d));
		exact = TRUE;
		if (! compare_within_range(fmt_d.PixelClock, pDTD->PixelClock, &exact)) continue;
		if (! compare_within_range(fmt_d.ActiveWidth, pDTD->NbHorizActivePixels, &exact)) continue;
		if (! compare_within_range(fmt_d.ActiveHeight, pDTD->NbVertActiveLines, &exact)) continue;
		if (! compare_within_range(fmt_d.HTotalSize, pDTD->NbHorizActivePixels + pDTD->NbHorizBlankingPixels, &exact)) continue;
		if (! compare_within_range(fmt_d.Progressive ? fmt_d.VTotalSize : (fmt_d.VTotalSize / 2), pDTD->NbVertActiveLines + pDTD->NbVertBlankingLines, &exact)) continue;
		if (! compare_within_range(fmt_d.HSyncWidth, pDTD->HorizSyncPulseWidth, &exact)) continue;
		if (! compare_within_range(fmt_d.VSyncWidth, pDTD->VertSyncPulseWidth * 2, &exact)) continue;
		if (fmt_d.Progressive == pDTD->Interlaced) continue;
		
		RMDBGLOG((LOCALDBG, "extracted %sstandard: %s\n", exact ? "exact " : "approx. ", TVFormatString[standard - 1]));
		
		*pTVStandard = standard;
		*pVIC = 0;
		return RM_OK;
	}
	
	// Match CVT mode
	for (standard = EMhwlibTVStandard_CVT_640x480x50; standard <= EMhwlibTVStandard_CVT_1600x1200x60; standard++) {
		RUAExchangeProperty(pRUA, DisplayBlock, 
			RMDisplayBlockPropertyID_TVFormatDigital, 
			&standard, sizeof(standard), 
			&fmt_d, sizeof(fmt_d));
		exact = TRUE;
		if (! compare_within_range(fmt_d.PixelClock, pDTD->PixelClock, &exact)) continue;
		if (! compare_within_range(fmt_d.ActiveWidth, pDTD->NbHorizActivePixels, &exact)) continue;
		if (! compare_within_range(fmt_d.ActiveHeight, pDTD->NbVertActiveLines, &exact)) continue;
		if (! compare_within_range(fmt_d.HTotalSize, pDTD->NbHorizActivePixels + pDTD->NbHorizBlankingPixels, &exact)) continue;
		if (! compare_within_range(fmt_d.Progressive ? fmt_d.VTotalSize : (fmt_d.VTotalSize / 2), pDTD->NbVertActiveLines + pDTD->NbVertBlankingLines, &exact)) continue;
		if (! compare_within_range(fmt_d.HSyncWidth, pDTD->HorizSyncPulseWidth, &exact)) continue;
		if (! compare_within_range(fmt_d.VSyncWidth, pDTD->VertSyncPulseWidth * 2, &exact)) continue;
		if (fmt_d.Progressive == pDTD->Interlaced) continue;
		
		RMDBGLOG((LOCALDBG, "extracted %sstandard: %s\n", exact ? "exact " : "approx. ", TVFormatString[standard - 1]));
		
		*pTVStandard = standard;
		*pVIC = 0;
		return RM_OK;
	}
	
	return RM_ERROR;
}

RMstatus DHGetVideoInfoFromCEAVideoIdentificationCode(
	RMuint32 VIC, 
	RMbool sixtyhertz, // use 60Hz instead of 59.94Hz mode, if applicable
	enum EMhwlibTVStandard *pTVStandard, 
	RMuint32 *pAspX, RMuint32 *pAspY, 
	enum EMhwlibColorSpace *pColorSpace, 
	RMbool *pMultipleAspectRatios)
{
	RMuint32 i, n;
	
	if (pTVStandard) *pTVStandard = EMhwlibTVStandard_Custom;
	if (pAspX) *pAspX = 4;
	if (pAspY) *pAspY = 3;
	if (pColorSpace) *pColorSpace = EMhwlibColorSpace_YUV_601;
	if (pMultipleAspectRatios) *pMultipleAspectRatios = FALSE;
	
	n = sizeof(VideoFormats) / sizeof(VideoFormats[0]);
	for (i = 0; i < n; i ++) {
		if (VideoFormats[i].Info.VIC == VIC) {
			if (sixtyhertz && (i + 1 < n) && (VideoFormats[i + 1].Info.VIC == VIC)) i++;
			if (pTVStandard) *pTVStandard = VideoFormats[i].Standard;
			if (VideoFormats[i].Info.aspect_ratio == DH_ar_4x3) {
				if (pAspX) *pAspX = 4;
				if (pAspY) *pAspY = 3;
			} else if (VideoFormats[i].Info.aspect_ratio == DH_ar_16x9) {
				if (pAspX) *pAspX = 16;
				if (pAspY) *pAspY = 9;
			}
			if (pColorSpace) *pColorSpace = VideoFormats[i].Info.color_space;
			if (pMultipleAspectRatios) *pMultipleAspectRatios = VideoFormats[i].Info.multiple_aspect_ratios;
			return RM_OK;
		}
	}
	
	return RM_ERROR;
}

RMstatus DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(
	struct RUA *pRUA, 
	RMuint32 VIC, 
	RMbool sixtyhertz, // use 60Hz instead of 59.94Hz mode, if applicable
	enum EMhwlibTVStandard *pTVStandard, 
	RMuint32 *pAspX, RMuint32 *pAspY)
{
	return DHGetVideoInfoFromCEAVideoIdentificationCode(VIC, sixtyhertz, pTVStandard, pAspX, pAspY, NULL, NULL);
}

RMstatus DHGetEmhwlibDigitalFormatFromCEADetailedTimingDescriptor(
	struct CEADetailedTimingDescriptor *pDTD,
	struct EMhwlibTVFormatDigital *format_dig)
{
	CHECK_PTR("DHGetEmhwlibDigitalFormatFromCEADetailedTimingDescriptor", pDTD);
	CHECK_PTR("DHGetEmhwlibDigitalFormatFromCEADetailedTimingDescriptor", format_dig);
	
	// consistency and reasonability check
	if (pDTD->PixelClock < 1000000) return RM_ERROR;
	if (pDTD->NbHorizActivePixels < 100) return RM_ERROR;
	if (pDTD->NbVertActiveLines < 100) return RM_ERROR;
	if (pDTD->NbHorizBlankingPixels == 0) return RM_ERROR;
	if (pDTD->NbVertBlankingLines == 0) return RM_ERROR;
	
	// build EMhwlibTVFormatDigital from EDID Detailed Timing Descriptor data
	format_dig->PixelClock = pDTD->PixelClock;
	format_dig->ActiveWidth = pDTD->NbHorizActivePixels;
	format_dig->ActiveHeight = pDTD->NbVertActiveLines;
	format_dig->XOffset = pDTD->NbHorizBlankingPixels - pDTD->HorizSyncOffset;
	format_dig->YOffsetTop = pDTD->NbVertBlankingLines - pDTD->VertSyncOffset;
	format_dig->YOffsetBottom = (pDTD->Interlaced) ? format_dig->YOffsetTop : 0;
	if (pDTD->SyncSignalDescription == 3) {
		format_dig->VSyncActiveLow = ! (pDTD->SyncSignalInfo & 2);
		format_dig->HSyncActiveLow = ! (pDTD->SyncSignalInfo & 1);
	} else if (pDTD->SyncSignalDescription == 2) {
		format_dig->VSyncActiveLow = TRUE;
		format_dig->HSyncActiveLow = ! (pDTD->SyncSignalInfo & 1);
	} else {
		format_dig->VSyncActiveLow = TRUE;
		format_dig->HSyncActiveLow = TRUE;
	}
	format_dig->HTotalSize = pDTD->NbHorizActivePixels + pDTD->NbHorizBlankingPixels;
	format_dig->VTotalSize = pDTD->NbVertActiveLines + pDTD->NbVertBlankingLines;
	if (pDTD->Interlaced) format_dig->VTotalSize = format_dig->VTotalSize * 2 + 1;
	format_dig->TopFieldHeight = (pDTD->Interlaced) ? format_dig->VTotalSize : 0;
	format_dig->HSyncWidth = pDTD->HorizSyncPulseWidth;
	format_dig->VSyncWidth = pDTD->VertSyncPulseWidth * 2;
	format_dig->VSyncFineAdjust = 0;
	format_dig->ColorSpace = EMhwlibColorSpace_RGB_0_255;
	format_dig->TrailingEdge = FALSE;
	format_dig->VSyncDelay1PixClk = FALSE;
	format_dig->Progressive = ! pDTD->Interlaced;
	format_dig->VbiStandard = EMhwlibVbiStandard_Custom;
	format_dig->HDTVMode = TRUE;
	
	return RM_OK;
}

RMstatus DHGetEmhwlibAnalogFormatFromCEADetailedTimingDescriptor(
	struct CEADetailedTimingDescriptor *pDTD,
	struct EMhwlibTVFormatAnalog *format_analog)
{
	CHECK_PTR("DHGetEmhwlibAnalogFormatFromCEADetailedTimingDescriptor", pDTD);
	CHECK_PTR("DHGetEmhwlibAnalogFormatFromCEADetailedTimingDescriptor", format_analog);
	
	// consistency and reasonability check
	if (pDTD->PixelClock < 1000000) return RM_ERROR;
	if (pDTD->NbHorizActivePixels < 100) return RM_ERROR;
	if (pDTD->NbVertActiveLines < 100) return RM_ERROR;
	if (pDTD->NbHorizBlankingPixels == 0) return RM_ERROR;
	if (pDTD->NbVertBlankingLines == 0) return RM_ERROR;
	
	// build EMhwlibTVFormatAnalog from EDID Detailed Timing Descriptor data
	format_analog->PixelClock = pDTD->PixelClock;
	format_analog->ActiveWidth = pDTD->NbHorizActivePixels;
	format_analog->ActiveHeight = pDTD->NbVertActiveLines;
	format_analog->XOffset = pDTD->NbHorizBlankingPixels - pDTD->HorizSyncOffset;
	format_analog->YOffsetTop = pDTD->NbVertBlankingLines - pDTD->VertSyncOffset;
	format_analog->YOffsetBottom = (pDTD->Interlaced) ? format_analog->YOffsetTop : 0;
	if (pDTD->SyncSignalDescription == 3) {
		format_analog->VSyncActiveLow = ! (pDTD->SyncSignalInfo & 2);
		format_analog->HSyncActiveLow = ! (pDTD->SyncSignalInfo & 1);
	} else if (pDTD->SyncSignalDescription == 2) {
		format_analog->VSyncActiveLow = TRUE;
		format_analog->HSyncActiveLow = ! (pDTD->SyncSignalInfo & 1);
	} else {
		format_analog->VSyncActiveLow = TRUE;
		format_analog->HSyncActiveLow = TRUE;
	}
	format_analog->Width = pDTD->NbHorizActivePixels + pDTD->NbHorizBlankingPixels;
	format_analog->Height = pDTD->NbVertActiveLines + pDTD->NbVertBlankingLines;
	if (pDTD->Interlaced) format_analog->Height = format_analog->Height * 2 + 1;
	format_analog->ColorSpace = EMhwlibColorSpace_YUV_601; // To be reviewed...
	format_analog->Progressive = ! pDTD->Interlaced;
	format_analog->ComponentMode = EMhwlibComponentMode_RGB_SCART;
	format_analog->CompositeMode = EMhwlibCompositeMode_Disable;
	format_analog->HDTVMode = TRUE;
	format_analog->HDSyncDown = TRUE;
	format_analog->OversampledInput = FALSE;
	format_analog->ChromaFilter = EMhwlibChromaFilter_3_25_MHz;
	format_analog->LumaFilter = EMhwlibLumaFilter_6_5_MHz;
	format_analog->SyncOnPbPr = FALSE;
	format_analog->Pedestal = FALSE;
	format_analog->HSync0 = format_analog->HSyncActiveLow ? 0 : pDTD->HorizSyncPulseWidth;
	format_analog->HSync1 = format_analog->HSyncActiveLow ? pDTD->HorizSyncPulseWidth : 0;
	format_analog->VSyncO0Line = format_analog->VSyncActiveLow ? 1 : pDTD->VertSyncPulseWidth + 1;
	format_analog->VSyncO0Pixel = format_analog->HSync0;
	format_analog->VSyncO1Line = format_analog->VSyncActiveLow ? pDTD->VertSyncPulseWidth + 1 : 1;
	format_analog->VSyncO1Pixel = format_analog->HSync1;
	if (pDTD->Interlaced) {
		format_analog->VSyncE0Line = format_analog->Height / 2 + (format_analog->VSyncActiveLow ? 1 : pDTD->VertSyncPulseWidth + 1);
		format_analog->VSyncE0Pixel = format_analog->Width / 2 + format_analog->HSync0;
		format_analog->VSyncE1Line = format_analog->Height / 2 + (format_analog->VSyncActiveLow ? pDTD->VertSyncPulseWidth + 1: 1);
		format_analog->VSyncE1Pixel = format_analog->Width / 2 + format_analog->HSync1;
	} else {
		format_analog->VSyncE0Line = 0;
		format_analog->VSyncE0Pixel = 0;
		format_analog->VSyncE1Line = 0;
		format_analog->VSyncE1Pixel = 0;
	}
	format_analog->HDHSyncWidth = pDTD->HorizSyncPulseWidth;
	format_analog->HDVSyncWidth = 0;
	format_analog->HDVSyncStart = 0;
	format_analog->VbiStandard = EMhwlibVbiStandard_Custom;
	
	return RM_OK;
}

RMstatus DHGetVideoFormatInfo(
	enum EMhwlibTVStandard TvStandard, 
	RMuint32 AspX, RMuint32 AspY, 
	struct DH_VideoFormatInfo *pvideo_format_info)
{
	RMuint8 nb_standards = sizeof(VideoFormats) / sizeof(VideoFormats[0]);
	RMuint8 index;
	
	CHECK_PTR("DHGetVideoFormatInfo", pvideo_format_info);
	
	RMDBGLOG((LOCALDBG, "Search for TV format %d in DHGetVideoFormatInfo\n", TvStandard));
	for (index = 0; index < nb_standards; index++) {
		RMDBGLOG((DISABLE, "TV format %d\n", VideoFormats[index].Standard));
		if (VideoFormats[index].Standard != TvStandard) continue;
		if (VideoFormats[index].Info.multiple_aspect_ratios) {
			if ((AspX == 4) && (AspY == 3) && (VideoFormats[index].Info.aspect_ratio != DH_ar_4x3)) continue;
			if ((AspX == 16) && (AspY == 9) && (VideoFormats[index].Info.aspect_ratio != DH_ar_16x9)) continue;
			RMDBGLOG((ENABLE, "Aspect Ratio found!\n"));
		}
		*pvideo_format_info = VideoFormats[index].Info;
		return RM_OK;
	}
	RMDBGLOG((ENABLE, "TV standard not found!!!\n"));
	return RM_ERROR;
}

#ifdef _DEBUG
#define NB_SHORT_VIDEO_DESCRIPTORS 60

static RMascii *CEA861_ShortDescriptorVideoResolutions[NB_SHORT_VIDEO_DESCRIPTORS] = {
	"No video code",
	"640x480p 59.94/60Hz (4:3)",          // 1
	"720x480p 59.94/60Hz (4:3)",          // 2
	"720x480p 59.94/60Hz (16:9)",         // 3
	"1280x720p 59.94/60Hz (16:9)",        // 4
	"1920x1080i 59.94/60Hz (16:9)",       // 5
	"720(1440)x480i 59.94/60Hz (4:3)",    // 6
	"720(1440)x480i 59.94/60Hz (16:9)",   // 7
	"720(1440)x240p 59.94/60Hz (4:3)",    // 8
	"720(1440)x240p 59.94/60Hz (16:9)",   // 9
	"(2880)x480i 59.94/60Hz (4:3)",       // 10
	"(2880)x480i 59.94/60Hz (16:9)",      // 11
	"(2880)x240p 59.94/60Hz (4:3)",       // 12
	"(2880)x240p 59.94/60Hz (16:9)",      // 13
	"1440x480p 59.94/60Hz (4:3)",         // 14
	"1440x480p 59.94/60Hz (16:9)",        // 15
	"1920x1080p 59.94/60Hz (16:9)",       // 16
	"720x576p 50Hz (4:3)",                // 17
	"720x576p 50Hz (16:9)",               // 18
	"1920x720p 50Hz (16:9)",              // 19
	"1920x1080i 50Hz (16:9)",             // 20
	"720(1440)x576i 50Hz (4:3)",          // 21
	"720(1440)x576i 50Hz (16:9)",         // 22
	"720(1440)x288p 50Hz (4:3)",          // 23
	"720(1440)x288p 50Hz (16:9)",         // 24
	"(2880)x576i 50Hz (4:3)",             // 25
	"(2880)x576i 50Hz (16:9)",            // 26
	"(2880)x288p 50Hz (4:3)",             // 27
	"(2880)x288p 50Hz (16:9)",            // 28
	"1440x576p 50Hz (4:3)",               // 29
	"1440x576p 50Hz (16:9)",              // 30
	"1920x1080p 50Hz (16:9)",             // 31
	"1920x1080p 23.97/24Hz (16:9)",       // 32
	"1920x1080p 25Hz (16:9)",             // 33
	"1920x1080p 29.97/30Hz (16:9)",       // 34
	"(2880)x480p 59.94/60Hz (4:3)",       // 35
	"(2880)x480p 59.94/60Hz (16:9)",      // 36
	"(2880)x576p 50Hz (4:3)",             // 37
	"(2880)x576p 50Hz (16:9)",            // 38
	"1920x1080i(1250) 50Hz (16:9)",       // 39
	"1920x1080i 100Hz (16:9)",            // 40
	"1920x720p 100Hz (16:9)",             // 41
	"720x576p 100Hz (4:3)",               // 42
	"720x576p 100Hz (16:9)",              // 43
	"720(1440)x576i 100Hz (4:3)",         // 44
	"720(1440)x576i 100Hz (16:9)",        // 45
	"1920x1080i 119.88/120Hz (16:9)",     // 46
	"1280x720p 119.88/120Hz (16:9)",      // 47
	"720x480p 119.88/120Hz (4:3)",        // 48
	"720x480p 119.88/120Hz (16:9)",       // 49
	"720(1440)x480i 119.88/120Hz (4:3)",  // 50
	"720(1440)x480i 119.88/120Hz (16:9)", // 51
	"720x576p 200Hz (4:3)",               // 52
	"720x576p 200Hz (16:9)",              // 53
	"720(1440)x576i 200Hz (4:3)",         // 54
	"720(1440)x576i 200Hz (16:9)",        // 55
	"720x480p 239.76/240Hz (4:3)",        // 56
	"720x480p 239.76/240Hz (16:9)",       // 57
	"720(1440)x480i 239.76/240Hz (4:3)",  // 58
	"720(1440)x480i 239.76/240Hz (16:9)", // 59
};

#endif

static RMstatus get_vic_from_descriptor(
	struct CEA861BDataBlockCollection pDBC[], 
	RMuint32 nDBC, 
	RMuint32 descriptor, 
	RMuint32 *pVIC)
{
	RMuint8 dbc;
	
	for (dbc = 0; dbc < nDBC; dbc++) {
		if (descriptor > pDBC[dbc].NbShortVideoDescriptors) {
			descriptor -= pDBC[dbc].NbShortVideoDescriptors;
		} else {
			*pVIC = pDBC[dbc].ShortVideoDescriptors[descriptor - 1] & 0x7F;
			return RM_OK;
		}
	}
	
	return RM_ERROR;
}

/* Compare two values and return a weighted match value.
   Return value is 'weight' if both values are equal,
   or 50% of 'weight' minus the percentage points 'a' is away from 'b'. */
static inline RMuint32 match_weighted(RMuint32 a, RMuint32 b, RMuint32 weight) {
	RMuint32 dist;
	
	RMDBGLOG((DISABLE, "Compare: %ld <> %ld\n", a, b));
	if (a == b) return weight;
	dist = (a > b) ? a - b : b - a;
	weight /= 2;
	dist = (dist * 100) / b;
	return (weight > dist) ? weight - dist : 0;
}

static RMstatus get_matching_vic(
	struct RUA *pRUA, 
	struct CEA861BDataBlockCollection pDBC[], 
	RMuint32 nDBC, 
	RMuint32 vfreq, 
	RMuint32 hsize, 
	RMuint32 vsize, 
	RMbool intl, 
	RMuint32 asp_x, 
	RMuint32 asp_y, 
	RMuint64 display_mask, 
	RMuint32 max_pixclk, 
	RMuint32 min_pixclk, 
	RMuint32 max_hfreq, 
	RMuint32 min_hfreq, 
	RMuint32 max_vfreq, 
	RMuint32 min_vfreq, 
	RMuint32 *pVIC, 
	enum EMhwlibTVStandard *pStandard, 
	RMuint32 *pAspX, RMuint32 *pAspY)
{
	RMstatus err = RM_OK;
	RMuint8 dbc, dbc_count;
	RMuint32 VIC, match, bestmatch = 0;
	RMbool sixtyhertz, onethousandone;
	RMuint32 vf;
	struct EMhwlibTVFormatDigital fmt_d;
	
	RMDBGLOG((ENABLE, "get_matching_vic(v=%lu, w=%lu, h=%lu, i=%u, x=%lu, y=%lu)\n", 
		vfreq, hsize, vsize, intl, asp_x, asp_y));
	*pVIC = 0;
	if ((! asp_x) || (! asp_y)) {
		asp_x = 16;
		asp_y = 9;
	}
	sixtyhertz = ((vfreq == 24) || (vfreq == 30) || (vfreq == 60) || (vfreq == 120) || (vfreq == 240));
	onethousandone = ((vfreq == 23) || (vfreq == 29) || (vfreq == 59) || (vfreq == 119) || (vfreq == 239));
	if (onethousandone) vfreq++;
	for (dbc = 0; dbc < nDBC; dbc++) {
		for (dbc_count = 0; dbc_count < pDBC[dbc].NbShortVideoDescriptors; dbc_count++) {
			VIC = pDBC[dbc].ShortVideoDescriptors[dbc_count] & 0x7F;
			// exclude VICs not in display_mask
			if (! (display_mask & (1LL << VIC))) continue;
			
			// get TVStandard from VIC number
			DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(pRUA, VIC, sixtyhertz, pStandard, pAspX, pAspY);
			*pAspX &= 0xFF;
			*pAspY &= 0xFF;
			RUAExchangeProperty(pRUA, DisplayBlock, 
				RMDisplayBlockPropertyID_TVFormatDigital, 
				pStandard, sizeof(*pStandard), 
				&fmt_d, sizeof(fmt_d));
			if (fmt_d.Progressive == intl) continue;
			if (! fmt_d.Progressive) {
				fmt_d.ActiveHeight *= 2;
				fmt_d.VTotalSize /= 2;
			}
			
			// exclude modes not matching range limits
			if (max_pixclk && (fmt_d.PixelClock > max_pixclk)) continue;
			if (min_pixclk && (fmt_d.PixelClock < min_pixclk)) continue;
			if (max_hfreq && (fmt_d.PixelClock / fmt_d.HTotalSize > max_hfreq)) continue;
			if (min_hfreq && (fmt_d.PixelClock / fmt_d.HTotalSize < min_hfreq)) continue;
			if (max_vfreq && (fmt_d.PixelClock / fmt_d.HTotalSize / fmt_d.VTotalSize > max_vfreq)) continue;
			if (min_vfreq && (fmt_d.PixelClock / fmt_d.HTotalSize / fmt_d.VTotalSize < min_vfreq)) continue;
			
			vf = vfreq * fmt_d.HTotalSize;
			if (onethousandone) vf = (vf * 1000) / 1001;
			vf *= fmt_d.VTotalSize;
			match = 0;
			match += match_weighted(fmt_d.PixelClock, vf , 200);
			match += match_weighted(fmt_d.ActiveWidth, hsize, 400);
			match += match_weighted(fmt_d.ActiveHeight, vsize, 400);
			match += match_weighted((*pAspX) * 10000 / (*pAspY), asp_x * 10000 / asp_y, 400);
			if (match > bestmatch) {
				RMDBGLOG((LOCALDBG, "Match: VIC %ld %s is better than VIC %ld %s (%ld > %ld)\n", 
					VIC, CEA861_ShortDescriptorVideoResolutions[VIC], 
					*pVIC, CEA861_ShortDescriptorVideoResolutions[*pVIC], 
					match, bestmatch));
				bestmatch = match;
				*pVIC = VIC;
			}
		}
	}
	
	if (! *pVIC) {
		*pVIC = 1;  // fallback to 640x480@60
		err = RM_ERROR;
	}
	
	DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(pRUA, *pVIC, sixtyhertz, pStandard, pAspX, pAspY);
	*pAspX &= 0xFF;
	*pAspY &= 0xFF;
	
	return err;
}

static RMstatus get_native_vic(
	struct CEA861BDataBlockCollection pDBC[], 
	RMuint32 nDBC, 
	RMuint32 *pVIC)
{
	RMuint8 dbc, dbc_count;
	
	*pVIC = 0;
	// take first native mode
	for (dbc = 0; dbc < nDBC; dbc++) {
		for (dbc_count = 0; dbc_count < pDBC[dbc].NbShortVideoDescriptors; dbc_count++) {
			if (pDBC[dbc].ShortVideoDescriptors[dbc_count] & 0x80) {
				*pVIC = pDBC[dbc].ShortVideoDescriptors[dbc_count] & 0x7F;
				return RM_OK;
			}
		}
	}
	return RM_ERROR;
}

static RMstatus DHGetVideoModeFromDTD(
	struct DH_control *pDH, 
	enum EMhwlibTVStandard *standard, 
	struct EMhwlibTVFormatDigital *format_digital, 
	struct EMhwlibTVFormatAnalog *format_analog, 
	RMbool *UseStandard, 
	RMuint32 *pVIC, 
	RMuint32 *ar_x, 
	RMuint32 *ar_y, 
	struct CEADetailedTimingDescriptor *pDTD)
{
	RMstatus err;
	
	// Try to match DTD against a pre-existing TVStandard
	*standard = EMhwlibTVStandard_Custom;
	err = DHGetEmhwlibTVStandardFromCEADetailedTimingDescriptor(pDH->pRUA, 
		pDTD, *ar_x, *ar_y, standard, pVIC);
	
	if ((! *pVIC) && pDTD->HorizImageSize_mm && pDTD->VertImageSize_mm) {
		// trick, works great for approx. 4:3, 16:9, 16:10 etc.
		*ar_x = 16;
		*ar_y = (pDTD->VertImageSize_mm * (*ar_x) + (pDTD->HorizImageSize_mm / 2)) / pDTD->HorizImageSize_mm;
		if (((*ar_y) % 4) == 0) {
			*ar_x /= 4;
			*ar_y /= 4;
		}
	}
	if (RMFAILED(err) || (*standard == EMhwlibTVStandard_Custom)) {
		// if that fails, derive TVFormat from EDID detailed descriptor
		*UseStandard = FALSE;
		*standard = EMhwlibTVStandard_Custom;
		
		if (RMFAILED(err = DHGetEmhwlibDigitalFormatFromCEADetailedTimingDescriptor(pDTD, format_digital))) {
			RMDBGLOG((ENABLE, "Cannot get Digital Format from descriptor!\n"));
			// fallback to VIC 1
			*UseStandard = TRUE;
			*pVIC = 1;
		} else 
		if (RMFAILED(err = DHGetEmhwlibAnalogFormatFromCEADetailedTimingDescriptor(pDTD, format_analog))) {
			RMDBGLOG((ENABLE, "Cannot get Analog Format from descriptor!\n"));
			// fallback to VIC 1
			*UseStandard = TRUE;
			*pVIC = 1;
		} else {
			err = RM_OK;
		}
	}
	
	return err;
}

RMstatus DHGetVideoModeFromEDID(
	struct DH_control *pDH, 
	enum EMhwlibTVStandard *standard, 
	struct EMhwlibTVFormatDigital *format_digital, 
	struct EMhwlibTVFormatAnalog *format_analog, 
	RMbool *UseStandard, 
	RMuint32 *pVIC, 
	RMuint32 *ar_x, 
	RMuint32 *ar_y, 
	RMbool *pHDMI_display, 
	struct CEA861BDataBlockCollection pDBC[], 
	RMuint32 *pnDBC, 
	enum EMhwlibColorSpace *color_space)
{
	struct EDID_Data edid;
	RMuint32 rpt, block, nDBC = 0;
	RMstatus err;
	RMbool underscan = FALSE, yuv_444 = FALSE, yuv_422 = FALSE, basic_audio = FALSE;
	RMuint64 EDID_display_mask = 0;  // VICs the display is able to do
	RMbool CEA_extensions = FALSE;
	struct CEADetailedTimingDescriptor DTD;
	RMuint32 i, n;
	RMuint32 target_ar_x, target_ar_y;
	RMbool target_rgb;
	RMbool own_DBC = FALSE;
	RMbool sixtyhertz = FALSE;
	
RMDBGLOG((ENABLE, "DHGetVideoModeFromEDID()\n"));
	CHECK_pDH("DHGetVideoModeFromEDID");
	
	if (ar_x && ar_y) {
		target_ar_x = *ar_x;
		target_ar_y = *ar_y;
	} else {
		target_ar_x = 0;
		target_ar_y = 0;
	}
	if (! target_ar_x || ! target_ar_y) {
		target_ar_x = 16;
		target_ar_y = 9;
	}
	target_rgb = FALSE;
	if (color_space) {
		target_rgb = ((*color_space == EMhwlibColorSpace_RGB_0_255) || (*color_space == EMhwlibColorSpace_RGB_16_235));
	}
	if (pVIC) *pVIC = 0;
	if (pHDMI_display) *pHDMI_display = FALSE;
	if (color_space) *color_space = EMhwlibColorSpace_RGB_0_255;
	if (pDBC && pnDBC) {
		nDBC = *pnDBC;
	} else {
		pDBC = NULL;
		pnDBC = NULL;
	}
	
	// get EDID block 0
	rpt = MAX_EDID_ATTEMPTS;
	do {
		err = DHLoadEDIDVersion1(pDH, &edid);
	} while (RMFAILED(err) && --rpt);
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Cannot load EDID Version 1 information from EEPROM (or info is incorrect)!\n"));
		if (UseStandard) *UseStandard = TRUE;  // No EDID, fall back to 640x480@60Hz
		nDBC = 0;
		if (pnDBC) *pnDBC = nDBC;
		
		if ((pDH->EDID_select == DH_EDID_none) || (! (standard && format_digital && format_analog && UseStandard && pVIC && ar_x && ar_y && color_space))) {
			return RM_ERROR;
		}
	} else {
		RMDBGPRINT((ENABLE, "Monitor vendor: "));
		DHPrintEDIDVendor(&edid);
		RMDBGPRINT((ENABLE, "\n"));
		
		// get EDID extension blocks
		if (pDBC == NULL) {
			pDBC = (struct CEA861BDataBlockCollection *)RMMalloc(edid.EDID_Extension * sizeof(struct CEA861BDataBlockCollection));
			if (pDBC) {
				own_DBC = TRUE;
				nDBC = edid.EDID_Extension;
			} else {
				nDBC = 0;
			}
		}
		n = 0;
		for (block = 1; block <= edid.EDID_Extension; block++) {
			RMuint8 edid_block[128];
			rpt = MAX_EDID_ATTEMPTS;
			do {
				err = DHLoadEDIDBlock(pDH, block, edid_block, sizeof(edid_block));
			} while (RMFAILED(err) && --rpt);
			if (RMFAILED(err)) {
				RMDBGLOG((ENABLE, "Cannot load EDID block %lu!\n", block));
			} else if (n < nDBC) {
				if (RMFAILED(DHGetCEADataBlockCollection(edid_block, sizeof(edid_block), &(pDBC[n])))) {
					RMDBGLOG((ENABLE, "Cannot parse DBC from EDID block %lu!\n", block));
				} else {
					if (pDBC[n].HDMI_sink) 
						CEA_extensions = TRUE;
					underscan = (edid_block[3] & TV_SUPPORT_UNDERSCAN) ? TRUE : FALSE;
					basic_audio = (edid_block[3] & TV_SUPPORT_BASIC_AUDIO) ? TRUE : FALSE;
					yuv_444 = (edid_block[3] & TV_SUPPORT_YUV444) ? TRUE : FALSE;
					yuv_422 = (edid_block[3] & TV_SUPPORT_YUV422) ? TRUE : FALSE;
					n++;
				}
			}
		}
		nDBC = n;
		if (pnDBC) *pnDBC = nDBC;
		if (pHDMI_display) *pHDMI_display = CEA_extensions;
		
		if ((pDH->EDID_select == DH_EDID_none)&&(standard && format_digital && format_analog))
		{

			RMDBGLOG((LOCALDBG,"Convert TVStandard %d to format_digital\n",*standard)) ;
			RUAExchangeProperty(pDH->pRUA, DisplayBlock, 
				RMDisplayBlockPropertyID_TVFormatDigital, 
				standard, sizeof(*standard), 
				format_digital, sizeof(*format_digital));
		}

		if ((pDH->EDID_select == DH_EDID_none) || (! (standard && format_digital && format_analog && UseStandard && pVIC && ar_x && ar_y && color_space))) {
			// Only read DBC blocks from EDID, we're done.
			if (own_DBC) RMFree(pDBC);
			return RM_OK;
		}
		
		*UseStandard = TRUE;
		*standard = EMhwlibTVStandard_Custom;
		// fill format_digital & format_analog with native or preferred DTD
		i = 0;
		do {
			DTD.PixelClock = 0;
			err = DHGetCEADetailedTimingDescriptor((i == 0) ? edid.EDID_Descriptor1 : edid.EDID_Descriptor2, &DTD);
			if (RMSUCCEEDED(err)) {
				if (format_digital && format_analog) {
					err = DHGetVideoModeFromDTD(pDH, 
						standard, format_digital, format_analog, 
						UseStandard, pVIC, ar_x, ar_y, 
						&DTD);
					if (RMSUCCEEDED(err) && *pVIC) {  // update aspect ratio
						DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(pDH->pRUA, *pVIC, sixtyhertz, standard, ar_x, ar_y);
					}
				} else {
					*UseStandard = TRUE;
					*standard = EMhwlibTVStandard_Custom;
					err = DHGetEmhwlibTVStandardFromCEADetailedTimingDescriptor(pDH->pRUA, 
						&DTD, *ar_x, *ar_y, standard, pVIC);
					if (*standard == EMhwlibTVStandard_Custom) {
						err = RM_ERROR;  // no format_digital or format_analog
					}
					if (RMSUCCEEDED(err) && *pVIC) {  // update aspect ratio
						DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(pDH->pRUA, *pVIC, sixtyhertz, standard, ar_x, ar_y);
					}
				}
				if (RMSUCCEEDED(err)) {
					// check if found mode needs to be excluded
					if (*UseStandard && *pVIC) {
						if (pDH->EDID_exclude_mask & (1LL << *pVIC)) err = RM_ERROR;
						if ((pDH->EDID_force_mask) && ! (pDH->EDID_force_mask & (1LL << *pVIC))) err = RM_ERROR;
					}
					if (RMFAILED(err)) RMDBGLOG((ENABLE, "DTD %lu not matching masks\n", i + 1));
					if (pDH->EDID_max_pixclk && (DTD.PixelClock > pDH->EDID_max_pixclk)) err = RM_ERROR;
					if (pDH->EDID_min_pixclk && (DTD.PixelClock < pDH->EDID_min_pixclk)) err = RM_ERROR;
					if (RMFAILED(err)) RMDBGLOG((ENABLE, "DTD %lu pixclk %lu not matching pixclk %lu..%lu Hz\n", i + 1, DTD.PixelClock, pDH->EDID_min_pixclk, pDH->EDID_max_pixclk));
					if (pDH->EDID_max_hfreq || pDH->EDID_min_hfreq) {
						RMuint32 hfreq = DTD.PixelClock / 
							(DTD.NbHorizActivePixels + DTD.NbHorizBlankingPixels);
						if (pDH->EDID_max_hfreq && (hfreq > pDH->EDID_max_hfreq)) err = RM_ERROR;
						if (pDH->EDID_min_hfreq && (hfreq < pDH->EDID_min_hfreq)) err = RM_ERROR;
						if (RMFAILED(err)) RMDBGLOG((ENABLE, "DTD %lu hfreq %lu not matching hfreq %lu..%lu Hz\n", i + 1, hfreq, pDH->EDID_min_hfreq, pDH->EDID_max_hfreq));
					}
					if (pDH->EDID_max_vfreq || pDH->EDID_min_vfreq) {
						RMuint32 vfreq = DTD.PixelClock / 
							(DTD.NbHorizActivePixels + DTD.NbHorizBlankingPixels) / 
							(DTD.NbVertActiveLines + DTD.NbVertBlankingLines);
						if (pDH->EDID_max_vfreq && (vfreq > pDH->EDID_max_vfreq)) err = RM_ERROR;
						if (pDH->EDID_min_vfreq && (vfreq < pDH->EDID_min_vfreq)) err = RM_ERROR;
						if (RMFAILED(err)) RMDBGLOG((ENABLE, "DTD %lu vfreq %lu not matching vfreq %lu..%lu Hz\n", i + 1, vfreq, pDH->EDID_min_vfreq, pDH->EDID_max_vfreq));
					}
				}
			} else {
				RMDBGLOG((ENABLE, "Cannot load EDID preferred timing information from EEPROM (or info is incorrect)!\n"));
			}
		} while (RMFAILED(err) && (++i < 2));
		if (RMFAILED(err)) {
			// failed to match with either DTD, fall back to VIC 1
			*UseStandard = TRUE;
			*standard = EMhwlibTVStandard_Custom;
			*pVIC = 1;
		}
	}
	
	// build up EDID_display_mask from SVDs
	{
		RMuint32 dbc, i;
		
		for (dbc = 0; dbc < nDBC; dbc++) {
			for (i = 0; i < pDBC[dbc].NbShortVideoDescriptors; i++) {
				EDID_display_mask |= (1LL << (pDBC[dbc].ShortVideoDescriptors[i] & 0x7F));
			}
		}
	}
	// remove excluded VICs from display mask
	EDID_display_mask &= ~(pDH->EDID_exclude_mask);
	// if necessary, fall back to VIC 1
	if (! EDID_display_mask) {
		EDID_display_mask = 0x0000000000000002LL;
	}
	// Limit VICs to those in force mask, but only if any would be left
	if (EDID_display_mask & pDH->EDID_force_mask) {
		EDID_display_mask &= pDH->EDID_force_mask;
	}
	
	switch (pDH->EDID_select) {
		case DH_EDID_preferred:
			break;  // already set
		case DH_EDID_auto:
			if (! *UseStandard) break;  // TVFormat and VIC already set to preferred mode
			if (*pVIC > 1) break;  // TVStandard and VIC already set to preferred mode
			/* no break for DH_EDID_auto, go on to check for native timing */
		case DH_EDID_native:
			// no break;
		case DH_EDID_mask:
			// Use EDID native timing mode
			if (CEA_extensions) {
				struct EMhwlibTVFormatDigital fmt_d;
				RMuint32 i = (pDH->EDID_select == DH_EDID_mask) ? 1 : 0;
				do {
					if (i == 0) {
						err = get_native_vic(pDBC, nDBC, pVIC);
					} else if (pDH->EDID_select != DH_EDID_native) {
						err = get_vic_from_descriptor(pDBC, nDBC, i, pVIC);
					} else {
						err = RM_ERROR;
					}
					if (RMFAILED(err)) break;
					i++;
					if (! (EDID_display_mask & (1LL << *pVIC))) {
						continue;
					}
					DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(pDH->pRUA, *pVIC, FALSE, standard, ar_x, ar_y);
					RUAExchangeProperty(pDH->pRUA, DisplayBlock, 
						RMDisplayBlockPropertyID_TVFormatDigital, 
						standard, sizeof(enum EMhwlibTVStandard), 
						&fmt_d, sizeof(fmt_d));
					if (! fmt_d.Progressive) {
						fmt_d.ActiveHeight *= 2;
						fmt_d.VTotalSize /= 2;
					}
					
					// exclude modes not matching range limits
					if (pDH->EDID_max_pixclk && (fmt_d.PixelClock > pDH->EDID_max_pixclk)) continue;
					if (pDH->EDID_min_pixclk && (fmt_d.PixelClock < pDH->EDID_min_pixclk)) continue;
					if (pDH->EDID_max_hfreq || pDH->EDID_min_hfreq) {
						RMuint32 hfreq = fmt_d.PixelClock / fmt_d.HTotalSize;
						if (pDH->EDID_max_hfreq && (hfreq > pDH->EDID_max_hfreq)) continue;
						if (pDH->EDID_min_hfreq && (hfreq < pDH->EDID_min_hfreq)) continue;
					}
					if (pDH->EDID_max_vfreq || pDH->EDID_min_vfreq) {
						RMuint32 vfreq = fmt_d.PixelClock / fmt_d.HTotalSize / fmt_d.VTotalSize;
						if (pDH->EDID_max_vfreq && (vfreq > pDH->EDID_max_vfreq)) continue;
						if (pDH->EDID_min_vfreq && (vfreq < pDH->EDID_min_vfreq)) continue;
					}
					break;
				} while (1);
				if (RMFAILED(err)) {
					RMDBGLOG((ENABLE, "Cannot get 1st VIC from EDID DBC\n"));
					// fallback to VIC 1
					*pVIC = 1;
					*standard = EMhwlibTVStandard_Custom;
				}
				*UseStandard = TRUE;
			}
			break;
		case DH_EDID_force:  // use VIC from display list, don't use exclusions
			if (CEA_extensions) {
				RMuint32 prefVIC = *pVIC;
				if (RMFAILED(get_vic_from_descriptor(pDBC, nDBC, pDH->EDID_selection, pVIC))) {
					RMDBGLOG((ENABLE, "Cannot get VIC from EDID DBC [%ld]\n", pDH->EDID_selection));
					*pVIC = prefVIC;  // fallback to preferred mode, if any
					*standard = EMhwlibTVStandard_Custom;
				} else {
					*UseStandard = TRUE;
				}
			}
			break;
		case DH_EDID_match:
			if (CEA_extensions) {
				RMuint32 prefVIC = *pVIC;
				RMuint32 asp_x, asp_y;
				asp_x = (ar_x) ? target_ar_x : (pDH->EDID_hsize > 720) ? 16 : 4;
				asp_y = (ar_y) ? target_ar_y : (pDH->EDID_hsize > 720) ? 9 : 3;
				if (RMFAILED(get_matching_vic(pDH->pRUA, 
					pDBC, nDBC, 
					pDH->EDID_vfreq, pDH->EDID_hsize, pDH->EDID_vsize, pDH->EDID_intl, 
					asp_x, asp_y, 
					EDID_display_mask, 
					pDH->EDID_max_pixclk, pDH->EDID_min_pixclk, 
					pDH->EDID_max_hfreq, pDH->EDID_min_hfreq, 
					pDH->EDID_max_vfreq, pDH->EDID_min_vfreq, 
					pVIC, standard, ar_x, ar_y))
				) {
					RMDBGLOG((ENABLE, "Cannot get matching VIC from EDID DBC\n"));
					*pVIC = prefVIC;  // fallback to preferred mode, if any
					*standard = EMhwlibTVStandard_Custom;
				} else {
					*UseStandard = TRUE;
				}
			}
			break;
		default:
			// Unknown, fall back to default mode
			*UseStandard = TRUE;
			*pVIC = 1;
			*standard = EMhwlibTVStandard_Custom;
			break;
	}
	
	if (*UseStandard) {
		if (! *pVIC && (*standard == EMhwlibTVStandard_Custom)) {
			*pVIC = 1;  // fallback to VIC 1, 640x480@60Hz
		}
		
		if (*standard == EMhwlibTVStandard_Custom) {// get TVStandard from VIC number
			if (RMFAILED(DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(pDH->pRUA, *pVIC, sixtyhertz, standard, ar_x, ar_y))) {
				RMDBGLOG((ENABLE, "Cannot get video standard from VIC %ld\n", *pVIC));
				*pVIC = 1;  // fallback to VIC 1
				DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(pDH->pRUA, *pVIC, sixtyhertz, standard, ar_x, ar_y);
			}
			*ar_x &= 0xFF;
			*ar_y &= 0xFF;
		}
 	}
 	
 	// Set colorspace, prefer YCrCb for HDMI monitor
	if (yuv_444 && (! target_rgb) && pDH->part_caps.HDMI && (*pVIC > 1)) {  // HDMI display with YCbCr support, and support for AVI info frames
		RMuint32 i;
		*color_space = EMhwlibColorSpace_YUV_601;  // fallback
		for (i = 0; i < sizeof(VideoFormats) / sizeof(VideoFormats[0]); i ++) {
			if (VideoFormats[i].Info.VIC == *pVIC) {
				*color_space = VideoFormats[i].Info.color_space;
				break;
			}
		}
	} else if (pDH->part_caps.HDMI && CEA_extensions && (*pVIC != 1)) {  // HDMI display
		*color_space = EMhwlibColorSpace_RGB_16_235;
	} else {  // DVI display/chipset, or VIC 1
		*color_space = EMhwlibColorSpace_RGB_0_255;
	}
	if (format_digital) format_digital->ColorSpace = *color_space;
	if (format_analog) format_analog->ColorSpace = *color_space;
	
#ifdef _DEBUG
	RMDBGLOG((LOCALDBG, "Colorspace from EDID: %s\n", 
		(*color_space == EMhwlibColorSpace_RGB_0_255) ? "RGB 0-255" : 
		(*color_space == EMhwlibColorSpace_RGB_16_235) ? "RGB 16-235" : 
		(*color_space == EMhwlibColorSpace_YUV_601) ? "YCbCr 601" : 
		(*color_space == EMhwlibColorSpace_YUV_709) ? "YCbCr 709" : 
		"Unknown!"));
	if (*UseStandard) {
		if (*pVIC) {
			RMDBGLOG((LOCALDBG, "Returning EDID timing VIC %ld: %s\n", *pVIC, CEA861_ShortDescriptorVideoResolutions[*pVIC]));
		}
		RMDBGLOG((LOCALDBG, "Returning EDID timing standard: %s\n", TVFormatString[*standard]));
	} else {
		RMuint32 FieldRate100;
		
		FieldRate100 = (RMuint32)((RMuint64)format_digital->PixelClock * 100LL / format_digital->HTotalSize);
		if (format_digital->Progressive) {
			FieldRate100 /= format_digital->VTotalSize;
		} else {
			FieldRate100 = FieldRate100 * 2 / (format_digital->VTotalSize * 2 + 1);
		}
		RMDBGLOG((LOCALDBG, "Returning EDID detailed timing: %ldx%ld%c %ld.%02ld Hz, PixClk: %ld Hz, AspectRatio: %ld:%ld\n", 
			format_digital->ActiveWidth, format_digital->ActiveHeight * (format_digital->Progressive ? 1 : 2), 
			format_digital->Progressive ? 'p' : 'i', FieldRate100 / 100, FieldRate100 % 100, format_digital->PixelClock, 
			*ar_x, *ar_y));
	}
#endif
	
	if (own_DBC) RMFree(pDBC);
	
	return RM_OK;
}

RMstatus DHGetHDMIModeFromEDID(struct DH_control *pDH, RMbool *HDMI)
{
	struct EDID_Data edid;
	RMuint8 edid_block[128];
	struct CEA861BDataBlockCollection DBC;
	RMuint32 rpt, n;
	RMstatus err;
	
	CHECK_pDH("DHGetHDMIModeFromEDID");
	CHECK_PTR("DHGetHDMIModeFromEDID", HDMI);
	
	// get EDID info
	*HDMI = FALSE;
	rpt = MAX_EDID_ATTEMPTS;
	do {
		err = DHLoadEDIDVersion1(pDH, &edid);
	} while (RMFAILED(err) && --rpt);
	if (RMSUCCEEDED(err)) for (n = 1; n <= edid.EDID_Extension; n++) {
		rpt = MAX_EDID_ATTEMPTS;
		do {
			err = DHLoadEDIDBlock(pDH, n, edid_block, sizeof(edid_block));
		} while (RMFAILED(err) && --rpt);
		if (RMSUCCEEDED(err)) {
			err = DHGetCEADataBlockCollection(edid_block, sizeof(edid_block), &DBC);
			if (RMSUCCEEDED(err) && DBC.HDMI_sink) *HDMI = TRUE;
		}
	}
	
	RMDBGLOG((ENABLE, "Call to DHGetHDMIModeFromEDID() results in %s\n", *HDMI ? "TRUE" : "FALSE"));
	
	return RM_OK;
}

RMstatus DHMuteOutput(struct DH_control *pDH, RMbool mute)
{
	RMstatus err = RM_OK;
	
	CHECK_pDH("DHMuteOutput");
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHMuteOutput() with unitialize device!"));
		return RM_ERROR;
	}
	
	if (pDH->Mute && mute) {  // already muted.
		return RM_OK;
	}
	
	switch(pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_OK;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		if (pDH->HDMI_mode) {
			RMDBGLOG((LOCALDBG, "  --- HDMI %sMUTE ---\n", mute ? "" : "UN"));
			DHClearInfoFrameEnable(pDH, INFO_FRAME_CP_ENABLE);
			if (mute) {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xDF, 0x01);  // set AVMute flag
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to set AVMute!\n"));
				DHSetInfoFrameEnable(pDH, INFO_FRAME_CP_ONCE);  // send one CP
				pDH->info_frame_enable &= ~INFO_FRAME_CP_ONCE;  // don't repeat
				RMDBGLOG((LOCALDBG, "info_frame_enable now 0x%04X\n", pDH->info_frame_enable));
				// wait until CP has been sent
				DHWaitInfoFrameEnable(pDH, INFO_FRAME_CP_ONCE);
			} else {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xDF, 0x10);  // clear AVMute flag
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear AVMute!\n"));
				DHSetInfoFrameEnable(pDH, INFO_FRAME_CP_ENABLE);  // send CPs repeatedly
			}
		} else {
			RMDBGLOG((LOCALDBG, "  --- DVI %sMUTE ---\n", mute ? "" : "UN"));
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, 0x00);
			if (mute) {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xDF, 0x01);  // set AVMute flag
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to set AVMute!\n"));
			} else {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xDF, 0x10);  // clear AVMute flag
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear AVMute!\n"));
			}
		}
		if (RMFAILED(err)) {
			RMDBGLOG((LOCALDBG, "Failed to mute / unmute DVI output, %s\n", RMstatusToString(err)));
		} else {
			pDH->Mute = mute;
		}
		break;
	case DH_ANX9030:
		if (pDH->HDMI_mode) {
			RMDBGLOG((LOCALDBG, "  --- HDMI %sMUTE ---\n", mute ? "" : "UN"));
			DHClearInfoFrameEnable(pDH, INFO_FRAME_CP_ENABLE);
			if (mute) {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xCC, 0x01);  // set AVMute flag
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to set AVMute!\n"));
				DHSetInfoFrameEnable(pDH, INFO_FRAME_CP_ONCE);  // send one CP
				pDH->info_frame_enable &= ~INFO_FRAME_CP_ONCE;  // don't repeat
				RMDBGLOG((LOCALDBG, "info_frame_enable now 0x%04X\n", pDH->info_frame_enable));
				// wait until CP has been sent
				DHWaitInfoFrameEnable(pDH, INFO_FRAME_CP_ONCE);
			} else {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xCC, 0x02);  // clear AVMute flag
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear AVMute!\n"));
				DHSetInfoFrameEnable(pDH, INFO_FRAME_CP_ENABLE);  // send CPs repeatedly
			}
		} else {
			RMDBGLOG((LOCALDBG, "  --- DVI %sMUTE ---\n", mute ? "" : "UN"));
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xC0, 0x00);
			if (mute) {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xCC, 0x01);  // set AVMute flag
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to set AVMute!\n"));
			} else {
				err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0xCC, 0x02);  // clear AVMute flag
				if (RMFAILED(err)) RMDBGLOG((ENABLE, "Failed to clear AVMute!\n"));
			}
		}
		if (RMFAILED(err)) {
			RMDBGLOG((LOCALDBG, "Failed to mute / unmute DVI output, %s\n", RMstatusToString(err)));
		} else {
			pDH->Mute = mute;
		}
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        if( mute )
        {
            DH_Set_CAT6611_AVMute(pDH) ;
        }
        else
        {
            DH_Clear_CAT6611_AVMute(pDH) ;
        }
        
        err = RM_OK ;
		pDH->Mute = mute;

        break ;
    //~jj_tseng@chipadvanced.com 2007/03/23
		
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

// mute audio only, without sending A/V-Mute packet
RMstatus DHMuteAudio(struct DH_control *pDH, RMbool mute)
{
	RMstatus err = RM_OK;
	RMuint8 reg;
	
	CHECK_pDH("DHMuteAudio");
	
	switch(pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
	case DH_ANX9030:
// 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
//~jj_tseng@chipadvanced.com 2007/03/23
		err = RM_NOT_SUPPORTED;  // feature not available
		break;
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0D, &reg);
		if (RMSUCCEEDED(err)) {
			RMinsShiftBool(&reg, mute, 1);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0D, reg);
		}
		break;
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

// send blank-level video, without sending A/V-Mute packet
RMstatus DHBlankVideo(struct DH_control *pDH, RMbool mute)
{
	RMstatus err = RM_OK;
	RMuint8 reg;
	
	CHECK_pDH("DHBlankVideo");
	
	switch(pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
	case DH_ANX9030:
// 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
//~jj_tseng@chipadvanced.com 2007/03/23
		err = RM_NOT_SUPPORTED;  // feature not available
		break;
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0D, &reg);
		if (RMSUCCEEDED(err)) {
			RMinsShiftBool(&reg, mute, 2);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x0D, reg);
		}
		break;
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

// Sets the video pixel value for blanking
RMstatus DHBlankVideoColor(struct DH_control *pDH, struct EMhwlibDeepColor Color)
{
	RMstatus err = RM_OK;
	RMuint8 reg[3];
	RMint32 shift;
	
	CHECK_pDH("DHBlankVideoColor");
	
	shift = Color.ComponentBitDepth - 8;
	reg[0] = (shift < 0) ? (Color.B_Cb << -shift) : (Color.B_Cb >> shift);
	reg[1] = (shift < 0) ? (Color.G_Y  << -shift) : (Color.G_Y  >> shift);
	reg[2] = (shift < 0) ? (Color.R_Cr << -shift) : (Color.R_Cr >> shift);
	
	switch(pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
// 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
//~jj_tseng@chipadvanced.com 2007/03/23
		err = RM_NOT_SUPPORTED;  // feature not available
		break;
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x4B, reg, 3);
		break;
	case DH_ANX9030:
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0xCC, reg, 3);
		break;
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMuint8 bCSCOffset_16_235[] =
{
    0x00, 0x80, 0x00
};

static RMuint8 bCSCOffset_0_255[] =
{
    0x10, 0x80, 0x10
};

static RMuint8 bCSCMtx_RGB2YUV_ITU601_16_235[] =
{
    0xB2,0x04,0x64,0x02,0xE9,0x00,
    0x93,0x3C,0x16,0x04,0x56,0x3F,
    0x49,0x3D,0x9F,0x3E,0x16,0x04
} ;

static RMuint8 bCSCMtx_RGB2YUV_ITU601_0_255[] =
{
    0x09,0x04,0x0E,0x02,0xC8,0x00,
    0x0E,0x3D,0x83,0x03,0x6E,0x3F,
    0xAC,0x3D,0xD0,0x3E,0x83,0x03
} ;

static RMuint8 bCSCMtx_RGB2YUV_ITU709_16_235[] =
{
    0xB8,0x05,0xB4,0x01,0x93,0x00,
    0x49,0x3C,0x16,0x04,0x9F,0x3F,
    0xD9,0x3C,0x10,0x3F,0x16,0x04
} ;

static RMuint8 bCSCMtx_RGB2YUV_ITU709_0_255[] =
{
    0xE5,0x04,0x78,0x01,0x81,0x00,
    0xCE,0x3C,0x83,0x03,0xAE,0x3F,
    0x49,0x3D,0x33,0x3F,0x83,0x03
} ;

static RMuint8 bCSCMtx_YUV2RGB_ITU601_16_235[] =
{
    0x00,0x08,0x6A,0x3A,0x4F,0x3D,
    0x00,0x08,0xF7,0x0A,0x00,0x00,
    0x00,0x08,0x00,0x00,0xDB,0x0D
} ;

static RMuint8 bCSCMtx_YUV2RGB_ITU601_0_255[] =
{
    0x4F,0x09,0x81,0x39,0xDF,0x3C,
    0x4F,0x09,0xC2,0x0C,0x00,0x00,
    0x4F,0x09,0x00,0x00,0x1E,0x10
} ;

static RMuint8 bCSCMtx_YUV2RGB_ITU709_16_235[] =
{
    0x00,0x08,0x53,0x3C,0x89,0x3E,
    0x00,0x08,0x51,0x0C,0x00,0x00,
    0x00,0x08,0x00,0x00,0x87,0x0E
} ;

static RMuint8 bCSCMtx_YUV2RGB_ITU709_0_255[] =
{
    0x4F,0x09,0xBA,0x3B,0x4B,0x3E,
    0x4F,0x09,0x56,0x0E,0x00,0x00,
    0x4F,0x09,0x00,0x00,0xE7,0x10
} ;
//~jj_tseng@chipadvanced.com 2007/03/23


static RMstatus DHSetVideoInputOutputConversion(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	RMbool dither = TRUE;  // TRUE: prefer dithering over truncating in color depth conversion
	RMbool InRGB = FALSE, OutRGB = FALSE;
	RMbool InRange = FALSE, OutRange = FALSE;
	RMbool In422 = FALSE, Out422 = FALSE;
	RMbool In709 = FALSE, Out709 = FALSE;
	RMuint32 InDC = 8, OutDC = 8;
	RMuint8 vid_ctrl, vid_acen, vid_mode, hdmi_ctrl;
    RMuint8 reg ;
    RMuint8 *pCSCOffset, *pCSCMatrix ;
    
	
	// convert input config to booleans
	switch(pDH->InputColorSpace) {
	case EMhwlibColorSpace_RGB_0_255:
		InRange = TRUE;
	case EMhwlibColorSpace_RGB_16_235:
		InRGB = TRUE;
		break;
	case EMhwlibColorSpace_YUV_709:
	case EMhwlibColorSpace_YUV_709_0_255:
		In709 = TRUE;
	case EMhwlibColorSpace_YUV_601:
	case EMhwlibColorSpace_YUV_601_0_255:
		switch (pDH->InputSamplingMode) {
			case EMhwlibSamplingMode_422:
				In422 = TRUE;
			case EMhwlibSamplingMode_444:
				break;
			default: return RM_ERROR;
		}
		break;
	default: return RM_ERROR;
	}
	InDC = pDH->InputComponentBitDepth;
	RMDBGLOG((LOCALDBG, "Video Input Format is %lu bit %s %s\n", InDC, 
		InRGB ? "RGB" : In422 ? "YCbCr 4:2:2" : "YCbCr 4:4:4", 
		InRGB ? (InRange ? "0..255" : "16..235") : (In709 ? "709" : "601")));
	
	// convert output config to booleans
	switch(pDH->OutputColorSpace) {
	case EMhwlibColorSpace_RGB_0_255:
		OutRange = TRUE;
	case EMhwlibColorSpace_RGB_16_235:
		OutRGB = TRUE;
		break;
	case EMhwlibColorSpace_YUV_709:
	case EMhwlibColorSpace_YUV_709_0_255:
		Out709 = TRUE;
	case EMhwlibColorSpace_YUV_601:
	case EMhwlibColorSpace_YUV_601_0_255:
		switch (pDH->OutputSamplingMode) {
			case EMhwlibSamplingMode_422:
				Out422 = TRUE;
			case EMhwlibSamplingMode_444:
				break;
			default: return RM_ERROR;
		}
		break;
	default: return RM_ERROR;
	}
	OutDC = pDH->OutputComponentBitDepth;
	RMDBGLOG((LOCALDBG, "Video Output Format is %lu bit %s %s\n", OutDC, 
		OutRGB ? "RGB" : Out422 ? "YCbCr 4:2:2" : "YCbCr 4:4:4", 
		OutRGB ? (OutRange ? "0..255" : "16..235") : (Out709 ? "709" : "601")));
	
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		// set up video control register
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x48, &vid_ctrl);
		if (RMFAILED(err)) {
			RMuint32 i;
			vid_ctrl = 0x00;
			for (i = 0; i < sizeof(init_SiI9030) / sizeof(init_SiI9030[0]); i++) {
				if (init_SiI9030[i][0] == 0x48) vid_ctrl = init_SiI9030[i][1];
			}
		}
		RMinsShiftBool(&vid_ctrl, In422 && (InDC > 8), 5);  // EXTN, 4:2:2 extended bit mode
		RMinsShiftBool(&vid_ctrl, In709 || Out709, 4);  // CSCSEL, color space conversion standard select
		if ((pDH->part != DH_siI9134) && (InDC > 8) && ! In422) {
			if (! manutest) fprintf(stderr, "[HDMI] ERROR: Input Color Depth %lu bits not supported in %s mode by chip!\n", InDC, In422 ? "4:2:2" : InRGB ? "RGB" : "4:4:4");
			return RM_ERROR;
		}
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x48, vid_ctrl);
		
		// Set up video action enable register
		if ((pDH->part == DH_siI9034) || (pDH->part == DH_siI9134)) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x49, &vid_acen);
			if (RMFAILED(err)) return RM_ERROR;
			switch (InDC) {
			case 8:  RMinsShiftBits(&vid_acen, 0, 2, 6); break;
			case 10: RMinsShiftBits(&vid_acen, 1, 2, 6); break;
			case 12: RMinsShiftBits(&vid_acen, 2, 2, 6); break;
			default:
				if (! manutest) fprintf(stderr, "[HDMI] ERROR: Input Color Depth %lu bits not supported by chip!\n", InDC);
				return RM_ERROR;
			}
			RMinsShiftBool(&vid_acen, ! OutRGB, 4);  // CLIP_CS_ID, output color space on the link for clipping
			RMinsShiftBool(&vid_acen, ! OutRange, 3);  // RANGE_CLIP, clip output values to 16..235 resp. 16..240
			RMinsShiftBool(&vid_acen, InRGB && ! OutRGB, 2);  // RGB_2_YCBCR, enable RGB to YCbCr color space converter
			RMinsShiftBool(&vid_acen, InRange && ! OutRange, 1);  // RANGE_CMPS, compress 0..255 input values to 16..235 on the output
			RMinsShiftBool(&vid_acen, ! In422 && Out422, 0);  // DOWN_SMPL, down sample 4:4:4 to 4:2:2
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x49, vid_acen);
		} else {
			if (InRGB && ! OutRGB) {
				if (! manutest) fprintf(stderr, "[HDMI] ERROR: RGB to YCbCr color space conversion not supported by chip!\n");
				return RM_ERROR;
			}
			if (! In422 && Out422) {
				if (! manutest) fprintf(stderr, "[HDMI] ERROR: 4:4:4 to 4:2:2 down sampling not supported by chip!\n");
				return RM_ERROR;
			}
		}
		
		// set up 4:4:4/RGB deep color
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x4A, &vid_mode);
		if (RMFAILED(err)) return RM_ERROR;
		if (pDH->part == DH_siI9134) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x2F, &hdmi_ctrl);
			if (Out422) {
				RMinsShiftBits(&vid_mode, 0, 2, 6);
				RMinsShiftBits(&hdmi_ctrl, 4, 3, 3);
				RMinsShiftBool(&hdmi_ctrl, FALSE, 6);  // DC_EN, deep-color packet enable
			} else {
				switch (OutDC) {
				case 8:  RMinsShiftBits(&vid_mode, 0, 2, 6); break;
				case 10: RMinsShiftBits(&vid_mode, 1, 2, 6); break;
				case 12: RMinsShiftBits(&vid_mode, 2, 2, 6); break;
				default:
					if (! manutest) fprintf(stderr, "[HDMI] Output Color Depth %lu bits not supported by chip!\n", OutDC);
					return RM_ERROR;
				}
				switch (OutDC) {
				case 8:  RMinsShiftBits(&hdmi_ctrl, 4, 3, 3); break;
				case 10: RMinsShiftBits(&hdmi_ctrl, 5, 3, 3); break;
				case 12: RMinsShiftBits(&hdmi_ctrl, 6, 3, 3); break;
				}
				RMinsShiftBool(&hdmi_ctrl, OutDC > 8, 6);  // DC_EN, deep-color packet enable  TODO: is there a case where we need DC_EN with 8 bit?
			}
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x2F, hdmi_ctrl);
		} else {
			if ((OutDC > 8) && ! Out422) {
				if (! manutest) fprintf(stderr, "[HDMI] ERROR: Output Color Depth %lu bits only supported in 4:2:2 mode by chip!\n", OutDC);
				return RM_ERROR;
			}
			RMinsShiftBits(&vid_mode, 0, 2, 6);  // DITHER_MODE, number of bits per output video channel
		}
		
		// set up rest of video mode register
		RMinsShiftBool(&vid_mode, dither, 5);  // DITHER, 1=video output is dithered, 0=video output is truncated to size
		RMinsShiftBool(&vid_mode, ! InRange && OutRange, 4);  // RANGE, expand 16..235 input values to 0..255
		RMinsShiftBool(&vid_mode, ! InRGB && OutRGB, 3);  // CSC, enable YCbCr to RGB color space converter
		RMinsShiftBool(&vid_mode, In422 && ! Out422, 2);  // UPSMP, enable 4:2:2 to 4:4:4 up sampling
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x4A, vid_mode);
		RMDBGLOG((LOCALDBG, "VID_CTRL=0x%02X VID_ACEN=0x%02X VID_MODE=0x%02X HDMI_CTRL=0x%02X\n", vid_ctrl, vid_acen, vid_mode, hdmi_ctrl));
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
    {
        if( InDC != 8 || OutDC != 8 )
        {
            return RM_NOT_SUPPORTED ;
        }
        
        // output RGB444 mode
        if(InRGB)
        {
            err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x70, &reg) ;
            reg &= ~0xc0 ; // reg70[7:6] = '00' ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x70, reg) ;
            
            if(OutRGB )
            {
                err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x72, &reg) ;
                reg &= 0xFC ; // reg72[1:0] = '00' for color space bypass convertion.
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x72, reg) ;
            }
            else
            {
                err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x72, &reg) ;
                reg &= 0xFC ; 
                reg |= 0x02 ; // reg72[1:0] = '10' for RGB2YUV convertion.
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x72, reg) ;
                
                if(InRange)
                {
                    // 0_255
                    pCSCOffset = bCSCOffset_0_255 ;

                    if(Out709)
                    {
                        pCSCMatrix = bCSCMtx_RGB2YUV_ITU709_0_255 ;
                    }
                    else
                    {
                        pCSCMatrix = bCSCMtx_RGB2YUV_ITU601_0_255 ;
                    }
                }
                else 
                {
                    pCSCOffset = bCSCOffset_16_235 ;
                    // 16_235
                    if(Out709)
                    {
                        pCSCMatrix = bCSCMtx_RGB2YUV_ITU709_16_235 ;
                    }
                    else
                    {
                        pCSCMatrix = bCSCMtx_RGB2YUV_ITU601_16_235 ;
                    }
                }
                
                err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x73, pCSCOffset, sizeof(bCSCOffset_0_255)) ;
                err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x76, pCSCMatrix, sizeof(bCSCMtx_RGB2YUV_ITU601_16_235)) ;
            }
        }
        else
        {
            err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x70, &reg) ;
            reg &= ~0xC0 ; // reg70[7:6] = '00' ;
            if( In422 )
            {
                reg |= 0x40 ;
            }
            else
            {
                reg |= 0x80 ;
            }
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x70, reg) ;

            if(OutRGB)
            {
                err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x72, &reg) ;
                reg &= 0xFC ; 
                reg |= 0x03 ; // reg72[1:0] = '10' for YUV2RGB convertion.
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x72, reg) ;
                
                if(OutRange)
                {
                    // 0_255
                    pCSCOffset = bCSCOffset_0_255 ;

                    if(Out709)
                    {
                        pCSCMatrix = bCSCMtx_YUV2RGB_ITU709_0_255 ;
                    }
                    else
                    {
                        pCSCMatrix = bCSCMtx_YUV2RGB_ITU601_0_255 ;
                    }
                }
                else 
                {
                    pCSCOffset = bCSCOffset_16_235 ;
                    // 16_235
                    if(Out709)
                    {
                        pCSCMatrix = bCSCMtx_YUV2RGB_ITU709_16_235 ;
                    }
                    else
                    {
                        pCSCMatrix = bCSCMtx_YUV2RGB_ITU601_16_235 ;
                    }
                }
                
                err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x73, pCSCOffset, sizeof(bCSCOffset_0_255)) ;
                err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x76, pCSCMatrix, sizeof(bCSCMtx_YUV2RGB_ITU601_16_235)) ;
            }
            else
            {
                err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x72, &reg) ;
                reg &= 0xFC ; // reg72[1:0] = '00' for color space bypass convertion.
                err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x72, reg) ;
            }
        }
        // output color mode is set in AVI Infoframe , do not write here.
    }

        break ;
    //~jj_tseng@chipadvanced.com 2007/03/23
	case DH_ANX9030:
		return RM_NOTIMPLEMENTED;
		break;
	default:
		break;
	}
	
	return err;
}

// Set up the format of the video from the 86xx to the HDMI chip
// (This has to match the format set on the DispDigitalOut emhwlib module)
RMstatus DHSetInputVideoFormat(struct DH_control *pDH, enum EMhwlibColorSpace ColorSpace, enum EMhwlibSamplingMode SamplingMode, RMuint32 ComponentBitDepth)
{
	pDH->InputColorSpace = ColorSpace;
	pDH->InputSamplingMode = SamplingMode;
	pDH->InputComponentBitDepth = ComponentBitDepth;
	return DHSetVideoInputOutputConversion(pDH);
}

// Set up the format of the video from the HDMI chip to the TMDS link
// (This has to be matched in the AVI info frame)
RMstatus DHSetOutputVideoFormat(struct DH_control *pDH, enum EMhwlibColorSpace ColorSpace, enum EMhwlibSamplingMode SamplingMode, RMuint32 ComponentBitDepth)
{
	pDH->OutputColorSpace = ColorSpace;
	pDH->OutputSamplingMode = SamplingMode;
	pDH->OutputComponentBitDepth = ComponentBitDepth;
	return DHSetVideoInputOutputConversion(pDH);
}

// Same as DHSetInputVideoFormat() and DHSetOutputVideoFormat(), whithout extra config cycle inbetween
RMstatus DHSetConversionVideoFormat(struct DH_control *pDH, 
	enum EMhwlibColorSpace InputColorSpace, enum EMhwlibSamplingMode InputSamplingMode, RMuint32 InputComponentBitDepth, 
	enum EMhwlibColorSpace OutputColorSpace, enum EMhwlibSamplingMode OutputSamplingMode, RMuint32 OutputComponentBitDepth)
{
	pDH->InputColorSpace = InputColorSpace;
	pDH->InputSamplingMode = InputSamplingMode;
	pDH->InputComponentBitDepth = InputComponentBitDepth;
	pDH->OutputColorSpace = OutputColorSpace;
	pDH->OutputSamplingMode = OutputSamplingMode;
	pDH->OutputComponentBitDepth = OutputComponentBitDepth;
	return DHSetVideoInputOutputConversion(pDH);
}

// AVI S[1:0]
static RMstatus DHInsertScanInfo(
	enum DH_scan_info ScanInfo, 
	struct CEA861InfoFrame *pInfoFrame)
{
	RMinsShiftBits(&(pInfoFrame->DataByte[1]), ScanInfo, 2, 0);
	return RM_OK;
}

// AVI A[0] and R[3:0]
static RMstatus DHInsertActiveFormat(
	RMbool ActiveFormatValid, 
	enum DH_active_format_aspect_ratio ActiveFormat, 
	struct CEA861InfoFrame *pInfoFrame)
{
	RMinsShiftBool(&(pInfoFrame->DataByte[1]), ActiveFormatValid, 4);
	if (ActiveFormatValid) {
		RMDBGLOG((LOCALDBG, "  AVI Frame - Active Format: %s (%lu)\n", 
			(ActiveFormat == DH_af_16x9_top) ? "16:9 content: at top of 4:3 frame, fills up 16:9 frame" : 
			(ActiveFormat == DH_af_14x9_top) ? "14:9 content: at top of 4:3 frame, centered on 16:9 frame" : 
			(ActiveFormat == DH_af_64x27_centered) ? "Cinemascope widesceeen (2.35:1, 64:27) content: centered on 4:3 or 16:9 frame" : 
			(ActiveFormat == DH_af_same_as_picture) ? "content fills up frame" : 
			(ActiveFormat == DH_af_4x3_centered) ? "4:3 content: fills up 4:3 frame, centered on 16:9 frame" : 
			(ActiveFormat == DH_af_16x9_centered) ? "16:9 content: centered on 4:3 frame, fills up 16:9 frame" : 
			(ActiveFormat == DH_af_14x9_centered) ? "14:9 content: centered on 4:3 frame, centered on 16:9 frame" : 
			(ActiveFormat == DH_af_4x3_centered_prot_14x9) ? "4:3 content with essential content in 14:9 centered portion" : 
			(ActiveFormat == DH_af_16x9_centered_prot_14x9) ? "16:9 content with essential content in 14:9 centered portion" : 
			(ActiveFormat == DH_af_16x9_centered_prot_4x3) ? "16:9 content with essential content in 4:3 centered portion" : 
			"unknown format code!", 
			ActiveFormat));
		RMinsShiftBits(&(pInfoFrame->DataByte[2]), ActiveFormat, 4, 0);
	} else {
		RMDBGLOG((LOCALDBG, "  AVI Frame - No Active Format defined, using frame as-is.\n"));
		RMinsShiftBits(&(pInfoFrame->DataByte[2]), DH_af_same_as_picture, 4, 0);
	}
	return RM_OK;
}

// AVI B[1:0] and Data Byte 6 through 13
static RMstatus DHInsertBarInfo(
	enum DH_AVI_info_bars info_bars, 
	RMuint16 end_top_bar_line_num, 
	RMuint16 start_bottom_bar_line_num, 
	RMuint16 end_left_bar_pixel_num, 
	RMuint16 start_right_bar_pixel_num, 
	struct CEA861InfoFrame *pInfoFrame)
{
	RMinsShiftBits(&(pInfoFrame->DataByte[1]), info_bars, 2, 2);
	if (info_bars & DH_horiz_bar_info_valid) {
		RMDBGLOG((LOCALDBG, "  AVI Frame - Horizontal bars in frame, from top to line %lu and from line %lu to bottom.\n", 
			end_top_bar_line_num, start_bottom_bar_line_num));
		pInfoFrame->DataByte[6] = (RMuint8)end_top_bar_line_num;
		pInfoFrame->DataByte[7] = (RMuint8)(end_top_bar_line_num >> 8);
		pInfoFrame->DataByte[8] = (RMuint8)start_bottom_bar_line_num;
		pInfoFrame->DataByte[9] = (RMuint8)(start_bottom_bar_line_num >> 8);
	}
	if (info_bars & DH_vert_bar_info_valid) {
		RMDBGLOG((LOCALDBG, "  AVI Frame - Vertical bars in frame, from left to column %lu and from column %lu to right.\n", 
			end_left_bar_pixel_num, start_right_bar_pixel_num));
		pInfoFrame->DataByte[10] = (RMuint8)end_left_bar_pixel_num;
		pInfoFrame->DataByte[11] = (RMuint8)(end_left_bar_pixel_num >> 8);
		pInfoFrame->DataByte[12] = (RMuint8)start_right_bar_pixel_num;
		pInfoFrame->DataByte[13] = (RMuint8)(start_right_bar_pixel_num >> 8);
	}
	return RM_OK;
}

// AVI Y[1:0], C[1:0] and EC[2:0] "x.v.Color"
// The VIC has to be already set in this InfoFrame
static RMstatus DHInsertColorSpace(
	enum EMhwlibColorSpace ColorSpace, 
	enum EMhwlibSamplingMode SamplingMode, 
	RMbool QuantisationSupport,  // Set only if sink support (DBC.VideoCapability & TV_SUPPORT_VIDEO_QUANTISATION)
	RMbool force, 
	struct CEA861InfoFrame *pInfoFrame)
{
	RMstatus err;
	RMuint32 VIC;
	enum EMhwlibColorSpace TargetColorSpace;
	
	// get current VIC and suggested color space
	VIC = RMunshiftBits(pInfoFrame->DataByte[4], 7, 0);
	err = DHGetVideoInfoFromCEAVideoIdentificationCode(VIC, FALSE, NULL, NULL, NULL, &TargetColorSpace, NULL);
	if (RMFAILED(err)) TargetColorSpace = EMhwlibColorSpace_RGB_0_255;
	if (ColorSpace == EMhwlibColorSpace_YUV_601_0_255) ColorSpace = EMhwlibColorSpace_YUV_601;
	if (ColorSpace == EMhwlibColorSpace_YUV_709_0_255) ColorSpace = EMhwlibColorSpace_YUV_709;
	
	// validate selected color space
	err = RM_OK;
	if (VIC <= 1) {
		if (ColorSpace != TargetColorSpace) {
			if (! manutest) fprintf(stderr, "[HDMI] Wrong color space for this video mode (VIC %lu), has to be RGB 0-255\n", VIC);
			err = RM_ERROR;
		}
	} else {
		if ((ColorSpace == EMhwlibColorSpace_YUV_601) || (ColorSpace == EMhwlibColorSpace_YUV_709)) {
			if (ColorSpace != TargetColorSpace) {
				if (! manutest) fprintf(stderr, "[HDMI] Wrong YCbCr color space for this video mode (VIC %lu), has to be YUV %u\n", VIC, (TargetColorSpace == EMhwlibColorSpace_YUV_601) ? 601 : 709);
				err = RM_ERROR;
			}
		} else if ((! QuantisationSupport) && (ColorSpace != EMhwlibColorSpace_RGB_16_235)) {
			if (! manutest) fprintf(stderr, "[HDMI] Wrong RGB color space for this video mode (VIC %lu), has to be RGB 16-235\n", VIC);
			err = RM_ERROR;
		}
	}
	
	// invalid color space for the current video resolution
	if (! force && RMFAILED(err)) return err;
	
	// default, no quantisation or extended colorimetry info
	RMinsShiftBits(&(pInfoFrame->DataByte[3]), 0x00, 2, 2);
	RMinsShiftBits(&(pInfoFrame->DataByte[3]), 0x00, 3, 4);
	
	// insert information into AVI
	switch(ColorSpace) {
	
	// RGB color spaces
	case EMhwlibColorSpace_RGB_16_235: // No differenciation, both RGB
		{
			if (QuantisationSupport) {
				RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace RGB 16..235\n"));
				RMinsShiftBits(&(pInfoFrame->DataByte[3]), 0x01, 2, 2);
			} else {
				RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace RGB (sending 16..235)\n"));
			}
		}
		if (0) // skip first line of next case
	case EMhwlibColorSpace_RGB_0_255:
		{
			if (QuantisationSupport) {
				RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace RGB 0..255\n"));
				RMinsShiftBits(&(pInfoFrame->DataByte[3]), 0x02, 2, 2);
			} else {
				RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace RGB (sending 0..255)\n"));
			}
		}
		RMinsShiftBits(&(pInfoFrame->DataByte[1]), 0x00, 2, 5);
		// Insert appropriate YCbCr color space for the current video format (used by some converter devices)
		switch (TargetColorSpace) {
			default:
				RMinsShiftBits(&(pInfoFrame->DataByte[2]), 0x00, 2, 6);
				break;
			case EMhwlibColorSpace_YUV_601:
				RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace RGB/YUV 601\n"));
				RMinsShiftBits(&(pInfoFrame->DataByte[2]), 0x01, 2, 6);
				break;
			case EMhwlibColorSpace_YUV_709:
				RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace RGB/YUV 709\n"));
				RMinsShiftBits(&(pInfoFrame->DataByte[2]), 0x02, 2, 6);
				break;
		}
		break;
		
	// YCbCr color spaces
	//case EMhwlibColorSpace_xvYCC_601:  // TODO x.v.Color not yet implemented
		{
			RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace vxYCC 601\n"));
			RMinsShiftBits(&(pInfoFrame->DataByte[3]), 0x00, 3, 4);
		}
		if (0)  // skip first block of next case
	//case EMhwlibColorSpace_xvYCC_709:  // TODO x.v.Color not yet implemented
		{
			RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace xvYCC 709\n"));
			RMinsShiftBits(&(pInfoFrame->DataByte[3]), 0x01, 3, 4);
		}
		RMinsShiftBits(&(pInfoFrame->DataByte[2]), 0x03, 2, 6);
		if (0)  // skip first block of next case
	case EMhwlibColorSpace_YUV_601:
	case EMhwlibColorSpace_YUV_601_0_255:
		{
			RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace YUV 601\n"));
			RMinsShiftBits(&(pInfoFrame->DataByte[2]), 0x01, 2, 6);
		}
		if (0)  // skip first block of next case
	case EMhwlibColorSpace_YUV_709:
	case EMhwlibColorSpace_YUV_709_0_255:
		{
			RMDBGLOG((LOCALDBG, "  AVI Frame - Colorspace YUV 709\n"));
			RMinsShiftBits(&(pInfoFrame->DataByte[2]), 0x02, 2, 6);
		}
		switch (SamplingMode) {
		case EMhwlibSamplingMode_444:
			RMDBGLOG((LOCALDBG, "  AVI Frame - Sampling Mode 4:4:4\n"));
			RMinsShiftBits(&(pInfoFrame->DataByte[1]), 0x02, 2, 5);
			break;
		case EMhwlibSamplingMode_422:
			RMDBGLOG((LOCALDBG, "  AVI Frame - Sampling Mode 4:2:2\n"));
			RMinsShiftBits(&(pInfoFrame->DataByte[1]), 0x01, 2, 5);
			break;
		default:
			RMDBGLOG((ENABLE, "Unsupported sampling mode in DHEnableAVIInfoFrame\n"));
			if (force) {
				RMinsShiftBits(&(pInfoFrame->DataByte[1]), 0x00, 2, 5);
			}
			err = RM_ERROR;
		}
		break;
	default:
		RMDBGLOG((ENABLE, "Unsupported color space in DHEnableAVIInfoFrame\n"));
		if (force) {
			RMinsShiftBits(&(pInfoFrame->DataByte[1]), 0x00, 2, 5);
			RMinsShiftBits(&(pInfoFrame->DataByte[2]), 0x00, 2, 6);
		}
		err = RM_ERROR;
	};
	
	return err;
}

// AVI M{1:0]
// The VIC has to be already set in this InfoFrame
static RMstatus DHInsertAspectRatio(
	struct EMhwlibAspectRatio AspectRatio, 
	RMbool force, 
	struct CEA861InfoFrame *pInfoFrame)
{
	RMstatus err;
	RMuint32 VIC;
	enum DH_picture_aspect_ratio ar, vic_ar;
	RMuint32 AspX, AspY;
	RMbool MultipleAspectRatios;
	
	// get current VIC and suggested aspect ratio
	VIC = RMunshiftBits(pInfoFrame->DataByte[4], 7, 0);
	err = DHGetVideoInfoFromCEAVideoIdentificationCode(VIC, FALSE, NULL, &AspX, &AspY, NULL, &MultipleAspectRatios);
	if (RMSUCCEEDED(err)) {
		vic_ar = ((AspX == 4) && (AspY == 3)) ? DH_ar_4x3 : DH_ar_16x9;
	} else {
		vic_ar = DH_ar_4x3;
		MultipleAspectRatios = FALSE;
	}
	
	// Match aspect ratio with some wiggle room
	err = RM_OK;
	if (! AspectRatio.X || ! AspectRatio.Y) {
		if (! manutest) fprintf(stderr, "[HDMI] Invalid aspect ratio: %ld:%ld\n", AspectRatio.X, AspectRatio.Y);
		ar = vic_ar;
		err = RM_ERROR;
		if (! force) return err;
	} else if (AspectRatio.X * 10 < AspectRatio.Y * 16) {
		ar = DH_ar_4x3;
	} else {
		ar = DH_ar_16x9;
	}
	RMDBGLOG((LOCALDBG, "  AVI Frame - Aspect Ratio %s\n", (ar == DH_ar_4x3) ? "4:3" : "16:9"));
	
	// See if VIC supports new aspect ratio, amend if possible
	if (vic_ar != ar) {
		if (MultipleAspectRatios) {
			if (ar == DH_ar_4x3) {  // CEA 861 lists 4:3 VIC before 16:9 VIC
				VIC--;
			} else {
				VIC++;
			}
			RMDBGLOG((LOCALDBG, "  AVI Frame - VIC to match aspect ratio: %ld\n", VIC));
			// Insert new VIC
			RMinsShiftBits(&(pInfoFrame->DataByte[4]), VIC, 7, 0);
		} else {
			if (! manutest) fprintf(stderr, "[HDMI] Aspect ratio not supported by this video mode (VIC %lu)\n", VIC);
			err = RM_ERROR;
			if (! force) return err;
		}
	}
	
	// Insert new aspect ratio
	RMinsShiftBits(&(pInfoFrame->DataByte[2]), ar, 2, 4);
	
	return err;
}

// AVI SC[1:0]
static RMstatus DHInsertNonUniformScalingInfo(
	RMbool HorizontalStrech,  // picture has been horizontally scaled (e.g. non-linear scaling)
	RMbool VericalStrech,  // picture has been vertically scaled
	struct CEA861InfoFrame *pInfoFrame)
{
	RMinsShiftBool(&(pInfoFrame->DataByte[3]), HorizontalStrech, 0);
	RMinsShiftBool(&(pInfoFrame->DataByte[3]), VericalStrech, 1);
	return RM_OK;
}

// AVI ITC[0]
static RMstatus DHInsertITContentInfo(
	RMbool ITContent, 
	struct CEA861InfoFrame *pInfoFrame)
{
	RMinsShiftBool(&(pInfoFrame->DataByte[3]), ITContent, 7);
	return RM_OK;
}

RMstatus DHEnableAVIInfoFrame(struct DH_control *pDH, struct DH_AVIInfoFrame *pAVIInfoFrame)
{
	RMstatus err;
	RMbool force = TRUE;  // TODO this should be FALSE, but it breaks too many apps. need to educate developer about restrictions!
	struct CEA861InfoFrame InfoFrame;
	RMuint8 reg;
	
	CHECK_pDH("DHEnableAVIInfoFrame");
	CHECK_PTR("DHEnableAVIInfoFrame", pAVIInfoFrame);
	
	
	if (pAVIInfoFrame->Version != 0x02) {
		RMDBGLOG((ENABLE, "Call to DHEnableAVIInfoFrame: Invalid version (0x%02X)\n", pAVIInfoFrame->Version));
		return RM_ERROR;
	}
	
	RMMemset(&InfoFrame, 0, sizeof(InfoFrame));
	InfoFrame.HeaderByte[0] = 0x82;  // 0x02 plus bit 7 (HDMI);
	InfoFrame.HeaderByte[1] = pAVIInfoFrame->Version;
	InfoFrame.HeaderByte[2] = 13;
	
	// AVI VIC and PR[3:0]
	RMinsShiftBits(&(InfoFrame.DataByte[4]), pAVIInfoFrame->VIC, 7, 0);
	RMinsShiftBits(&(InfoFrame.DataByte[5]), pAVIInfoFrame->pixel_repetition, 4, 0);
	RMDBGLOG((LOCALDBG, "  AVI Frame - VIC: %ld, pixel rep: %ld\n", InfoFrame.DataByte[4], InfoFrame.DataByte[5]));
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_OK;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		// set ICLK field in SiI9030 video control for selected pixel repetitions
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x48, &reg);
		if (RMFAILED(err)) {
			RMuint32 i;
			reg = 0x00;
			for (i = 0; i < sizeof(init_SiI9030) / sizeof(init_SiI9030[0]); i++) {
				if (init_SiI9030[i][0] == 0x48) reg = init_SiI9030[i][1];
			}
		}
		RMinsShiftBits(&reg, 0x00, 2, 0);  // already replicated inside SMP8634
		//	(pAVIInfoFrame->pixel_repetition == DH_pixel_sent_twice) ? 0x01 : 
		//	(pAVIInfoFrame->pixel_repetition == DH_pixel_sent_4_times) ? 0x03 : 0x00
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x48, reg);
		break;
	case DH_ANX9030:  // TODOANX
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
	    // do nothing here.
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		err = RM_ERROR;
	}
	
	// Picture content information
	DHInsertScanInfo(pAVIInfoFrame->scan_info, 
		&InfoFrame);
	DHInsertActiveFormat(pAVIInfoFrame->active_format_valid, pAVIInfoFrame->active_aspect_ratio, 
		&InfoFrame);
	DHInsertBarInfo(pAVIInfoFrame->info_bars, 
		pAVIInfoFrame->end_top_bar_line_num, pAVIInfoFrame->start_bottom_bar_line_num, 
		pAVIInfoFrame->end_left_bar_pixel_num, pAVIInfoFrame->start_right_bar_pixel_num, 
		&InfoFrame);
	
	// Color space and sampling (xYYx.xxxx and CCxx.xxxx)
	err = DHInsertColorSpace(pAVIInfoFrame->color_space, pAVIInfoFrame->sampling, 
		pDH->CEA_QuantSupport, force, 
		&InfoFrame);
	if (RMFAILED(err)) {
		if (! manutest) fprintf(stderr, "\n\n\tdvi_hdmi.c - WARNING! The color space does not match the current video mode!!!\n\tPlease fix your application!\n\n");
		if (! force) return err;
	}
	
	// Aspect ratio (yyMM.yyyy)
	err = DHInsertAspectRatio(pAVIInfoFrame->aspect_ratio, 
		force, 
		&InfoFrame);
	if (RMFAILED(err)) {
		if (! manutest) fprintf(stderr, "\n\n\tdvi_hdmi.c - WARNING! The aspect ratio does not match the current video mode!!!\n\tPlease fix your application!\n\n");
		if (! force) return err;
	}
	
	// TODO add to param struct
	err = DHInsertNonUniformScalingInfo(FALSE, FALSE, &InfoFrame);
	
	// TODO add to param struct
	err = DHInsertITContentInfo(FALSE, &InfoFrame);
	
	RMDBGLOG((LOCALDBG, "AVI Info Frame Data = 1:0x%02X 2:0x%02X 3:0x%02X 4:0x%02X 5:0x%02X\n", 
		InfoFrame.DataByte[1], InfoFrame.DataByte[2], InfoFrame.DataByte[3], InfoFrame.DataByte[4], InfoFrame.DataByte[5]));
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHEnableInfoFrame(pDH, DH_InfoFrameOffset_AVI, &InfoFrame);
}

RMstatus DHDisableAVIInfoFrame(struct DH_control *pDH)
{
	CHECK_pDH("DHDisableAVIInfoFrame");
	return DHDisableInfoFrame(pDH, DH_InfoFrameOffset_AVI);
}

static RMstatus DHReadAVI(struct DH_control *pDH, struct CEA861InfoFrame *pInfoFrame)
{
	RMstatus err;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_OK;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), DH_InfoFrameOffset_AVI + 0x00, pInfoFrame->HeaderByte, 3);
		if (RMFAILED(err)) return err;
		if ((pInfoFrame->HeaderByte[2] & 0x1F) != 13) return RM_ERROR;
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), DH_InfoFrameOffset_AVI + 0x03, pInfoFrame->DataByte, (pInfoFrame->HeaderByte[2] & 0x1F) + 1);
		break;
	case DH_ANX9030:
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), DH_InfoFrameOffsetANX_AVI + 0x00, pInfoFrame->HeaderByte, 3);
		if (RMFAILED(err)) return err;
		if ((pInfoFrame->HeaderByte[2] & 0x1F) != 13) return RM_ERROR;
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), DH_InfoFrameOffsetANX_AVI + 0x03, pInfoFrame->DataByte, (pInfoFrame->HeaderByte[2] & 0x1F) + 1);
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        DH_Switch_6611BANK(pDH,1) ;
    	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5D, pInfoFrame->DataByte[0] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x58, pInfoFrame->DataByte[1] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x59, pInfoFrame->DataByte[2] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5a, pInfoFrame->DataByte[3] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5b, pInfoFrame->DataByte[4] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5c, pInfoFrame->DataByte[5] ) ;
        
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5E, pInfoFrame->DataByte[6] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5F, pInfoFrame->DataByte[7] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x60, pInfoFrame->DataByte[8] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x61, pInfoFrame->DataByte[9] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x62, pInfoFrame->DataByte[10] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x63, pInfoFrame->DataByte[11] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x64, pInfoFrame->DataByte[12] ) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x65, pInfoFrame->DataByte[13] ) ;
        DH_Switch_6611BANK(pDH,0) ;
        break ;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		err = RM_ERROR;
	}
	
    {
        RMuint32 i ;
        RMDBGLOG((LOCALDBG,"DHReadAVI: ")) ;
        for( i = 0 ; i <= 13 ; i++ )
        {
            RMDBGPRINT((LOCALDBG," %02X", pInfoFrame->DataByte[i])) ;
        }
        RMDBGPRINT((LOCALDBG,"\n")) ;
    }	
	return err;
}

static RMstatus DHWriteAVI(struct DH_control *pDH, struct CEA861InfoFrame *pInfoFrame)
{
	RMstatus err;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_OK;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), DH_InfoFrameOffset_AVI + 0x00, pInfoFrame->HeaderByte, 3);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), DH_InfoFrameOffset_AVI + 0x03, pInfoFrame->DataByte, (pInfoFrame->HeaderByte[2] & 0x1F) + 1);
		if (RMFAILED(err)) return err;
		break;
	case DH_ANX9030:
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), DH_InfoFrameOffsetANX_AVI + 0x00, pInfoFrame->HeaderByte, 3);
		if (RMFAILED(err)) return err;
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), DH_InfoFrameOffsetANX_AVI + 0x03, pInfoFrame->DataByte, (pInfoFrame->HeaderByte[2] & 0x1F) + 1);
		if (RMFAILED(err)) return err;
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        DH_Switch_6611BANK(pDH,1) ;
    	pInfoFrame->HeaderByte[0] = 0x82;  // 0x02 plus bit 7 (HDMI);
    	pInfoFrame->HeaderByte[1] = 0x02;
    	pInfoFrame->HeaderByte[2] = 13;
    	err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x5D, &(pInfoFrame->DataByte[0] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x58, &(pInfoFrame->DataByte[1] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x59, &(pInfoFrame->DataByte[2] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x5a, &(pInfoFrame->DataByte[3] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x5b, &(pInfoFrame->DataByte[4] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x5c, &(pInfoFrame->DataByte[5] )) ;
        
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x5E, &(pInfoFrame->DataByte[6] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x5F, &(pInfoFrame->DataByte[7] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x60, &(pInfoFrame->DataByte[8] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x61, &(pInfoFrame->DataByte[9] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x62, &(pInfoFrame->DataByte[10] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x63, &(pInfoFrame->DataByte[11] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x64, &(pInfoFrame->DataByte[12] )) ;
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x65, &(pInfoFrame->DataByte[13] )) ;
        DH_Switch_6611BANK(pDH,0) ;        
        RMDBGLOG((LOCALDBG,"DHWriteAVI()\n"));
        DHDump_CAT6611reg(pDH) ;
    break ;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		err = RM_ERROR;
	}
	
	return err;
}

RMstatus DHModifyAVIScanInfo(struct DH_control *pDH, 
	enum DH_scan_info ScanInfo)
{
	RMstatus err;
	struct CEA861InfoFrame InfoFrame;
	
	CHECK_pDH("DHModifyAVIScanInfo");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIScanInfo: Not applicable, chipset does not support info frames\n"));
		return RM_OK;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIScanInfo: Not applicable, display does not support info frames\n"));
		return RM_OK;
	}
	
	err = DHReadAVI(pDH, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	err = DHInsertScanInfo(ScanInfo, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHWriteAVI(pDH, &InfoFrame);
}

RMstatus DHModifyAVIActiveFormat(struct DH_control *pDH, 
	RMbool ActiveFormatValid, 
	enum DH_active_format_aspect_ratio ActiveFormat)
{
	RMstatus err;
	struct CEA861InfoFrame InfoFrame;
	
	CHECK_pDH("DHModifyAVIActiveFormat");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIActiveFormat: Not applicable, chipset does not support info frames\n"));
		return RM_OK;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIActiveFormat: Not applicable, display does not support info frames\n"));
		return RM_OK;
	}
	
	err = DHReadAVI(pDH, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	err = DHInsertActiveFormat(ActiveFormatValid, ActiveFormat, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHWriteAVI(pDH, &InfoFrame);
}

RMstatus DHModifyAVIBarInfo(struct DH_control *pDH, 
	enum DH_AVI_info_bars info_bars, 
	RMuint16 end_top_bar_line_num, 
	RMuint16 start_bottom_bar_line_num, 
	RMuint16 end_left_bar_pixel_num, 
	RMuint16 start_right_bar_pixel_num)
{
	RMstatus err;
	struct CEA861InfoFrame InfoFrame;
	
	CHECK_pDH("DHModifyAVIBarInfo");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIBarInfo: Not applicable, chipset does not support info frames\n"));
		return RM_OK;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIBarInfo: Not applicable, display does not support info frames\n"));
		return RM_OK;
	}
	
	err = DHReadAVI(pDH, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	err = DHInsertBarInfo(info_bars, 
		end_top_bar_line_num, start_bottom_bar_line_num, 
		end_left_bar_pixel_num, start_right_bar_pixel_num, 
		&InfoFrame);
	if (RMFAILED(err)) return err;
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHWriteAVI(pDH, &InfoFrame);
}

RMstatus DHModifyAVIColorSpace(struct DH_control *pDH, 
	enum EMhwlibColorSpace ColorSpace, 
	enum EMhwlibSamplingMode SamplingMode)
{
	RMstatus err;
	RMbool force = TRUE;
	struct CEA861InfoFrame InfoFrame;
	
	CHECK_pDH("DHModifyAVIColorSpace");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIColorSpace: Not applicable, chipset does not support info frames\n"));
		return RM_OK;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIColorSpace: Not applicable, display does not support info frames\n"));
		return RM_OK;
	}
	
	err = DHReadAVI(pDH, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	err = DHInsertColorSpace(ColorSpace, SamplingMode, pDH->CEA_QuantSupport, force, &InfoFrame);
	if (RMFAILED(err) && ! force) return err;
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHWriteAVI(pDH, &InfoFrame);
}

RMstatus DHModifyAVIAspectRatio(struct DH_control *pDH, 
	struct EMhwlibAspectRatio AspectRatio)
{
	RMstatus err;
	RMbool force = TRUE;
	struct CEA861InfoFrame InfoFrame;
	
	CHECK_pDH("DHModifyAVIAspectRatio");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIAspectRatio: Not applicable, chipset does not support info frames\n"));
		return RM_OK;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIAspectRatio: Not applicable, display does not support info frames\n"));
		return RM_OK;
	}
	
	err = DHReadAVI(pDH, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	err = DHInsertAspectRatio(AspectRatio, force, &InfoFrame);
	if (RMFAILED(err) && ! force) return err;
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHWriteAVI(pDH, &InfoFrame);
}

RMstatus DHModifyNonUniformScaling(struct DH_control *pDH, 
	RMbool HorizontalStrech,  // picture has been horizontally scaled (e.g. non-linear scaling)
	RMbool VericalStrech)  // picture has been vertically scaled
{
	RMstatus err;
	struct CEA861InfoFrame InfoFrame;
	
	CHECK_pDH("DHModifyNonUniformScaling");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHModifyNonUniformScaling: Not applicable, chipset does not support info frames\n"));
		return RM_OK;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHModifyNonUniformScaling: Not applicable, display does not support info frames\n"));
		return RM_OK;
	}
	
	err = DHReadAVI(pDH, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	err = DHInsertNonUniformScalingInfo(HorizontalStrech, VericalStrech, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHWriteAVI(pDH, &InfoFrame);
}

RMstatus DHModifyITContent(struct DH_control *pDH, 
	RMbool ITContent)
{
	RMstatus err;
	struct CEA861InfoFrame InfoFrame;
	
	CHECK_pDH("DHModifyAVIAspectRatio");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIAspectRatio: Not applicable, chipset does not support info frames\n"));
		return RM_OK;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHModifyAVIAspectRatio: Not applicable, display does not support info frames\n"));
		return RM_OK;
	}
	
	err = DHReadAVI(pDH, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	err = DHInsertITContentInfo(ITContent, &InfoFrame);
	if (RMFAILED(err)) return err;
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHWriteAVI(pDH, &InfoFrame);
}

RMstatus DHEnableSPDInfoFrame(struct DH_control *pDH, 
	struct DH_SPDInfoFrame *pSPDInfoFrame, 
	enum DH_InfoFrameOffset offset)
{
	struct CEA861InfoFrame InfoFrame;
	RMuint32 n, i;
	RMascii ch;
	
	CHECK_pDH("DHEnableSPDInfoFrame");
	CHECK_PTR("DHEnableSPDInfoFrame", pSPDInfoFrame);
	
	if (pSPDInfoFrame->Version != 0x01) {
		RMDBGLOG((ENABLE, "Call to DHEnableSPDInfoFrame: Invalid version (0x%02X)\n", pSPDInfoFrame->Version));
		return RM_ERROR;
	}
	
	RMDBGLOG((ENABLE, "Sending SPD Info Frame:\n"));
	RMDBGLOG((ENABLE, "  Vendor:\"%0.8s\" - Product:\"%0.16s\" - Type:0x%02X\n", pSPDInfoFrame->VendorName, pSPDInfoFrame->ProductDescription, pSPDInfoFrame->SDI));
	RMMemset(&InfoFrame, 0, sizeof(InfoFrame));
	InfoFrame.HeaderByte[0] = 0x83;  // 0x03 plus bit 7 (HDMI);
	InfoFrame.HeaderByte[1] = pSPDInfoFrame->Version;
	InfoFrame.HeaderByte[2] = 25;
	
	n = 0;
	ch = pSPDInfoFrame->VendorName[n++];
	for (i = 1; i <= 8; i++) {
		InfoFrame.DataByte[i] = (RMuint8)ch & 0x7F;
		if (ch) ch = pSPDInfoFrame->VendorName[n++];
	}
	
	n = 0;
	ch = pSPDInfoFrame->ProductDescription[n++];
	for (i = 9; i <= 24; i++) {
		InfoFrame.DataByte[i] = (RMuint8)ch & 0x7F;
		if (ch) ch = pSPDInfoFrame->ProductDescription[n++];
	}
	
	InfoFrame.DataByte[25] = pSPDInfoFrame->SDI;
	
	RMDBGLOG((ENABLE, "SPD Info Frame Data = 1:0x%02X 2:0x%02X 3:0x%02X 4:0x%02X 5:0x%02X\n",
		InfoFrame.DataByte[1], InfoFrame.DataByte[2], InfoFrame.DataByte[3], InfoFrame.DataByte[4], InfoFrame.DataByte[5]));
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	if (! offset) offset = DH_InfoFrameOffset_SPD;
	return DHEnableInfoFrame(pDH, offset, &InfoFrame);
}

RMstatus DHDisableSPDInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset)
{
	CHECK_pDH("DHDisableSPDInfoFrame");
	
	if (! offset) offset = DH_InfoFrameOffset_SPD;
	return DHDisableInfoFrame(pDH, offset);
}

RMstatus DHEnableAudioInfoFrame(struct DH_control *pDH, struct DH_AudioInfoFrame *pAudioInfoFrame)
{
	struct CEA861InfoFrame InfoFrame;
	
	CHECK_pDH("DHEnableAudioInfoFrame");
	CHECK_PTR("DHEnableAudioInfoFrame", pAudioInfoFrame);
	
	if (pAudioInfoFrame->Version != 0x01) {
		RMDBGLOG((ENABLE, "Call to DHEnableAudioInfoFrame: Invalid version (0x%02X)\n", pAudioInfoFrame->Version));
		return RM_ERROR;
	}
	if ((pAudioInfoFrame->CA == DH_Audio_CA_FLC7_FRC8) || 
		(pAudioInfoFrame->CA && (! pAudioInfoFrame->FC_Ch4_Enable) && (! pAudioInfoFrame->LFE_Ch3_Enable))) {
		if (! manutest) fprintf(stderr, "ERROR: LPCM Audio channel mapping incompatible with SiI9030!\n");
		return RM_ERROR;
	}
	
	RMMemset(&InfoFrame, 0, sizeof(InfoFrame));
	InfoFrame.HeaderByte[0] = 0x84;  // 0x04 plus bit 7 (HDMI);
	InfoFrame.HeaderByte[1] = pAudioInfoFrame->Version;
	InfoFrame.HeaderByte[2] = 10;
	
	// HDMI 1.1 and CEA 861-C: CodingType, SampleFrequency, SampleSize shall be set to 0 "Refer to Stream Header"
	// to avoid redundancy with IEC-60958 stream header
	pAudioInfoFrame->CodingType = DH_AudioCodingType_FromStreamHeader;
	pAudioInfoFrame->SampleFrequency = DH_AudioSampleFrequency_FromStreamHeader;
	pAudioInfoFrame->SampleSize = DH_AudioSampleSize_FromStreamHeader;
	
	RMinsShiftBits(&(InfoFrame.DataByte[1]), pAudioInfoFrame->CodingType, 4, 4);
	RMinsShiftBits(&(InfoFrame.DataByte[1]), pAudioInfoFrame->ChannelCount, 3, 0);
	RMinsShiftBits(&(InfoFrame.DataByte[2]), pAudioInfoFrame->SampleFrequency, 3, 2);
	RMinsShiftBits(&(InfoFrame.DataByte[2]), pAudioInfoFrame->SampleSize, 2, 0);
	
	switch (pAudioInfoFrame->CodingType) {
	case DH_AudioCodingType_FromStreamHeader:
		InfoFrame.DataByte[3] = 0;
		break;
	case DH_AudioCodingType_PCM: 
		InfoFrame.DataByte[3] = 0;
		break;
	case DH_AudioCodingType_AC3: 
	case DH_AudioCodingType_MPEG1_12: 
	case DH_AudioCodingType_MPEG1_3: 
	case DH_AudioCodingType_MPEG2: 
	case DH_AudioCodingType_AAC: 
	case DH_AudioCodingType_DTS: 
	case DH_AudioCodingType_ATRAC: 
		InfoFrame.DataByte[3] = pAudioInfoFrame->MaxBitRate;
		break;
	case DH_AudioCodingType_OneBit:
	case DH_AudioCodingType_DDPlus:
	case DH_AudioCodingType_DTSHD:
	case DH_AudioCodingType_MLP:
		InfoFrame.DataByte[3] = 0;  // Reserved
		break;
	}
	InfoFrame.DataByte[4] = 
		((RMuint8)(pAudioInfoFrame->CA) << 2) | 
		(pAudioInfoFrame->FC_Ch4_Enable ? 0x02 : 0x00) | 
		(pAudioInfoFrame->LFE_Ch3_Enable ? 0x01 : 0x00);
	if (InfoFrame.DataByte[4]) {  // multi-channel audio only
		InfoFrame.DataByte[5] = 
			(pAudioInfoFrame->DownMixInhibit ? 0x80 : 0x00) | 
			((pAudioInfoFrame->LevelShift & 0x0F) << 3);
	}
	
	RMDBGLOG((ENABLE, "Audio Info Frame Data = 1:0x%02X 2:0x%02X 3:0x%02X 4:0x%02X 5:0x%02X\n",
		InfoFrame.DataByte[1], InfoFrame.DataByte[2], InfoFrame.DataByte[3], InfoFrame.DataByte[4], InfoFrame.DataByte[5]));
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	return DHEnableInfoFrame(pDH, DH_InfoFrameOffset_Audio, &InfoFrame);
}

RMstatus DHDisableAudioInfoFrame(struct DH_control *pDH)
{
	CHECK_pDH("DHDisableAudioInfoFrame");
	return DHDisableInfoFrame(pDH, DH_InfoFrameOffset_Audio);
}

RMstatus DHEnableMPEGInfoFrame(struct DH_control *pDH, struct DH_MPEGInfoFrame *pMPEGInfoFrame, enum DH_InfoFrameOffset offset)
{
	struct CEA861InfoFrame InfoFrame;
	RMuint32 i;
	
	CHECK_pDH("DHEnableMPEGInfoFrame");
	CHECK_PTR("DHEnableMPEGInfoFrame", pMPEGInfoFrame);
	
	if (pMPEGInfoFrame->Version != 0x01) {
		RMDBGLOG((ENABLE, "Call to DHEnableMPEGInfoFrame: Invalid version (0x%02X)\n", pMPEGInfoFrame->Version));
		return RM_INVALID_PARAMETER;
	}
	
	RMMemset(&InfoFrame, 0, sizeof(InfoFrame));
	InfoFrame.HeaderByte[0] = 0x85;  // 0x05 plus bit 7 (HDMI);
	InfoFrame.HeaderByte[1] = pMPEGInfoFrame->Version;
	InfoFrame.HeaderByte[2] = 10;
	
	InfoFrame.DataByte[1] = pMPEGInfoFrame->BitRate & 0xFF;
	InfoFrame.DataByte[2] = (pMPEGInfoFrame->BitRate >> 8) & 0xFF;
	InfoFrame.DataByte[3] = (pMPEGInfoFrame->BitRate >> 16) & 0xFF;
	InfoFrame.DataByte[4] = (pMPEGInfoFrame->BitRate >> 24) & 0xFF;
	InfoFrame.DataByte[5] = 
		(pMPEGInfoFrame->RepeatedField ? 0x10 : 0x00) | 
		(pMPEGInfoFrame->Frame & 0x03);
	for (i = 6; i <= (RMuint32)(InfoFrame.HeaderByte[2] & 0x1F); i++) InfoFrame.DataByte[i] = 0;
	
	RMDBGLOG((ENABLE, "MPEG Info Frame Data = 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
		InfoFrame.DataByte[1], InfoFrame.DataByte[2], InfoFrame.DataByte[3], InfoFrame.DataByte[4], InfoFrame.DataByte[5]));
	
	DHCalcInfoFrameCheckSum(&InfoFrame);
	if (! offset) offset = DH_InfoFrameOffset_MPEG;
	return DHEnableInfoFrame(pDH, offset, &InfoFrame);
}

RMstatus DHDisableMPEGInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset)
{
	CHECK_pDH("DHDisableMPEGInfoFrame");
	
	if (! offset) offset = DH_InfoFrameOffset_MPEG;
	return DHDisableInfoFrame(pDH, offset);
}

// calculate CEA861 checksum and store in DataByte[0], data size in HeaderByte[2]
RMstatus DHCalcInfoFrameCheckSum(struct CEA861InfoFrame *pInfoFrame)
{
	RMuint32 i, checksum = 0;
	
	CHECK_PTR("DHCalcInfoFrameCheckSum", pInfoFrame);
	
	for (i = 0; i < 3; i++) {
		checksum += pInfoFrame->HeaderByte[i];
	}
	for (i = 1; i <= (RMuint32)(pInfoFrame->HeaderByte[2] & 0x1F); i++) {
		checksum += pInfoFrame->DataByte[i];
	}
	pInfoFrame->DataByte[0] = (0x100 - (checksum & 0xFF)) & 0xFF;
	
	return RM_OK;
}

// load content and enable repeated transmission of selected info frame
RMstatus DHEnableInfoFrame(struct DH_control *pDH, 
	enum DH_InfoFrameOffset offset, 
	struct CEA861InfoFrame *pInfoFrame)
{
	RMstatus err = RM_OK ;
	RMuint32 enable, offs2 ;
	
	CHECK_pDH("DHEnableInfoFrame");
	CHECK_PTR("DHEnableInfoFrame", pInfoFrame);
	RMDBGLOG((LOCALDBG,"DHEnableInfoFrame(): offset = %04x\n",offset)) ;
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHEnableInfoFrame: Error, chipset does not support info frames\n"));
		return RM_ERROR;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHEnableInfoFrame: Error, display does not support info frames\n"));
		return RM_ERROR;
	}
	if ((offset & 0x1F) || ((offset & 0xC0) < 0x40)) {
		RMDBGLOG((ENABLE, "Call to DHEnableInfoFrame with invalid offset: 0x%02X\n", offset));
		return RM_INVALID_PARAMETER;
	}
	enable = 3 << ((offset >> 4) - 4);
	if (offset == 0xE0) enable <<= 2;
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
		err = RM_OK;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), offset + 0x00, pInfoFrame->HeaderByte, 3);
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), offset + 0x03, pInfoFrame->DataByte, (pInfoFrame->HeaderByte[2] & 0x1F) + 1);
		break;
	case DH_ANX9030:
		switch (offset) {
		case DH_InfoFrameOffset_AVI:
			offs2 = DH_InfoFrameOffsetANX_AVI; break;
		case DH_InfoFrameOffset_SPD:
			offs2 = DH_InfoFrameOffsetANX_SPD; break;
		case DH_InfoFrameOffset_Audio:
			offs2 = DH_InfoFrameOffsetANX_Audio; break;
		case DH_InfoFrameOffset_MPEG:
			offs2 = DH_InfoFrameOffsetANX_MPEG; break;
		case DH_InfoFrameOffset_Generic:
			offs2 = DH_InfoFrameOffsetANX_Generic; break;
		case DH_InfoFrameOffset_Generic2:
			offs2 = DH_InfoFrameOffsetANX_Generic2; break;
		default:
			RMDBGLOG((ENABLE, "Unknown InfoFrameOffset value! 0x%02X\n", offset));
			return RM_ERROR;
		}
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), offs2 + 0x00, pInfoFrame->HeaderByte, 3);
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), offs2 + 0x03, pInfoFrame->DataByte, (pInfoFrame->HeaderByte[2] & 0x1F) + 1);
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        switch(offset)
        {
    	case DH_InfoFrameOffset_AVI :
            DH_Switch_6611BANK(pDH,1) ;
        	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5D, pInfoFrame->DataByte[0] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x58, pInfoFrame->DataByte[1] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x59, pInfoFrame->DataByte[2] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5a, pInfoFrame->DataByte[3] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5b, pInfoFrame->DataByte[4] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5c, pInfoFrame->DataByte[5] ) ;
            
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5E, pInfoFrame->DataByte[6] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x5F, pInfoFrame->DataByte[7] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x60, pInfoFrame->DataByte[8] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x61, pInfoFrame->DataByte[9] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x62, pInfoFrame->DataByte[10] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x63, pInfoFrame->DataByte[11] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x64, pInfoFrame->DataByte[12] ) ;
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x65, pInfoFrame->DataByte[13] ) ;
            DH_Switch_6611BANK(pDH,0) ;
            // err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xCD, 3); // enable AVI infoframe and repeat.
    	    RMDBGLOG((LOCALDBG,"DH_InfoFrameOffset_AVI\n")) ;
    	    // DHDump_CAT6611reg(pDH) ;
            break ;
    	case DH_InfoFrameOffset_SPD :
            DH_Switch_6611BANK(pDH,1) ;
            err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x70, pInfoFrame->DataByte, 26) ; 
            DH_Switch_6611BANK(pDH,0) ;
    	    RMDBGLOG((LOCALDBG,"DH_InfoFrameOffset_SPD\n")) ;
    	    // DHDump_CAT6611reg(pDH) ;
            
            // err = DH_i2c_write(pDH->pRUA,&(pDH->i2c_tx), 0xCF, 0x1) ;
            break ;

    	case DH_InfoFrameOffset_Audio :
            DH_Switch_6611BANK(pDH,1) ;
        	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x6D, pInfoFrame->DataByte[0]);
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x68, pInfoFrame->DataByte[1]);
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x6B, pInfoFrame->DataByte[3]);
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x6C, pInfoFrame->DataByte[4]);
            DH_Switch_6611BANK(pDH,0) ;
    	    RMDBGLOG((LOCALDBG,"DH_InfoFrameOffset_Audio\n")) ;
    	    // DHDump_CAT6611reg(pDH) ;
            break ;
    	case DH_InfoFrameOffset_MPEG :
            DH_Switch_6611BANK(pDH,1) ;
        	err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x8F, pInfoFrame->DataByte[0]);
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x8A, pInfoFrame->DataByte[1]);
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x8B, pInfoFrame->DataByte[2]);
            err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x8E, pInfoFrame->DataByte[5]);
            DH_Switch_6611BANK(pDH,0) ;
    	    RMDBGLOG((LOCALDBG,"DH_InfoFrameOffset_MPEG\n")) ;
    	    // DHDump_CAT6611reg(pDH) ;
            break ;
        default:
            // don't process the remained block.
            RMDBGLOG((LOCALDBG,"Other Infoframe Insert, don't care\n")) ;
            err = RM_OK ;
    	}
        break ;
    //~jj_tseng@chipadvanced.com 2007/03/23
		
	default:
		err = RM_ERROR;
	}
	
	if (RMSUCCEEDED(err)) {
		err = DHSetInfoFrameEnable(pDH, enable);
		if (RMSUCCEEDED(err)) {
			RMDBGLOG((LOCALDBG, "Info frame at 0x%02X sent and enabled\n", offset));
		} else {
			RMDBGLOG((ENABLE, "Failed to enable Info frame at 0x%02X!\n", offset));
		}
	} else {
		RMDBGLOG((ENABLE, "Failed to send Info frame at 0x%02X!\n", offset));
	}
	
	return err;
}

// disable repeated transmission of selected info frame
RMstatus DHDisableInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset)
{
	RMstatus err;
	RMuint32 enable;
	
	CHECK_pDH("DHDisableInfoFrame");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHDisableInfoFrame: Error, chipset does not support info frames\n"));
		return RM_ERROR;
	}
	if (! pDH->HDMI_mode) {
		RMDBGLOG((ENABLE, "Call to DHDisableInfoFrame: Display does not support info frames (no error)\n"));
		return RM_OK;
	}
	if ((offset & 0x1F) || ((offset & 0xC0) < 0x40)) {
		RMDBGLOG((ENABLE, "Call to DHDisableInfoFrame with invalid offset: 0x%02X\n", offset));
		return RM_INVALID_PARAMETER;
	}
	
	enable = 3 << ((offset >> 4) - 4);
	if (offset == 0xE0) enable <<= 2;
	
	err = DHClearInfoFrameEnable(pDH, enable);
	
	RMDBGLOG((LOCALDBG, "Info frame at 0x%02X disabled\n", offset));
	
	return err;
}

// wait until selected info frame has been sent once
RMstatus DHWaitInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset)
{
	RMstatus err;
	RMuint32 enable;
	RMuint8 ife;
	RMuint64 t0, t1;
	
	CHECK_pDH("DHWaitInfoFrame");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHWaitInfoFrame: Error, chipset does not support info frames\n"));
		return RM_ERROR;
	}
	if ((offset & 0x1F) || ((offset & 0xC0) < 0x40)) {
		RMDBGLOG((ENABLE, "Call to DHWaitInfoFrame with invalid offset: 0x%02X\n", offset));
		return RM_INVALID_PARAMETER;
	}
	
	if(pDH->part == DH_CAT6611)
	{
        return RM_OK ;
	}
	
	enable = 2 << ((offset >> 4) - 4);
	if (offset == 0xE0) enable <<= 2;
	
	t0 = RMGetTimeInMicroSeconds();
	do {
		if (enable < 0x100) {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x3E, &ife);
			ife &= (enable & 0xFF);
		} else {
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x3F, &ife);
			ife &= ((enable >> 8) & 0xFF);
		}
		t1 = RMGetTimeInMicroSeconds();
		if (RMCyclesElapsed64(t0, t1) > 200000) err = RM_TIMEOUT;  // 0.2 sec
	} while (RMSUCCEEDED(err) && ife);
	
	return err;
}

RMstatus DHDisableHDMIMode(struct DH_control *pDH)
{
	CHECK_pDH("DHDisableHDMIMode");
	return DHSetHDMIMode(pDH, FALSE);
}

// set up first 16 bit in IEC-60958 header
RMstatus DHSetAudioHeader(
	struct DH_control *pDH, 
	RMuint32 mask,   // 1 for each bit to be set
	RMuint32 value)  // 1 or 0 to go into the header
{
	RMuint8 IEC60958channelstatus[5];
	RMstatus err = RM_OK;
	
	CHECK_pDH("DHSetAudioHeader");
	RMDBGLOG((LOCALDBG,"DHSetAudioHeader()\n")) ;
	mask &= ~0x0006;  // don't allow to change bits 1 (CP) and 2 (PCM)
	pDH->AudioHeader = (pDH->AudioHeader & ~mask) | (value & mask);
	
	// set up IEC-60958 channel status header, for PCM only
	RMDBGLOG((LOCALDBG, "Modifying IEC-60958 channel status with I2S stream: 0xXX.XX.XX.%02X.%02X\n", 
		(pDH->AudioHeader >> 8) & 0xFF, pDH->AudioHeader & 0xFF));
	
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x1E, pDH->AudioHeader & 0xFF);  // bit 0..7
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x1F, (pDH->AudioHeader >> 8) & 0xFF);  // bit 8..15
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), 0x1E, IEC60958channelstatus, 5);
		break;
	case DH_ANX9030:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x56, pDH->AudioHeader & 0xFF);  // bit 0..7
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x57, (pDH->AudioHeader >> 8) & 0xFF);  // bit 8..15
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x56, IEC60958channelstatus, 5);
		break;
    case DH_CAT6611:
    // 2007/03/23 Added by jj_tseng@chipadvanced
        DH_Switch_6611BANK(pDH,1) ;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x91, pDH->AudioHeader & 0xFF);  // bit 0..7
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x92, (pDH->AudioHeader >> 8) & 0xFF);  // bit 8..15
		DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x91, IEC60958channelstatus, 5) ;
        DH_Switch_6611BANK(pDH,0) ;
        DHDump_CAT6611reg(pDH) ;
        break ;		
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		break;
	}
	
	if (RMSUCCEEDED(err)) {
		RMDBGLOG((LOCALDBG, "Sending IEC-60958 channel status with I2S stream: 0x%02X.%02X.%02X.%02X.%02X\n", 
			IEC60958channelstatus[4], IEC60958channelstatus[3], IEC60958channelstatus[2], IEC60958channelstatus[1], IEC60958channelstatus[0]));
	} else {
		RMDBGLOG((LOCALDBG, "Could not read back IEC-60958 channel status!\n"));
	}
	
	return err;
}

// set up C bit in IEC-60958 header
RMstatus DHSetAudioCP(
	struct DH_control *pDH, 
	RMbool AudioCP)
{
	RMuint8 IEC60958channelstatus[5];
	RMstatus err = RM_OK;
	
	CHECK_pDH("DHSetAudioCP");
	
	RMDBGLOG((LOCALDBG,"DHSetAudioCP()\n")) ;
	
	pDH->AudioHeader = (pDH->AudioHeader & ~0x04) | (AudioCP ? 0x04 : 0x00);
	
	// set up IEC-60958 channel status header, for PCM only
	RMDBGLOG((LOCALDBG, "Modifying IEC-60958 channel status with I2S stream: 0xXX.XX.XX.XX.X%01X\n", 
		pDH->AudioHeader & 0xF));
	
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x1E, pDH->AudioHeader & 0xFF);  // bit 0..7
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx2), 0x1E, IEC60958channelstatus, 5);
		break;
	case DH_ANX9030:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x56, pDH->AudioHeader & 0xFF);  // bit 0..7
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x56, IEC60958channelstatus, 5);
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
    case DH_CAT6611:
        DH_Switch_6611BANK(pDH,1) ;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x91, pDH->AudioHeader & 0xFF);  // bit 0..7
		err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2c_tx), 0x91, IEC60958channelstatus, 5) ;
        DH_Switch_6611BANK(pDH,0) ;
        RMDBGPRINT((LOCALDBG,"reg191 = %02X%02X%02X%02X%02X\n",IEC60958channelstatus[0],IEC60958channelstatus[1],IEC60958channelstatus[2],IEC60958channelstatus[3],IEC60958channelstatus[4]));
        break ;
        
    //~jj_tseng@chipadvanced.com 2007/03/23
		
	default:
		break;
	}
	
	if (RMSUCCEEDED(err)) {
		RMDBGLOG((LOCALDBG, "Sending IEC-60958 channel status with I2S stream: 0x%02X.%02X.%02X.%02X.%02X\n", 
			IEC60958channelstatus[4], IEC60958channelstatus[3], IEC60958channelstatus[2], IEC60958channelstatus[1], IEC60958channelstatus[0]));
	} else {
		RMDBGLOG((LOCALDBG, "Could not read back IEC-60958 channel status!\n"));
	}
	
	return err;
}

static RMuint32 iec_header_fs(RMuint32 SampleFrequency)
{
	switch (SampleFrequency) {
		case 22050: return 4;
		case 24000: return 6;
		case 32000: return 3;
		case 44100: return 0;
		case 48000: return 2;
		case 88200: return 8;
		case 96000: return 10;
		case 176400: return 12;
		case 192000: return 14;
		case 768000: return 9;
	}
	return 1;
}

// set up N value of clock divider for HDMI audio sample clock recovery
static RMstatus DHSetAudioClock(
	struct DH_control *pDH, 
	RMuint32 PixelClock, 
	RMuint32 SampleClock)
{
	RMstatus err;
	RMuint32 n = 0;
	RMuint32 sf_i = 0, pc_i = 0, dc_i = 0;
	RMuint8 reg;
	
	// for each fs, n value for "other", 25.2/1.001, 27*1.001, 74.25/1.001, 148.5/1.001 pixclk
	static const RMuint32 hdmi_expected_n[4][7][5] = {
		{  // For 8 bit color components
		{ 4096,  4576,  4096, 11648, 11648}, 
		{ 6272,  7007,  6272, 17836,  8918}, 
		{ 6144,  6864,  6144, 11648,  5824}, 
		{12544, 14014, 12544, 35672, 17836}, 
		{12288, 13728, 12288, 23296, 11648}, 
		{25088, 28028, 25088, 71344, 35672}, 
		{24576, 27456, 24576, 46592, 23296}
		}, {  // For 10 bit deep color components (TODO: not defined by HDMI 1.3!)
		{ 4096,  4576,  4096, 11648, 11648}, 
		{ 6272,  7007,  6272, 17836,  8918}, 
		{ 6144,  6864,  6144, 11648,  5824}, 
		{12544, 14014, 12544, 35672, 17836}, 
		{12288, 13728, 12288, 23296, 11648}, 
		{25088, 28028, 25088, 71344, 35672}, 
		{24576, 27456, 24576, 46592, 23296}
		}, {  // For 12 bit deep color components
		{ 4096,  9152,  8192, 11648, 11648}, 
		{ 6272,  7007,  6272, 17836, 17836}, 
		{ 6144,  9152,  8192, 11648, 11648}, 
		{12544, 14014, 12544, 35672, 35672}, 
		{12288, 18304, 16384, 23296, 23296}, 
		{25088, 28028, 25088, 71344, 71344}, 
		{24576, 36608, 32768, 46592, 46592}
		}, {  // For 16 bit deep color components (same as 8 bit)
		{ 4096,  4576,  4096, 11648, 11648}, 
		{ 6272,  7007,  6272, 17836,  8918}, 
		{ 6144,  6864,  6144, 11648,  5824}, 
		{12544, 14014, 12544, 35672, 17836}, 
		{12288, 13728, 12288, 23296, 11648}, 
		{25088, 28028, 25088, 71344, 35672}, 
		{24576, 27456, 24576, 46592, 23296}
		}
	};
	
	if (! pDH->part_caps.HDMI) {
		if (! manutest) fprintf(stderr, "[HDMI] Call to DHSetAudioClock: Error, chipset does not support audio clocks\n");
		return RM_ERROR;
	}
	RMDBGLOG((LOCALDBG,"DHSetAudioClock(pDH, %ld, %ld)\n",PixelClock,SampleClock)) ;
	
	
	// Disable previous audio output during change
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x14, pDH->audio_mode & 0xFE);
		break;
	case DH_ANX9030:  // TODOANX
		err = RM_OK;
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
	case DH_CAT6611:  // TODOANX
	    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &reg ) ; // disable audio clock
	    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, reg | 4 ) ;
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		err = RM_ERROR;
	}
	
	if (! manutest) fprintf(stderr, "[HDMI] Setting the HDMI Audio Clock, PixClk=%ld, SampleClk=%ld\n", PixelClock, SampleClock);
	// avrg. CTS = (PixelClock * n) / (128 * SampleClock)  (CTS is measured by SiI9030)
	// (128 * SampleClock) / 1500 <= n <= (128 * SampleClock) / 300
	switch (SampleClock) {
		case  32000: sf_i = 0; break;
		case  44100: sf_i = 1; break;
		case  48000: sf_i = 2; break;
		case  88200: sf_i = 3; break;
		case  96000: sf_i = 4; break;
		case 176400: sf_i = 5; break;
		case 192000: sf_i = 6; break;
		default: 
			if (! manutest) fprintf(stderr, "[HDMI] Invalid HDMI Audio sample clock: %lu Hz!\n", SampleClock);
			break;  // try anyhow
	}
	switch (PixelClock) {
		case  25174825: pc_i = 1; break;  // 25.2 MHz / 1.001
		case  27002700: pc_i = 2; break;  // 27 MHz * 1.001
		case  74175824: pc_i = 3; break;  // 74.25 MHz / 1.001
		case 148351648: pc_i = 4; break;  // 148.5 MHz / 1.001
		default: break;  // all other
	}
	if (pDH->part_caps.DeepColor) {
		RMuint32 dc = 8;
		switch (pDH->part) {
		case DH_siI9134:
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x2F, &reg);
			if (RMSUCCEEDED(err)) {
				switch (RMunshiftBits(reg, 3, 3)) {
					case 4: dc = 8; break;
					case 5: dc = 10; break;
					case 6: dc = 12; break;
				}
			}
			break;
		default:
			break;
		}
		switch (dc) {
			case  8: dc_i = 0; break;
			case 10: dc_i = 1; break;
			case 12: dc_i = 2; break;
			case 16: dc_i = 3; break;
			default: break;
		}
	}
	if (! n) n = hdmi_expected_n[dc_i][sf_i][pc_i];
	
//	if ((pAudioFormat->NumChannel > 2) && (
//		((SampleClock > 48000) && (PixelClock < 54000000)) || 
//		((SampleClock > 96000) && (PixelClock < 74000000))
//	)) {
//		if (! manutest) fprintf(stderr, "[HDMI] %lu channel audio at %lu Hz can not be sent via video mode with pixel clock %lu!\n", 
//			pAudioFormat->NumChannel, SampleClock, PixelClock);
//		return RM_ERROR;
//	}
	
	RMDBGLOG((LOCALDBG, "Setting N value: %ld\n", n));
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x03, n & 0xFF);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x04, (n >> 8) & 0xFF);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x05, (n >> 16) & 0xF);
		if (pDH->ForceCTS) {
			RMuint32 cts;
			cts = SampleClock * 128;
			cts = RM64div64((RMuint64)PixelClock * n + cts / 2, cts);
			RMDBGLOG((LOCALDBG, "Setting CTS value: %ld\n", cts));
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x06, cts & 0xFF);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x07, (cts >> 8) & 0xFF);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x08, (cts >> 16) & 0xF);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x01, 0x03);  // N/CTS packet enabled, SW-updated CTS
		} else {
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x01, 0x02);  // N/CTS packet enabled, HW-updated CTS
		}
		RMDBGLOG((LOCALDBG, "Flushing Audio FIFO\n"));
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, 0x02);  // flush audio FIFO
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x05, 0x00);
		
		// Re-Enable previous audio output
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x14, pDH->audio_mode);
		break;
	case DH_ANX9030:  // TODOANX
		err = RM_OK;
		break;
		
    // 2007/03/23 Added by jj_tseng@chipadvanced
	case DH_CAT6611:  // TODOANX
        DH_Switch_6611BANK(pDH,1) ;
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x33, n & 0xFF);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x34, (n >> 8) & 0xFF);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x35, (n >> 16) & 0xF);
     	reg = 0 ;
    	if (pDH->ForceCTS) {
    		RMuint32 cts;
    		cts = SampleClock * 128;
    		cts = RM64div64((RMuint64)PixelClock * n + cts / 2, cts);
    		RMDBGLOG((LOCALDBG, "Setting CTS value: %ld\n", cts));
    		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x30, cts & 0xFF);
    		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x31, (cts >> 8) & 0xFF);
    		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x32, (cts >> 16) & 0xF);
    		reg = 2 ;
    	}
        DH_Switch_6611BANK(pDH,0) ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xc5, reg) ;
    
    	RMDBGLOG((LOCALDBG, "Reset audio\n"));
        err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &reg) ; // 
    	reg &= ~4 ;
        err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, reg) ; // reset audio
    	DHDump_CAT6611reg(pDH) ;
        
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23

	default:
		err = RM_ERROR;
	}
	
	return RM_OK;
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHSetAudioFormat_CAT(
	struct DH_control *pDH, 
	struct DH_AudioFormat *pAudioFormat)
{
	RMstatus err;
	RMuint8 fs;
	RMuint8 data ;

	RMDBGLOG((ENABLE, "DHSetAudioFormat_CAT: Setting the Audio Format: %ld channel, %ld kHz, \n",
		  pAudioFormat->NumChannel,
		  pAudioFormat->SampleFrequency));

	switch (pAudioFormat->SampleFrequency) {
		case 32000: fs = 3; break;
		case 44100: fs = 0; break;
		case 48000: fs = 2; break;
		case 88200: fs = 8; break;
		case 96000: fs = 10; break;
		case 176400: fs = 12; break;
		case 192000: fs = 14; break;
		default: fs = 1;
	}
	DH_Switch_6611BANK(pDH,0) ;

    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &data) ; // enable video on
	data |= 4 ;
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, data) ; // enable video on


    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xE3, 0x00);  // don't swap l/r on any i2s input
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xE2, 0xE4);  // default FIFO mapping
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xE4, 0x08);  // default FIFO mapping

    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xE1, 0x41);

    DH_Switch_6611BANK(pDH,    1) ;

    // err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x91, 0x00); // src number = 0.
    // err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x92, 0x00); // src number = 0.
    // err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x93, 0x00); // src number = 0.
    // don't care the channel number
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x94, 0x21);  // channels, src num
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x95, 0x43);  // channels, src num
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x96, 0x65);  // channels, src num
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x97, 0x87);  // channels, src num



    //err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x20, 0x00 | (pAudioFormat->NumChannel << 4));  // channels, src num
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x98, 0x00 | (fs & 0x0F));  // spdif fs
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x99, 0x0B | ((~fs) << 4));  // 24 bit i2s

    DH_Switch_6611BANK(pDH,    0) ;
    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x59, &data);  // 0: orig sample rate, 1: half sample rate, 3: quarter sample rate
    data &= ~3 ;
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x59, data);  // 0: orig sample rate, 1: half sample rate, 3: quarter sample rate
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xE0,
    	((pAudioFormat->NumChannel>2) ? 0x2 : 0) |
    	((pAudioFormat->NumChannel>4) ? 0x4 : 0) |
    	((pAudioFormat->NumChannel>6) ? 0x8 : 0) |
        (pAudioFormat->SPDIF_Enable ? 0x10 : 0) |
    	0xC1 );  // 24bit audio

    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC1, 0);  // clear AVMute flag
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0xC6, 3);  // send GCP
	DH_Clear_CAT6611_AVMute(pDH) ;
    // reg E0, E1, E3, ... etc
    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &data) ;
	data &= ~4 ;
    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, data) ; // enable video on
	DHDump_CAT6611reg(pDH) ;
	return RM_OK;
}
//~jj_tseng@chipadvanced.com 2007/03/23

// set up audio format in the SiI9030
RMstatus DHSetAudioFormat(
	struct DH_control *pDH, 
	struct DH_AudioFormat *pAudioFormat)
{
	RMstatus err;
	RMuint8 reg, fs, fs_orig, src, freq_sval;
	RMuint32 mclk_f;
	RMuint8 IEC60958channelstatus[5];
	
	CHECK_pDH("DHSetAudioFormat");
	CHECK_PTR("DHSetAudioFormat", pAudioFormat);
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHSetAudioFormat: Error, chipset does not support audio formatting\n"));
		return RM_ERROR;
	}

    if( pDH->part == DH_CAT6611 )
    {
        return DHSetAudioFormat_CAT(pDH,pAudioFormat) ;
    }
    	
	if (! manutest) fprintf(stderr, "[HDMI] Setting the Audio Format: %ld channel, %ld Hz\n",
		pAudioFormat->NumChannel,
		pAudioFormat->SampleFrequency);
	
	pDH->AudioSampleClock = pAudioFormat->SampleFrequency;
	fs = iec_header_fs(pAudioFormat->SampleFrequency);
	if (fs == 1) fs_orig = 0;
	else fs_orig = 15 - fs;
	
	src = 0x00;
	if ((! pAudioFormat->I2S1_Enable) && (pAudioFormat->NumChannel == 2)) {
		switch (pAudioFormat->SRC_Factor) {
		case 2:
			src = 0x01;
			if (0) // skip first line of next case
		case 4:
			src = 0x03;
			if (! manutest) fprintf(stderr, "[HDMI] Downsampling 2 channel to %lu Hz\n", 
				pAudioFormat->SampleFrequency / pAudioFormat->SRC_Factor);
		}
	}
	
	pDH->HDMI_audio = (pAudioFormat->I2S0_Enable | pAudioFormat->I2S1_Enable | pAudioFormat->I2S2_Enable | pAudioFormat->I2S3_Enable);
	if (pAudioFormat->SPDIF_Enable) {
		if (pDH->HDMI_audio) {
			if (! manutest) fprintf(stderr, "[HDMI] Can not send I2S and SPDIF at the same time!\n");
			return RM_ERROR;
		}
		if (pAudioFormat->SampleFrequency > 96000) {
			if (! manutest) fprintf(stderr, "[HDMI] SiI9030 can not accept SPDIF Audio with more than 96kHz!\n");
			return RM_ERROR;
		}
		RMDBGLOG((LOCALDBG, "Sending audio over S/P-DIF to HDMI\n"));
		pDH->HDMI_audio = TRUE;
	} else {
		if (pAudioFormat->SampleFrequency > 192000) {
			if (! manutest) fprintf(stderr, "[HDMI] SiI9030 can not accept I2S Audio with more than 192kHz!\n");
			return RM_ERROR;
		}
		RMDBGLOG((LOCALDBG, "Sending PCM audio over I2S to HDMI\n"));
	}
	
	mclk_f = pAudioFormat->MClkFrequency / pAudioFormat->SampleFrequency;
#if ((EM86XX_CHIP==EM86XX_CHIPID_TANGO2) && (EM86XX_REVISION==8))
	mclk_f = 128;  // ES8/RevC only supports 128
#endif
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		switch (mclk_f) {
			case 128:  freq_sval = 0x00; break;
			case 384:  freq_sval = 0x02; break;
			case 512:  freq_sval = 0x03; break;
			case 768:  freq_sval = 0x04; break;
			case 1024: freq_sval = 0x05; break;
			case 1152: freq_sval = 0x06; break;
			case 192:  freq_sval = 0x07; break;
			default:  // default: 256
				mclk_f = 256;
				pAudioFormat->MClkFrequency = pAudioFormat->SampleFrequency * mclk_f;
			case 256:
				freq_sval = 0x01;
				break;
		}
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x02, freq_sval);
		if ((mclk_f >= 512) && (pAudioFormat->SampleFrequency > 96000)) {
			if (! manutest) fprintf(stderr, "[HDMI] SiI9x3x can not accept Audio at MClk factor %luXfs with more than 96kHz! Max MClk is 384Xfs\n", mclk_f);
			return RM_ERROR;
		}
		if ((mclk_f >= 1024) && (pAudioFormat->SampleFrequency > 48000)) {
			if (! manutest) fprintf(stderr, "[HDMI] SiI9x3x can not accept Audio at MClk factor %luXfs with more than 48kHz! Max MClk is 768Xfs\n", mclk_f);
			return RM_ERROR;
		}
		break;
	case DH_ANX9030:
		//TODOANX
		break;
	default:
		break;
	}
	RMDBGLOG((LOCALDBG, "Audio MClk is %luXfs\n", mclk_f));
	
	pDH->audio_mode = 
		(pAudioFormat->I2S0_Enable ? 0x10 : 0) | 
		(pAudioFormat->I2S1_Enable ? 0x20 : 0) | 
		(pAudioFormat->I2S2_Enable ? 0x40 : 0) | 
		(pAudioFormat->I2S3_Enable ? 0x80 : 0) | 
		(pAudioFormat->SPDIF_Enable ? 0x02 : 0) | 
		((pDH->VideoPixelClock < 59000000) ? 0x04 : 0x00) | // use pixclk * 2.5 for spdif, if needed
		((pDH->HDMI_mode && pDH->HDMI_audio) ? 0x01 : 0);  // enable audio alltogether
	RMDBGLOG((ENABLE, "******************** Audio Mode Config: 0x%02X, %s\n", pDH->audio_mode, (pDH->audio_mode & 0x01) ? "ENABLED" : "DISABLED"));
	
	// set up IEC-60958 channel status header (bit 0..39), for I2S only
	IEC60958channelstatus[0] = pDH->AudioHeader & 0xFF;  // bit 0..7
	IEC60958channelstatus[1] = (pDH->AudioHeader >> 8) & 0xFF;  // bit 8..15
	IEC60958channelstatus[2] = 0x00 | (pAudioFormat->NumChannel << 4);  // channels, src num
	IEC60958channelstatus[3] = 0x00 | fs;  // spdif fs, if fs_override is set
	IEC60958channelstatus[4] = 0x0B | (fs_orig << 4);  // 24 bit i2s
	if (! pAudioFormat->SPDIF_Enable) RMDBGLOG((LOCALDBG, "Sending IEC-60958 channel status with I2S stream: 0x%02X.%02X.%02X.%02X.%02X\n", 
		IEC60958channelstatus[4], IEC60958channelstatus[3], IEC60958channelstatus[2], IEC60958channelstatus[1], IEC60958channelstatus[0]));
	
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x14, pDH->audio_mode & 0xFE);
#ifdef HDMI_CLEAR_CHSTAT
		if ((pDH->part == DH_siI9030) && (pDH->i2c_module == 2)) {  // audio drop-out workaround: cleared channel status does not get corrupted
			RMMemset(IEC60958channelstatus, 0, sizeof(IEC60958channelstatus));
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x15, 0x02);  // override s/p-dif sample freq
		} else 
#endif
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x15, 0x00);  // don't override s/p-dif sample freq
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x19, 0x09 | (((pDH->audio_mode & 0xF0) && (pAudioFormat->NumChannel > 2)) ? 0x20 : 0x00));  // swap l/r on i2s 1 (C<->LFE)
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x1C, (pAudioFormat->NumChannel == 1) ? 0xE5 : 0xE4);  // FIFO mapping: 2234 or 1234
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x1D, 0x40);  // MSB first left-aligned samples, 1 bit shift
		
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx2), 0x1E, IEC60958channelstatus, 5);
		if (RMFAILED(err)) RMDBGLOG((LOCALDBG, "Failed to write channel status!\n"));
		
		// Sample rate conversion of 2Ch. Audio
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x23, src);  // 0: orig sample rate, 1: half sample rate, 3: quarter sample rate
		
		// Set audio packet header layout
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x2F, &reg);
		if (RMFAILED(err)) reg = 0x01;  // Assume HDMI mode is on
		RMinsShiftBool(&reg, (pDH->audio_mode & 0xF0) && (pAudioFormat->NumChannel > 2), 1);  // audio packet header format
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x2F, reg);
		RMDBGLOG((LOCALDBG, "Audio packet header: %uch\n", (reg & 0x02) ? 8 : 2));
		
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx2), 0x14, pDH->audio_mode);
		break;
	case DH_ANX9030:  // TODOANX
		
		err = DH_i2c_write_data(pDH->pRUA, &(pDH->i2c_tx), 0x56, IEC60958channelstatus, 5);
		if (RMFAILED(err)) RMDBGLOG((LOCALDBG, "Failed to write channel status!\n"));
		
		break;
	default:
		err = RM_ERROR;
	}
	
	return RM_OK;
}

static RMstatus DHUpdateVideoPixelClock(
	struct DH_control *pDH, 
	RMuint32 PixelClock, 
	RMbool wait_clock)
{
	RMstatus err;
	RMuint8 reg;
	RMuint64 t0, t1;
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHUpdateVideoPixelClock: Error, chipset does not support audio formatting\n"));
		return RM_OK;
	}
	
	RMDBGLOG((LOCALDBG,"DHUpdateVideoPixelClock(PCLK = %ld, %s)\n",PixelClock,wait_clock?"TRUE":"FALSE")) ;
	
	if (! manutest) fprintf(stderr, "[HDMI] DHUpdateVideoPixelClock(%ld)\n", PixelClock);
	pDH->VideoPixelClock = PixelClock;
	
	// set TMDS GPIO, if any
	DHSetTMDSResistor(pDH);
	
	// Video mode might have changed, force authentication on next Integrity check
	DHMuteOutput(pDH, TRUE);
	DHDisableEncryption(pDH);
	
	if (wait_clock) {
		// wait until IDCK is stable
		t0 = RMGetTimeInMicroSeconds();
		do {
			switch (pDH->part) {
			case DH_siI164:
			case DH_siI170:
			case DH_siI9030:
			case DH_siI9034:
			case DH_siI9134:
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x09, &reg);
				break;
			case DH_ANX9030:
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x10, &reg);
				reg >>= 5;
				break;
            // 2007/03/23 Added by jj_tseng@chipadvanced
			case DH_CAT6611:
			    err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x04, &reg) ;
			    reg &= ~0x28 ;
			    err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x04, reg) ;
			    
				err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x0E, &reg);
				reg = (reg & 0x40)?1:0 ;
				break;
            //~jj_tseng@chipadvanced.com 2007/03/23
			default:
				err = RM_ERROR;
			}
			t1 = RMGetTimeInMicroSeconds();
			if (RMCyclesElapsed64(t0, t1) > 1000000) {  // 1 sec
				RMDBGLOG((ENABLE, "Timeout waiting for stable Video Pixel Clock!\n"));
				err = RM_TIMEOUT;
			}
		} while (RMSUCCEEDED(err) && ((reg & 0x01) == 0x00));
		RMDBGLOG((LOCALDBG, "Waited %lu mSec for stable video clock.\n", RMCyclesElapsed64(t0, t1) / 1000));
		
		// soft-reset on new pixel clock
		RMDBGLOG((LOCALDBG, "Performing SoftReset of HDMI chip\n"));
		DHSoftReset(pDH);
		
		// unmute video unless HDCP requested
		if (! pDH->CPDesired) {
			DHMuteOutput(pDH, FALSE);
		}
	} else {
		RMDBGLOG((LOCALDBG, "Clock might have been changed, deferring TMDS enable.\n"));
		pDH->GuardTMDS = TRUE;  // don't turn on TMDS until after SoftReset
		pDH->HDMIState.InputClockStable = FALSE;  // force soft reset later on
	}
	
	return RM_OK;
}

RMstatus DHSetVideoPixelClock(
	struct DH_control *pDH, 
	RMuint32 PixelClock)
{
#ifndef HDMI_CLEAR_CHSTAT
	RMstatus err;
	RMuint8 reg;
#endif
	
	CHECK_pDH("DHSetVideoPixelClock");

	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHSetVideoPixelClock: Chipset does not support audio formatting\n"));
		return RM_OK;
	}
	
	RMDBGLOG((LOCALDBG, "DHSetVideoPixelClock(%ld)\n", PixelClock));
	
	DHUpdateVideoPixelClock(pDH, PixelClock, ! pDH->CheckClock);
	
#ifndef HDMI_CLEAR_CHSTAT
	// get current audio sample clock from HDMI chip
	switch (pDH->part) {
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx2), 0x22, &reg);
		break;
	case DH_ANX9030:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x5A, &reg);
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
        DH_Switch_6611BANK(pDH, 1) ;
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x99, &reg);
        DH_Switch_6611BANK(pDH, 0) ;
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		err = RM_ERROR;
	}
	if (RMSUCCEEDED(err)) switch (15 - ((reg >> 4) & 0x0F)) {
		case 0x00: pDH->AudioSampleClock =  44100; break;
		case 0x02: pDH->AudioSampleClock =  48000; break;
		case 0x03: pDH->AudioSampleClock =  32000; break;
		case 0x04: pDH->AudioSampleClock =  22050; break;
		case 0x06: pDH->AudioSampleClock =  24000; break;
		case 0x08: pDH->AudioSampleClock =  88200; break;
		case 0x09: pDH->AudioSampleClock = 768000; break;
		case 0x0A: pDH->AudioSampleClock =  96000; break;
		case 0x0C: pDH->AudioSampleClock = 176400; break;
		case 0x0E: pDH->AudioSampleClock = 192000; break;
		default: break;
	}
#endif
	
	return (pDH->AudioSampleClock) ? DHSetAudioClock(pDH, pDH->VideoPixelClock, pDH->AudioSampleClock) : RM_OK;
}

RMstatus DHSetVideoSyncPolarity(
	struct DH_control *pDH, 
	RMbool VSyncPolarity, 
	RMbool HSyncPolarity)
{
	RMstatus err;
	RMuint8 reg;
	
	CHECK_pDH("DHSetVideoSyncPolarity");
	
	RMDBGLOG((LOCALDBG, "DHSetVideoSyncPolarity(Vsync=%s, HSync=%s)\n", VSyncPolarity ? "HIGH" : "LOW", HSyncPolarity ? "HIGH" : "LOW"));
	
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x33, &reg);
		if (RMFAILED(err)) {
			RMuint32 i;
			reg = 0x30;
			for (i = 0; i < sizeof(init_SiI9030) / sizeof(init_SiI9030[0]); i++) {
				if (init_SiI9030[i][0] == 0x33) reg = init_SiI9030[i][1];
			}
		}
		RMinsShiftBool(&reg, ! VSyncPolarity, 5);
		RMinsShiftBool(&reg, ! HSyncPolarity, 4);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x33, reg);
		break;
	case DH_ANX9030:
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x14, &reg);
		if (RMFAILED(err)) {
			RMuint32 i;
			reg = 0x00;
			for (i = 0; i < sizeof(init_ANX9030) / sizeof(init_ANX9030[0]); i++) {
				if (init_ANX9030[i][0] == 0x14) reg = init_ANX9030[i][1];
			}
		}
		RMinsShiftBool(&reg, VSyncPolarity, 6);
		RMinsShiftBool(&reg, HSyncPolarity, 5);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x14, reg);
		break;
    case DH_CAT6611:
        err = RM_OK ; 
        // CAT6611 do not support modifying input sync polarity.
        break ;		
	default:
		err = RM_ERROR;
	}
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Failed to set Sync polarity on DVI/HDMI output, %s\n", RMstatusToString(err)));
	}
	
	return err;
}

RMstatus DHSetVideoPixelClockFromTVStandard(
	struct DH_control *pDH, 
	enum EMhwlibTVStandard standard)
{
	struct EMhwlibTVFormatDigital format_digital;
	
	CHECK_pDH("DHSetVideoPixelClockFromTVStandard");
	
	RUAExchangeProperty(pDH->pRUA, DisplayBlock, 
		RMDisplayBlockPropertyID_TVFormatDigital, 
		&standard, sizeof(standard), 
		&format_digital, sizeof(format_digital));
	
	return DHSetVideoPixelClockFromTVFormat(pDH, &format_digital);
}

RMstatus DHSetVideoPixelClockFromTVFormat(
	struct DH_control *pDH, 
	struct EMhwlibTVFormatDigital *format_digital)
{
	CHECK_pDH("DHSetVideoPixelClockFromTVFormat");
	CHECK_PTR("DHSetVideoPixelClockFromTVFormat", format_digital);
	
	DHSetVideoSyncPolarity(pDH, ! format_digital->VSyncActiveLow, ! format_digital->HSyncActiveLow);
	return DHSetVideoPixelClock(pDH, format_digital->PixelClock);
}

RMstatus DHSetAudioSampleClock(
	struct DH_control *pDH, 
	RMuint32 SampleClock)
{
	RMstatus err;
	RMuint32 PixelClock;
	
	CHECK_pDH("DHSetAudioSampleClock");
	
	if (! pDH->part_caps.HDMI) {
		RMDBGLOG((ENABLE, "Call to DHSetAudioSampleClock: Chipset does not support audio formatting\n"));
		return RM_OK;
	}
	
	RMDBGLOG((LOCALDBG, "DHSetAudioSampleClock(%ld)\n", SampleClock));
	pDH->AudioSampleClock = SampleClock;
	
	// query nominal pixel clock from digital output
	err = RUAGetProperty(pDH->pRUA, DispDigitalOut, 
		RMGenericPropertyID_PixelClock, 
		&PixelClock, sizeof(PixelClock));
	if (RMSUCCEEDED(err) && (pDH->VideoPixelClock != PixelClock)) {
		// new pixel clock, take appropriate action
		err = DHUpdateVideoPixelClock(pDH, PixelClock, FALSE);
		if (RMFAILED(err)) {
			pDH->VideoPixelClock = PixelClock;
		}
	}
	
	if (pDH->VideoPixelClock) {
		// same pixel clock, just set up new audio divider
		err = DHSetAudioClock(pDH, pDH->VideoPixelClock, pDH->AudioSampleClock);
	} else {
		// no known pixel clock, nothing to do yet.
		err = RM_OK;
	}
	
	return RM_OK;
}

RMstatus DHSetDEGeneratorFromTVStandard(
	struct DH_control *pDH, 
	RMbool EnableDE, 
	RMbool Enable656, 
	RMbool EnableDemux, 
	enum EMhwlibTVStandard standard)
{
	struct EMhwlibTVFormatDigital format_digital;
	
	CHECK_pDH("DHSetDEGeneratorFromTVStandard");
	
	RUAExchangeProperty(pDH->pRUA, DisplayBlock, 
		RMDisplayBlockPropertyID_TVFormatDigital, 
		&standard, sizeof(standard), 
		&format_digital, sizeof(format_digital));
	
	return DHSetDEGeneratorFromTVFormat(pDH, EnableDE, Enable656, EnableDemux, &format_digital);
}

// 2007/03/23 Added by jj_tseng@chipadvanced
//~jj_tseng@chipadvanced.com 2007/03/23

RMstatus DHSetDEGeneratorFromTVFormat(
	struct DH_control *pDH, 
	RMbool EnableDE, 
	RMbool Enable656, 
	RMbool EnableDemux, 
	struct EMhwlibTVFormatDigital *format_digital)
{
	//RMstatus err;
	RMstatus err = RM_OK;
	RMuint8 reg;
	
	CHECK_pDH("DHSetDEGeneratorFromTVFormat");
	CHECK_PTR("DHSetDEGeneratorFromTVFormat", format_digital);
	
	RMDBGLOG((LOCALDBG,"DHSetDEGeneratorFromTVFormat()\n")) ;
	switch (pDH->part) {
	case DH_siI164:
	case DH_siI170:
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		// Set up 601/656 and 8/16 bit
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x4A, &reg);
		if (RMFAILED(err)) {
			RMuint32 i;
			reg = 0x00;
			for (i = 0; i < sizeof(init_SiI9030) / sizeof(init_SiI9030[0]); i++) {
				if (init_SiI9030[i][0] == 0x4A) reg = init_SiI9030[i][1];
			}
		}
		RMinsShiftBool(&reg, Enable656, 0);
		RMinsShiftBool(&reg, EnableDemux, 1);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x4A, reg);
		
		// Disable DE while programming registers
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x33, &reg);
		if (RMFAILED(err)) {
			RMuint32 i;
			reg = 0x30;
			for (i = 0; i < sizeof(init_SiI9030) / sizeof(init_SiI9030[0]); i++) {
				if (init_SiI9030[i][0] == 0x33) reg = init_SiI9030[i][1];
			}
		}
		RMinsShiftBool(&reg, FALSE, 6);
		RMinsShiftBool(&reg, format_digital->VSyncActiveLow, 5);
		RMinsShiftBool(&reg, format_digital->HSyncActiveLow, 4);
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x33, reg);
		
		// Set up DE and 656 values from video mode
		if (EnableDE) {
			RMuint32 hfp, vfp;
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x32, format_digital->XOffset & 0xFF);
			RMinsShiftBits(&reg, format_digital->XOffset >> 8, 2, 0);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x34, format_digital->YOffsetTop);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x36, format_digital->ActiveWidth & 0xFF);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x37, format_digital->ActiveWidth >> 8);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x38, format_digital->ActiveHeight & 0xFF);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x39, format_digital->ActiveHeight >> 8);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x3A, format_digital->HTotalSize & 0xFF);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x3B, format_digital->HTotalSize >> 8);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x3C, format_digital->VTotalSize & 0xFF);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x3D, format_digital->VTotalSize >> 8);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x40, (format_digital->HTotalSize / 2) & 0xFF);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x41, (format_digital->HTotalSize / 2) >> 8);
			hfp = format_digital->HTotalSize - format_digital->ActiveWidth - format_digital->XOffset;
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x42, hfp & 0xFF);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x43, hfp >> 8);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x44, format_digital->HSyncWidth & 0xFF);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x45, format_digital->HSyncWidth >> 8);
			vfp = format_digital->VTotalSize;
			if (! format_digital->Progressive) vfp /= 2;
			vfp = vfp - format_digital->ActiveHeight - format_digital->YOffsetTop;
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x46, vfp);
			DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x47, (format_digital->VSyncWidth / 2));
			RMinsShiftBool(&reg, EnableDE, 6);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x33, reg);
		}
		break;
	case DH_ANX9030:
		// TODO
		err = DH_i2c_read(pDH->pRUA, &(pDH->i2c_tx), 0x14, &reg);
		if (RMFAILED(err)) {
			RMuint32 i;
			reg = 0x00;
			for (i = 0; i < sizeof(init_ANX9030) / sizeof(init_ANX9030[0]); i++) {
				if (init_ANX9030[i][0] == 0x14) reg = init_ANX9030[i][1];
			}
		}
		err = DH_i2c_write(pDH->pRUA, &(pDH->i2c_tx), 0x14, reg);
		break;
    case DH_CAT6611:
        // TO DO
        break ;		
	default:
		err = RM_ERROR;
	}
	if (RMFAILED(err)) {
		RMDBGLOG((ENABLE, "Failed to set DE Generatoron DVI/HDMI output, %s\n", RMstatusToString(err)));
	}
	
	return DHSetVideoPixelClock(pDH, format_digital->PixelClock);
}


RMstatus DHDone(struct DH_control *pDH)
{
	RMstatus err = RM_OK;
	
	CHECK_pDH("DHDone");
	
	if ( (pDH->state == DH_enabled) || 
	     (pDH->state == DH_enabled_authenticated) || 
	     (pDH->state == DH_enabled_encrypted) || 
	     (pDH->state == DH_enabled_repeater_wait) 
	) {
		RMDBGLOG((LOCALDBG, "Disabling DVI device\n"));
		DHDisableOutput(pDH);
	}
	
	RMFree(pDH);
	
	return err;
}

RMstatus DHGetChipInit(struct DH_control *pDH, RMuint32 *dvi_init, RMuint32 *dvi_size)
{
	RMstatus err = RM_OK;
	RMuint32 i;
	
	CHECK_pDH("DHGetChipInit");
	
	if (pDH->state == DH_uninitialized) {
		RMDBGLOG((LOCALDBG, "Call to DHGetChipInit with unitialize device!"));
		return RM_ERROR;
	}
	
	if (*dvi_size < 6 + (sizeof(init_SiI164) / sizeof(init_SiI164[0])) * 2) {  // required space plus checksum
		RMDBGLOG((LOCALDBG, "Call to DHGetChipInit with insufficient dvi_size!"));
		return RM_ERROR;
	}
	
	dvi_init[0] = (RMuint8)(pDH->i2c_tx.dev.Clock - GPIOId_Sys_0);
	dvi_init[1] = (RMuint8)(pDH->i2c_tx.dev.Data - GPIOId_Sys_0);
	dvi_init[2] = (pDH->i2c_tx.dev.Speed << 16) | (pDH->i2c_tx.dev.Delay & 0xFFFF);
	dvi_init[3] = pDH->i2c_tx.dev.DevAddr;
	dvi_init[4] = EMHWLIB_MODULE_INDEX(pDH->i2c_tx.I2C);
	
	switch(pDH->part) {
	case DH_siI164:
		for (i = 0; i < sizeof(init_SiI164) / sizeof(init_SiI164[0]); i++) {
			dvi_init[5 + i * 2] = init_SiI164[i][0];
			dvi_init[6 + i * 2] = init_SiI164[i][1];
			if (i == 0) dvi_init[6 + i * 2] |= 0x01;  // power up TMDS
		}
		*dvi_size = 5 + (sizeof(init_SiI164) / sizeof(init_SiI164[0])) * 2;
		break;
	case DH_siI170:
		for (i = 0; i < sizeof(init_SiI170) / sizeof(init_SiI170[0]); i++) {
			dvi_init[5 + i * 2] = init_SiI170[i][0];
			dvi_init[6 + i * 2] = init_SiI170[i][1];
			if (i == 0) dvi_init[6 + i * 2] |= 0x01;  // power up TMDS
		}
		*dvi_size = 5 + (sizeof(init_SiI170) / sizeof(init_SiI170[0])) * 2;
		break;
	case DH_siI9030:
	case DH_siI9034:
	case DH_siI9134:
		for (i = 0; i < sizeof(init_SiI9030) / sizeof(init_SiI9030[0]); i++) {
			dvi_init[5 + i * 2] = init_SiI9030[i][0];
			dvi_init[6 + i * 2] = init_SiI9030[i][1];
			if (i == 0) dvi_init[6 + i * 2] |= 0x01;  // power up TMDS
		}
		*dvi_size = 5 + (sizeof(init_SiI9030) / sizeof(init_SiI9030[0])) * 2;
		break;
	case DH_ANX9030:
		for (i = 0; i < sizeof(init_ANX9030) / sizeof(init_ANX9030[0]); i++) {
			dvi_init[5 + i * 2] = init_ANX9030[i][0];
			dvi_init[6 + i * 2] = init_ANX9030[i][1];
			if (i == 3) dvi_init[6 + i * 2] |= 0x01;  // power up TMDS
		}
		*dvi_size = 5 + (sizeof(init_ANX9030) / sizeof(init_ANX9030[0])) * 2;
		break;
    // 2007/03/23 Added by jj_tseng@chipadvanced
	case DH_CAT6611:
		for (i = 0; i < sizeof(init_CAT6611) / sizeof(init_CAT6611[0]); i++) {
			dvi_init[5 + i * 2] = init_CAT6611[i][0];
			dvi_init[6 + i * 2] = init_CAT6611[i][1];
			if (i == 3) dvi_init[6 + i * 2] |= 0x01;  // power up TMDS
		}
		*dvi_size = 5 + (sizeof(init_CAT6611) / sizeof(init_CAT6611[0])) * 2;
		break;
    //~jj_tseng@chipadvanced.com 2007/03/23
	default:
		RMDBGLOG((LOCALDBG, "Unknown DVI or HDMI part (%d)!!!\n", pDH->part));
		err = RM_ERROR;
	}
	
	return err;
}

RMstatus DHGetDviHdmiPart(struct DH_control *pDH, enum DH_vendor_parts *part)
{
	CHECK_pDH("DHGetDviHdmiPart");
	CHECK_PTR("DHGetDviHdmiPart", part);
	
	*part = pDH->part;
	return RM_OK;
}

static RMbool i2cdbgdigit(RMascii key, RMuint8 *data)
{
	if ((key >= '0') && (key <= '9')) {
		*data = (*data << 4) | (RMuint8)(key - '0');
	} else if ((key >= 'a') && (key <= 'f')) {
		*data = (*data << 4) | (RMuint8)(key - 'a' + 10);
	} else if ((key >= 'A') && (key <= 'F')) {
		*data = (*data << 4) | (RMuint8)(key - 'A' + 10);
	} else {
		return FALSE;
	}
	return TRUE;
}

// 2007/03/23 Added by jj_tseng@chipadvanced
static RMstatus DHDump_CAT6611reg(struct DH_control *pDH)
{
#if 0
    RMstatus i,j;
    RMuint8 data;

    RMDBGPRINT((LOCALDBG,"Dump Registers \n"));
    RMDBGPRINT((LOCALDBG,"     ")) ;
    for( j = 0 ; j < 4 ; j++ )
    {
        RMDBGPRINT((LOCALDBG," %02X",j)) ;
    }
    RMDBGPRINT((LOCALDBG,"  ")) ;

    for( ; j < 8 ; j++ )
    {
        RMDBGPRINT((LOCALDBG," %02X",j)) ;
    }
    RMDBGPRINT((LOCALDBG,"  ")) ;
    for( ; j < 12 ; j++ )
    {
        RMDBGPRINT((LOCALDBG," %02X",j)) ;
    }
    RMDBGPRINT((LOCALDBG,"  ")) ;

    for( ; j < 16 ; j++ )
    {
        RMDBGPRINT((LOCALDBG," %02X",j)) ;
    }
    RMDBGPRINT((LOCALDBG,"\n")) ;

    for( i = 0 ; i < 0x1A0 ; i+=0x10 )
    {
        RMDBGPRINT((LOCALDBG,"%3X  ", i)) ;
        
        DH_Switch_6611BANK(pDH,    (i/256)) ;
        for( j = 0 ; j < 4 ; j++ )
        {
            DH_i2c_read((pDH)->pRUA,&((pDH)->i2c_tx), i+j, &data ) ;
            RMDBGPRINT((LOCALDBG," %02X",data)) ;
        }
        RMDBGPRINT((LOCALDBG," -"));

        for( ; j < 8 ; j++ )
        {
            DH_i2c_read((pDH)->pRUA,&((pDH)->i2c_tx), i+j, &data ) ;
            RMDBGPRINT((LOCALDBG," %02X",data)) ;
        }
        RMDBGPRINT((LOCALDBG," -"));


        for( ; j < 12 ; j++ )
        {
            DH_i2c_read((pDH)->pRUA,&((pDH)->i2c_tx), i+j, &data ) ;
            RMDBGPRINT((LOCALDBG," %02X",data)) ;
        }

        RMDBGPRINT((LOCALDBG," -")) ;

        for( ; j < 16 ; j++ )
        {
            DH_i2c_read((pDH)->pRUA,&((pDH)->i2c_tx), i+j, &data ) ;
            RMDBGPRINT((LOCALDBG," %02X",data)) ;
        }
        RMDBGPRINT((LOCALDBG,"\n")) ;
    }
    DH_Switch_6611BANK(pDH,   0 ) ;
#endif // LOCALDBG

    return RM_OK;

}
//~jj_tseng@chipadvanced.com 2007/03/23

// pass key = '\0' to reset state machine
RMstatus DHDebugI2C(struct DH_control *pDH, RMascii trigger, RMascii key)
{
	RMstatus err = RM_OK;
	RMuint8 *item;
	enum DH_i2cdbgstate prev_state;
	RMuint8 data[256];
	RMuint32 i, n;
	
	CHECK_pDH("DHDebugI2C");
	
	prev_state = pDH->i2cdbgstate;
	
	if ((key == 'h') && (pDH->i2cdbgstate != DH_i2cdbgstate_idle)) {
		fprintf(stderr, "\n");
		pDH->i2cdbgstate = DH_i2cdbgstate_idle;
		key = trigger;
	}
	
	if (key == '\0') {
		pDH->i2cdbgstate = DH_i2cdbgstate_idle;
	} else switch (pDH->i2cdbgstate) {
	// start up and help text
	case DH_i2cdbgstate_idle:
		if (key == trigger) {
			fprintf(stderr, "I2C debugger: Using ModuleID %lu and Device 0x%02X\n", EMHWLIB_MODULE_INDEX(pDH->i2cdbg.I2C), pDH->i2cdbg.dev.DevAddr);
			fprintf(stderr, "  [r]ead, [w]rite, [b]lock read, [d]dc read, [e]did read, \n");
			fprintf(stderr, "  [i]2c device, [I]2C ModuleID, [D]ump all regs, [P]robe all devs, \n");
			fprintf(stderr, "  e[x]it, [h]elp\n");
			pDH->i2cdbgstate++;
		} else {
			// key was not handled
			err = RM_PENDING;
		}
		break;
	// parse command
	case DH_i2cdbgstate_dispatch:
		pDH->i2cdbgaddr = 0x00;
		pDH->i2cdbgsize = 0x00;
		pDH->i2cdbgdatasize = 0;
		pDH->i2cdbgdata = 0x00;
		switch (key) {
		case 'r':
			fprintf(stderr, " Read addr:0x");
			pDH->i2cdbgstate = DH_i2cdbgstate_read_addr_hi;
			break;
		case 'w':
			fprintf(stderr, " Write addr:0x");
			pDH->i2cdbgstate = DH_i2cdbgstate_write_addr_hi;
			break;
		case 'b':
			fprintf(stderr, " Block Read addr:0x");
			pDH->i2cdbgstate = DH_i2cdbgstate_blockread_addr_hi;
			break;
		case 'd':
			fprintf(stderr, " DDC Read addr:0x");
			pDH->i2cdbgstate = DH_i2cdbgstate_ddcread_addr_hi;
			break;
		case 'e':
			fprintf(stderr, " EDID Read block:0x");
			pDH->i2cdbgstate = DH_i2cdbgstate_edidread_block_hi;
			break;
		case 'i':
			fprintf(stderr, " I2C Device 0x");
			pDH->i2cdbgstate = DH_i2cdbgstate_device_hi;
			break;
		case 'I':
			fprintf(stderr, " I2C ModuleID ");
			pDH->i2cdbgstate = DH_i2cdbgstate_module;
			break;
		case 'D':
			fprintf(stderr, " Dump:\n");
			DH_dump_i2c(pDH);
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
			break;
		case 'P':
			fprintf(stderr, " Probe:\n");
			DH_probe_i2c(pDH, &(pDH->i2cdbg));
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
			break;
		case 'B':  // hidden option: Burst write test
			fprintf(stderr, " Testing burst write (9x3x only):\n");
			DH_burst_verify_i2c(pDH);
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
			break;
		case 'q':
		case 'Q':
		case 'x':
		case 'X':
			fprintf(stderr, " Bye!\n");
			pDH->i2cdbgstate = DH_i2cdbgstate_idle;
			break;
		default:
			fprintf(stderr, "\n");
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
		}
		break;
	// read high nibble and advance
	case DH_i2cdbgstate_read_addr_hi:
	case DH_i2cdbgstate_write_addr_hi:
	case DH_i2cdbgstate_blockread_addr_hi:
	case DH_i2cdbgstate_ddcread_addr_hi:
	case DH_i2cdbgstate_device_hi:
	case DH_i2cdbgstate_edidread_block_hi:
		item = &(pDH->i2cdbgaddr);
		if (0) 
	case DH_i2cdbgstate_blockread_size_hi:
	case DH_i2cdbgstate_ddcread_size_hi:
	case DH_i2cdbgstate_edidread_size_hi:
		item = &(pDH->i2cdbgsize);
		if (0) 
	case DH_i2cdbgstate_write_data_hi:
		item = &(pDH->i2cdbgdata);
		if (i2cdbgdigit(key, item)) {
			fprintf(stderr, "%c", key);
			pDH->i2cdbgstate++;
		}
		break;
	// read low nibble and advance or perform
	case DH_i2cdbgstate_read_addr_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgaddr))) {
			fprintf(stderr, "%c = ", key);
			err = DH_i2c_read(pDH->pRUA, &(pDH->i2cdbg), pDH->i2cdbgaddr, &(pDH->i2cdbgdata));
			if (RMFAILED(err)) {
				fprintf(stderr, "\nError reading from 0x%02X:0x%02X: %s\n", 
					pDH->i2cdbg.dev.DevAddr, pDH->i2cdbgaddr, RMstatusToString(err));
				err = RM_OK;
			}
			fprintf(stderr, "0x%02X\n", pDH->i2cdbgdata);
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
		}
		break;
	case DH_i2cdbgstate_blockread_addr_lo:
	case DH_i2cdbgstate_ddcread_addr_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgaddr))) {
			fprintf(stderr, "%c size:0x", key);
			pDH->i2cdbgstate++;
		}
		break;
	case DH_i2cdbgstate_blockread_size_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgsize))) {
			fprintf(stderr, "%c =\n", key);
			pDH->i2cdbgdatasize = pDH->i2cdbgsize;
			if (pDH->i2cdbgdatasize == 0xFF) pDH->i2cdbgdatasize = 0x100;
			if (pDH->i2cdbgdatasize > (RMuint32)(0x100 - pDH->i2cdbgaddr)) pDH->i2cdbgdatasize = 0x100 - pDH->i2cdbgaddr;
			err = DH_i2c_read_data(pDH->pRUA, &(pDH->i2cdbg), pDH->i2cdbgaddr, data, pDH->i2cdbgdatasize);
			if (RMFAILED(err)) {
				fprintf(stderr, "Error reading %lu bytes from 0x%02X:0x%02X: %s\n", 
					pDH->i2cdbgdatasize, pDH->i2cdbg.dev.DevAddr, pDH->i2cdbgaddr, RMstatusToString(err));
				err = RM_OK;
				pDH->i2cdbgdatasize = 0;
			}
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
		}
		break;
	case DH_i2cdbgstate_ddcread_size_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgsize))) {
			fprintf(stderr, "%c =\n", key);
			pDH->i2cdbgdatasize = pDH->i2cdbgsize;
			if (pDH->i2cdbgdatasize == 0xFF) pDH->i2cdbgdatasize = 0x100;
			if (pDH->i2cdbgdatasize > (RMuint32)(0x100 - pDH->i2cdbgaddr)) pDH->i2cdbgdatasize = 0x100 - pDH->i2cdbgaddr;
			err = DHDDCBlockRead(pDH, pDH->i2cdbg.dev.DevAddr, pDH->i2cdbgaddr, data, pDH->i2cdbgdatasize);
			if (RMFAILED(err)) {
				fprintf(stderr, "Error reading %lu bytes from DDC channel at 0x%02X:0x%02X: %s\n", 
					pDH->i2cdbgdatasize, pDH->i2cdbg.dev.DevAddr, pDH->i2cdbgaddr, RMstatusToString(err));
				err = RM_OK;
				pDH->i2cdbgdatasize = 0;
			}
			
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
		}
		break;
	case DH_i2cdbgstate_edidread_block_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgaddr))) {
			fprintf(stderr, "%c num:0x", key);
			pDH->i2cdbgstate++;
		}
		break;
	case DH_i2cdbgstate_edidread_size_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgsize))) {
			fprintf(stderr, "%c =\n", key);
			for (n = 0; n < pDH->i2cdbgsize; n++) {
				err = DHDDCBlockReadSegment(pDH, DDC_EDID_DEV, DDC_EDID_SEG, (pDH->i2cdbgaddr + n) * EDID_SIZE, data, EDID_SIZE);
				if (RMSUCCEEDED(err)) {
					for (i = 0; i < EDID_SIZE; i++) {
						if (i % 8 == 0) fprintf(stderr, " EDID[%02lX:%02lX]: ", pDH->i2cdbgaddr + n, i);
						fprintf(stderr, "%02X ", data[i]);
						if (i % 8 == 7) fprintf(stderr, "\n");
					}
				} else {
					fprintf(stderr, "Error reading EDID block #%lu from DDC channel: %s\n", 
						pDH->i2cdbgaddr + n, RMstatusToString(err));
					err = RM_OK;
				}
			}
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
		}
		break;
	case DH_i2cdbgstate_write_addr_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgaddr))) {
			fprintf(stderr, "%c data:0x", key);
			pDH->i2cdbgstate++;
		}
		break;
	case DH_i2cdbgstate_write_data_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgdata))) {
			fprintf(stderr, "%c\n", key);
			err = DH_i2c_write(pDH->pRUA, &(pDH->i2cdbg), pDH->i2cdbgaddr, pDH->i2cdbgdata);
			if (RMFAILED(err)) {
				fprintf(stderr, "Error writing 0x%02X to 0x%02X:0x%02X: %s\n", 
					pDH->i2cdbgdata, pDH->i2cdbg.dev.DevAddr, pDH->i2cdbgaddr, RMstatusToString(err));
				err = RM_OK;
			}
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
		}
		break;
	case DH_i2cdbgstate_device_lo:
		if (i2cdbgdigit(key, &(pDH->i2cdbgaddr))) {
			fprintf(stderr, "%c\n", key);
			pDH->i2cdbg.dev.DevAddr = pDH->i2cdbgaddr;
			pi2c_prev_dev = NULL;
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
		}
		break;
	case DH_i2cdbgstate_module:
		if (i2cdbgdigit(key, &(pDH->i2cdbgaddr))) {
			fprintf(stderr, "%c\n", key);
			pDH->i2cdbg.I2C = EMHWLIB_MODULE(I2C, pDH->i2cdbgaddr);
			pi2c_prev_dev = NULL;
			pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
		}
		break;
		
	// code error
	default:
		fprintf(stderr, " Internal error, unknown state!\n");
		pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
	}
	
	// print block, if any
	if ((pDH->i2cdbgstate == DH_i2cdbgstate_dispatch) && pDH->i2cdbgdatasize) {
		for (i = 0; i < pDH->i2cdbgdatasize; i++) {
			if (i % 8 == 0) fprintf(stderr, " Data[%02lX]: ", i + pDH->i2cdbgaddr);
			fprintf(stderr, "%02X ", data[i]);
			if (i % 8 == 7) fprintf(stderr, "\n");
		}
		fprintf(stderr, "\n");
		pDH->i2cdbgdatasize = 0x00;
	}
	
	// error if no advance
	if ((pDH->i2cdbgstate > DH_i2cdbgstate_dispatch) && (pDH->i2cdbgstate == prev_state)) {
		fprintf(stderr, "<error>\n");
		pDH->i2cdbgstate = DH_i2cdbgstate_dispatch;
	}
	
	// prompt
	if (pDH->i2cdbgstate == DH_i2cdbgstate_dispatch) {
		fprintf(stderr, ">");
	}
	
	return err;
};

