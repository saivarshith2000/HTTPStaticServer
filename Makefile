CC = gcc
CFLAGS = -Wall -g

server: server.c
	${CC} ${CFLAGS} server.c -o server

run: server
	./server

clean:
	rm server

