all: mte

mte: mte.c
	$(CC) -o mte mte.c -Wall -Wextra -pedantic -std=c11

clean:
	rm mte
