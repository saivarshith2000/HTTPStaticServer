CC = gcc
CFLAGS = -Wall -lpthread

server: server.c
	${CC} ${CFLAGS} server.c -o server

run: server
	./server

debug: server.c
	${CC} -g ${CFLAGS} server.c -o debug


clean:
	rm -rf server debug

