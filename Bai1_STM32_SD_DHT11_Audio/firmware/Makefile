CC=arm-none-eabi-gcc
CFLAGS=-mcpu=cortex-m3 -mthumb -Os -Wall -ffunction-sections -fdata-sections -Ifatfs -I.
LDFLAGS=-Tlink.ld -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs -nostartfiles
SRC=main.c sd_spi.c startup.c ff_O0.o
all: btl_ktvxl.hex
# FatFs bien dich -O0: tranh khoi lenh IT nhieu lenh (vd. ITTTE + MLA) ma Proteus mo phong sai
ff_O0.o: fatfs/ff.c
	$(CC) $(CFLAGS) -O0 -c $< -o $@
btl_ktvxl.elf: main.c sd_spi.c startup.c ff_O0.o
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@
	arm-none-eabi-size $@
btl_ktvxl.hex: btl_ktvxl.elf
	arm-none-eabi-objcopy -O ihex $< $@
clean:
	rm -f *.elf *.hex *.o
