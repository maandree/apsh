.POSIX:

CONFIGFILE = config.mk
include $(CONFIGFILE)

OBJ =\
	apsh.o

HDR =\
	config.h

all: apsh
$(OBJ): $(@:.o=.c) $(HDR)

.c.o:
	$(CC) -c -o $@ $< $(CPPFLAGS) $(CFLAGS)

apsh: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

clean:
	-rm -f -- *.o *.su apsh

.SUFFIXES:
.SUFFIXES: .o .c

.PHONY: all clean
