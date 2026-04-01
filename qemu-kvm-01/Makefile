obj-m += mykvm.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

CFLAGS_myqemu := -O2 -Wall -Wextra

all: module user

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user: myqemu.c mykvm_uapi.h
	$(CC) $(CFLAGS_myqemu) -o myqemu myqemu.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f myqemu
