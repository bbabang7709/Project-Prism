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
test: prism tests/out/prism_ref tests/out/baseline
	tests/out/baseline

# 원본은 지금 빌드와 같은 컴파일러·플래그로 빌드해야 이미지까지 비교할 수 있다
tests/out/prism_ref: tests/reference/prism_orig.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

tests/out/baseline: tests/baseline.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -o $@ $<

clean:
	rm -rf build prism tests/out

.PHONY: all test clean
