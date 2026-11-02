# Liquid in C - build with `make` (release) or `make debug`.
CC      ?= cc
AR      ?= ar
CSTD    := -std=c17
WARN    := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wno-unused-parameter
OPT     ?= -O2
CFLAGS  ?=
ALLCFLAGS := $(CSTD) $(WARN) $(OPT) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L $(CFLAGS)
LDLIBS  := -lm

BUILD   := build
SRCS    := src/value.c src/num.c src/parse.c src/render.c src/filters.c src/date.c \
           src/drops.c src/json.c src/api.c src/fc_abi.c
OBJS    := $(SRCS:src/%.c=$(BUILD)/obj/%.o)
HDRS    := src/lq.h src/internal.h include/liquid.h harness/liquid_fc.h

.PHONY: all clean debug asan
all: $(BUILD)/libliquid_fc.a $(BUILD)/liquid-server

$(BUILD)/obj/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -c $< -o $@

$(BUILD)/libliquid_fc.a: $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $(OBJS)

$(BUILD)/obj/server.o: server/main.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -c $< -o $@

$(BUILD)/liquid-server: $(BUILD)/obj/server.o $(BUILD)/libliquid_fc.a
	$(CC) $(ALLCFLAGS) -o $@ $^ $(LDLIBS)

debug:
	$(MAKE) BUILD=build-debug OPT="-O0 -g"

asan:
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDLIBS="-lm -fsanitize=address,undefined"

clean:
	rm -rf build build-debug build-asan
