CC := gcc-10
cflags = -g
cflags += -Wall
cflags += -lpthread
cflags += -fsanitize=thread
#cflags += -fsanitize=address

all:
	$(CC) -o test main.c $(cflags)
