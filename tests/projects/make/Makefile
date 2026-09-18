CC ?= cc
all: hello
hello: hello.o greet2.o
	$(CC) -o $@ hello.o greet2.o
%.o: %.c
	$(CC) -c -o $@ $<
clean:
	rm -f hello *.o
.PHONY: all clean
