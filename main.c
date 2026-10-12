/*
 * BTL Ky thuat Vi xu ly - Bai 1 (a, b, c) - STM32F103C8T6, viet truc tiep thanh ghi (khong HAL)
 *  a) DHT11 (PA1) -> moi 5s ghi nhiet do/do am vao DATA.CSV tren the SD
 *  b) Micro MAX9814 (PA0/ADC1_IN0) -> nhan nut REC (PB12) ghi 5s am thanh -> RECxx.WAV (8kHz, 8bit, mono)
 *  c) Nhan nut PLAY (PB13) -> phat file WAV vua ghi ra PB6 (TIM4_CH1 PWM) -> loc RC -> PAM8403 -> loa
 *  The SD: SPI1 PA4=CS PA5=SCK PA6=MISO PA7=MOSI ; UART1 PA9=TX PA10=RX 115200 (log) ; LED PC13
 *  Clock: HSI 8MHz/2 x16 = 64MHz (khong can thach anh -> chay de dang tren Proteus)
 */
#include <stdio.h>
#include <string.h>
#include "stm32f103.h"
#include "fatfs/ff.h"

#if defined(__CC_ARM)            /* Keil ARM Compiler 5 */
  #define IRQ_OFF() __disable_irq()
  #define IRQ_ON()  __enable_irq()
#else                            /* GCC / ARM Compiler 6 */
  #define IRQ_OFF() __asm volatile("cpsid i")
  #define IRQ_ON()  __asm volatile("cpsie i")
#endif

#define SAMPLE_RATE   8000
#define REC_SECONDS   5
#define BUF_SZ        512
#define LOG_PERIOD_MS 5000

volatile uint32_t ms_ticks;
void SysTick_Handler(void){ ms_ticks++; }
void sd_spi_hw_init(void);

/* ---------------- UART log ---------------- */
static void uart_init(void){
  RCC_APB2ENR |= (1u<<14);
  USART1->BRR = 64000000/115200;
  USART1->CR1 = (1u<<13)|(1u<<3)|(1u<<2);
}
static void uart_puts(const char *s){ while(*s){ while(!(USART1->SR & (1u<<7))); USART1->DR = *s++; } }
static char line[96];
#define LOG(...) do{ snprintf(line,sizeof line,__VA_ARGS__); uart_puts(line); }while(0)

