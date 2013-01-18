/*****************************************
 Copyright © 2001-2004
 Sigma Designs, Inc. All Rights Reserved
 Proprietary and Confidential
 *****************************************/
/**
  @file   dvi_hdmi.h
  @brief  Enable DVI and HDMI (including HDCP and Audio)

  long description

  @author Jacques Mahe
  @date   2004-07-28
*/

#ifndef __DVI_HDMI_H__
#define __DVI_HDMI_H__

#include "../rmdef/rmdef.h"
#include "../rua/include/rua.h"
#include "../rua/include/rua_property.h"

struct DH_HDMI_state {
	RMbool HotPlugChanged;      // HotPlug state has changed
	RMbool HotPlugState;        // current HotPlug state
	RMbool ReceiverSignal;      // TRUE if Rx signal is supported
	RMbool ReceiverChanged;     // Rx detection has changed
	RMbool ReceiverState;       // current Rx detection state
	RMbool HDCPLost;            // HDCP encryption has been lost
	RMbool HDCPState;           // current HDCP encryption state
	RMbool InputClockChanged;   // Input Clock from EM8xxx to SiI chip has changed
	RMbool InputClockStable;    // current state of Input Clock from EM8xxx (stable or not)
	RMbool TMDSState;           // TRUE: TMDS is active, FALSE: TMDS is off
};

enum DH_EDID_select {
	DH_EDID_none = 0,   // Don't use EDID to detect video mode, leave mode as-is
	DH_EDID_auto,       // Try: 1. preferred (detailed) mode, 2. native mode, 3. first VIC in short descriptor list
	DH_EDID_preferred,  // use preferred mode, fail if not found
	DH_EDID_native,     // use native mode, fail if not found
	DH_EDID_force,      // use n-th mode from short descriptor list, n specified separately
	DH_EDID_match,      // find closest matching mode to specification (vfreq, hsize, vsize, intl)
	DH_EDID_mask        // select first VIC that matches the force and exclude masks
};

enum DH_vendor_parts {
	DH_no_chip = -3, // no transmitter chip, plain digital output
	DH_lvds = -2,    // LVDS TMDS part
	DH_auto_detect = -1, // Default value will cause "auto-detection" on Sigma reference designs

	DH_siI164 = 0,   // Silicon Image 164 (DVI) (Value follows the part_info[] array indexes)
	DH_siI170,       // Silicon Image 170 (DVI, HDCP)
	DH_siI9030,      // Silicon Image 9030 (HDMI 1.2)
// 2007/03/22 added by jj_tseng@chipadvanced.com
	DH_CAT6611,      // Chipadvanced Tech CAT6011 (HDMI 1.1)
//~jj_tseng@chipadvanced.com 2007/03/22
	DH_ANX9030,      // Analogix 9030 (HDMI 1.2)
	DH_siI9034,      // Silicon Image 9034 (HDMI 1.3)
	DH_siI9134,      // Silicon Image 9134 (HDMI 1.3, 36 bit video)
	DH_reserved      // All values from there are reserved
};

/* Link state */
enum DH_connection {
	DH_disconnected = 0, // H0: no Rx attached
	DH_connected,        // H1: read EDID
	DH_connection_DVI,   // H2: transmit DVI
	DH_connection_HDMI   // H3: transmit HDMI
};

/* Authentication state */
enum DH_device_state {
	DH_uninitialized,           // Device has been registered but not detected
	DH_disabled,                // Device is disabled (TDMS transmitter off)
	DH_enabled,                 // Device is enabled and not encrypted (device detected)
	DH_enabled_authenticated,   // Device is enabled and authenticated, but not encrypted (device detected)
	DH_enabled_encrypted,       // Device encryption (HDCP) is ON and device is enabled (device detected)
	DH_enabled_repeater_wait    // Repeater is authenticated, waiting for receiver READY
};


// Types for new status struct

enum DH_cable {
	DH_cable_connected,     // Hot-Plug signal active
	DH_cable_disconnected   // Hot-Plug signal not active
};

enum DH_receiver {
	DH_receiver_unkown,   // receiver state not supported or receiver off
	DH_receiver_active,   // receiver on
	DH_receiver_disabled  // receiver off
};

enum DH_transmitter {
	DH_transmitter_disabled,  // TMDS off
	DH_transmitter_enable     // TMDS on
};

enum DH_chipset {
	DH_chipset_none,     // no transmitter chip available
	DH_chipset_DVI,      // DVI chip without HDCP
	DH_chipset_DVIHDCP,  // DVI chip with HDCP
	DH_chipset_HDMI      // HDMI chip with HDCP
};

enum DH_HDMI_Mute {
	DH_HDMI_Muted, 
	DH_HDMI_Unmuted
};

enum DH_HDCP {
	DH_HDCP_disabled, 
	DH_HDCP_authenticated, 
	DH_HDCP_repeater_wait, 
	DH_HDCP_enabled
};

struct DH_state {
	enum DH_cable cable;
	enum DH_receiver receiver;
	enum DH_transmitter transmitter;
	enum DH_chipset chipset;
	enum DH_HDMI_Mute HDMI_Mute;
	enum DH_HDCP HDCP;
};


