# BTL KTVXL – Bài 1: STM32F103C8 + thẻ SD + DHT11 + ghi/phát âm thanh

## Các file
| File | Dùng cho |
|---|---|
| `btl_ktvxl_PROTEUS.hex` | Mô phỏng Proteus 8 (nạp vào U1) |
| `btl_ktvxl_MACH_THAT.hex` | Nạp vào mạch thật (Blue Pill, ST-Link) |
| `sdcard.img` | Ảnh thẻ SD sạch (FAT16, 32 MB, có sẵn PLAY.WAV) cho MMC1 |
| `firmware/` | Mã nguồn C (bare-metal, GCC) + FatFs |

## Sơ đồ chân
| Chân | Nối tới |
|---|---|
| PA0 (ADC) | MIC / MAX9814 OUT |
| PA1 | DHT11 DATA (kéo lên 10k) |
| PA4 / PA5 / PA6 / PA7 | SD: CS / SCK / MISO(DO) / MOSI(DI) |
| PA9 / PA10 | UART TX / RX, 115200 baud |
| PB6 (PWM) | R 1k → C 100nF (lọc) → C 10uF → loa |
| PB12 / PB13 | Nút REC / nút PLAY (nối đất, có pull-up trong) |
| PC13 | LED báo hoạt động |

## Chức năng
- **1a:** cứ 5 giây đọc DHT11, ghi một dòng `thoi_gian_s,nhiet_do,do_am` vào `DATA.CSV`.
- **1b:** bấm REC (PB12) để ghi âm từ ADC thành file `RECxx.WAV` (8-bit mono PCM).
- **1c:** bấm PLAY (PB13) để phát file WAV vừa ghi ra loa bằng PWM.

| | Mạch thật | Proteus |
|---|---|---|
| Ghi âm | 8000 Hz, 5 giây, ghi thẳng ra thẻ (bộ đệm đôi) | 4000 Hz, 3,5 giây, thu vào RAM rồi mới ghi |
| Phát lại | Đọc thẻ liên tục (bộ đệm đôi) | Đọc cả file vào RAM rồi phát |

## Những điểm phải xử lý riêng cho Proteus (để viết vào báo cáo)
1. **Khối SPI1 của STM32 mô phỏng không đúng:** thẻ luôn trả về 0x00. Cách xử lý: dùng SPI phần mềm (tự bật/tắt chân PA5/PA7 và đọc PA6).
2. **Bộ định thời chạy chậm khoảng 8 lần so với 64 MHz** (TIM1/TIM2/SysTick), trong khi UART vẫn đúng tốc độ. Cách xử lý: bản Proteus tính bộ định thời theo 8 MHz (`-DPROTEUS`).
3. **CPU mô phỏng quá chậm để đọc DHT11 bằng vòng lặp.** Cách xử lý: đo xung bằng phần cứng, dùng Input Capture TIM2_CH2 trên PA1. Đây cũng là cách tốt hơn cho mạch thật.
4. **Lệnh IT nhiều điều kiện (ITTTE + MLA) bị Proteus mô phỏng sai**, làm hàm `clust2sect` của FatFs trả về 0 nên ghi file thất bại (FR_INT_ERR). Cách xử lý: biên dịch FatFs với `-O0`.
5. **Không kịp vừa lấy mẫu âm thanh vừa ghi thẻ** trong mô phỏng. Cách xử lý: thu vào RAM trước, ghi sau.

## Biên dịch lại (Linux/WSL, arm-none-eabi-gcc)
```
cd firmware
make CFLAGS="-mcpu=cortex-m3 -mthumb -Os -Wall -ffunction-sections -fdata-sections -Ifatfs -I. -DPROTEUS"   # bản Proteus
make clean && make                                                                                       # bản mạch thật
# thêm -DDEBUG=1 để in thông tin chẩn đoán [dbg]
```

## Kiểm tra kết quả trên Proteus
1. Chạy ▶. Terminal (115200 baud) hiện `Mount OK`, rồi cứ 5 giây một dòng `[DHT11] ...` và `Da ghi DATA.CSV`.
2. Bấm PB12 → `[REC] Xong`. Bấm PB13 → `[PLAY] Xong`.
3. Bấm Stop, mở `sdcard.img` bằng 7-Zip: có `DATA.CSV` và `REC01.WAV`.
