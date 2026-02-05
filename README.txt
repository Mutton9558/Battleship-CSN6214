# Battleship-CSN6124
Battleship game under TT1L Group 3 for MMU Course CSN6124


BATTLESHIP MULTIPLAYER GAME (CLIENT–SERVER)
*****OVERVIEW*****
This project is a multiplayer Battleship game written in C using:
- TCP sockets for networking
- pthreads for multithreading
- fork() and pipes for process communication
- shared memory for game state

Up to 4 players can connect to the server, and the players are automatically divided into:
- Blue Team (2 players)
- Red Team (2 players)

Teams place ships and take turns attacking until one of the teams loses all its ships.



**HOW TO COMPILE**
Make sure these files are in the same folder:
- client.c
- server.c
- Makefile

Compile everything using:
make

This will generate two executables:
- client
- server

*****HOW TO RUN*****
Step 1: Start the server first
        ./server

        The server listens on:
        127.0.0.1 port 6013

Step 2: Start clients in separate terminals, open 4 terminals, and run:
        ./client

        Each player will:
        1. Enter their name
        2. Wait for all players to connect
        3. Be assigned to a team automatically



*****EXAMPLE COMMANDS*****
Compile:
make

Run server:
./server

Run client:
./client

Clean build files:
make clean

Recompile:
make



*****GAME RULES*****
BOARD SIZE
    7 x 7 grid

PHASE 1 – SHIP PLACEMENT
Each player places 4 ships.
    Ships:
      Ship a : length 2
      Ship b : length 3
      Ship c : length 4
      Ship d : length 5

    Enter placement format:
      RowColumn&Direction (e.g A0h)

Meaning:
    Row = A to G
    Column = 0 to 6
    Direction:
      h = horizontal
      v = vertical

PHASE 2 – PLAYING (ATTACK)
Players take turns attacking enemy positions.
      Enter attack format:
        RowColumn(B4)
          Results:
          X = hit
          O = miss
          . = water

PHASE 3 – GAME OVER
The game ends when all ships of one team are destroyed.
        Winning team:
          - Score updated in score.txt
          - Game recorded in game.log



*****MODES SUPPORTED*****
Multiplayer Team Mode:
- 4 players total
- 2 vs 2 teams
- Turn-based gameplay
- TCP network communication
- Localhost play supported
