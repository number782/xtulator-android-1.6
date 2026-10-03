CC ?= cc
CFLAGS = -std=gnu99 -Wall -Wno-unused-parameter -Wno-unused-variable
# -fno-pie -no-pie: dynrec.c casts helper pointers through (uint32_t), which
# is exact on the 32-bit IS01 but silently truncates under PIE ASLR (>4GB).
PIC_FLAGS = -fno-pie -no-pie
SRCS = dynrectest.c
BIN = dynrectest

all: $(BIN)

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) $(PIC_FLAGS) -I. -o $@ $(SRCS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
