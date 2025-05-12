#include <string.h>

#include "common.h"
#include "hardware.h"

#include "encoders.h"
#include "motors.h"
#include "messages.h"
#include "timer.h"

static const int MOTOR_SERVICE_COUNTDOWN = 4;
static const int MAX_ENCODER_FRAMES = 25; // 0.1250s
static const int MAX_POWER = 0x8000;
static const uint16_t MOTOR_PERIOD = 20000; // 20khz
static const int16_t MOTOR_MAX_POWER = SYSTEM_CLOCK / MOTOR_PERIOD;
static const int DRIVEN_POWER = MOTOR_MAX_POWER / 10;

#define ABS(x) (((x) < 0) ? -(x) : (x))

enum MotorDirection {
  DIRECTION_UNINIT = 0,
  DIRECTION_FORWARD,
  DIRECTION_BACKWARD,
  DIRECTION_IDLE
};

struct MotorConfig {
  // Pin BRSS
  volatile uint32_t* P_BSRR;
  uint32_t           P_Set;

  // N1 Pin
  volatile uint32_t* N1CC;
  GPIO_TypeDef*      N1_Bank;
  uint32_t           N1_ModeMask;
  uint32_t           N1_ModeAlt;
  uint32_t           N1_ModeOutput;

  // N2 Pin
  volatile uint32_t* N2CC;
  GPIO_TypeDef*      N2_Bank;
  uint32_t           N2_ModeMask;
  uint32_t           N2_ModeAlt;
  uint32_t           N2_ModeOutput;
};

// Current status of the motors
struct MotorStatus {
  uint32_t        position;
  uint32_t        last_time;
  int             power;
  MotorDirection  direction;
  MotorDirection  hysteresis_direction;
  uint8_t         serviceCountdown;
};

#define CONFIG_N(PIN) \
  (PIN::bank), ~(3 << (PIN::pin * 2)), (MODE_ALTERNATE << (PIN::pin * 2)), (MODE_OUTPUT << (PIN::pin * 2))

static const MotorConfig MOTOR_DEF[MOTOR_COUNT] = {
  {
    &LTP1::bank->BSRR, LTP1::mask,
    &TIM1->CCR2, CONFIG_N(LTN1),
    &TIM1->CCR2, CONFIG_N(LTN2)
  },
  {
    &RTP1::bank->BSRR, RTP1::mask,
    &TIM1->CCR1, CONFIG_N(RTN1),
    &TIM1->CCR1, CONFIG_N(RTN2)
  },
  {
    &LP1::bank->BSRR, LP1::mask,
    &TIM1->CCR3, CONFIG_N(LN1),
    &TIM1->CCR4, CONFIG_N(LN2)
  },
  {
    &HP1::bank->BSRR, HP1::mask,
    &TIM3->CCR2, CONFIG_N(HN1),
    &TIM3->CCR4, CONFIG_N(HN2)
  },
};

bool Motors::lift_driven;
bool Motors::head_driven;
bool Motors::treads_driven;

static MotorStatus motorStatus[MOTOR_COUNT];
static int16_t motorPower[MOTOR_COUNT];
static int moterServiced;

static void Motors::receive(HeadToBody *payload) {
  moterServiced = MOTOR_SERVICE_COUNTDOWN;
  memcpy(motorPower, payload->motorPower, sizeof(motorPower));
}

static void Motors::transmit(BodyToHead *payload) {
  uint32_t* time_last;
  int32_t* delta_last;

  Encoders::flip(time_last, delta_last);

  // Radio silence = power down motors
  if (moterServiced <= 0) {
    memset(motorPower, 0, sizeof(motorPower));
  } else {
    moterServiced--;
  }

  for (int i = 0; i < MOTOR_COUNT; i++) {
    MotorStatus* state = &motorStatus[i];

    // Calculate motor power
    motorStatus[i].power = (MOTOR_MAX_POWER * (int)motorPower[i]) / MAX_POWER;

    // Clear our delta when we've been working too hard
    if (++state->serviceCountdown > MAX_ENCODER_FRAMES) {
      payload->motor[i].delta = 0;
    }

    // Calculate the encoder values
    if (delta_last[i] != 0) {
      // Update robot positions
      switch (i) {
        case MOTOR_LEFT:
        case MOTOR_RIGHT:
          payload->motor[i].delta = (state->hysteresis_direction == DIRECTION_FORWARD) ? delta_last[i] : -delta_last[i];
          break ;
        default:
          payload->motor[i].delta = delta_last[i];
          break ;
      }

      // Copy over tick values
      state->position += payload->motor[i].delta;
      payload->motor[i].position = state->position;
      payload->motor[i].time = state->last_time - time_last[i];

      // We will survives
      state->last_time = time_last[i];
      state->serviceCountdown = 0;
    }
  }



  // Flush invalid flags when device is moving
  Motors::lift_driven = ABS(motorStatus[MOTOR_LIFT].power) > DRIVEN_POWER;
  Motors::head_driven = ABS(motorStatus[MOTOR_HEAD].power) > DRIVEN_POWER;
  Motors::treads_driven = ABS(motorStatus[MOTOR_LEFT].power) > DRIVEN_POWER
                       || ABS(motorStatus[MOTOR_RIGHT].power) > DRIVEN_POWER;
}

