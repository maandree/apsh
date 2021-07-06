.POSIX:

CONFIGFILE = config.mk
include $(CONFIGFILE)

OBJ =\
	apsh.o\
	preparser.o\
	tokeniser.o\
	parser.o

HDR =\
	common.h\
	config.h

all: apsh
$(OBJ): $(@:.o=.c) $(HDR)

.c.o:
	$(CC) -c -o $@ $< $(CPPFLAGS) $(CFLAGS)

apsh: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

install: apsh
	mkdir -p -- "$(DESTDIR)$(PREFIX)/bin/"
	cp -- apsh "$(DESTDIR)$(PREFIX)/bin/"

uninstall:
	-rm -f -- "$(DESTDIR)$(PREFIX)/bin/apsh"

clean:
	-rm -f -- *.o *.su apsh

.SUFFIXES:
.SUFFIXES: .o .c

.PHONY: all install uninstall clean
