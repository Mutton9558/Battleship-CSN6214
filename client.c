#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <time.h>

#define SIZE 7
#define WATER '.'
#define HIT 'X'
#define MISS 'O'
#define PHASE_PLACEMENT 1
#define PHASE_PLAYING 2
#define PHASE_GAME_OVER 3
#define PHASE_WAITING 0
#define MSG_PLACE_SHIP 2
#define SKIP_PLAYER 3

// networking
int sockfd;
int my_player_id;
int game_phase;
int current_turn;

typedef struct
{
    bool disconnected;
    int msg_type; // e.g., MSG_PLACE_SHIP = 2
    char ship_id; // 'a', 'b', 'c', 'd'
    int row;
    int col;
    char dir; // 'h' or 'v'
} clientMsg;

typedef struct
{
    bool hit;
    bool sunk;
    bool game_over;
} ResultMsg;

typedef struct
{
    char symbol;
    int length;
} Ship;

typedef enum
{
    BLUE,
    RED
} teamList;

Ship ships[] = {
    {'a', 2},
    {'b', 3},
    {'c', 4},
    {'d', 5}};

char **createBoard()
{
    char **board = malloc(SIZE * sizeof(char *));
    if (!board)
    {
        perror("malloc");
        exit(1);
    }

    for (int i = 0; i < SIZE; i++)
    {
        board[i] = malloc(SIZE * sizeof(char));
        if (!board[i])
        {
            perror("malloc");
            exit(1);
        }
        memset(board[i], WATER, SIZE);
    }

    return board;
}

void clearScreen()
{
    system("clear");
}

void recreateBoard(char **clientBoard, char serverBoard[SIZE][SIZE])
{
    for (int row = 0; row < SIZE; row++)
    {
        for (int col = 0; col < SIZE; col++)
        {
            if (serverBoard[row][col] == '\0')
            {
                clientBoard[row][col] = WATER;
            }
            else
            {
                clientBoard[row][col] = serverBoard[row][col];
            }
        }
    }
}

void printTaBoard(char **board)
{
    printf("  ");
    for (int c = 0; c < SIZE; c++)
        printf("%d ", c);
    printf("\n");

    for (int r = 0; r < SIZE; r++)
    {
        printf("%c ", 'A' + r);
        for (int c = 0; c < SIZE; c++)
            printf("%c ", board[r][c]);
        printf("\n");
    }
}

void printTbBoard(char **board)
{
    printf("  ");
    for (int c = 0; c < SIZE; c++)
        printf("%d ", c);
    printf("\n");

    for (int r = 0; r < SIZE; r++)
    {
        printf("%c ", 'A' + r);
        for (int c = 0; c < SIZE; c++)
        {
            if (board[r][c] == HIT || board[r][c] == MISS)
                printf("%c ", board[r][c]);
            else
                printf("%c ", WATER);
        }
        printf("\n");
    }
}

void printGameBoards(char **enemyView, char **playerBoard)
{
    printf("\n\n====== ENEMY BOARD ======\n");
    printTbBoard(enemyView);
    printf("\n====== YOUR BOARD ======\n");
    printTaBoard(playerBoard);
}

int shipPosition(char **board, int row, int col, int length, char dir)
{
    for (int i = 0; i < length; i++)
    {
        int r = row + (dir == 'v' ? i : 0); // vertical
        int c = col + (dir == 'h' ? i : 0); // horizontal
        if (r >= SIZE || c >= SIZE || board[r][c] != WATER)
            return 0;
    }
    return 1;
}

