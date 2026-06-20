# 1. Handle Cross-Compiler Prefix Selection
# Check if CROSS_COMPILE variable is non-empty rather than checking for '1'
ifneq ($(CROSS_COMPILE),)
    CC = $(CROSS_COMPILE)gcc
else
    CC = gcc
endif

# 2. Assignment 8 Build Switches (Defaulting to 1)
USE_AESD_CHAR_DEVICE ?= 1

# 3. Base Compiler Options
CFLAGS ?= -Wall -Werror -g
LDFLAGS ?= -pthread

# 4. Append the compiler flag define based on the configuration variable
ifeq ($(USE_AESD_CHAR_DEVICE),1)
    CFLAGS += -DUSE_AESD_CHAR_DEVICE=1
else
    CFLAGS += -DUSE_AESD_CHAR_DEVICE=0
endif

INC_DIR ?= ../aesd-char-driver
CFLAGS += -I$(INC_DIR)

TARGET = aesdsocket
SRC = aesdsocket.c

all: $(TARGET)

default: all

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

.PHONY: all default clean

