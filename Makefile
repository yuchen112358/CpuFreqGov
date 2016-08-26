obj-m := study.o
study-objs := cpu_load_metric.o my_governor.o
KDIR := /work/Odroid/AndroidSRC/kernel/linux
PWD := $(shell pwd)

CC = arm-eabi-gcc

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	rm -f *.o *.ko *.mod.c *.mod.o modules.order Module.symvers
