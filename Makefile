trigger6-y := \
	trigger6_audio.o \
	trigger6_commands.o \
	trigger6_drm.o \
	trigger6_drv.o \
	trigger6_transfer.o

obj-m := trigger6.o

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

all:	modules

modules:
	make CHECK="/usr/bin/sparse" -C $(KSRC) M=$(PWD) modules

clean:
	make -C $(KSRC) M=$(PWD) clean
	rm -f $(PWD)/Module.symvers $(PWD)/*.ur-safe
