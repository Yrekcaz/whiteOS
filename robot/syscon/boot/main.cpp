#include "vectors.h"
#include "bignum.h"
#include "rsa_pss.h"
#include "flash.h"
#include "comms.h"

#include "stm32f0xx.h"

#include "power.h"
#include "analog.h"

#include "common.h"
#include "hardware.h"

extern "C" void StartApplication(const uint32_t* stack, VectorPtr reset);

static const uint16_t MAIN_EXEC_PRESCALE = 4; // Timer prescale
static const uint16_t MAIN_EXEC_PERIOD = SYSTEM_CLOCK / MAIN_EXEC_PRESCALE / 200;        // 200hz (5ms period)

bool validate(void) {
  // Elevate the watchdog kicker while a cert check is running
  NVIC_SetPriority(TIM14_IRQn, 0);

  // If our hashes do not match, cert is bunk
  bool valid = verify_cert(&APP->signedStart, COZMO_APPLICATION_SIZE - COZMO_APPLICATION_HEADER, APP->certificate, sizeof(APP->certificate));

  NVIC_SetPriority(TIM14_IRQn, 3);

  return valid;
}

static bool boot_test(void) {
  // Failure count reached max
  if (APP->faultCounter[MAX_FAULT_COUNT - 1] != FAULT_NONE) {
    if (APP->faultCounter[MAX_FAULT_COUNT - 1] == FAULT_USER_WIPE) {
      BODY_TX::mode(MODE_ALTERNATE);
    } else {
      // Signal recovery
      BODY_TX::reset();
    }

    return false;
  }

  // This is not a whiskey compatible APPLICATIOn
  if (APP->whiskeyCompatible != WHISKEY_COMPATIBLE) {
    return false;
  }

  // Evil flag not set
  if (APP->fingerPrint != COZMO_APPLICATION_FINGERPRINT) {
    return false;
  }

  // Check certificate
  return validate();
}

void timer_init(void) {
  // Start our cheese watchdog
  #ifndef DEBUG
  // Start the watchdog up
  IWDG->KR = 0xCCCC;
  IWDG->KR = 0x5555;
  IWDG->PR = 0;
  IWDG->RLR = WATCHDOG_LIMIT;
  IWDG->KR = 0xAAAA;
  #endif

  TIM14->CR1 = 0;
  TIM14->PSC = MAIN_EXEC_PRESCALE - 1;
  TIM14->ARR = MAIN_EXEC_PERIOD - 1;
  TIM14->DIER = TIM_DIER_UIE;
  TIM14->CR1 = TIM_CR1_CEN;

  NVIC_SetPriority(TIM14_IRQn, 1);
  NVIC_EnableIRQ(TIM14_IRQn);
}

extern "C" void SoftReset(void) {
  NVIC_SystemReset();
}

extern "C" void TIM14_IRQHandler(void) {
  #ifndef DISABLE_WDOG
  IWDG->KR = 0xAAAA;
  #endif
  TIM14->SR = 0;
  Analog::tick();
  Comms::tick();
}

int main(void) {
  Power::init();
  Analog::init();
  timer_init();

  // If fingerprint is invalid, cert is invalid, or reset counter is zero
  // 1) Wipe flash
  // 2) Start recovery
  if (!boot_test()) {
    // We need to run recovery
    Flash::eraseApplication();

    // Wait for firmware
    Comms::run();
  } else if (~RCC->CSR & RCC_CSR_PINRSTF) {
    // Hardware watchdog trap
    Comms::run();
  }

  // Clear reset pin flag (for reset detect).
  RCC->CSR |= RCC_CSR_RMVF;

  // This flag is only set when DFU has validated the image
  if (APP->fingerPrint != COZMO_APPLICATION_FINGERPRINT) {
    NVIC_SystemReset();
  }

  Analog::stop();

  StartApplication(APP->stackStart, APP->resetVector);
}
