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
#include "diskio.h"
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
/* Proteus 8.17: do thuc te, timer/SysTick chay cham ~8 lan so voi 64MHz (UART van dung).
   Ban mo phong (-DPROTEUS) tinh timer theo 8MHz; ban mach that dung 64MHz. */
#ifdef PROTEUS
  #define TIMCLK 8000000u
#else
  #define TIMCLK 64000000u
#endif

volatile uint32_t ms_ticks;
volatile uint32_t st_ticks;
void SysTick_Handler(void){ st_ticks++; }                 /* chi de chan doan */
void TIM1_UP_IRQHandler(void){ TIM1->SR = 0; ms_ticks++; } /* 1 ms tick (TIM1) */
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
#ifndef DEBUG
  #define DEBUG 0          /* make ... CFLAGS+=-DDEBUG=1 de in thong tin chan doan [dbg] */
#endif
#define DLOG(...) do{ if(DEBUG) LOG(__VA_ARGS__); }while(0)

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
  TIM2->PSC = TIMCLK/1000000-1; TIM2->ARR = 0xFFFF; TIM2->EGR = 1; TIM2->CR1 = 1;  /* 1 MHz free-run (us) */
  TIM3->PSC = 0;  TIM3->ARR = TIMCLK/SAMPLE_RATE - 1; TIM3->DIER = 1; /* sample clock */
  NVIC_ISER0 = 1u<<29;
  TIM4->PSC = 0; TIM4->ARR = 255; TIM4->CCR1 = 128;                 /* 8-bit PWM 250kHz */
  TIM4->CCMR1 = (6u<<4)|(1u<<3); TIM4->CCER = 1; TIM4->EGR = 1; TIM4->CR1 = (1u<<7)|1;
  SYST_RVR = TIMCLK/1000-1; SYST_CVR = 0; SYST_CSR = 7;
  RCC_APB2ENR |= 1u<<11;                                             /* TIM1 */
  TIM1->PSC = TIMCLK/1000000-1; TIM1->ARR = 999; TIM1->EGR = 1; TIM1->SR = 0; TIM1->DIER = 1; TIM1->CR1 = 1;
  NVIC_ISER0 = 1u<<25;                                               /* TIM1_UP IRQ */
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
static uint16_t dht_ts[44]; static int dht_n, dht_err;
static uint32_t dht_start_ms = 30;
/* Doc DHT11 bang INPUT CAPTURE phan cung: PA1 = TIM2_CH2, bat moi suon xuong.
   Khoang cach 2 suon xuong = 50us thap + 26us (bit 0) hoac 70us (bit 1) -> ~76us / ~120us.
   Timer ghi thoi diem bang phan cung nen CPU (hay Proteus) cham cung khong sai. */
static int dht11_read(int *t, int *h){
  uint8_t d[5]={0}; int i; uint32_t el=0; uint16_t last;
  dht_n=0; dht_err=0;
  GPIOA->CRL = (GPIOA->CRL & ~(0xFu<<4)) | (0x7u<<4);       /* PA1 open-drain out */
  GPIOA->BRR = 1u<<1; delay_ms(dht_start_ms); GPIOA->BSRR = 1u<<1;
  GPIOA->CRL = (GPIOA->CRL & ~(0xFu<<4)) | (0x4u<<4);       /* PA1 input (TIM2_CH2) */
  TIM2->CCER &= ~(1u<<4);
  TIM2->CCMR1 = (TIM2->CCMR1 & ~(0xFFu<<8)) | (1u<<8);      /* CC2S=01: IC2 <- TI2 */
  TIM2->CCER |= (1u<<5) | (1u<<4);                          /* falling edge, enable */
  (void)TIM2->CCR2; TIM2->SR = 0;
  IRQ_OFF();
  last = TIM2->CNT;
  while(dht_n < 42){
    if(TIM2->SR & (1u<<2)){ dht_ts[dht_n++] = (uint16_t)TIM2->CCR2; }
    uint16_t now = TIM2->CNT; el += (uint16_t)(now-last); last = now;
    if(el > 15000){ dht_err = (dht_n==0)?1:11; break; }     /* 15 ms */
  }
  IRQ_ON();
  TIM2->CCER &= ~(1u<<4);
  GPIOA->CRL = (GPIOA->CRL & ~(0xFu<<4)) | (0x7u<<4);       /* tro lai open-drain, muc cao */
  if(TIM2->SR & (1u<<10)) DLOG("  [dbg] overcapture!\r\n");
  if(dht_n < 41) return dht_err ? -dht_err : -11;
  /* dht_ts[0] = DHT keo xuong (phan hoi), ts[1] = bat dau bit0, ... ts[41] = ket thuc bit39 */
  { int o = (dht_n >= 42) ? 1 : 0;   /* 41 suon: suon phan hoi da qua truoc khi bat -> ts[0] = bat dau bit0 */
    for(i=0;i<40;i++){ uint16_t p = (uint16_t)(dht_ts[i+1+o]-dht_ts[i+o]); d[i/8] = (d[i/8]<<1) | (p > 110); } }
  if((uint8_t)(d[0]+d[1]+d[2]+d[3])!=d[4]){ DLOG("  [dbg] checksum sai: %u %u %u %u %u\r\n",d[0],d[1],d[2],d[3],d[4]); return -4; }
  *h = d[0]; *t = d[2];
  return 0;
}

