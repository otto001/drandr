# drandr - dynamic menu
# See LICENSE file for copyright and license details.

include config.mk

SRC = drw.c drandr.c util.c
OBJ = $(SRC:.c=.o)

all: options drandr
debug: debug_flags options drandr

debug_flags:
    CFLAGS += -ggdb

options:
	@echo drandr build options:
	@echo "CFLAGS   = $(CFLAGS)"
	@echo "LDFLAGS  = $(LDFLAGS)"
	@echo "CC       = $(CC)"

.c.o:
	$(CC) -c $(CFLAGS) $<

config.h:
	cp config.def.h $@

$(OBJ): arg.h config.h drw.h config.mk

drandr: drandr.o drw.o util.o
	$(CC) -o $@ drandr.o drw.o util.o $(LDFLAGS)

clean:
	rm -f drandr $(OBJ) drandr-$(VERSION).tar.gz

dist: clean
	mkdir -p drandr-$(VERSION)
	cp LICENSE Makefile README arg.h config.def.h config.mk drandr.1\
		drw.h util.h $(SRC)\
		drandr-$(VERSION)
	tar -cf drandr-$(VERSION).tar drandr-$(VERSION)
	gzip drandr-$(VERSION).tar
	rm -rf drandr-$(VERSION)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f drandr $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/drandr
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < drandr.1 > $(DESTDIR)$(MANPREFIX)/man1/drandr.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/drandr.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/drandr\
		$(DESTDIR)$(MANPREFIX)/man1/drandr.1\

.PHONY: all options clean dist install uninstall
