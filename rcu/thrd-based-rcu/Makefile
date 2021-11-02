CC := gcc-10
cflags = -g
cflags += -Wall
cflags += -lpthread
clfags += -std=c89
cflags += -fsanitize=thread
#cflags += -fsanitize=address

all:
	$(CC) -o test main.c $(cflags)

read:
	$(CC) -o test read_side.c $(cflags) 

update:
	$(CC) -o test update_side.c $(cflags)

clean:
	rm -f test 
	rm -rf test.dSYM

indent:
	clang-format -i *[.ch]
