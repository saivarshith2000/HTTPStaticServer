CC = gcc
CFLAGS = -Wall -lpthread

server: server.c
	${CC} ${CFLAGS} server.c -o server

debug: server.c
	${CC} ${CFLAGS} -g server.c -o debug

run: server
	./server

clean:
	rm -rf server debug