/* ---------------- Clock / GPIO / timers ---------------- */
static void clock_init(void){
  FLASH_ACR = 0x12;                               /* prefetch + 2 wait states */
  RCC_CFGR = (14u<<18) | (4u<<8) | (2u<<14);      /* PLLMUL x16, HSI/2 src, APB1 /2, ADC /6 */
  RCC_CR |= 1u<<24; while(!(RCC_CR & (1u<<25)));  /* PLL on */
  RCC_CFGR |= 2u; while(((RCC_CFGR>>2)&3)!=2);    /* SYSCLK = PLL 64MHz */
}
static void gpio_init(void){
  RCC_APB2ENR |= (1u<<0)|(1u<<2)|(1u<<3)|(1u<<4); /* AFIO, GPIOA, B, C */
  /* PA0 analog, PA1 open-drain out, PA4 out PP, PA5 AF, PA6 in pull-up, PA7 AF */
  GPIOA->CRL = (0x0u<<0)|(0x7u<<4)|(0x4u<<8)|(0x4u<<12)|(0x3u<<16)|(0xBu<<20)|(0x8u<<24)|(0xBu<<28);
  GPIOA->CRH = (GPIOA->CRH & ~0xFF0u) | (0xBu<<4) | (0x4u<<8);   /* PA9 TX AF, PA10 RX in */
  GPIOA->ODR |= (1u<<1)|(1u<<4)|(1u<<6);
  GPIOB->CRL = (GPIOB->CRL & ~(0xFu<<24)) | (0xBu<<24);           /* PB6 TIM4_CH1 AF PP */
  GPIOB->CRH = (GPIOB->CRH & ~(0xFFu<<16)) | (0x88u<<16);         /* PB12, PB13 in pull-up */
  GPIOB->ODR |= (1u<<12)|(1u<<13);
  GPIOC->CRH = (GPIOC->CRH & ~(0xFu<<20)) | (0x2u<<20);           /* PC13 LED */
  GPIOC->BSRR = 1u<<13;
}
static void timers_init(void){
  RCC_APB1ENR |= (1u<<0)|(1u<<1)|(1u<<2);         /* TIM2, TIM3, TIM4 */
  TIM2->PSC = 63; TIM2->ARR = 0xFFFF; TIM2->EGR = 1; TIM2->CR1 = 1;  /* 1 MHz free-run (us) */
  TIM3->PSC = 0;  TIM3->ARR = 64000000/SAMPLE_RATE - 1; TIM3->DIER = 1; /* sample clock */
  NVIC_ISER0 = 1u<<29;
  TIM4->PSC = 0; TIM4->ARR = 255; TIM4->CCR1 = 128;                 /* 8-bit PWM 250kHz */
  TIM4->CCMR1 = (6u<<4)|(1u<<3); TIM4->CCER = 1; TIM4->EGR = 1; TIM4->CR1 = (1u<<7)|1;
  SYST_RVR = 64000-1; SYST_CVR = 0; SYST_CSR = 7;                    /* 1 ms tick */
}
static void adc_init(void){
  RCC_APB2ENR |= 1u<<9;
  ADC1->SMPR2 = 4u;                 /* ch0: 41.5 cycles */
  ADC1->SQR3 = 0;                   /* channel 0 = PA0 */
  ADC1->CR2 = 1; for(volatile int i=0;i<1000;i++);
  ADC1->CR2 |= 1u<<3; while(ADC1->CR2 & (1u<<3));   /* calibrate */
  ADC1->CR2 |= 1u<<2; while(ADC1->CR2 & (1u<<2));
  ADC1->CR2 |= (7u<<17)|(1u<<20);   /* EXTSEL = SWSTART, EXTTRIG */
}

static void delay_ms(uint32_t ms){ uint32_t s=ms_ticks; while(ms_ticks-s<ms); }

/* ---------------- DHT11 on PA1 ---------------- */
#define DHT_IN() (GPIOA->IDR & (1u<<1))
static int wait_level(int lvl, uint16_t to){ uint16_t s=TIM2->CNT; while((DHT_IN()?1:0)!=lvl){ if((uint16_t)(TIM2->CNT-s)>to) return -1; } return (uint16_t)(TIM2->CNT-s); }
static int dht11_read(int *t, int *h){
  uint8_t d[5]={0};
  GPIOA->BRR = 1u<<1; delay_ms(20); GPIOA->BSRR = 1u<<1;   /* start signal */
  IRQ_OFF();
  if(wait_level(0,100)<0 || wait_level(1,100)<0 || wait_level(0,100)<0){ IRQ_ON(); return -1; }
  for(int i=0;i<40;i++){
    if(wait_level(1,80)<0){ IRQ_ON(); return -2; }
    int w = wait_level(0,100); if(w<0){ IRQ_ON(); return -3; }
    d[i/8] = (d[i/8]<<1) | (w>40);      /* ~26us = 0, ~70us = 1 */
  }
  IRQ_ON();
  if((uint8_t)(d[0]+d[1]+d[2]+d[3])!=d[4]) return -4;
  *h = d[0]; *t = d[2];
  return 0;
}

/* ---------------- Audio double buffer (ISR <-> main) ---------------- */
static uint8_t abuf[2][BUF_SZ];
static volatile uint8_t  a_cur, a_ready[2];   /* rec: ready = full ; play: ready = needs refill */
static volatile uint16_t a_pos;
static volatile uint32_t a_count, a_total;
static volatile uint8_t  mode;                /* 0 idle, 1 record, 2 play */

