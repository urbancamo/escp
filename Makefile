# Portable POSIX Makefile for the escp and escpmd utilities.
#
# Tested on macOS (clang), Linux (gcc/clang), FreeBSD and OpenBSD.
# Uses only features defined by POSIX.1-2008, so it works with both BSD
# make and GNU make.

PROGS      = escp escpmd
SRCDIR     = src
VENDORDIR  = src/vendor
BINDIR     = bin
TESTDIR    = tests
DOCDIR     = docs

CC        ?= cc
CFLAGS    ?= -O2
WARNINGS   = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
STD        = -std=c99
CPPFLAGS  ?=
LDFLAGS   ?=

# iconv(3) is in libc on glibc Linux but a separate library on
# macOS/FreeBSD/NetBSD/OpenBSD. The default below works on the BSDs
# and macOS; on a glibc-only Linux system that doesn't ship libiconv,
# pass ICONV_LIBS="" on the make command line:
#     make ICONV_LIBS=""
ICONV_LIBS ?= -liconv

PREFIX    ?= /usr/local
BINPREFIX ?= $(PREFIX)/bin
MANPREFIX ?= $(PREFIX)/share/man/man1

CORE_OBJ   = $(SRCDIR)/escp_codes.o
ESCP_OBJ   = $(SRCDIR)/escp.o
MD4C_OBJ   = $(VENDORDIR)/md4c/md4c.o
ESCPMD_OBJ = $(SRCDIR)/escpmd.o

all: $(BINDIR)/escp $(BINDIR)/escpmd

$(BINDIR)/escp: $(ESCP_OBJ) $(CORE_OBJ) | $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ESCP_OBJ) $(CORE_OBJ)

$(BINDIR)/escpmd: $(ESCPMD_OBJ) $(CORE_OBJ) $(MD4C_OBJ) | $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ESCPMD_OBJ) $(CORE_OBJ) $(MD4C_OBJ) $(ICONV_LIBS)

$(SRCDIR)/escp.o: $(SRCDIR)/escp.c $(SRCDIR)/escp_codes.h
	$(CC) $(STD) $(WARNINGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $(SRCDIR)/escp.c

$(SRCDIR)/escp_codes.o: $(SRCDIR)/escp_codes.c $(SRCDIR)/escp_codes.h
	$(CC) $(STD) $(WARNINGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $(SRCDIR)/escp_codes.c

$(SRCDIR)/escpmd.o: $(SRCDIR)/escpmd.c $(SRCDIR)/escp_codes.h $(VENDORDIR)/md4c/md4c.h
	$(CC) $(STD) $(WARNINGS) $(CPPFLAGS) $(CFLAGS) -I$(VENDORDIR)/md4c -c -o $@ $(SRCDIR)/escpmd.c

# md4c is vendored upstream code: compile without our strict warnings.
$(VENDORDIR)/md4c/md4c.o: $(VENDORDIR)/md4c/md4c.c $(VENDORDIR)/md4c/md4c.h
	$(CC) $(STD) $(CPPFLAGS) $(CFLAGS) -c -o $@ $(VENDORDIR)/md4c/md4c.c

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -f $(SRCDIR)/*.o $(VENDORDIR)/md4c/*.o
	rm -rf $(BINDIR)
	rm -f $(TESTDIR)/*.actual $(TESTDIR)/*.tmp
	rm -f $(TESTDIR)/md-cases/*.actual $(TESTDIR)/md-cases/*.tmp

test: $(BINDIR)/escp $(BINDIR)/escpmd
	@cd $(TESTDIR) && ESCP=../$(BINDIR)/escp sh ./run.sh
	@cd $(TESTDIR) && ESCPMD=../$(BINDIR)/escpmd sh ./run-md.sh

install: $(BINDIR)/escp $(BINDIR)/escpmd $(DOCDIR)/escp.1 $(DOCDIR)/escpmd.1
	mkdir -p $(DESTDIR)$(BINPREFIX) $(DESTDIR)$(MANPREFIX)
	cp $(BINDIR)/escp   $(DESTDIR)$(BINPREFIX)/escp
	cp $(BINDIR)/escpmd $(DESTDIR)$(BINPREFIX)/escpmd
	chmod 755 $(DESTDIR)$(BINPREFIX)/escp $(DESTDIR)$(BINPREFIX)/escpmd
	cp $(DOCDIR)/escp.1   $(DESTDIR)$(MANPREFIX)/escp.1
	cp $(DOCDIR)/escpmd.1 $(DESTDIR)$(MANPREFIX)/escpmd.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/escp.1 $(DESTDIR)$(MANPREFIX)/escpmd.1

uninstall:
	rm -f $(DESTDIR)$(BINPREFIX)/escp   $(DESTDIR)$(BINPREFIX)/escpmd
	rm -f $(DESTDIR)$(MANPREFIX)/escp.1 $(DESTDIR)$(MANPREFIX)/escpmd.1

.PHONY: all clean test install uninstall
