all:
	gcc-10 -o test test_tsm.c -g -lpthread

clean:
	rm -f test
	rm -rf test.dSYM

indent:
	clang-format -i *.[ch]
