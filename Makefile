CC = gcc
CFLAGS = -Wall -Wextra -O2 -march=native -ffast-math
LDLIBS = -lm -pthread

all: prism

prism: prism.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f prism

.PHONY: all clean