/* ---------------- Audio double buffer (ISR <-> main) ---------------- */
#ifdef PROTEUS
/* Proteus: CPU mo phong qua cham de vua ghi the vua lay mau -> ghi vao RAM truoc, xong moi ghi the */
#define SIM_RATE 4000u
#define SIM_BUF  14000u                     /* 3.5 s @ 4 kHz */
static uint8_t sbuf[SIM_BUF];
static volatile uint32_t a_pos, a_total;
static volatile uint8_t  mode;                /* 0 idle, 1 record, 2 play */
void TIM3_IRQHandler(void){
  TIM3->SR = 0;
  if(mode==1){ sbuf[a_pos++] = (uint8_t)(ADC1->DR >> 4); ADC1->CR2 |= 1u<<22; }
  else if(mode==2){ TIM4->CCR1 = sbuf[a_pos++]; }
  else return;
  if(a_pos >= a_total) mode = 0;
}
#else
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

#endif

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

#ifdef PROTEUS
static uint32_t rd32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static void do_record(void){
  static int idx = 0; uint8_t h[44]; FILINFO fi;
  do { snprintf(last_wav,sizeof last_wav,"REC%02d.WAV",idx++); } while(f_stat(last_wav,&fi)==FR_OK && idx<100);
  LOG("[REC] Dang ghi %s (%lu Hz, %lu ms) vao RAM...\r\n", last_wav,(unsigned long)SIM_RATE,(unsigned long)(SIM_BUF*1000u/SIM_RATE));
  a_pos=0; a_total=SIM_BUF;
  TIM3->ARR = TIMCLK/SIM_RATE - 1; ADC1->CR2 |= 1u<<22; TIM3->CNT=0; mode=1; TIM3->CR1=1;
  while(mode==1);
  TIM3->CR1=0;
  LOG("[REC] Da thu %lu mau, dang ghi ra the...\r\n",(unsigned long)a_pos);
  if(f_open(&fil,last_wav,FA_CREATE_ALWAYS|FA_WRITE)!=FR_OK){ LOG("[REC] Loi tao file\r\n"); return; }
  wav_header(h,a_pos,SIM_RATE); f_write(&fil,h,44,&bw);
  FRESULT w=f_write(&fil,sbuf,a_pos,&bw); FRESULT c=f_close(&fil);
  LOG("[REC] Xong: %s, %lu byte am thanh (%s)\r\n",last_wav,(unsigned long)bw,(w==FR_OK&&c==FR_OK)?"OK":"LOI");
}
static void do_play(const char *name){
  uint8_t h[44]; uint32_t rate=8000, len=0;
  if(f_open(&fil,name,FA_READ)!=FR_OK){ LOG("[PLAY] Khong thay %s\r\n",name); return; }
  f_read(&fil,h,12,&bw);
  if(memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)){ LOG("[PLAY] Khong phai WAV\r\n"); f_close(&fil); return; }
  for(;;){
    if(f_read(&fil,h,8,&bw)!=FR_OK || bw<8){ f_close(&fil); LOG("[PLAY] File loi\r\n"); return; }
    uint32_t sz = rd32(h+4);
    if(!memcmp(h,"fmt ",4)){ uint8_t f[16]; f_read(&fil,f,16,&bw); rate=rd32(f+4);
      if(f[2]!=1 || f[14]!=8){ LOG("[PLAY] Chi ho tro WAV 8-bit mono\r\n"); f_close(&fil); return; }
      if(sz>16) f_lseek(&fil,f_tell(&fil)+sz-16); }
    else if(!memcmp(h,"data",4)){ len=sz; break; }
    else f_lseek(&fil,f_tell(&fil)+sz);
  }
  if(len>SIM_BUF) len=SIM_BUF;
  LOG("[PLAY] %s: %lu Hz, doc %lu byte vao RAM...\r\n",name,(unsigned long)rate,(unsigned long)len);
  f_read(&fil,sbuf,len,&bw); f_close(&fil);
  a_pos=0; a_total=bw;
  TIM3->ARR = TIMCLK/rate - 1; TIM3->CNT=0; mode=2; TIM3->CR1=1;
  while(mode==2);
  TIM3->CR1=0; TIM4->CCR1=128;
  LOG("[PLAY] Xong (%lu mau)\r\n",(unsigned long)a_total);
}
#else
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
  TIM3->ARR = TIMCLK/rate - 1; TIM3->CNT=0; mode=2; TIM3->CR1=1;
  while(mode==2){
    for(int k=0;k<2;k++) if(a_ready[k]){ if(f_read(&fil,abuf[k],BUF_SZ,&bw)!=FR_OK || bw<BUF_SZ) memset(abuf[k]+bw,128,BUF_SZ-bw); a_ready[k]=0; }
  }
  TIM3->CR1=0; TIM3->ARR = TIMCLK/SAMPLE_RATE - 1; TIM4->CCR1=128;
  f_close(&fil);
  LOG("[PLAY] Xong\r\n");
}