void placeShip(char **board, Ship ship, int *out_row, int *out_col, char *out_dir)
{

    char input[10];
    while (1)
    {
        clearScreen();
        printTaBoard(board);
        printf("\nPlacing ship %c (length %d)\n", ship.symbol, ship.length);
        printf("Enter row (A-G), column (0-6), direction (h/v) [e.g., A0h]: ");
        if (!fgets(input, sizeof(input), stdin))
            continue;

        if (strlen(input) < 3 || input[2] == '\n')
        {
            printf("\nHmm, that doesn't look right. Try again.\n\n");
            sleep(1);
            continue;
        }

        char rowChar = toupper(input[0]);
        char colChar = input[1];
        char dir = tolower(input[2]);

        if (!isdigit(colChar))
        {
            printf("\nColumns are numbers. Not letters. Give it another shot.\n\n");
            sleep(1);
            continue;
        }

        int row = rowChar - 'A';
        int col = colChar - '0';

        if (row < 0 || row >= SIZE || col < 0 || col >= SIZE || (dir != 'h' && dir != 'v'))
        {
            printf("\nThat won't fit. Pick a better spot.\n\n");
            sleep(1);
            continue;
        }

        if (shipPosition(board, row, col, ship.length, dir))
        {
            for (int i = 0; i < ship.length; i++)
            {
                int r = row + (dir == 'v' ? i : 0);
                int c = col + (dir == 'h' ? i : 0);
                board[r][c] = ship.symbol;
            }

            *out_row = row;
            *out_col = col;
            *out_dir = dir;

            printf("\nNice! Ship '%c' is in place.\n\n", ship.symbol);
            break;
        }
        else
        {
            printf("\nSpot is taken or out of bounds. Try another one.\n\n");
            sleep(1);
        }
    }
}

void hitTarget(char **enemyView, char **playerBoard)
{
    char input[10];
    int row, col;

    while (1)
    {
        // clearScreen();
        printGameBoards(enemyView, playerBoard);

        printf("\nEnter target (A-G)(0-6): ");
        if (!fgets(input, sizeof(input), stdin))
            continue;

        if (strlen(input) < 2)
        {
            printf("\nInvalid input. Try again.\n");
            sleep(1);
            continue;
        }

        row = toupper(input[0]) - 'A';
        col = input[1] - '0';

        if (row < 0 || row >= SIZE || col < 0 || col >= SIZE)
        {
            printf("\nOut of bounds! Please choose A-G and 0-6.\n");
            sleep(1);
            continue;
        }

        if (!enemyView || !playerBoard)
        {
            printf("Boards not initialized!\n");
            return;
        }

        if (enemyView[row][col] == HIT || enemyView[row][col] == MISS)
        {
            printf("\nYou already attacked this position. Choose another.\n");
            sleep(1);
            continue;
        }

        printf("Hi\n");

        clientMsg attack;
        attack.disconnected = false;
        attack.row = row;
        attack.col = col;

        write(sockfd, &attack, sizeof(attack));

        ResultMsg result;
        read(sockfd, &result, sizeof(result));

        enemyView[row][col] = result.hit ? HIT : MISS;

        if (result.hit)
        {
            printf("\nYou hit the enemy\n");
        }
        else
        {
            printf("\nYou missed!\n");
        }

        if (result.sunk)
            printf("\nShip sunk!\n");

        if (result.game_over)
            printf("\nGame Over!\n");

        sleep(3);
        break;
    }
}

void waitForTurn()
{
    while (1)
    {
        clearScreen();
        ssize_t p = read(sockfd, &game_phase, sizeof(int));
        if (p <= 0)
        {
            printf("Server might be down or inactive...\n");
            printf("We apologise for the inconvenience\n");
            close(sockfd);
            exit(0);
        }

        if (game_phase == PHASE_GAME_OVER)
        {
            break;
        }
        // Blocking read: server sends current_turn when turn changes
        read(sockfd, &current_turn, sizeof(int));
        if (current_turn == my_player_id)
            break;

        printf("Waiting for your turn...\n");
        sleep(1);
    }
}

