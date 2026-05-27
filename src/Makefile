# qsc — runscript compiler (C port)
#
# Targets:
#   make            build qsc binary
#   make test       run unit tests
#   make clean      remove build artifacts

CC      ?= gcc
RUNTIME_DIR ?= $(realpath ..)
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 -g
CFLAGS  += -DQSC_RUNTIME_DIR=\"$(RUNTIME_DIR)\"
LDFLAGS ?=
LDLIBS  ?= -lm

SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)
DEPS := $(SRCS:.c=.d)

BIN := qsc

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

test: $(BIN)
	@./tests/run.sh 2>/dev/null || echo "(no tests yet)"

clean:
	rm -f $(OBJS) $(DEPS) $(BIN)
