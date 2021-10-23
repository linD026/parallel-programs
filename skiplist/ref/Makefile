obj-m += sl_test.o
sl_test-objs := skiplist.o skiplist_test.o

PWD := $(CURDIR)

THREADS = 8
ROUNDS = 1000
ITEMS = 409600

all:
		$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

test:all
		sudo insmod sl_test.ko threads=$(THREADS) rounds=$(ROUNDS) items=$(ITEMS)
		sudo rmmod sl_test

clean:
		$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