int main()
{
    while (true)
    {
        char **playerBoard = createBoard();
        char **enemyBoardView = createBoard();

        struct sockaddr_in serv_addr = {0};
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
        {
            perror("Socket creation failed");
            exit(1);
        }

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(6013);                     // server port
        inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr); // server IP

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        {
            perror("Connection failed");
            exit(1);
        }
        // get user's name
        char name[50];
        printf("Enter your name\n");
        fgets(name, sizeof(name), stdin);
        name[strcspn(name, "\r\n")] = '\0';
        write(sockfd, name, sizeof(name));

        time_t elapsedTime;
        // retrieve id from server
        read(sockfd, &my_player_id, sizeof(my_player_id));
        printf("Connected to server as player %d\n", my_player_id);
        read(sockfd, &elapsedTime, sizeof(elapsedTime));

        bool gameStart = false;
        // idle client until game starts
        // leave commented until production
        printf("Waiting for more players (%ds remaining)...\n", (60 - elapsedTime));
        ssize_t n = read(sockfd, &gameStart, sizeof(gameStart));

        if (n <= 0)
        {
            printf("Server might be down or inactive...\n");
            printf("We apologise for the inconvenience\n");
            close(sockfd);
            exit(0);
        }

        clearScreen();

        int teamShipCount;

        while (1)
        {
            // get board
            char teamServerBoard[7][7];
            char enemyServerBoard[7][7];

            waitForTurn();

            if (game_phase == PHASE_PLACEMENT)
            {

                read(sockfd, teamServerBoard, sizeof(teamServerBoard));
                read(sockfd, enemyServerBoard, sizeof(enemyServerBoard));
                recreateBoard(playerBoard, teamServerBoard);
                recreateBoard(enemyBoardView, enemyServerBoard);
                ssize_t n = read(sockfd, &teamShipCount, sizeof(int));
                int row, col;
                char dir;
                if (teamShipCount >= 4)
                {
                    clientMsg msg;
                    msg.disconnected = false;
                    msg.msg_type = SKIP_PLAYER;
                    write(sockfd, &msg, sizeof(msg));
                }
                placeShip(playerBoard, ships[teamShipCount], &row, &col, &dir);

                clientMsg msg;
                msg.disconnected = false;
                msg.msg_type = MSG_PLACE_SHIP;
                msg.ship_id = ships[teamShipCount].symbol;
                msg.row = row;
                msg.col = col;
                msg.dir = dir;

                write(sockfd, &msg, sizeof(msg));
            }
            else if (game_phase == PHASE_PLAYING)
            {
                read(sockfd, teamServerBoard, sizeof(teamServerBoard));
                read(sockfd, enemyServerBoard, sizeof(enemyServerBoard));
                recreateBoard(playerBoard, teamServerBoard);
                recreateBoard(enemyBoardView, enemyServerBoard);
                hitTarget(enemyBoardView, playerBoard);
            }
            else if (game_phase == PHASE_GAME_OVER)
            {
                teamList winningTeam;
                char (*winners)[51] = malloc(sizeof(char[2][51]));
                printf("Game Over!\n");
                read(sockfd, &winningTeam, sizeof(teamList));
                read(sockfd, winners, sizeof(winners));

                printf("The winners are Team %s\n", winningTeam == RED ? "RED" : "BLUE");
                printf("Winning players:\n");
                for (int i = 0; i < sizeof(winners) / sizeof(winners[0]); i++)
                {
                    printf("%s\n", winners[i]);
                }
                break;
            }
        }

        for (int i = 0; i < SIZE; i++)
        {
            free(playerBoard[i]);
            free(enemyBoardView[i]);
        }
        free(playerBoard);
        free(enemyBoardView);

        char ans;
        bool playAgain;

        do
        {
            printf("Play again? (y/n)\n");
            fgets(ans, sizeof(char), stdin);

            if (tolower(ans) == 'y')
            {
                playAgain = true;
                break;
            }
            else if (tolower(ans) == 'n')
            {
                playAgain = false;
                break;
            }
            else
            {
                printf("Please only enter either y or n!\n");
            }
        } while (true);
    }

    return 0;
}