void TIM3_IRQHandler(void){
  TIM3->SR = 0;
  if(mode==1){
    abuf[a_cur][a_pos++] = (uint8_t)(ADC1->DR >> 4);   /* 12bit -> 8bit unsigned PCM */
    ADC1->CR2 |= 1u<<22;                               /* start next conversion */
  } else if(mode==2){
    TIM4->CCR1 = abuf[a_cur][a_pos++];
  } else return;
  if(++a_count >= a_total){ mode = 0; }
  if(a_pos >= BUF_SZ){ a_ready[a_cur]=1; a_cur^=1; a_pos=0; }
}

/* ---------------- WAV helpers ---------------- */
static FATFS fs; static FIL fil; static UINT bw;
static void put32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void wav_header(uint8_t *h, uint32_t datalen, uint32_t rate){
  memcpy(h,"RIFF",4); put32(h+4,36+datalen); memcpy(h+8,"WAVEfmt ",8);
  put32(h+16,16); h[20]=1; h[21]=0; h[22]=1; h[23]=0;     /* PCM, mono */
  put32(h+24,rate); put32(h+28,rate); h[32]=1; h[33]=0; h[34]=8; h[35]=0; /* 8 bit */
  memcpy(h+36,"data",4); put32(h+40,datalen);
}
static char last_wav[13] = "PLAY.WAV";   /* chua ghi am -> phat file mau PLAY.WAV */

static void do_record(void){
  static int idx = 0; uint8_t h[44]; FILINFO fi;
  do { snprintf(last_wav,sizeof last_wav,"REC%02d.WAV",idx++); } while(f_stat(last_wav,&fi)==FR_OK && idx<100);
  if(f_open(&fil,last_wav,FA_CREATE_ALWAYS|FA_WRITE)!=FR_OK){ LOG("[REC] Loi tao file\r\n"); return; }
  wav_header(h,0,SAMPLE_RATE); f_write(&fil,h,44,&bw);
  LOG("[REC] Dang ghi %s (%ds)...\r\n", last_wav, REC_SECONDS);
  a_cur=0; a_pos=0; a_ready[0]=a_ready[1]=0; a_count=0; a_total=SAMPLE_RATE*REC_SECONDS;
  ADC1->CR2 |= 1u<<22; mode=1; TIM3->CNT=0; TIM3->CR1=1;
  uint32_t written=0, overrun=0;
  while(mode==1 || a_ready[0] || a_ready[1]){
    for(int k=0;k<2;k++) if(a_ready[k]){ f_write(&fil,abuf[k],BUF_SZ,&bw); written+=bw; a_ready[k]=0; if(a_ready[k^1]) overrun++; }
    if(mode!=1 && !a_ready[0] && !a_ready[1]) break;
  }
  TIM3->CR1=0;
  if(a_pos){ f_write(&fil,abuf[a_cur],a_pos,&bw); written+=bw; }
  wav_header(h,written,SAMPLE_RATE); f_lseek(&fil,0); f_write(&fil,h,44,&bw);
  f_close(&fil);
  LOG("[REC] Xong: %lu byte, overrun=%lu\r\n",(unsigned long)written,(unsigned long)overrun);
}

