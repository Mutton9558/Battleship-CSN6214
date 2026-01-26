CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g
LDFLAGS = -pthread

CLIENT = game
SERVER = server

all: $(CLIENT) $(SERVER)

$(CLIENT): game.c
	$(CC) $(CFLAGS) game.c -o $(CLIENT)

$(SERVER): server.c
	$(CC) $(CFLAGS) server.c -o $(SERVER) $(LDFLAGS)

clean:
	rm -f $(CLIENT) $(SERVER)

.PHONY: all clean
