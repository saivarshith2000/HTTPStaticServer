CC = gcc
CFLAGS = -Wall -lpthread

server: server.c
	${CC} ${CFLAGS} server.c -o server

run: server
	./server

clean:
	rm -rf server

