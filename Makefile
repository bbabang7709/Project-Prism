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

# 기준 커밋(REF, 기본은 마지막 커밋)의 빌드와 결과를 비교하고, 모든 방이 풀리는지 확인한다.
#   make test               커밋하지 않은 변경이 결과를 바꾸는지
#   make test REF=f95e08b   원본 프로토타입(첫 커밋)과 비교
REF ?= HEAD

test: prism ref tests/out/baseline
	tests/out/baseline tests/out/refsrc/prism

# 기준 커밋의 소스를 꺼내 같은 컴파일러·플래그로 빌드한다 (이미지까지 비교하려면 플래그가 같아야 한다)
ref:
	@rm -rf tests/out/refsrc && mkdir -p tests/out/refsrc
	git -c safe.directory='*' archive $(REF) | tar -x -C tests/out/refsrc
	$(MAKE) -s -C tests/out/refsrc prism

tests/out/baseline: tests/baseline.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -o $@ $<

clean:
	rm -rf build prism tests/out

.PHONY: all test ref clean