static uint32_t rd32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static void do_play(const char *name){
  uint8_t h[44]; uint32_t rate=8000, len=0;
  if(f_open(&fil,name,FA_READ)!=FR_OK){ LOG("[PLAY] Khong thay %s\r\n",name); return; }
  f_read(&fil,h,12,&bw);
  if(memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)){ LOG("[PLAY] Khong phai WAV\r\n"); f_close(&fil); return; }
  for(;;){                                         /* tim chunk fmt / data */
    if(f_read(&fil,h,8,&bw)!=FR_OK || bw<8){ f_close(&fil); return; }
    uint32_t sz = rd32(h+4);
    if(!memcmp(h,"fmt ",4)){ uint8_t f[16]; f_read(&fil,f,16,&bw); rate=rd32(f+4);
      if(f[2]!=1 || f[14]!=8){ LOG("[PLAY] Chi ho tro WAV 8-bit mono\r\n"); f_close(&fil); return; }
      if(sz>16) f_lseek(&fil,f_tell(&fil)+sz-16); }
    else if(!memcmp(h,"data",4)){ len=sz; break; }
    else f_lseek(&fil,f_tell(&fil)+sz);
  }
  LOG("[PLAY] %s: %lu Hz, %lu byte\r\n",name,(unsigned long)rate,(unsigned long)len);
  f_read(&fil,abuf[0],BUF_SZ,&bw); f_read(&fil,abuf[1],BUF_SZ,&bw);
  a_cur=0; a_pos=0; a_ready[0]=a_ready[1]=0; a_count=0; a_total=len;
  TIM3->ARR = 64000000/rate - 1; TIM3->CNT=0; mode=2; TIM3->CR1=1;
  while(mode==2){
    for(int k=0;k<2;k++) if(a_ready[k]){ if(f_read(&fil,abuf[k],BUF_SZ,&bw)!=FR_OK || bw<BUF_SZ) memset(abuf[k]+bw,128,BUF_SZ-bw); a_ready[k]=0; }
  }
  TIM3->CR1=0; TIM3->ARR = 64000000/SAMPLE_RATE - 1; TIM4->CCR1=128;
  f_close(&fil);
  LOG("[PLAY] Xong\r\n");
}

/* ---------------- DHT11 -> DATA.CSV ---------------- */
static void log_dht(void){
  int t,h,r = dht11_read(&t,&h);
  if(r){ LOG("[DHT11] Loi doc (%d)\r\n",r); return; }
  LOG("[DHT11] t=%lus  Nhiet do=%dC  Do am=%d%%\r\n",(unsigned long)(ms_ticks/1000),t,h);
  if(f_open(&fil,"DATA.CSV",FA_OPEN_ALWAYS|FA_WRITE)!=FR_OK){ LOG("[SD] Loi mo DATA.CSV\r\n"); return; }
  if(f_size(&fil)==0) f_puts("Thoi_gian_s,Nhiet_do_C,Do_am_%\r\n",&fil);
  f_lseek(&fil,f_size(&fil));
  snprintf(line,sizeof line,"%lu,%d,%d\r\n",(unsigned long)(ms_ticks/1000),t,h);
  f_puts(line,&fil);
  f_close(&fil);
}

int main(void){
  clock_init(); gpio_init(); uart_init(); timers_init(); adc_init(); sd_spi_hw_init();
  LOG("\r\n=== BTL KTVXL - STM32F103 SD + DHT11 + Audio ===\r\n");
  FRESULT fr; int tries=0;
  while((fr=f_mount(&fs,"",1))!=FR_OK){ LOG("[SD] Mount loi %d, thu lai...\r\n",fr); delay_ms(1000); if(++tries>5) break; }
  if(fr==FR_OK) LOG("[SD] Mount OK. REC=PB12, PLAY=PB13\r\n");
  uint32_t last = ms_ticks - LOG_PERIOD_MS + 2000;     /* lan doc dau sau 2s (DHT11 khoi dong) */
  for(;;){
    if(ms_ticks - last >= LOG_PERIOD_MS){ last = ms_ticks; GPIOC->BRR=1u<<13; log_dht(); GPIOC->BSRR=1u<<13; }
    if(!(GPIOB->IDR & (1u<<12))){ delay_ms(30); if(!(GPIOB->IDR&(1u<<12))){ GPIOC->BRR=1u<<13; do_record(); GPIOC->BSRR=1u<<13; while(!(GPIOB->IDR&(1u<<12))); } }
    if(!(GPIOB->IDR & (1u<<13))){ delay_ms(30); if(!(GPIOB->IDR&(1u<<13))){ GPIOC->BRR=1u<<13; do_play(last_wav); GPIOC->BSRR=1u<<13; while(!(GPIOB->IDR&(1u<<13))); } }
  }
}
