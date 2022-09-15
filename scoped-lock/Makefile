all:
	gcc -o test main.c -g -lpthread  -fsanitize=thread

clean:
	rm -f test
	rm -rf test.dSYM

indent:
	clang-format -i *.[ch]
