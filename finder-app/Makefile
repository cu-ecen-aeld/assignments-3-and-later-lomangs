ifeq ($(NEED_CROSS_COMPILE), 1)
	CC = aarch64-none-linux-gnu-gcc
else
	CC = gcc
endif

all: writer

writer: writer.c
	$(CC) -Wall $< -o $@

clean:
	rm -f writer *.o