struct EDID_Data {
	RMuint8 EDID_Header[8];
	RMuint8 EDID_ManufacturerName[2];
	RMuint8 EDID_ProductCode[2];
	RMuint8 EDID_SerialNumber[4];
	RMuint8 EDID_ManufactureWeek[1];
	RMuint8 EDID_ManufactureYear[1];
	RMuint8 EDID_Version;
	RMuint8 EDID_Revision;
	RMuint8 EDID_BasicData[5];
	RMuint8 EDID_Phos_Filter[10];
	RMuint8 EDID_Estab_Timing[3];
	RMuint8 EDID_Stand_Timing_mode0[8];
	RMuint8 EDID_Stand_Timing_mode1[8];
	RMuint8 EDID_Descriptor1[18];
	RMuint8 EDID_Descriptor2[18];
	RMuint8 EDID_Descriptor3[18];
	RMuint8 EDID_Descriptor4[18];
	RMuint8 EDID_Extension;
	RMuint8 EDID_Checksum;
};

#define MAX_VIDEO_DESCRIPTORS 64
#define MAX_AUDIO_DESCRIPTORS 32

enum DH_AudioCodingType {
	DH_AudioCodingType_FromStreamHeader = 0, 
	DH_AudioCodingType_PCM = 1, 
	DH_AudioCodingType_AC3 = 2, 
	DH_AudioCodingType_MPEG1_12 = 3, 
	DH_AudioCodingType_MPEG1_3 = 4, 
	DH_AudioCodingType_MPEG2 = 5, 
	DH_AudioCodingType_AAC = 6, 
	DH_AudioCodingType_DTS = 7, 
	DH_AudioCodingType_ATRAC = 8, 
	DH_AudioCodingType_OneBit = 9, 
	DH_AudioCodingType_DDPlus = 10, 
	DH_AudioCodingType_DTSHD = 11, 
	DH_AudioCodingType_MLP = 12
};

#define TV_SUPPORT_AUDIO_FREQ_32000  0x01
#define TV_SUPPORT_AUDIO_FREQ_44100  0x02
#define TV_SUPPORT_AUDIO_FREQ_48000  0x04
#define TV_SUPPORT_AUDIO_FREQ_88200  0x08
#define TV_SUPPORT_AUDIO_FREQ_96000  0x10
#define TV_SUPPORT_AUDIO_FREQ_176400 0x20
#define TV_SUPPORT_AUDIO_FREQ_192000 0x40

#define TV_SUPPORT_AUDIO_PCM_16BIT 0x01
#define TV_SUPPORT_AUDIO_PCM_20BIT 0x02
#define TV_SUPPORT_AUDIO_PCM_24BIT 0x03

struct CEAShortAudioDescriptor {
	enum DH_AudioCodingType AudioFormatCode; // See CEA-861B Table 35 page 84
	RMuint8 MaxNumberOfChannels;
	RMuint8 FrequencyMask;       // See CEA-861B Table 33 page 83 and TV_SUPPORT_AUDIO_FREQ_xxxx above
	union {
		RMuint8 BitMask;  // supported sample sizes for codec 1, see TV_SUPPORT_AUDIO_PCM_XXX above
		RMuint32 MaximumBitrate; // max_bitrate / 8 kHz for codecs 2..8
		RMuint8 Option;  // optional parameter for codecs 9..15
	} u;
};

// Bit Mask for BasicTVSupport member
#define TV_SUPPORT_YUV422      0x10
#define TV_SUPPORT_YUV444      0x20
#define TV_SUPPORT_BASIC_AUDIO 0x40
#define TV_SUPPORT_UNDERSCAN   0x80

// Bit Mask for ColorimetrySupport member
#define TV_SUPPORT_COLORIMETRY_XVYCC601 0x01
#define TV_SUPPORT_COLORIMETRY_XVYCC709 0x02

// Bit Mask for VideoCapability member
#define TV_SUPPORT_VIDEO_CE_OVERSCAN  0x01
#define TV_SUPPORT_VIDEO_CE_UNDERSCAN 0x02
#define TV_SUPPORT_VIDEO_IT_OVERSCAN  0x04
#define TV_SUPPORT_VIDEO_IT_UNDERSCAN 0x08
#define TV_SUPPORT_VIDEO_PT_OVERSCAN  0x10
#define TV_SUPPORT_VIDEO_PT_UNDERSCAN 0x20
#define TV_SUPPORT_VIDEO_QUANTISATION 0x40  // supports selection of RGB quantisation range with Q bit in AVI info frame

// Bit Mask for SinkCapability member
#define SINK_SUPPORT_DVI_DUAL 0x01  // Sink supports dual link DVI connection
#define SINK_SUPPORT_DEEP_COLOR_444 0x08  // Sink supports deep color in 4:4:4 YCbCr, in addition to RGB
#define SINK_SUPPORT_DEEP_COLOR_30 0x10  // Sink supports 30 bits/pixel (10 bit color components)
#define SINK_SUPPORT_DEEP_COLOR_36 0x20  // Sink supports 36 bits/pixel (12 bit color components)
#define SINK_SUPPORT_DEEP_COLOR_48 0x40  // Sink supports 48 bits/pixel (16 bit color components)
#define SINK_SUPPORT_AI 0x80  // Sink supports ACP, ISRC1, ISRC2

// Bit Mask for LatencyPresent member
#define SINK_LATENCY_PRESENT 0x80  // Sink provides data in VideoLatency and AudioLatency
#define SINK_LATENCY_INTERLACED_PRESENT 0x40  // Sink provides data in InterlacedVideoLatency and InterlacedAudioLatency (VideoLatency and AudioLatency for Progressive)

