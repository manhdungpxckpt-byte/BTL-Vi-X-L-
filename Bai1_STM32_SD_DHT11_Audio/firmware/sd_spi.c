/* SD card driver over SPI1 (PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI) + FatFs diskio glue */
#include "stm32f103.h"
#include "fatfs/diskio.h"

extern volatile uint32_t ms_ticks;
static uint8_t card_type; /* 0=none, 1=MMC, 2=SDv1, 4=SDv2(SDSC), 8=block addr (SDHC) */
static DSTATUS stat = STA_NOINIT;

extern void sd_dbg(const char*,int);
#define CS_LOW()  (GPIOA->BRR  = 1u<<4)
#define CS_HIGH() (GPIOA->BSRR = 1u<<4)

/* Software (bit-bang) SPI mode 0 - avoids Proteus SPI1 peripheral model issues */
static volatile int spi_slow = 1;
static inline void bdelay(void){ if(spi_slow){ for(volatile int i=0;i<4;i++); } }
static uint8_t spi_xfer(uint8_t b){
  uint8_t r = 0;
  for(int i=0;i<8;i++){
    if(b & 0x80) GPIOA->BSRR = 1u<<7; else GPIOA->BRR = 1u<<7;   /* MOSI */
    b <<= 1; bdelay();
    GPIOA->BSRR = 1u<<5; bdelay();                              /* SCK high */
    r = (r<<1) | ((GPIOA->IDR >> 6) & 1);                       /* sample MISO */
    GPIOA->BRR = 1u<<5;                                         /* SCK low */
  }
  return r;
}
static void spi_speed(int fast){ spi_slow = !fast; }
static int wait_ready(uint32_t t){
  uint32_t s = ms_ticks;
  do { if(spi_xfer(0xFF)==0xFF) return 1; } while(ms_ticks - s < t);
  return 0;
}
static void deselect(void){ CS_HIGH(); spi_xfer(0xFF); }
static int  select_(void){ CS_LOW(); spi_xfer(0xFF); wait_ready(50); return 1; } /* lenient for Proteus */

static uint8_t send_cmd(uint8_t cmd, uint32_t arg){
  uint8_t r, n;
  if(cmd & 0x80){ cmd &= 0x7F; r = send_cmd(55,0); if(r>1) return r; }  /* ACMD */
  deselect(); if(!select_()) return 0xFF;
  spi_xfer(0x40|cmd); spi_xfer(arg>>24); spi_xfer(arg>>16); spi_xfer(arg>>8); spi_xfer(arg);
  n = 0x01; if(cmd==0) n=0x95; if(cmd==8) n=0x87;
  spi_xfer(n);
  if(cmd==12) spi_xfer(0xFF);
  n = 10; do r = spi_xfer(0xFF); while((r & 0x80) && --n);
  return r;
}

void sd_spi_hw_init(void){
  RCC_APB2ENR |= (1u<<2);                                    /* GPIOA */
  GPIOA->CRL = (GPIOA->CRL & ~(0xFFFFu<<16)) | (0x3u<<16) | (0x3u<<20) | (0x8u<<24) | (0x3u<<28); /* PA4 CS out, PA5 SCK out, PA6 MISO in-PU, PA7 MOSI out */
  GPIOA->ODR |= (1u<<6);
  GPIOA->BRR = 1u<<5;
  CS_HIGH();
}

