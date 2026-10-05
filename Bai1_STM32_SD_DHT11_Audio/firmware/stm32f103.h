/* Minimal register definitions STM32F103 (no HAL needed) */
#include <stdint.h>
#define R(a) (*(volatile uint32_t*)(a))
#define RCC_CR R(0x40021000)
#define RCC_CFGR R(0x40021004)
#define RCC_APB2ENR R(0x40021018)
#define RCC_APB1ENR R(0x4002101C)
#define FLASH_ACR R(0x40022000)
typedef struct { volatile uint32_t CRL,CRH,IDR,ODR,BSRR,BRR,LCKR; } GPIO_t;
#define GPIOA ((GPIO_t*)0x40010800)
#define GPIOB ((GPIO_t*)0x40010C00)
#define GPIOC ((GPIO_t*)0x40011000)
typedef struct { volatile uint32_t CR1,CR2,SR,DR,CRCPR,RXCRCR,TXCRCR; } SPI_t;
#define SPI1 ((SPI_t*)0x40013000)
typedef struct { volatile uint32_t SR,DR,BRR,CR1,CR2,CR3,GTPR; } USART_t;
#define USART1 ((USART_t*)0x40013800)
typedef struct { volatile uint32_t CR1,CR2,SMCR,DIER,SR,EGR,CCMR1,CCMR2,CCER,CNT,PSC,ARR,RCR,CCR1,CCR2,CCR3,CCR4,BDTR,DCR,DMAR; } TIM_t;
#define TIM1 ((TIM_t*)0x40012C00)
#define TIM2 ((TIM_t*)0x40000000)
#define TIM3 ((TIM_t*)0x40000400)
#define TIM4 ((TIM_t*)0x40000800)
typedef struct { volatile uint32_t SR,CR1,CR2,SMPR1,SMPR2,JOFR[4],HTR,LTR,SQR1,SQR2,SQR3,JSQR,JDR[4],DR; } ADC_t;
#define ADC1 ((ADC_t*)0x40012400)
#define NVIC_ISER0 R(0xE000E100)
#define SYST_CSR R(0xE000E010)
#define SYST_RVR R(0xE000E014)
#define SYST_CVR R(0xE000E018)
