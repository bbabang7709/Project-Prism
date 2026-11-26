CC = gcc
# -pthread goes to both compiling and linking, as in the original
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

# Compare with the build of a reference commit (REF, default: the last commit) and check that every room solves.
#   make test               do uncommitted changes change the results?
#   make test REF=f95e08b   compare with the original prototype (first commit)
REF ?= HEAD

# A copy without git history (zip etc.) skips the comparison and only checks the rooms
HAVE_REF := $(shell git -c safe.directory='*' rev-parse --verify -q '$(REF)^{commit}' >/dev/null 2>&1 && echo yes)

ifeq ($(HAVE_REF),yes)
test: prism ref tests/out/baseline
	tests/out/baseline tests/out/refsrc/prism
else
test: check
endif

# rooms only: every room starts with the door closed and can be solved
check: prism tests/out/baseline
	tests/out/baseline -

# Extract the reference commit and build it with the same compiler and flags (images match only with equal flags)
ref:
	@rm -rf tests/out/refsrc && mkdir -p tests/out/refsrc
	git -c safe.directory='*' archive $(REF) | tar -x -C tests/out/refsrc
	$(MAKE) -s -C tests/out/refsrc prism

tests/out/baseline: tests/baseline.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -o $@ $<

# Memory error checking build (AddressSanitizer + UndefinedBehaviorSanitizer): prism-san
#   catches out-of-bounds access, use after free, leaks, integer overflow etc. while running
SAN_FLAGS = -Wall -Wextra -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread
SAN_OBJS = $(SRCS:src/%.c=build-san/%.o)

sanitize: prism-san

prism-san: $(SAN_OBJS)
	$(CC) $(SAN_FLAGS) -o $@ $^ $(LDLIBS)

build-san/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SAN_FLAGS) -c -o $@ $<

-include $(SAN_OBJS:.o=.d)

# Windows mouse-lock helper and launcher (opens the game in Windows Terminal, gives first-person mouse look)
#   needs gcc-mingw-w64-x86-64 in WSL; run tools\mouselock.exe on Windows
winhelper: tools/mouselock.exe

tools/mouselock.exe: tools/mouselock.c
	x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows $< -o $@ -lws2_32 -lwinmm -lshell32

clean:
	rm -rf build build-san prism prism-san tests/out

.PHONY: all test check ref sanitize winhelper clean
