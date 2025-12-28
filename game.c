// Anis and Zi Xuan's part
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

int main()
{
    // connect to server
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(6013);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);
    connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    printf("Connected\n");
    char buffer[50] = "Shawn";
    write(sockfd, buffer, sizeof(buffer));
    sleep(1000);
    close(sockfd);
    return 0;
}


//zx: only 1 player and fixed demo enemy ship not yet set for other players

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>     
#include <pthread.h>    
#include <stdatomic.h>

#define SIZE 7
#define WATER '.'
#define HIT 'X'
#define MISS 'O'

typedef struct {
    char symbol;
    int length;
} Ship;

Ship ships[] = {
    {'a', 2},
    {'b', 3},
    {'c', 4},
    {'d', 5}
};

char** createBoard() {
    char** board = malloc(SIZE * sizeof(char*));
    for (int i = 0; i < SIZE; i++) {
        board[i] = malloc(SIZE * sizeof(char));
        for (int j = 0; j < SIZE; j++)
            board[i][j] = WATER;
    }
    return board;
}

void clearScreen() {
    system("clear");
}

void printTaBoard(char** board) {
    printf("  ");
    for (int c = 0; c < SIZE; c++)
        printf("%d ", c);
    printf("\n");

    for (int r = 0; r < SIZE; r++) {
        printf("%c ", 'A' + r);
        for (int c = 0; c < SIZE; c++)
            printf("%c ", board[r][c]);
        printf("\n");
    }
}

void printTbBoard(char** board) {
    printf("  ");
    for (int c = 0; c < SIZE; c++)
        printf("%d ", c);
    printf("\n");

    for (int r = 0; r < SIZE; r++) {
        printf("%c ", 'A' + r);
        for (int c = 0; c < SIZE; c++) {
            if (board[r][c] == HIT || board[r][c] == MISS)
                printf("%c ", board[r][c]);
            else
                printf("%c ", WATER);
        }
        printf("\n");
    }
}

void printGameBoards(char** enemyView, char** playerBoard) {
    printf("\n\n====== ENEMY BOARD ======\n");
    printTbBoard(enemyView);
    printf("\n====== YOUR BOARD ======\n");
    printTaBoard(playerBoard);
}

int shipPosition(char** board, int row, int col, int length, char dir) {
    for (int i = 0; i < length; i++) {
        int r = row + (dir == 'v' ? i : 0); //vertical
        int c = col + (dir == 'h' ? i : 0); //horizontal
        if (r >= SIZE || c >= SIZE || board[r][c] != WATER)
            return 0;
    }
    return 1;
}

void placeShip(char** board, Ship ship) {
    char input[10];
    while (1) {
        clearScreen(); 
        printTaBoard(board);
        printf("\nPlacing ship %c (length %d)\n", ship.symbol, ship.length);
        printf("Enter row (A-G), column (0-6), direction (h/v) [e.g., A0h]: ");
        if (!fgets(input, sizeof(input), stdin)) continue;

        if (strlen(input) < 3) {
            printf("\nHmm, that doesn't look right. Try again.\n\n");
            sleep(1);
            continue;
        }

        char rowChar = toupper(input[0]);
        char colChar = input[1];
        char dir = tolower(input[2]);

        if (!isdigit(colChar)) {
            printf("\nColumns are numbers. Not letters. Give it another shot.\n\n");
            sleep(1);
            continue;
        }

        int row = rowChar - 'A';
        int col = colChar - '0';

        if (row < 0 || row >= SIZE || col < 0 || col >= SIZE || (dir != 'h' && dir != 'v')) {
            printf("\nThat won't fit. Pick a better spot.\n\n");
            sleep(1);
            continue;
        }

        if (shipPosition(board, row, col, ship.length, dir)) {
            for (int i = 0; i < ship.length; i++) {
                int r = row + (dir == 'v' ? i : 0);
                int c = col + (dir == 'h' ? i : 0);
                board[r][c] = ship.symbol;
            }
            printf("\nNice! Ship '%c' is in place.\n\n", ship.symbol);
            sleep(1); 
            break;
        } else {
            printf("\nSpot is taken or out of bounds. Try another one.\n\n");
            sleep(1);
        }
    }
}


