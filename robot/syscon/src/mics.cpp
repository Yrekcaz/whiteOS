#include <string.h>

#include "common.h"
#include "hardware.h"
#include "timer.h"

#include "mics.h"

#include "messages.h"

extern "C" {
  void start_mic_spi(int16_t a, int16_t b, void* tim);
  void dec_odd(int32_t* acc, const uint16_t* samples, int16_t* output);
  void dec_even(int32_t* acc, const uint16_t* samples, int16_t* output);
}

const int SAMPLES_PER_IRQ = 20;
static const int IRQS_PER_FRAME = AUDIO_SAMPLES_PER_FRAME / SAMPLES_PER_IRQ;
static const int PDM_BYTES_PER_IRQ = SAMPLES_PER_IRQ * AUDIO_DECIMATION * 2 / 8;
  
static int16_t audio_data[2][AUDIO_SAMPLES_PER_FRAME * 4];
static uint16_t pdm_data[2][2][PDM_BYTES_PER_IRQ / 2];
static int sample_index;
static bool reduced;

static int16_t MIC_SPI_CR1 = 0
           | SPI_CR1_MSTR                 // Master
           | SPI_CR1_RXONLY               // Read only
           | SPI_CR1_SSM                  // Software slave
           | SPI_CR1_SSI                  // Slave selected
           | SPI_CR1_LSBFIRST             // LSB First
           | (SPI_CR1_BR_0 * AUDIO_SHIFT) // Prescale
           | SPI_CR1_SPE
           ;

static const int BUFFER_WORDS = sizeof(pdm_data[0]) / sizeof(uint16_t);

static void configurePerf(SPI_TypeDef* spi, DMA_Channel_TypeDef* ch, int perf) {
  ch->CCR = 0;
  ch->CPAR = (uint32_t) &spi->DR;
  ch->CMAR = (uint32_t) &pdm_data[perf];
  ch->CNDTR = BUFFER_WORDS;
  ch->CCR = 0
          | DMA_CCR_MINC      // Memory is incrementing
          | DMA_CCR_PSIZE_0   // 16-bit mode
          | DMA_CCR_MSIZE_0
          | DMA_CCR_CIRC      // Circular mode
          | DMA_CCR_PL        // Highest Priority
          | DMA_CCR_EN        // Enabled
          ;

  spi->CR2 = 0
           | SPI_CR2_DS            // 16-Bit word
           | SPI_CR2_RXDMAEN       // RX DMA Enable
           ;
}

void Mics::init(void) {
  sample_index = 0;

  // Set our MISO lines to SPI1 and SPI2
  MIC1_MISO::alternate(0);
  MIC1_MISO::mode(MODE_ALTERNATE);

  MIC2_MISO::alternate(0);
  MIC2_MISO::mode(MODE_ALTERNATE);

  // Setup our output clock to TIM15
  MIC_LR::alternate(1);
  MIC_LR::mode(MODE_ALTERNATE);

  // Set and output clock for the SPI perf so reads work (not connected)
  MIC1_SCK::alternate(0);
  MIC1_SCK::mode(MODE_ALTERNATE);

  MIC2_SCK::alternate(0);
  MIC2_SCK::mode(MODE_ALTERNATE);

  // Start configuring out clock
  TIM15->PSC = 0;
  TIM15->CNT = 0;
  TIM15->ARR = AUDIO_PRESCALE * 2 - 1;
  TIM15->CCR2 = AUDIO_PRESCALE;
  TIM15->CCMR1 = TIM_CCMR1_OC2PE | TIM_CCMR1_OC2FE | (TIM_CCMR1_OC2M_0 * 6);
  TIM15->CCER = TIM_CCER_CC2E;
  TIM15->BDTR = TIM_BDTR_MOE;

  // Configure PERFs (but don't start them)
  configurePerf(SPI1, DMA1_Channel2, 0);
  configurePerf(SPI2, DMA1_Channel4, 1);

  // Enable IRQs on channel 2 only, it will do double duty
  DMA1_Channel2->CCR |= 0
                     | DMA_CCR_HTIE
                     | DMA_CCR_TCIE
                     ;

  NVIC_SetPriority(DMA1_Channel2_3_IRQn, PRIORITY_MICS);

  reduced = false;

  start_mic_spi(TIM_CR1_CEN, MIC_SPI_CR1, (void*)&TIM15->CR1);
}

void Mics::start(void) {
  DMA1->IFCR = DMA_IFCR_CGIF2;
  NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);
}

void Mics::stop(void) {
  NVIC_DisableIRQ(DMA1_Channel2_3_IRQn);
}

void Mics::reduce(bool reduce) {
  reduced = reduce;
}

void Mics::errorCode(uint16_t* data) {
  data[0] = *(uint16_t*)&pdm_data[0][0];
  data[1] = *(uint16_t*)&pdm_data[1][0];
}

void Mics::transmit(int16_t* payload) {
  memcpy(payload, audio_data[sample_index < IRQS_PER_FRAME ? 1 : 0], sizeof(audio_data[0]));
}

static void decimate(const uint16_t* input, int32_t* acc, int16_t* output) {
  dec_odd(&acc[0], input, &output[0]);
  if (!reduced) dec_even(&acc[2], input, &output[1]);
}

extern "C" void DMA1_Channel2_3_IRQHandler(void) {
  static int16_t *output = audio_data[0];
  uint32_t isr = DMA1->ISR;
  DMA1->IFCR = DMA_IFCR_CGIF2;

  static int32_t accumulator[2][4]; // 2 data lines, 2x2 channel accumulators

  // Note: if this falls behind, it will drop a bunch of samples
  if (isr & DMA_ISR_HTIF2) {
    decimate(pdm_data[0][0], accumulator[0], &output[0]);
    if (!reduced) decimate(pdm_data[1][0], accumulator[1], &output[2]);
    output += SAMPLES_PER_IRQ * 4;
    sample_index++;
  }

  if (isr & DMA_ISR_TCIF2) {
    decimate(pdm_data[0][1], accumulator[0], &output[0]);
    if (!reduced) decimate(pdm_data[1][1], accumulator[1], &output[2]);
    output += SAMPLES_PER_IRQ * 4;
    sample_index++;
  }

  // Circular buffer increment
  if (sample_index >= IRQS_PER_FRAME * 2) {
    output = audio_data[0];
    sample_index = 0;
  }
}
