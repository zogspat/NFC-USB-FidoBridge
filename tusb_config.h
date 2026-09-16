#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------
// defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
  #define CFG_TUSB_MCU                OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
  #define CFG_TUSB_OS                 OPT_OS_PICO
#endif

// Enable Device stack
#define CFG_TUD_ENABLED               1

// RHPort mode
#define CFG_TUSB_RHPORT0_MODE         OPT_MODE_DEVICE

// Default is max speed that hardware controller could support with on-chip PHY
#define CFG_TUD_MAX_SPEED             OPT_MODE_DEFAULT_SPEED

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 * TinyUSB use as following macros to declare transferring memory so that they can be
 * put into those specific section.
 */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN            __attribute__ ((aligned(4)))

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
  #define CFG_TUD_ENDPOINT0_SIZE      64
#endif

//------------- CLASS -------------//
#define CFG_TUD_HID                   1
#define CFG_TUD_CDC                   1
#define CFG_TUD_CDC_EP_BUFSIZE        64
#define CFG_TUD_CDC_RX_BUFSIZE        64
#define CFG_TUD_CDC_TX_BUFSIZE        64
#define CFG_TUD_MSC                   0
#define CFG_TUD_MIDI                  0
#define CFG_TUD_VENDOR                0

//--------------------------------------------------------------------
// HID CONFIGURATION
//--------------------------------------------------------------------

// HID buffer size: Sufficient for CTAP HID which uses 64-byte reports
#define CFG_TUD_HID_EP_BUFSIZE        64

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