static void configure_timer(TIM_TypeDef* timer) {
  // PWM mode 2, edge aligned with fast output enabled (prebuffered)
  // Complementary outputs are inverted
  timer->CCMR2 =
  timer->CCMR1 =
    TIM_CCMR1_OC1PE | TIM_CCMR1_OC1FE | (TIM_CCMR1_OC1M_0 * 6) |
    TIM_CCMR1_OC2PE | TIM_CCMR1_OC2FE | (TIM_CCMR1_OC2M_0 * 6);
  timer->CCER =
    TIM_CCER_CC1E | TIM_CCER_CC2E | TIM_CCER_CC3E | TIM_CCER_CC4E |
    TIM_CCER_CC1NE | TIM_CCER_CC2NE | TIM_CCER_CC3NE |
    TIM_CCER_CC1NP | TIM_CCER_CC2NP | TIM_CCER_CC3NP;

  timer->BDTR = TIM_BDTR_MOE;

  // Clear our comparison registers
  timer->CCR1 =
  timer->CCR2 =
  timer->CCR3 =
  timer->CCR4 = 0;

  // Configure (PWM edge-aligned, count up, 20khz)
  timer->PSC = 0;
  timer->ARR = MOTOR_MAX_POWER - 1;
  timer->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

void Motors::init() {
  // Setup motor power
  memset(&motorStatus, 0, sizeof(motorStatus));

  // Preconfigure the timers for the motor treads
  LN1::alternate(2);
  LN2::alternate(2);
  HN1::alternate(1);
  HN2::alternate(1);
  LTN1::alternate(2);
  LTN2::alternate(2);
  RTN1::alternate(2);
  RTN2::alternate(2);

  // Configure P pins
  LP1::type(TYPE_OPENDRAIN);
  HP1::type(TYPE_OPENDRAIN);
  RTP1::type(TYPE_OPENDRAIN);
  LTP1::type(TYPE_OPENDRAIN);

  LP1::reset();
  HP1::reset();
  RTP1::reset();
  LTP1::reset();

  LP1::mode(MODE_OUTPUT);
  HP1::mode(MODE_OUTPUT);
  RTP1::mode(MODE_OUTPUT);
  LTP1::mode(MODE_OUTPUT);

  configure_timer(TIM1);
  configure_timer(TIM3);
}

void Motors::stop() {
  LN1::mode(MODE_OUTPUT);
  LN2::mode(MODE_OUTPUT);
  HN1::mode(MODE_OUTPUT);
  HN2::mode(MODE_OUTPUT);
  RTN1::mode(MODE_OUTPUT);
  RTN2::mode(MODE_OUTPUT);
  LTN1::mode(MODE_OUTPUT);
  LTN2::mode(MODE_OUTPUT);
}

// Reset hysteresis values to Vector factory settings
// so that debug screen cursors work
void Motors::resetEncoderHysteresis() {
  motorStatus[MOTOR_LEFT].hysteresis_direction = DIRECTION_BACKWARD;
  motorStatus[MOTOR_RIGHT].hysteresis_direction = DIRECTION_BACKWARD;
}


static MotorDirection motorDirection(int power) {
  if (power > 0) {
    return DIRECTION_FORWARD;
  } else if (power < 0) {
    return DIRECTION_BACKWARD;
  } else {
    return DIRECTION_IDLE;
  }
}

// This treats 0 power as a 'transitional' state
// This can be optimized so that if in configured and direction has not changed, to reconfigure the pins, otherwise set power
void Motors::tick() {
  // Configure pins
  for(int i = 0; i < MOTOR_COUNT; i++) {
    MotorConfig* config =  (MotorConfig*) &MOTOR_DEF[i];
    MotorStatus* state = &motorStatus[i];
    MotorDirection direction = motorDirection(state->power);

    // Motor is configured and running
    if (state->direction == direction) {
      if (direction == DIRECTION_FORWARD) {
        *config->N1CC = state->power;
      } else {
        *config->N2CC = -state->power;
      }

      continue ;
    }

    // We need to change to a new direction on our motor
    switch (state->direction) {
      // Disable this motor channel if we are moving from anything other than idle
      default:
        *config->N1CC = 0;
        *config->N2CC = 0;

        // Set our Ns to 0 (should happen anyway)
        config->N2_Bank->MODER = (config->N2_Bank->MODER & config->N2_ModeMask) | config->N2_ModeOutput;
        config->N1_Bank->MODER = (config->N1_Bank->MODER & config->N1_ModeMask) | config->N1_ModeOutput;

        state->direction = DIRECTION_IDLE;
        break ;

      // We are transitioning out of 'idle' state
      case DIRECTION_IDLE:
        switch (direction) {
          case DIRECTION_FORWARD:
            *config->P_BSRR = config->P_Set;
            config->N1_Bank->MODER = (config->N1_Bank->MODER & config->N1_ModeMask) | config->N1_ModeAlt;
            break ;
          case DIRECTION_BACKWARD:
            *config->P_BSRR = config->P_Set << 16;
            config->N2_Bank->MODER = (config->N2_Bank->MODER & config->N2_ModeMask) | config->N2_ModeAlt;
            break ;
          default:
            break ;
        }

        state->direction = direction;
        state->hysteresis_direction = direction;
        break ;
    }
  }
}
