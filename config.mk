PREFIX    = /usr
MANPREFIX = /usr/share/man

CC = cc

CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700 -D_GNU_SOURCE
CFLAGS   = -std=c11 -Wall -g
LDFLAGS  = -lsimple