DSTATUS disk_initialize(BYTE pdrv){
  uint8_t n, ty = 0, ocr[4]; uint32_t t;
  if(pdrv) return STA_NOINIT;
  spi_speed(0);
  CS_HIGH(); for(n=0;n<10;n++) spi_xfer(0xFF);   /* 80 dummy clocks */
  sd_dbg("MISO idle (CS high, mong 0xFF)", spi_xfer(0xFF));
  { uint8_t r0=0xFF; for(n=0;n<10 && r0!=1;n++) r0=send_cmd(0,0); sd_dbg("CMD0",r0); if(r0==1) goto idle; }
  if(0){ idle:
    t = ms_ticks;
    uint8_t r8=send_cmd(8,0x1AA); sd_dbg("CMD8",r8);
    if(r8==1){                     /* SD v2 */
      for(n=0;n<4;n++) ocr[n]=spi_xfer(0xFF);
      if(ocr[2]==0x01 && ocr[3]==0xAA){
        while(ms_ticks-t<1000 && send_cmd(0x80|41,1UL<<30));
        if(ms_ticks-t<1000 && send_cmd(58,0)==0){
          for(n=0;n<4;n++) ocr[n]=spi_xfer(0xFF);
          ty = (ocr[0]&0x40) ? (4|8) : 4;
        }
      }
    } else {                                      /* SD v1 or MMC (Proteus MMC model) */
      uint8_t c;
      if(send_cmd(0x80|41,0)<=1){ ty=2; c=0x80|41; } else { ty=1; c=1; }
      sd_dbg(ty==2?"type SDv1":"type MMC",ty);
      { uint8_t r; while((r=send_cmd(c,0)) && ms_ticks-t<3000); sd_dbg("init",r); }
      { uint8_t r16=send_cmd(16,512); sd_dbg("CMD16",r16); if(ms_ticks-t>=3000 || r16!=0) ty=0; }
    }
  }
  card_type = ty; deselect();
  if(ty){ spi_speed(1); stat &= ~STA_NOINIT; } else stat = STA_NOINIT;
  return stat;
}
DSTATUS disk_status(BYTE pdrv){ return pdrv ? STA_NOINIT : stat; }

static int rcvr_block(BYTE *buf, UINT n){
  uint8_t tk; uint32_t s = ms_ticks;
  do tk = spi_xfer(0xFF); while(tk==0xFF && ms_ticks-s<200);
  if(tk!=0xFE) return 0;
  while(n--) *buf++ = spi_xfer(0xFF);
  spi_xfer(0xFF); spi_xfer(0xFF);
  return 1;
}
static int xmit_block(const BYTE *buf, uint8_t tk){
  if(!wait_ready(500)) return 0;
  spi_xfer(tk);
  if(tk!=0xFD){
    for(int i=0;i<512;i++) spi_xfer(buf[i]);
    spi_xfer(0xFF); spi_xfer(0xFF);
    { uint8_t dr=spi_xfer(0xFF); if((dr&0x1F)!=0x05){ sd_dbg("data resp",dr); return 0; } }
  }
  return 1;
}
DRESULT disk_read(BYTE pdrv, BYTE *buf, DWORD sector, UINT count){
  if(pdrv || !count) return RES_PARERR;
  if(stat & STA_NOINIT) return RES_NOTRDY;
  if(!(card_type & 8)) sector *= 512;
  while(count){
    if(send_cmd(17,sector)!=0 || !rcvr_block(buf,512)) break;
    buf += 512; sector += (card_type&8)?1:512; count--;
  }
  deselect();
  return count ? RES_ERROR : RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buf, DWORD sector, UINT count){
  if(pdrv || !count) return RES_PARERR;
  if(stat & STA_NOINIT) return RES_NOTRDY;
  if(!(card_type & 8)) sector *= 512;
  while(count){
    { uint8_t r=send_cmd(24,sector); if(r!=0){ sd_dbg("CMD24 loi",r); break; } }
    if(!xmit_block(buf,0xFE)){ sd_dbg("xmit_block loi, sector",(int)(sector&0xFF)); break; }
    buf += 512; sector += (card_type&8)?1:512; count--;
  }
  deselect();
  sd_dbg(count?"disk_write LOI sector":"disk_write ok sector",(int)(sector/((card_type&8)?1:512)) & 0xFF);
  return count ? RES_ERROR : RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff){
  (void)buff;
  if(pdrv) return RES_PARERR;
  if(cmd==CTRL_SYNC){ int ok; CS_LOW(); ok=wait_ready(500); deselect(); return ok?RES_OK:RES_ERROR; }
  return RES_PARERR;
}