int emptybox(char** board, char shipSymbol) {
    int count = 0;
    for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++)
            if (board[r][c] == shipSymbol)
                count++;
    return count;
}

int allShipsSunk(char** board) {
    for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++)
            if (board[r][c] != WATER)
                return 0;
    return 1;
}

atomic_int inputDone = 0;

void* countdown(void* args) {
    int* seconds = (int*)args;
    for (int i = *seconds; i >= 0 && !inputDone; i--) {
        printf("\033[s");                  
        printf("\033[1;1H");                
        printf("Time left: %2d seconds  ", i);
        printf("\033[u");                  
        fflush(stdout);
        sleep(1);
    }

    if (!inputDone) {
        inputDone = 1;
        clearScreen();
        printf("Time's up! You missed your chance!\n");
        fflush(stdout);
        sleep(2);
    }
    return NULL;
}


void hitTarget(char** enemyReal, char** enemyView, char** playerBoard) {
    inputDone = 0;
    int timeLimit = 15;

    clearScreen();
    printGameBoards(enemyView, playerBoard);

    printf("\nEnter target in 15s (A-G)(0-6): ");            // User input prompt below timer
    fflush(stdout);

    pthread_t timerThread;
    pthread_create(&timerThread, NULL, countdown, &timeLimit);

    char input[10];
    if (!fgets(input, sizeof(input), stdin)) input[0] = '\0';
    inputDone = 1;  
    pthread_join(timerThread, NULL);

   
    if (strlen(input) < 2 || input[0] == '\n') return;

    char rowChar = toupper(input[0]);
    char colChar = input[1];

    if (!isdigit(colChar)) {
        printf("\nThat's not a number. Try again.\n");
    } else {
        int row = rowChar - 'A';
        int col = colChar - '0';

        if (row < 0 || row >= SIZE || col < 0 || col >= SIZE) {
            printf("\nWoah! That’s off the board. Pick a spot inside.\n");
        } else if (enemyView[row][col] == HIT || enemyView[row][col] == MISS) {
            printf("\nAlready tried that spot. Pick another.\n");
        } else if (enemyReal[row][col] != WATER) {
            char hitShip = enemyReal[row][col];
            enemyView[row][col] = HIT;
            enemyReal[row][col] = WATER;
            if (emptybox(enemyReal, hitShip) == 0)
                printf("\nBOOM! Ship '%c' sunk. Nice one!\n", hitShip);
            else
                printf("\nHit! That ship is hurting. Keep going!\n");
        } else {
            enemyView[row][col] = MISS;
            printf("\nMiss! Don’t worry, you’ll get it next time.\n");
        }
    }

    fflush(stdout);
    sleep(2);
}



int main() {
    char** playerBoard = createBoard();
    char** enemyBoardReal = createBoard();
    char** enemyBoardView = createBoard();

    for (int i = 0; i < sizeof(ships)/sizeof(ships[0]); i++)
        placeShip(playerBoard, ships[i]);

    // Sample enemy ship placements
    enemyBoardReal[2][2] = 'a';
    enemyBoardReal[2][3] = 'a';
    enemyBoardReal[4][1] = 'b';
    enemyBoardReal[5][1] = 'b';
    enemyBoardReal[6][1] = 'b';

    clearScreen();

while (1) {
    hitTarget(enemyBoardReal, enemyBoardView, playerBoard);

    if (allShipsSunk(enemyBoardReal)) {
        clearScreen();  
        printGameBoards(enemyBoardView, playerBoard);
        printf("\nAll enemy ships are sunk! Victory is yours!\n");
        sleep(3);      
        break;
    }
}


    for (int i = 0; i < SIZE; i++) {
        free(playerBoard[i]);
        free(enemyBoardReal[i]);
        free(enemyBoardView[i]);
    }
    free(playerBoard);
    free(enemyBoardReal);
    free(enemyBoardView);

    return 0;
}