struct CEA861BDataBlockCollection {
	RMuint8 BasicTVSupport; // As defined in Table 28, page 79 of EIA/CEA-861-B, see TV_SUPPORT_... above
	RMuint8 NbShortVideoDescriptors;
	RMuint8 ShortVideoDescriptors[MAX_VIDEO_DESCRIPTORS];
	RMuint8 NbShortAudioDescriptors; // See "enum DH_AudioCodingType" below
	struct CEAShortAudioDescriptor ShortAudioDescriptors[MAX_AUDIO_DESCRIPTORS];
	RMuint8 SpeakerConfigurationCode;
	RMuint32 IEEERegistrationIdentifier;  // unused
	RMbool HDMI_sink;  // TRUE if display is HDMI and capable to receive info frames
	RMuint16 HDMI_PhysAddr;  // valid if HDMI_sink, physical position in DDC chain. See HDMI 1.3, sect. 8.7
	RMbool Supports_AI;  // Whether sink support ACP, ISRC1, ISRC2 or not
	RMuint8 SinkCapability;  // See SINK_SUPPORT_... and HDMI 1.3 sect. 8.3.2
	RMuint8 MaxTMDSClock;  // 0: not indicated, 1..255: TMDS supports clocks up to MaxTMDSClock * 5 MHz
	RMuint8 LatencyPresent;  // See SINK_LATENCY_... and HDMI 1.3 sect. 8.3.2
	RMuint8 VideoLatency;  // Video latency introduced by the sink, 0: unknown, 255: video not supported, 1..251: (VideoLatency - 1) * 2 mSec (for progressive video only, when interlaced values are provided)
	RMuint8 AudioLatency;  // Audio latency introduced by the sink, 0: unknown, 255: audio not supported, 1..251: (AudioLatency - 1) * 2 mSec ( " " )
	RMuint8 InterlacedVideoLatency;  // Video latency introduced by the sink, 0: unknown, 255: video not supported, 1..251: (InterlacedVideoLatency - 1) * 2 mSec
	RMuint8 InterlacedAudioLatency;  // Audio latency introduced by the sink, 0: unknown, 255: audio not supported, 1..251: (InterlacedAudioLatency - 1) * 2 mSec
	RMuint8 ColorimetrySupport;  // See TV_SUPPORT_COLORIMETRY_... and CEA 861-D 7.5.5
	RMuint8 MetaDataProfile;
	RMuint8 VideoCapability;  // See #define TV_SUPPORT_VIDEO_... and CEA 861-D 7.5.6
	RMbool BasicTVValid;  // BasicTVSupport is valid
	RMbool SpeakerValid;  // SpeakerConfigurationCode is valid
	RMbool CEASinkValid;  // Supports_AI, SinkCapability are valid
	RMbool CEATMDSValid;  // MaxTMDSClock is valid
	RMbool CEALatencyValid;  // LatencyPresent, VideoLatency, AudioLatency, InterlacedVideoLatency, InterlacedAudioLatency are valid
	RMbool ColorimetryValid;  // ColorimetrySupport, MetaDataProfile are valid
	RMbool VideoValid;  // VideoCapability is present
};

struct CEADetailedTimingDescriptor {
	RMuint32 PixelClock;
	RMuint16 NbHorizActivePixels;
	RMuint16 NbHorizBlankingPixels;
	RMuint16 NbVertActiveLines;
	RMuint16 NbVertBlankingLines;
	RMuint16 HorizSyncOffset;
	RMuint16 HorizSyncPulseWidth;
	RMuint16 VertSyncOffset;
	RMuint16 VertSyncPulseWidth;
	RMuint16 HorizImageSize_mm;
	RMuint16 VertImageSize_mm;
	RMuint8 HorizBorder;
	RMuint8 VertBorder;
	RMbool Interlaced;
	RMuint8 StereoModeDecode;       // See E-EDID Standard Release A, Revision 1 (VESA) Table 3.17 page 19
	RMuint8 SyncSignalDescription;  // See E-EDID Standard Release A, Revision 1 (VESA) Table 3.18 page 19
	RMuint8 SyncSignalInfo;
};

struct CEA861InfoFrame {
	RMuint8 HeaderByte[3];
	RMuint8 DataByte[28];
};

// Location in the SiI9030 I2C address space
// Will be internally adapted for other chips
enum DH_InfoFrameOffset {
	DH_InfoFrameOffset_AVI = 0x40,  // max data len: 16
	DH_InfoFrameOffset_SPD = 0x60, 
	DH_InfoFrameOffset_Audio = 0x80,  // max data len: 11
	DH_InfoFrameOffset_MPEG = 0xA0, 
	DH_InfoFrameOffset_Generic = 0xC0, 
	DH_InfoFrameOffset_Generic2 = 0xE0, 
};

enum DH_AVI_info_bars {                 // See CEA-861B Table 8 page 57
	DH_bar_data_not_valid = 0,      // Values are kept according to spec
	DH_vert_bar_info_valid = 1,
	DH_horiz_bar_info_valid = 2,
	DH_vert_and_horiz_bar_info_valid = 3
};

enum DH_scan_info {                     // See CEA-861B Table 8 page 57
	DH_scan_undefined = 0,
	DH_overscanned = 1,             // (television)
	DH_underscanned = 2             // (computer)
};

enum DH_picture_aspect_ratio {          // See CEA-861B Table 8 page 57
	DH_ar_no_info = 0,
	DH_ar_4x3 = 1,
	DH_ar_16x9 = 2
};

