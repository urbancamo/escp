# Portable POSIX Makefile for the escp utility.
#
# Tested on macOS (clang), Linux (gcc/clang), FreeBSD and OpenBSD.
# Uses only features defined by POSIX.1-2008, so it works with both BSD
# make and GNU make.

PROG       = escp
SRCDIR     = src
BINDIR     = bin
TESTDIR    = tests
DOCDIR     = docs

CC        ?= cc
CFLAGS    ?= -O2
WARNINGS   = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
STD        = -std=c99
CPPFLAGS  ?=
LDFLAGS   ?=

PREFIX    ?= /usr/local
BINPREFIX ?= $(PREFIX)/bin
MANPREFIX ?= $(PREFIX)/share/man/man1

OBJ = $(SRCDIR)/escp.o

all: $(BINDIR)/$(PROG)

$(BINDIR)/$(PROG): $(OBJ) | $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

$(SRCDIR)/escp.o: $(SRCDIR)/escp.c
	$(CC) $(STD) $(WARNINGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -f $(SRCDIR)/*.o
	rm -rf $(BINDIR)
	rm -f $(TESTDIR)/*.actual $(TESTDIR)/*.tmp

test: $(BINDIR)/$(PROG)
	@cd $(TESTDIR) && ESCP=../$(BINDIR)/$(PROG) sh ./run.sh

install: $(BINDIR)/$(PROG) $(DOCDIR)/$(PROG).1
	mkdir -p $(DESTDIR)$(BINPREFIX) $(DESTDIR)$(MANPREFIX)
	cp $(BINDIR)/$(PROG) $(DESTDIR)$(BINPREFIX)/$(PROG)
	chmod 755 $(DESTDIR)$(BINPREFIX)/$(PROG)
	cp $(DOCDIR)/$(PROG).1 $(DESTDIR)$(MANPREFIX)/$(PROG).1
	chmod 644 $(DESTDIR)$(MANPREFIX)/$(PROG).1

uninstall:
	rm -f $(DESTDIR)$(BINPREFIX)/$(PROG)
	rm -f $(DESTDIR)$(MANPREFIX)/$(PROG).1

.PHONY: all clean test install uninstall
