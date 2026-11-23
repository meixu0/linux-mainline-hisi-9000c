CC ?= cc
CFLAGS ?= -O2 -static -Wall

all: charlotte-diag

charlotte-diag: charlotte-diag.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f charlotte-diag

.PHONY: all clean
