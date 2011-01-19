#
# main Makefile

PREFIX ?= /usr/local

#
# XXXrcd: enable these flags when developing using gcc.  I turn them
#         off by default for portability.

CFLAGS+= -Wall
CFLAGS+= -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith
CFLAGS+= -Wno-sign-compare -Wno-traditional -Wreturn-type
CFLAGS+= -Wswitch -Wshadow -Wcast-qual -Wwrite-strings -Wextra
CFLAGS+= -Wno-unused-parameter -Wsign-compare
CFLAGS+= -Werror

all: lnetd lnetd.0

clean:
	rm -f lnetd lnetd.o lnetd.0

install:
	mkdir -p			$(DESTDIR)/$(PREFIX)/sbin
	mkdir -p			$(DESTDIR)/$(PREFIX)/man/man1
	mkdir -p			$(DESTDIR)/$(PREFIX)/man/cat1
	install -c -m755 lnetd		$(DESTDIR)/$(PREFIX)/sbin
	install -c -m644 lnetd.1	$(DESTDIR)/$(PREFIX)/man/man1/
	install -c -m644 lnetd.0	$(DESTDIR)/$(PREFIX)/man/cat1/

lnetd.0: lnetd.1
	nroff -mandoc lnetd.1 > $@