enum DH_active_format_aspect_ratio {    // See CEA-861C Table 83 page 144, annex H
	DH_af_reserved_0 = 0x0,             // sometimes improperly used for "no info"
	DH_af_reserved_1 = 0x1, 
	DH_af_16x9_top = 0x2,               // 16x9 content: at top of 4x3 frame, fills up 16x9 frame
	DH_af_14x9_top = 0x3,               // 14x9 content: at top of 4x3 frame, centered on 16x9 frame
	DH_af_64x27_centered = 0x4,         // Cinemascope widescreen (2.35x1, 64:27) content: centered on 4x3 or 16x9 frame
	DH_af_reserved_5 = 0x5, 
	DH_af_reserved_6 = 0x6, 
	DH_af_reserved_7 = 0x7, 
	DH_af_same_as_picture = 0x8,        // content fills up frame
	DH_af_4x3_centered = 0x9,           // 4x3 content: fills up 4x3 frame, centered on 16x9 frame
	DH_af_16x9_centered = 0xA,          // 16x9 content: centered on 4x3 frame, fills up 16x9 frame
	DH_af_14x9_centered = 0xB,          // 14x9 content: centered on 4x3 frame, centered on 16x9 frame
	DH_af_reserved_12 = 0xC, 
	DH_af_4x3_centered_prot_14x9 = 0xD, // 4x3 content with essential content in 14x9 centered portion
	DH_af_16x9_centered_prot_14x9 = 0xE,// 16x9 content with essential content in 14x9 centered portion
	DH_af_16x9_centered_prot_4x3 = 0xF  // 16x9 content with essential content in 4x3 centered portion
};

enum DH_pixel_repetition {
	DH_no_repetition = 0,
	DH_pixel_sent_twice = 1,
	DH_pixel_sent_3_times = 2,
	DH_pixel_sent_4_times = 3,
	DH_pixel_sent_5_times = 4,
	DH_pixel_sent_6_times = 5,
	DH_pixel_sent_7_times = 6,
	DH_pixel_sent_8_times = 7,
	DH_pixel_sent_9_times = 8,
	DH_pixel_sent_10_times = 9
};

struct DH_AVIInfoFrame {
	RMuint8 Version;                                        // 0x01 or 0x02
	enum EMhwlibColorSpace color_space;                     // RGB_0_255, YUV_601 or YUV_709
	enum EMhwlibSamplingMode sampling;                      // 4:4:4 or 4:2:2 only
	enum DH_scan_info scan_info;                            // Overscanned,  underscanned
	struct EMhwlibAspectRatio aspect_ratio;                 // 4x3 ot 16x9
	RMbool active_format_valid;                             // Active format present (DVB)
	enum DH_active_format_aspect_ratio active_aspect_ratio; // Active aspect ratio
	struct EMhwlibNonLinearScalingMode non_linear_scaling;
	enum DH_pixel_repetition pixel_repetition;              // Pixel repetition (Used for interlace display)
	enum DH_AVI_info_bars info_bars;                        // Defines validity of 4 following bars
	RMuint32 VIC;                                           // EIA/CEA-861B Video ID Code (Table 13 page 61)
	RMuint16 end_top_bar_line_num;
	RMuint16 start_bottom_bar_line_num;
	RMuint16 end_left_bar_pixel_num;
	RMuint16 start_right_bar_pixel_num;
};

enum DH_source_dev_info {
	DH_source_dev_unknown = 0, 
	DH_source_dev_DigitalSTB, 
	DH_source_dev_DVD, 
	DH_source_dev_DVHS, 
	DH_source_dev_HDDVideo, 
	DH_source_dev_DVC, 
	DH_source_dev_DSC, 
	DH_source_dev_VideoCD, 
	DH_source_dev_Game, 
	DH_source_dev_PC, 
	DH_source_dev_BluRay, 
	DH_source_dev_SACD, 
};

struct DH_SPDInfoFrame {
	RMuint8 Version;              // 0x01
	RMascii *VendorName;          // up to 8 chars: Name of the company whose name appears on the product
	RMascii *ProductDescription;  // up to 16 chars: Model number of the product, and maybe a short description also.
	enum DH_source_dev_info SDI;  // classification of source device: see enum values
};

enum DH_AudioChannelCount {
	DH_AudioChannelCount_FromStreamHeader = 0, 
	DH_AudioChannelCount_2, 
	DH_AudioChannelCount_3, 
	DH_AudioChannelCount_4, 
	DH_AudioChannelCount_5, 
	DH_AudioChannelCount_6, 
	DH_AudioChannelCount_7,
	DH_AudioChannelCount_8
};

enum DH_AudioSampleSize {
	DH_AudioSampleSize_FromStreamHeader = 0, 
	DH_AudioSampleSize_16, 
	DH_AudioSampleSize_20, 
	DH_AudioSampleSize_24
};

enum DH_AudioSampleFrequency {
	DH_AudioSampleFrequency_FromStreamHeader = 0, 
	DH_AudioSampleFrequency_32000, 
	DH_AudioSampleFrequency_44100, 
	DH_AudioSampleFrequency_48000, 
	DH_AudioSampleFrequency_88200, 
	DH_AudioSampleFrequency_96000, 
	DH_AudioSampleFrequency_176400, 
	DH_AudioSampleFrequency_192000
};

