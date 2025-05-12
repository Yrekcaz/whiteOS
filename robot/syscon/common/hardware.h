#ifndef __HARDWARE_H
#define __HARDWARE_H

#include <stdint.h>

#include "common.h"

/*
TIM1  Motors
TIM3  Motors
TIM6  Byte counter (uart DMA)
TIM14 Main-exec / Backpack
TIM15 SPI Master clock
TIM16 Touch Sense 1
TIM17 Backpack LEDs
*/

//#define DEBUG

static const uint32_t SYSTEM_CLOCK = 48000000;
static const uint32_t CONTACT_BAUDRATE = 115200;
static const uint32_t COMMS_BAUDRATE = 3000000;

static const uint32_t WATCHDOG_CLOCK  = 10000;
static const uint32_t WATCHDOG_LIMIT  = WATCHDOG_CLOCK / 200 * 10; // 10 main execution frames

static const uint32_t* HW_REVISION    = (uint32_t*)0x08000010;
static const uint32_t* HW_MODEL       = (uint32_t*)0x08000014;
static const uint32_t  WHISKEY_MIN_VERSION = 7;
static const uint32_t  XRAY_MIN_VERSION = 1;
static const uint32_t  XRAY_MAX_VERSION = 1;

// had to pack a version in to existing field so we don't break
// Whiskey's in the field. To do that we took over the MSB from
// the model field. from model, MSB = REV2, LSB = MODEL. 
// Currently Xray is the only robot with REV2 > 0

#define IS_WHISKEY (*HW_REVISION >= WHISKEY_MIN_VERSION)
#define IS_XRAY (((*HW_MODEL >> 8) >= XRAY_MIN_VERSION) && (((*HW_MODEL >> 8) <= XRAY_MAX_VERSION)))

enum IRQ_Priority {
  PRIORITY_ADC = 0,
  PRIORITY_ENCODERS = 1,
  PRIORITY_TOUCH_SENSE = 1,
  PRIORITY_LIGHTS = 1,
  PRIORITY_MAIN_EXEC = 2,
  PRIORITY_I2C_TRANSMIT = 2,
  PRIORITY_SPINE_COMMS = 2,
  PRIORITY_CONTACTS_COMMS = 2,
  PRIORITY_MICS = 3,
};

// H-Bridge
namespace LP1 GPIO_DEFINE(F, 0);
namespace LN1 GPIO_DEFINE(A, 10);
namespace LN2 GPIO_DEFINE(A, 11);

namespace HP1 GPIO_DEFINE(F, 1);
namespace HN1 GPIO_DEFINE(B, 5);
namespace HN2 GPIO_DEFINE(B, 1);

namespace RTP1 GPIO_DEFINE(B, 12);
namespace RTN1 GPIO_DEFINE(A, 8);
namespace RTN2 GPIO_DEFINE(A, 7);

namespace LTP1 GPIO_DEFINE(A, 15);
namespace LTN1 GPIO_DEFINE(A, 9);
namespace LTN2 GPIO_DEFINE(B, 0);

// Encoders (A is always lowest pin)
namespace HENCA GPIO_DEFINE(A, 0);
namespace HENCB GPIO_DEFINE(A, 1);
namespace LENCA GPIO_DEFINE(B, 2);
namespace LENCB GPIO_DEFINE(B, 3);
namespace RTENC GPIO_DEFINE(C, 14);
namespace LTENC GPIO_DEFINE(C, 15);

// Power
namespace POWER_EN GPIO_DEFINE(A, 6);
namespace nCHG_PWR GPIO_DEFINE(B, 9);
namespace nVENC_EN GPIO_DEFINE(C, 13);
namespace VEXT_SENSE GPIO_DEFINE(A, 2);
namespace VMAIN_SENSE GPIO_DEFINE(A, 4);

namespace MAIN_EN_VIC GPIO_DEFINE(A, 3);
namespace MAIN_EN_WIS GPIO_DEFINE(A, 12);

// Microphones
namespace MIC_LR      GPIO_DEFINE(B, 15);
namespace MIC2_MISO   GPIO_DEFINE(B, 4);
namespace MIC1_MISO   GPIO_DEFINE(B, 14);
namespace MIC2_SCK    GPIO_DEFINE(A, 5);
namespace MIC1_SCK    GPIO_DEFINE(B, 13);

// Cap Sense
namespace CAPI GPIO_DEFINE(B, 8);
namespace CAPO_VIC GPIO_DEFINE(A, 14);

// Communication
namespace VEXT_TX GPIO_DEFINE(A, 2);
namespace BODY_TX GPIO_DEFINE(B, 6);
namespace BODY_RX GPIO_DEFINE(B, 7);
namespace SCL1 GPIO_DEFINE(B, 10);
namespace SDA1 GPIO_DEFINE(B, 11);
namespace SCL2 GPIO_DEFINE(F, 6);
namespace SDA2 GPIO_DEFINE(F, 7);

// Lights
namespace LED_DAT GPIO_DEFINE(A, 13);
namespace LED_CLK_VIC GPIO_DEFINE(A, 12);
namespace LED_CLK_WIS GPIO_DEFINE(A, 14);

#define DFU_ENTRY_POINT (0xC0C35473)
#define DFU_FLAG (*(uint32_t*)0x20001FFC)

#endif
