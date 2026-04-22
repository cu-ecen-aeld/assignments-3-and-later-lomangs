#CC = $(CROSS_COMPILE)gcc
ifeq ($(CROSS_COMPILE), 1)
    CC = aarch64-none-linux-gnu-gcc
else
    CC = gcc
endif

CFLAGS ?= -Wall -Werror -g
TARGET = aesdsocket
SRC = aesdsocket.c

all: $(TARGET)

default: all

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) *.o

.PHONY: all default clean

