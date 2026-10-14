CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE
LDFLAGS = -pthread

all: server worker client

server: src/server.c src/common.h src/protocol.h
	$(CC) $(CFLAGS) -o server src/server.c $(LDFLAGS)

worker: src/worker.c src/common.h src/protocol.h
	$(CC) $(CFLAGS) -o worker src/worker.c $(LDFLAGS)

client: src/client.c src/common.h src/protocol.h
	$(CC) $(CFLAGS) -o client src/client.c $(LDFLAGS)

clean:
	rm -f server worker client *.o
