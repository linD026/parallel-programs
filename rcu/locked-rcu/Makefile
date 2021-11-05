CC := gcc-10
cflags = -g
cflags += -Wall
cflags += -lpthread
clfags += -std=c89
#cflags += -fsanitize=thread
#cflags += -fsanitize=address

READER_NUM = 10
UPDATER_NUM = 1
TRACE_LOOP = 1000
cflags += -D'READER_NUM=$(READER_NUM)'
cflags += -D'UPDATER_NUM=$(UPDATER_NUM)'
cflags += -D'TRACE_LOOP=$(TRACE_LOOP)'

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
