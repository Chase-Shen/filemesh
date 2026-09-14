CC ?= cc
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic

PROGRAMS := gateway pdf_server text_server zip_server client
BINARIES := $(addprefix build/,$(PROGRAMS))
SOURCES := $(addprefix src/,$(addsuffix .c,$(PROGRAMS)))

.PHONY: all check clean

all: $(BINARIES)

build:
	mkdir -p $@

build/%: src/%.c include/server_utils.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

check:
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -fsyntax-only $(SOURCES)

clean:
	$(RM) $(BINARIES)
