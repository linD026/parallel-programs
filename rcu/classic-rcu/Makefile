obj-m += test_lrcu.o

PWD := $(CURDIR)

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build CC=$(CC) M=$(PWD) modules

test:all
	sudo insmod test_lrcu.ko
	sudo rmmod test_lrcu

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build CC=$(CC) M=$(PWD) clean

indent:
	clang-format -i *[.ch]
