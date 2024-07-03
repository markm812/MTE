all: mte

mte: mte.c
	$(CC) -o mte mte.c -Wall -Wextra -pedantic -std=c11 -O2

clean:
	rm mte
