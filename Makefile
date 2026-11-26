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

# git 기록이 없는 사본(zip 등)에서는 기준 비교를 건너뛰고 방 풀이 확인만 한다
HAVE_REF := $(shell git -c safe.directory='*' rev-parse --verify -q '$(REF)^{commit}' >/dev/null 2>&1 && echo yes)

ifeq ($(HAVE_REF),yes)
test: prism ref tests/out/baseline
	tests/out/baseline tests/out/refsrc/prism
else
test: check
endif

# 방 풀이 확인만: 모든 방이 닫힌 문으로 시작하고 풀 수 있는지
check: prism tests/out/baseline
	tests/out/baseline -

# 기준 커밋의 소스를 꺼내 같은 컴파일러·플래그로 빌드한다 (이미지까지 비교하려면 플래그가 같아야 한다)
ref:
	@rm -rf tests/out/refsrc && mkdir -p tests/out/refsrc
	git -c safe.directory='*' archive $(REF) | tar -x -C tests/out/refsrc
	$(MAKE) -s -C tests/out/refsrc prism

tests/out/baseline: tests/baseline.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -o $@ $<

# 메모리 오류 검사용 빌드 (AddressSanitizer + UndefinedBehaviorSanitizer): prism-san
#   배열 밖 접근, 해제한 메모리 사용, 메모리 누수, 정수 넘침 등을 실행 중에 잡아 바로 알려 준다
SAN_FLAGS = -Wall -Wextra -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread
SAN_OBJS = $(SRCS:src/%.c=build-san/%.o)

sanitize: prism-san

prism-san: $(SAN_OBJS)
	$(CC) $(SAN_FLAGS) -o $@ $^ $(LDLIBS)

build-san/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SAN_FLAGS) -c -o $@ $<

-include $(SAN_OBJS:.o=.d)

# Windows 마우스 잠금 도우미 겸 실행기 (게임을 Windows Terminal에서 띄우고 1인칭 마우스 시선을 준다)
#   WSL에서 gcc-mingw-w64-x86-64 가 필요하다. Windows에서 tools\mouselock.exe 를 실행한다
winhelper: tools/mouselock.exe

tools/mouselock.exe: tools/mouselock.c
	x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows $< -o $@ -lws2_32 -lwinmm -lshell32

clean:
	rm -rf build build-san prism prism-san tests/out

.PHONY: all test check ref sanitize winhelper clean
