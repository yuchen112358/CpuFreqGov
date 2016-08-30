obj-m := research.o
research-objs := cpu_load_metric.o my_governor.o
KDIR := /work/Odroid/AndroidSRC/kernel/linux
PWD := $(shell pwd)

CC = arm-eabi-gcc

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