// speaker assignments of channels 5 - 8, 
// FC=FrontCenter, RC=RearCenter, RR=RearRight, RL=RealLeft
// RLC=RealLeftCenter, RRC=RearRightCenter, 
// FLC=FrontLeftCenter, FRC=FrontRightCenter
enum DH_Audio_ChannelAssignment {
	DH_Audio_CA_none = 0, 
	DH_Audio_CA_RC5, 
	DH_Audio_CA_RL5_RR6, 
	DH_Audio_CA_RL5_RR6_RC7, 
	DH_Audio_CA_RL5_RR6_RLC7_RRC8, 
	DH_Audio_CA_FLC7_FRC8, 
	DH_Audio_CA_RC5_FLC7_FRC8, 
	DH_Audio_CA_RL5_RR6_FLC7_FRC8
};

struct DH_AudioInfoFrame {
	RMuint8 Version;  // 0x01
	enum DH_AudioCodingType CodingType;
	enum DH_AudioChannelCount ChannelCount;
	enum DH_AudioSampleFrequency SampleFrequency;
	enum DH_AudioSampleSize SampleSize;
	RMbool LFE_Ch3_Enable;
	RMbool FC_Ch4_Enable;
	enum DH_Audio_ChannelAssignment CA;
	RMbool DownMixInhibit;
	RMuint32 LevelShift;  // in dB
	RMuint32 MaxBitRate;  // maximum bitrate of compressed formats divided by 8kHz
};

enum DH_MPEG_Frame {
	DH_MPEG_Frame_unknown = 0, 
	DH_MPEG_Frame_I, 
	DH_MPEG_Frame_B, 
	DH_MPEG_Frame_P, 
};

struct DH_MPEGInfoFrame {
	RMuint8 Version;  // 0x01
	RMuint32 BitRate;  // bits per second
	RMbool RepeatedField;  // about current field, for 3:2 pulldown
	enum DH_MPEG_Frame Frame;  // about current picture
};

struct DH_AudioFormat {
	RMuint32 NumChannel;
	RMuint32 SampleFrequency;
	RMbool I2S0_Enable;
	RMbool I2S1_Enable;
	RMbool I2S2_Enable;
	RMbool I2S3_Enable;
	RMbool SPDIF_Enable;
	RMuint32 MClkFrequency;
	RMuint32 SRC_Factor;
};

struct DH_VideoFormatInfo {
	enum DH_picture_aspect_ratio aspect_ratio;
	RMbool multiple_aspect_ratios;         // Indicates the same emhlib video standard support several aspect ratios
	enum DH_pixel_repetition pixel_rep;
	RMuint32 VIC;                          // Video Format Identification Code
	enum EMhwlibColorSpace color_space;    // Can be YUV 601 (SD) or 709 (HD)
};

#define AUDIO_DATA_BLOCK_SH_TAGS 1
#define VIDEO_DATA_BLOCK_SH_TAGS 2
#define VENDOR_SPECIFIC_TAG 3
#define SPEAKER_ALLOCATION_TAGS 4
#define EXTENDED_DATA_BLOCK_TAG 7

#define EXTENDED_TAG_VIDEO_CAPABILITY 0
#define EXTENDED_TAG_VENDOR_VIDEO 1
#define EXTENDED_TAG_COLORIMETRY 5
#define EXTENDED_TAG_CEA_MISC 16
#define EXTENDED_TAG_VENDOR_AUDIO 17
#define EXTENDED_TAG_HDMI_AUDIO 18

struct DH_control;

RM_EXTERN_C_BLOCKSTART

// Use DHOpenChip for all Sigma reference designs. Please also check GPIO_RESET in "dvi_hdmi.c".
/** Reset: if TRUE, perform hard reset of internal SiI9030 core before initialisation */
RMstatus DHOpenChip(struct RUA *pRUA, enum DH_vendor_parts part, RMbool Reset, enum GPIOId_type reset_GPIO, 
	RMuint32 i2c_tx_ModuleNumber, enum GPIOId_type i2c_tx_GPIOClock, enum GPIOId_type i2c_tx_GPIOData, RMuint32 i2c_tx_Speed_kHz, 
	RMuint32 i2c_rx_ModuleNumber, enum GPIOId_type i2c_rx_GPIOClock, enum GPIOId_type i2c_rx_GPIOData, RMuint32 i2c_rx_Speed_kHz, 
	struct DH_control **ppDH);

// Obolete, please use DHOpenChip instead.
RMstatus DHInitWithAutoDetectReset(struct RUA *pRUA, 
	RMuint8 pio_clock_transmitter, RMuint8 pio_data_transmitter, RMuint32 i2c_module, 
	RMuint8 gpio_reset, struct DH_control **pDH, RMbool Reset);
RMstatus DHInitWithAutoDetect(struct RUA *pRUA, 
	RMuint8 pio_clock_transmitter, RMuint8 pio_data_transmitter, RMuint32 i2c_module, 
	RMuint8 gpio_reset, struct DH_control **pDH);
RMstatus DHInitReset(struct RUA *pRUA, RMuint8 pio_clock_transmitter, RMuint8 pio_data_transmitter, 
	RMuint8 pio_clock_receiver, RMuint8 pio_data_receiver, RMuint32 i2c_module, 
	enum DH_vendor_parts part, struct DH_control **pDH, RMbool Reset);
RMstatus DHInit(struct RUA *pRUA, RMuint8 pio_clock_transmitter, RMuint8 pio_data_transmitter, 
	RMuint8 pio_clock_receiver, RMuint8 pio_data_receiver, RMuint32 i2c_module, 
	enum DH_vendor_parts part, struct DH_control **pDH);

RMstatus DHCheckHDCPKeyMem(struct DH_control *pDH);

// Pointer to current SRM in app memory
// This is the SRM which is used by the HDMI code to check the display keys
RMstatus DHSetSRM(struct DH_control *pDH, RMuint8 *pSRM);

