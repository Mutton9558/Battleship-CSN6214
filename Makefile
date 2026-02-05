CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g
LDFLAGS = -pthread

CLIENT = client
SERVER = server

all: $(CLIENT) $(SERVER)

$(CLIENT): client.c
	$(CC) $(CFLAGS) client.c -o $(CLIENT)

$(SERVER): server.c
	$(CC) $(CFLAGS) server.c -o $(SERVER) $(LDFLAGS)

clean:
	rm -f $(CLIENT) $(SERVER)

.PHONY: all clean