#endif

/* ---------------- DHT11 -> DATA.CSV ---------------- */
static void log_dht(void){
  int t,h,r = dht11_read(&t,&h);
  if(r==-1){ static const uint16_t tryms[]={60,160,20}; /* tu thu xung start dai hon */
    for(int k=0;k<3 && r==-1;k++){ delay_ms(1200); dht_start_ms=tryms[k]; r=dht11_read(&t,&h);
      DLOG("  [dbg] thu start=%lums -> %d\r\n",(unsigned long)dht_start_ms,r); } }
  if(r){ LOG("[DHT11] Loi doc (%d), so suon=%d, khoang:",r,dht_n);
         if(DEBUG){ for(int k=1;k<dht_n && k<12;k++){ LOG(" %u",(uint16_t)(dht_ts[k]-dht_ts[k-1])); } } LOG("\r\n"); return; }
  LOG("[DHT11] t=%lus  Nhiet do=%dC  Do am=%d%%\r\n",(unsigned long)(ms_ticks/1000),t,h);
  if(f_open(&fil,"DATA.CSV",FA_OPEN_ALWAYS|FA_WRITE)!=FR_OK){ LOG("[SD] Loi mo DATA.CSV\r\n"); return; }
  DLOG("  [dbg] mo DATA.CSV: sclust=%lu size=%lu\r\n",(unsigned long)fil.sclust,(unsigned long)fil.fsize);
  if(fil.sclust==1 || fil.sclust>=fs.n_fatent || (fil.sclust==0 && fil.fsize)){   /* entry hong tu lan chay truoc */
    DLOG("  [dbg] DATA.CSV hong (sclust=%lu size=%lu) -> tao lai\r\n",(unsigned long)fil.sclust,(unsigned long)fil.fsize);
    fil.sclust=0; fil.fsize=0; fil.fptr=0; fil.clust=0; fil.flag |= FA__WRITTEN;
  }
  if(f_size(&fil)==0){ static const char hd[]="Thoi_gian_s,Nhiet_do_C,Do_am_%\r\n"; FRESULT w=f_write(&fil,hd,sizeof hd-1,&bw); if(w||bw!=sizeof hd-1) DLOG("  [dbg] ghi header loi %d bw=%u\r\n",w,bw); }
  f_lseek(&fil,f_size(&fil));
  snprintf(line,sizeof line,"%lu,%d,%d\r\n",(unsigned long)(ms_ticks/1000),t,h);
  { UINT L=strlen(line); FRESULT w=f_write(&fil,line,L,&bw); if(w||bw!=L) LOG("[SD] Loi ghi DATA.CSV (%d)\r\n",w); }
  { unsigned long sz=(unsigned long)f_size(&fil); FRESULT c=f_close(&fil); LOG("[SD] Da ghi DATA.CSV: %lu byte (%s)\r\n",sz,c==FR_OK?"OK":"LOI"); }
}