// Check DCP LLC signature of SRM and check if it is more recent than the one set by DHSetSRM()
// If *pUpdate is returned TRUE, the app needs to store the SRM from the disc in persistent memory.
// Suggested sequence: 
//   When app is started, app reads SRM from persistent memory, calls DHSetSRM()
//   When disc is mounted, app reads SRM from disc, calls DHValidateSRM()
//   When DHValidateSRM() sets *pUpdate to TRUE, app writes SRM to persistent memory.
RMstatus DHValidateSRM(struct DH_control *pDH, RMuint8 *pSRM, RMbool *pUpdate);

RMstatus DHSetHDMIMode(struct DH_control *pDH, RMbool HDMI);

// Set up control of the external swing resistor of the HDMI TMDS.
// If the pixel clock is above PixelClockThreshold, the specified GPIO pin will 
// be set to HIGH, otherwise to LOW. PixelClockThreshold = 0 disables this feature (default)
// GPIO should be set to GPIOId_Eth_21 for the Pioneer BluRay board (860E1)
RMstatus DHSetTMDSMode(struct DH_control *pDH, enum GPIOId_type GPIO, RMuint32 PixelClockThreshold);

RMstatus DHSetEDIDMode(struct DH_control *pDH, 
	enum DH_EDID_select EDID_select, 
	RMuint32 EDID_selection,  // preferred entry in VIC table
	RMuint32 EDID_vfreq,  // preferred vsync frequency (50, 59, 60 etc.)
	RMuint32 EDID_hsize,  // preferred horizontal active size
	RMuint32 EDID_vsize,  // preferred vertical active size
	RMbool EDID_intl);    // preferred interlaced (TRUE) / progressive (FALSE) mode
RMstatus DHSetEDIDForceMask(struct DH_control *pDH, 
	RMuint64 EDID_force_mask);  // try these VICs first (bit 1 = VIC 1, bit 2 = VIC 2 etc.)
RMstatus DHSetEDIDExcludeMask(struct DH_control *pDH, 
	RMuint64 EDID_exclude_mask); // never use these VICs
RMstatus DHSetEDIDFrequencyLimits(struct DH_control *pDH, 
	RMuint32 EDID_max_pixclk, 
	RMuint32 EDID_min_pixclk, 
	RMuint32 EDID_max_hfreq, 
	RMuint32 EDID_min_hfreq, 
	RMuint32 EDID_max_vfreq, 
	RMuint32 EDID_min_vfreq);

RMstatus DHGetState(struct DH_control *pDH, enum DH_device_state *pDevState, enum DH_connection *pConnection);
RMstatus DHGetAKSV(struct DH_control *pDH, RMuint8 *pAKSV);
RMstatus DHRequestHDCP(struct DH_control *pDH);
RMstatus DHCancelHDCP(struct DH_control *pDH);

RMstatus DHInitChip(struct DH_control *pDH);
RMstatus DHSetDDRMode(struct DH_control *pDH, RMbool ddr_mode);
RMstatus DHEnableOutput(struct DH_control *pDH);
RMstatus DHReEnableOutput(struct DH_control *pDH);  // enable and restore state from before DHDisableOutput()
RMstatus DHDisableOutput(struct DH_control *pDH);
RMstatus DHMuteOutput(struct DH_control *pDH, RMbool mute); // TRUE: mute, FALSE: unmute
RMstatus DHMuteAudio(struct DH_control *pDH, RMbool mute); // TRUE: mute audio only, without sending A/V-Mute packet, FALSE: unmute
RMstatus DHBlankVideo(struct DH_control *pDH, RMbool mute); // TRUE: send blank-level video, without sending A/V-Mute packet, FALSE: unmute
RMstatus DHBlankVideoColor(struct DH_control *pDH, struct EMhwlibDeepColor Color); // Sets the video pixel value for blanking

// Set up the format of the video from the 86xx to the HDMI chip
// (This has to match the format set on the DispDigitalOut emhwlib module)
RMstatus DHSetInputVideoFormat(struct DH_control *pDH, enum EMhwlibColorSpace ColorSpace, enum EMhwlibSamplingMode SamplingMode, RMuint32 ComponentBitDepth);
// Set up the format of the video from the HDMI chip to the TMDS link
// (This has to be matched in the AVI info frame)
RMstatus DHSetOutputVideoFormat(struct DH_control *pDH, enum EMhwlibColorSpace ColorSpace, enum EMhwlibSamplingMode SamplingMode, RMuint32 ComponentBitDepth);
// Same as DHSetInputVideoFormat() and DHSetOutputVideoFormat(), whithout extra config cycle inbetween
RMstatus DHSetConversionVideoFormat(struct DH_control *pDH, 
	enum EMhwlibColorSpace InputColorSpace, enum EMhwlibSamplingMode InputSamplingMode, RMuint32 InputComponentBitDepth, 
	enum EMhwlibColorSpace OutputColorSpace, enum EMhwlibSamplingMode OutputSamplingMode, RMuint32 OutputComponentBitDepth);

