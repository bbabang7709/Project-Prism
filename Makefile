CC = gcc
# -pthread는 원본처럼 컴파일과 링크 모두에 준다
CFLAGS = -Wall -Wextra -O2 -march=native -ffast-math -pthread
CPPFLAGS = -Isrc -MMD -MP
LDLIBS = -lm -pthread

SRCS = $(wildcard src/*.c src/*/*.c)
OBJS = $(SRCS:src/%.c=build/%.o)

all: prism

prism: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

# 원본(tests/reference/prism_orig.c)과 결과를 비교한다
test: prism
	sh tests/baseline.sh

clean:
	rm -rf build prism tests/out

.PHONY: all test clean
