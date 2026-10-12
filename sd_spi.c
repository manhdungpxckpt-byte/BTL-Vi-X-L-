/* SD card driver over SPI1 (PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI) + FatFs diskio glue */
#include "stm32f103.h"
#include "fatfs/diskio.h"

extern volatile uint32_t ms_ticks;
static uint8_t card_type; /* 0=none, 1=MMC, 2=SDv1, 4=SDv2(SDSC), 8=block addr (SDHC) */
static DSTATUS stat = STA_NOINIT;

#define CS_LOW()  (GPIOA->BRR  = 1u<<4)
#define CS_HIGH() (GPIOA->BSRR = 1u<<4)

static uint8_t spi_xfer(uint8_t b){
  while(!(SPI1->SR & 2));          /* TXE */
  SPI1->DR = b;
  while(!(SPI1->SR & 1));          /* RXNE */
  return (uint8_t)SPI1->DR;
}
static void spi_speed(int fast){
  SPI1->CR1 &= ~(1u<<6);
  SPI1->CR1 = (SPI1->CR1 & ~(7u<<3)) | ((fast ? 2u : 7u) << 3); /* /8 = 8MHz  or /256 = 250kHz */
  SPI1->CR1 |= 1u<<6;
}
static int wait_ready(uint32_t t){
  uint32_t s = ms_ticks;
  do { if(spi_xfer(0xFF)==0xFF) return 1; } while(ms_ticks - s < t);
  return 0;
}
static void deselect(void){ CS_HIGH(); spi_xfer(0xFF); }
static int  select_(void){ CS_LOW(); spi_xfer(0xFF); if(wait_ready(500)) return 1; deselect(); return 0; }

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
  RCC_APB2ENR |= (1u<<12) | (1u<<2);           /* SPI1, GPIOA */
  SPI1->CR1 = (1u<<2) | (1u<<9) | (1u<<8) | (7u<<3); /* master, SSM, SSI, /256 */
  SPI1->CR1 |= 1u<<6;
  CS_HIGH();
}

DSTATUS disk_initialize(BYTE pdrv){
  uint8_t n, ty = 0, ocr[4]; uint32_t t;
  if(pdrv) return STA_NOINIT;
  spi_speed(0);
  CS_HIGH(); for(n=0;n<10;n++) spi_xfer(0xFF);   /* 80 dummy clocks */
  if(send_cmd(0,0)==1){                           /* GO_IDLE */
    t = ms_ticks;
    if(send_cmd(8,0x1AA)==1){                     /* SD v2 */
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
      while(ms_ticks-t<1000 && send_cmd(c,0));
      if(ms_ticks-t>=1000 || send_cmd(16,512)!=0) ty=0;
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
    if((spi_xfer(0xFF)&0x1F)!=0x05) return 0;
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
    if(send_cmd(24,sector)!=0 || !xmit_block(buf,0xFE)) break;
    buf += 512; sector += (card_type&8)?1:512; count--;
  }
  deselect();
  return count ? RES_ERROR : RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff){
  (void)buff;
  if(pdrv) return RES_PARERR;
  if(cmd==CTRL_SYNC){ int ok; CS_LOW(); ok=wait_ready(500); deselect(); return ok?RES_OK:RES_ERROR; }
  return RES_PARERR;
}
