CC := gcc-10
cflags = -g
cflags += -Wall
cflags += -lpthread
clfags += -std=c89
#cflags += -fsanitize=thread
cflags += -fsanitize=address

all:
	$(CC) -o test main.c $(cflags)