RMstatus DHSoftReset(struct DH_control *pDH);
RMstatus DHReset(struct DH_control *pDH); // Use to reset siI170 using a GPIO to allow re-authentication
RMstatus DHAuthenticate(struct DH_control *pDH);
RMstatus DHCheckKeyRevocation(RMuint8 *pSRM, RMuint8 *BKSV, RMuint32 nBKSV, RMbool *revoked);
RMstatus DHCheckRepeaterKSV(struct DH_control *pDH);
RMstatus DHCheckRevocationList(struct DH_control *pDH, RMuint8 *pSRM, RMbool *revoked);
RMstatus DHUpdateState(struct DH_control *pDH, enum DH_device_state *pState, enum DH_connection *pCable, RMbool *pRx, RMbool *pHPD, RMbool *pUpdateEDID);
RMstatus DHGetConnection(struct DH_control *pDH, enum DH_connection *cable, RMbool *Rx, RMbool *HPD);
RMstatus DHGetVideoModeFromEDID(
	struct DH_control *pDH, 
	enum EMhwlibTVStandard *standard, 
	struct EMhwlibTVFormatDigital *format_digital, 
	struct EMhwlibTVFormatAnalog *format_analog, 
	RMbool *UseStandard, 
	RMuint32 *pVIC, 
	RMuint32 *ar_x, 
	RMuint32 *ar_y, 
	RMbool *HDMI_display, 
	struct CEA861BDataBlockCollection pDBC[], 
	RMuint32 *pnDBC, 
	enum EMhwlibColorSpace *color_space);
RMstatus DHGetHDMIModeFromEDID(struct DH_control *pDH, RMbool *HDMI);
RMstatus DHVerifyIntegrity(struct DH_control *pDH);
RMstatus DHReadEDID(struct DH_control *pDH);

// Production version of integrity check
RMstatus DHCheckInterrupt(struct DH_control *pDH, RMbool *pInterrupt);
RMstatus DHCheckHDMI(struct DH_control *pDH, struct DH_HDMI_state *pHDMIState);

// temporary define for integration
#ifndef DH_AVAILABLE_CheckInterrupt
#define DH_AVAILABLE_CheckInterrupt
#endif

// Only for test applications (see dvi_hdmi.c)
RMstatus DHIntegrityCheck(struct DH_control *pDH, 
	RMbool *ReceiverPresent, 
	RMbool *HotPlugDetected, 
	RMbool *UpdateVideoMode);

RMstatus DH_dump_i2c(struct DH_control *pDH);
RMstatus DHDDCBlockReadSegment(struct DH_control *pDH, RMuint8 i2cAddr, RMuint8 i2cSegmentPtr, RMuint32 RegAddr, RMuint8 *pData, RMuint32 NbBytes);
RMstatus DHDDCBlockWriteSegment(struct DH_control *pDH, RMuint8 i2cAddr, RMuint8 i2cSegmentPtr, RMuint32 RegAddr, RMuint8 *pData, RMuint32 NbBytes);
RMstatus DHDDCBlockRead(struct DH_control *pDH, RMuint8 i2cAddr, RMuint8 RegAddr, RMuint8 *pData, RMuint32 NbBytes);
RMstatus DHDDCBlockWrite(struct DH_control *pDH, RMuint8 i2cAddr, RMuint8 RegAddr, RMuint8 *pData, RMuint32 NbBytes);
void DHPrintEDIDVendor(struct EDID_Data *edid);
RMstatus DHSetEDID(struct DH_control *pDH, RMuint8 *pEDID, RMuint32 EDID_blocks);
RMstatus DHLoadEDIDVersion1(struct DH_control *pDH, struct EDID_Data *pData);
RMstatus DHLoadEDIDBlock(struct DH_control *pDH, RMuint8 block_number, RMuint8 *pData, RMuint8 data_size);
RMstatus DHWriteEDIDBlock(struct DH_control *pDH, RMuint8 block_number, RMuint8 *pData, RMuint8 data_size);
RMstatus DHGetCEADataBlockCollection(RMuint8 *pedid, RMuint8 data_size, struct CEA861BDataBlockCollection *pDBC);

RMstatus DHGetCEADetailedTimingDescriptor(RMuint8 *pedid_dtc, struct CEADetailedTimingDescriptor *pDTD);
RMstatus DHGetEmhwlibTVStandardFromCEADetailedTimingDescriptor(
	struct RUA *pRUA, 
	struct CEADetailedTimingDescriptor *pDTD,
	RMuint32 AspX, RMuint32 AspY, 
	enum EMhwlibTVStandard *pTVStandard, RMuint32 *pVIC);
RMstatus DHGetVideoInfoFromCEAVideoIdentificationCode(
	RMuint32 VIC, 
	RMbool sixtyhertz, // use 60Hz instead of 59.94Hz mode, if applicable
	enum EMhwlibTVStandard *pTVStandard, 
	RMuint32 *pAspX, RMuint32 *pAspY, 
	enum EMhwlibColorSpace *pColorSpace, 
	RMbool *pMultipleAspectRatios);
RMstatus DHGetEmhwlibTVStandardFromCEAVideoIdentificationCode(
	struct RUA *pRUA, 
	RMuint32 VIC, 
	RMbool sixtyhertz, // use 60Hz instead of 59.94Hz mode, if applicable
	enum EMhwlibTVStandard *pTVStandard, 
	RMuint32 *pAspX, RMuint32 *pAspY);
RMstatus DHGetEmhwlibDigitalFormatFromCEADetailedTimingDescriptor(
	struct CEADetailedTimingDescriptor *pDTD,
	struct EMhwlibTVFormatDigital *format_dig);
RMstatus DHGetEmhwlibAnalogFormatFromCEADetailedTimingDescriptor(
	struct CEADetailedTimingDescriptor *pDTD,
	struct EMhwlibTVFormatAnalog *format_analog);
