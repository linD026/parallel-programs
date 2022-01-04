all:
	gcc -o test centralized_barrier.c -g -lpthread

clean:
	rm -f test
	rm -rf test.dSYM

indent:
	clang-format -i *.[ch]
