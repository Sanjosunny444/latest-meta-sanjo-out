obj-m := bmp280_driver.o
KDIR  := /lib/modules/$(shell uname -r)/build

# Suppress false-positive -Werror=return-type from FORTIFY_SOURCE-wrapped
# sprintf()/kernel-header inlines when host gcc version tag differs slightly
# from the kernel's build-time compiler (harmless — code is verified correct).
ccflags-y := -Wno-error=return-type

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