RMstatus DHGetVideoFormatInfo(
	enum EMhwlibTVStandard TvStandard, 
	RMuint32 AspX, RMuint32 AspY, 
	struct DH_VideoFormatInfo *pvideo_format_info);

RMstatus DHEnableAVIInfoFrame(struct DH_control *pDH, struct DH_AVIInfoFrame *pAVIInfoFrame);
RMstatus DHDisableAVIInfoFrame(struct DH_control *pDH);
RMstatus DHModifyAVIScanInfo(struct DH_control *pDH, enum DH_scan_info ScanInfo);
RMstatus DHModifyAVIActiveFormat(struct DH_control *pDH, RMbool ActiveFormatValid, enum DH_active_format_aspect_ratio ActiveFormat);
RMstatus DHModifyAVIBarInfo(struct DH_control *pDH, 
	enum DH_AVI_info_bars info_bars, 
	RMuint16 end_top_bar_line_num, 
	RMuint16 start_bottom_bar_line_num, 
	RMuint16 end_left_bar_pixel_num, 
	RMuint16 start_right_bar_pixel_num);
RMstatus DHModifyAVIColorSpace(struct DH_control *pDH, enum EMhwlibColorSpace ColorSpace, enum EMhwlibSamplingMode SamplingMode);
RMstatus DHModifyAVIAspectRatio(struct DH_control *pDH, struct EMhwlibAspectRatio AspectRatio);
RMstatus DHModifyNonUniformScaling(struct DH_control *pDH, 
	RMbool HorizontalStrech,  // picture has been horizontally scaled (e.g. non-linear scaling)
	RMbool VericalStrech);    // picture has been vertically scaled
RMstatus DHModifyITContent(struct DH_control *pDH, RMbool ITContent);

// SPD can be sent via SPD, MPEG, Generic or Generic2 frame, 
// offset can be set to 0 for DH_InfoFrameOffset_SPD
RMstatus DHEnableSPDInfoFrame(struct DH_control *pDH, struct DH_SPDInfoFrame *pSPDInfoFrame, enum DH_InfoFrameOffset offset);
RMstatus DHDisableSPDInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset);

RMstatus DHEnableAudioInfoFrame(struct DH_control *pDH, struct DH_AudioInfoFrame *pAudioInfoFrame);
RMstatus DHDisableAudioInfoFrame(struct DH_control *pDH);

// MPEG can be sent via SPD, MPEG, Generic or Generic2 frame, 
// offset can be set to 0 for DH_InfoFrameOffset_MPEG
RMstatus DHEnableMPEGInfoFrame(struct DH_control *pDH, struct DH_MPEGInfoFrame *pMPEGInfoFrame, enum DH_InfoFrameOffset offset);
RMstatus DHDisableMPEGInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset);

// calculate CEA861 checksum and store in DataByte[0], data size in HeaderByte[2]
RMstatus DHCalcInfoFrameCheckSum(struct CEA861InfoFrame *pInfoFrame);
// load content and enable repeated transmission of selected info frame
RMstatus DHEnableInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset, struct CEA861InfoFrame *pInfoFrame);
// disable repeated transmission of selected info frame
RMstatus DHDisableInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset);
// wait until selected info frame has been sent once
RMstatus DHWaitInfoFrame(struct DH_control *pDH, enum DH_InfoFrameOffset offset);

// Equivalent to disabling all Info frames, making the output "DVI" only
RMstatus DHDisableHDMIMode(struct DH_control *pDH);

// Allow application to set bits 0 and 3 through 15 of PCM IEC-60958 Header
RMstatus DHSetAudioHeader(struct DH_control *pDH, RMuint32 mask, RMuint32 value);
// Set the PCM IEC-60958 Header Bit 2 (copy protection) to 1, if AudioCP is TRUE
RMstatus DHSetAudioCP(struct DH_control *pDH, RMbool AudioCP);
RMstatus DHSetAudioFormat(struct DH_control *pDH, struct DH_AudioFormat *pAudioFormat);
RMstatus DHSetVideoPixelClock(struct DH_control *pDH, RMuint32 PixelClock);
RMstatus DHSetVideoSyncPolarity(struct DH_control *pDH, RMbool VSyncPolarity, RMbool HSyncPolarity);
RMstatus DHSetVideoPixelClockFromTVStandard(struct DH_control *pDH, enum EMhwlibTVStandard standard);
RMstatus DHSetVideoPixelClockFromTVFormat(struct DH_control *pDH, struct EMhwlibTVFormatDigital *format_digital);
RMstatus DHSetAudioSampleClock(struct DH_control *pDH, 	RMuint32 SampleClock);
RMstatus DHSetDEGeneratorFromTVStandard(struct DH_control *pDH, RMbool EnableDE, RMbool Enable656, RMbool EnableDemux, enum EMhwlibTVStandard standard);
RMstatus DHSetDEGeneratorFromTVFormat(struct DH_control *pDH, RMbool EnableDE, RMbool Enable656, RMbool EnableDemux, struct EMhwlibTVFormatDigital *format_digital);

RMstatus DHDone(struct DH_control *pDH);

RMstatus DHGetChipInit(struct DH_control *pDH, RMuint32 *dvi_init, RMuint32 *dvi_size);
RMstatus DHGetDviHdmiPart(struct DH_control *pDH, enum DH_vendor_parts *part);

// I2C debug access, pass key = '\0' to reset state machine
RMstatus DHDebugI2C(struct DH_control *pDH, RMascii trigger, RMascii key);

RM_EXTERN_C_BLOCKEND

#endif // __DVI_HDMI_H__
