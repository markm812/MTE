CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11 -O2

all: mte

mte: mte.c
	$(CC) $(CFLAGS) -o mte mte.c

install: mte
	install -m 755 mte /usr/local/bin

uninstall:
	rm -f /usr/local/bin/mte

clean:
	rm -f mte