void sd_dbg(const char*s,int v){ DLOG("  [dbg] %s = 0x%02X\r\n",s,v); }
int main(void){
  clock_init(); gpio_init(); uart_init(); timers_init(); adc_init(); sd_spi_hw_init();
  LOG("\r\n=== BTL KTVXL - STM32F103 SD + DHT11 + Audio ===\r\n");
  FRESULT fr; int tries=0;
  while((fr=f_mount(&fs,"",1))!=FR_OK){ LOG("[SD] Mount loi %d, thu lai...\r\n",fr); delay_ms(1000); if(++tries>5) break; }
  if(fr==FR_OK) LOG("[SD] Mount OK. REC=PB12, PLAY=PB13\r\n");
#if DEBUG
  if(fr==FR_OK){ static BYTE sb[512];
    DLOG("  [dbg] type %d nfat %lu fatbase %lu dirbase %lu database %lu csize %d\r\n",fs.fs_type,(unsigned long)fs.n_fatent,(unsigned long)fs.fatbase,(unsigned long)fs.dirbase,(unsigned long)fs.database,fs.csize);
    { extern DWORD clust2sect(FATFS*,DWORD); DLOG("  [dbg] clust2sect(18) = %lu (mong 228)\r\n",(unsigned long)clust2sect(&fs,18)); }
    DLOG("  [dbg] (mong: type 2 nfat 16345 fatbase 4 dirbase 132 database 164 csize 4)\r\n");
    for(int pass=0;pass<2;pass++){ DRESULT dr=disk_read(0,sb,fs.fatbase,1);
      DLOG("  [dbg] doc FAT lan %d (%d):",pass,dr); for(int k=0;k<12;k++) LOG(" %02X",sb[k]); LOG("\r\n"); }
    DLOG("  [dbg] (mong: F8 FF FF FF 03 00 04 00 05 00 06 00)\r\n"); }
  { uint32_t a=ms_ticks, b=st_ticks; for(int k=0;k<100;k++){ uint16_t s0=TIM2->CNT; uint32_t g=200000; while((uint16_t)(TIM2->CNT-s0)<1000 && --g); }
    { uint16_t s1=TIM2->CNT; if(DEBUG) uart_puts("0123456789\r\n"); while(!(USART1->SR&(1u<<6))); DLOG("  [dbg] 12 ky tu UART = %u us (mong ~1040)\r\n",(uint16_t)(TIM2->CNT-s1)); }
    DLOG("  [dbg] 100ms: TIM1 tick +%lu, SysTick +%lu (mong ~100)\r\n",(unsigned long)(ms_ticks-a),(unsigned long)(st_ticks-b)); }
#endif
  uint32_t last = ms_ticks - LOG_PERIOD_MS + 1000;     /* lan doc dau sau 2s (DHT11 khoi dong) */
  uint32_t hb = ms_ticks;
  for(;;){
    if(ms_ticks - hb >= 1000){ hb = ms_ticks; DLOG("  [t=%lus]\r\n",(unsigned long)(ms_ticks/1000)); }
    if(ms_ticks - last >= LOG_PERIOD_MS){ last = ms_ticks; GPIOC->BRR=1u<<13; log_dht(); GPIOC->BSRR=1u<<13; }
    if(!(GPIOB->IDR & (1u<<12))){ delay_ms(30); if(!(GPIOB->IDR&(1u<<12))){ GPIOC->BRR=1u<<13; do_record(); GPIOC->BSRR=1u<<13; while(!(GPIOB->IDR&(1u<<12))); } }
    if(!(GPIOB->IDR & (1u<<13))){ delay_ms(30); if(!(GPIOB->IDR&(1u<<13))){ GPIOC->BRR=1u<<13; do_play(last_wav); GPIOC->BSRR=1u<<13; while(!(GPIOB->IDR&(1u<<13))); } }
  }
}
