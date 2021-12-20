obj-m += test_lrcu.o
obj-m += test_rcu.o

PWD := $(CURDIR)

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build CC=$(CC) M=$(PWD) modules

test:all
	sudo insmod test_lrcu.ko
	sudo rmmod test_lrcu

lrcu:
	sudo insmod test_lrcu.ko
	sudo rmmod test_lrcu

rcu:
	sudo insmod test_rcu.ko
	sudo rmmod test_rcu

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build CC=$(CC) M=$(PWD) clean

indent:
	clang-format -i *.[ch]
