// Shawn and Imran's part
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>

// Fix for macOS compatibility
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define HIT 'X'
#define MISS 'O'
#define PHASE_PLACEMENT 1
#define PHASE_PLAYING 2
#define PHASE_GAME_OVER 3

const int PORT = 8080;

typedef struct
{
    int x;
    int y;
} Tuple;

typedef enum
{
    BLUE,
    RED
} teamList;

typedef struct
{
    char name[50];
    teamList team;
    int client_fd;
    int playerId;
    int score;
} Player;

typedef struct PlayerNode
{
    Player *player;
    struct PlayerNode *next;
} PlayerNode;

typedef struct
{
    Player *curTurnPlayer;
    Tuple hitTarget;
    bool shipDestroyed;
    bool shipHit;
    bool gameEnd;
} turnState;

enum threadList
{
    HANDLER,
    SCHEDULER,
    LOGGER
};

enum threadList threadTurn = SCHEDULER;

typedef struct
{
    enum threadList threadTurn;
    pthread_mutex_t turnStructLock;
    pthread_cond_t turnStructCond;
    turnState gameState;
    bool turnDone;
    char blueShips[7][7];
    char redShips[7][7];
    int redShipCount;
    int blueShipCount;
    int gamePhase;
    PlayerNode *playerQueueHead;
    PlayerNode *currentPlayerNode;
    int activePlayerCount;
} SharedData;

typedef struct
{
    bool disconnected;
    int msg_type; // e.g., MSG_PLACE_SHIP = 2
    char ship_id; // 'a', 'b', 'c', 'd'
    int row;
    int col;
    char dir; // 'h' or 'v' if 'n', error occured
    bool hit;
    bool sunk;
} msg;

typedef struct
{
    bool hit;
    bool sunk;
    bool game_over;
} resultMsg;

// global variables
Player *playerQueue[4];
turnState gameState;
bool gameEnd = false;
bool gameStart = false;
pthread_mutex_t lock;
pthread_mutex_t turnStructLock;
pthread_cond_t turnStructConditionVar;
int pipes[4][2];
int gamePhase = PHASE_PLACEMENT;

void sigchild_handler(int sig)
{
    int saved_errno = errno;
    pid_t pid;
    int status;

    // Reap all exited children
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        printf("Child %d exited\n", pid);
    }

    errno = saved_errno;
}

void setup_sigchild()
{
    struct sigaction sa;
    sa.sa_handler = sigchild_handler;
    sigemptyset(&sa.sa_mask);
    // restart interrupted syscalls
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction\n");
        exit(1);
    }
}

// Opens score file
void *load_score(void *arg)
{
    Player *player = (Player *)arg;
    FILE *file = fopen("score.txt", "r");
    if (!file)
    {
        file = fopen("score.txt", "w");
        if (!file)
        {
            printf("Error in creating score file\n");
            exit(0);
        }
        else
        {
            fclose(file);
            file = fopen("score.txt", "r");
        }
    }

    // Username: Wins
    // max 50 for username, 2 for the colon and whitespace, 10 for max int digits
    char line[63];
    int score;
    char name[51];
    while (fgets(line, 63, file))
    {
        if (sscanf(line, "%50[^:]: %d", name, &score) == 2)
        {
            if (strcmp(name, player->name) == 0)
            {
                player->score = score;
            }
        }
    }
    fclose(file);

    FILE *logFile = fopen("game.log", "a");
    if (!logFile)
    {
        printf("Error in opening log file\n");
        exit(0);
    }
    char team[5];
    if (player->team == BLUE)
    {
        strncpy(team, "Blue", sizeof("Blue"));
    }
    else
    {
        strncpy(team, "Red", sizeof("Red"));
    }
    fprintf(logFile, "Player %s joined.\n", player->name);
    fprintf(logFile, "Player %s assigned team %s.\n", player->name, team);

    fclose(logFile);
    return NULL;
}

void *updateScore(void *arg)
{
    char (*winnerList)[51] = (char (*)[51])arg;
    FILE *tempFile = fopen("temp.txt", "w");
    FILE *scoreFile = fopen("score.txt", "r");
    if (!scoreFile)
    {
        printf("Error opening score file\n");
    }
    char line[63];
    int score;
    char name[51];
    while (fgets(line, 63, scoreFile))
    {
        if (sscanf(line, "%50[^:]: %d", name, &score) == 2)
        {
            if (strcmp(name, winnerList[0]) == 0 || strcmp(name, winnerList[1]) == 0)
            {
                fprintf(tempFile, "%s: %d\n", name, score + 1);
            }
            else
            {
                fprintf(tempFile, "%s: %d\n", name, score);
            }
        }
    }
    fclose(tempFile);
    fclose(scoreFile);

    int fileRemoveSuccess = remove("score.txt");
    if (fileRemoveSuccess == 0)
    {
        int fileRenameSuccess = rename("temp.txt", "score.txt");
        if (fileRenameSuccess == 0)
        {
            printf("Successfully updated score.txt\n");
        }
        else
        {
            printf("Failed to rename temp file!\n");
        }
    }
    else
    {
        printf("Failed to remove score file!\n");
    }
    free(winnerList);
}

void *updateFileDisconnect(void *arg)
{
    char *name = (char *)arg;
    FILE *logFile = fopen("game.log", "a");
    if (!logFile)
    {
        printf("Error in opening log file\n");
        free(name);
        exit(0);
    }
    fprintf(logFile, "PLayer %s disconnected.\n", name);
    fclose(logFile);
    return NULL;
}

void *pollForDisconnect(void *arg)
{
    pthread_t fileManipThread;
    int *playerCount = (int *)arg;
    while (!gameEnd)
    {

        struct pollfd poller[4];

        // avoid race conditions/incorrect read
        pthread_mutex_lock(&lock);

        int count = *playerCount;
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < count; i++)
        {
            pthread_mutex_lock(&lock);
            poller[i].fd = playerQueue[i]->client_fd;
            pthread_mutex_unlock(&lock);
            poller[i].events = POLLIN;
        }

        // waits for 1s, if no event returns 0
        // not so fun fact i set the timeout to 0 and almost destroyed my linux - shawn
        int check = poll(poller, count, 1000);

        if (check > 0)
        {
            for (int i = 0; i < count; i++)
            {
                if (poller[i].revents & POLLIN)
                {
                    char buf;
                    ssize_t n = recv(poller[i].fd, &buf, 1, MSG_PEEK);

                    if (n == 0)
                    {
                        close(poller[i].fd);
                        pthread_mutex_lock(&lock);
                        char *name = malloc(strlen(playerQueue[i]->name) + 1);
                        strcpy(name, playerQueue[i]->name);
                        printf("Player %s disconnected...\n", name);
                        pthread_create(&fileManipThread, NULL, updateFileDisconnect, name);
                        pthread_detach(fileManipThread);

                        free(playerQueue[i]);
                        // shift a player to fill in that player's pos and remove last item of the array
                        playerQueue[i] = NULL;
                        (*playerCount)--;
                        pthread_mutex_unlock(&lock);
                    }
                }
            }
        }
    }
    return NULL;
}

bool dfsCheck(char board[7][7], char visited[7][7], char target, int x, int y)
{
    if (x < 0 || x > 6 || y < 0 || y > 6)
        return false;

    if (visited[x][y])
        return false;

    if (board[x][y] != target && board[x][y] != HIT)
        return false;

    if (board[x][y] == target)
        return true;

    return false ||
           dfsCheck(board, visited, target, x - 1, y) ||
           dfsCheck(board, visited, target, x + 1, y) ||
           dfsCheck(board, visited, target, x, y - 1) ||
           dfsCheck(board, visited, target, x, y + 1);
}

void clientHandler(SharedData *shared, Player *player, int pipefd[2])
{
    pthread_mutex_lock(&shared->turnStructLock);
    while (shared->threadTurn != HANDLER)
    {
        pthread_cond_wait(&shared->turnStructCond, &shared->turnStructLock);
    }

    int clientFd = player->client_fd;
    int id = player->playerId;
    while (shared->gameState.curTurnPlayer == NULL)
    {
        sleep(1);
    }
    pthread_mutex_unlock(&shared->turnStructLock);
    teamList team = player->team;
    teamList enemyTeam;
    char myShip[7][7];
    char enemyShip[7][7];
    bool gameStarted = true;
    write(clientFd, &gameStarted, sizeof(bool));
    if (team == RED)
    {
        enemyTeam = BLUE;
        pthread_mutex_lock(&shared->turnStructLock);
        memcpy(myShip, shared->redShips, sizeof(shared->redShips));
        memcpy(enemyShip, shared->blueShips, sizeof(shared->blueShips));
        pthread_mutex_unlock(&shared->turnStructLock);
    }
    else
    {
        enemyTeam = RED;
        pthread_mutex_lock(&shared->turnStructLock);
        memcpy(myShip, shared->blueShips, sizeof(shared->blueShips));
        memcpy(enemyShip, shared->redShips, sizeof(shared->redShips));
        pthread_mutex_unlock(&shared->turnStructLock);
    }

    while (true)
    {
        write(clientFd, myShip, sizeof(myShip));
        write(clientFd, enemyShip, sizeof(enemyShip));
        pthread_mutex_lock(&shared->turnStructLock);
        write(clientFd, &shared->gamePhase, sizeof(shared->gamePhase));
        pthread_mutex_unlock(&shared->turnStructLock);
        int curTurnPlayerId;
        do
        {
            pthread_mutex_lock(&shared->turnStructLock);
            curTurnPlayerId = shared->gameState.curTurnPlayer->playerId;
            pthread_mutex_unlock(&shared->turnStructLock);
            write(clientFd, &curTurnPlayerId, sizeof(curTurnPlayerId));
            sleep(1);
        } while (id != curTurnPlayerId);

        msg clientMsg;
        ssize_t n = read(clientFd, &clientMsg, sizeof(msg));
        if (n <= 0)
        {
            printf("Client disconnected\n");
            clientMsg.disconnected = true;
            write(pipefd[1], &clientMsg, sizeof(clientMsg));
            _exit(-1);
        }
        if (shared->gamePhase == PHASE_PLACEMENT)
        {
            pthread_mutex_lock(&shared->turnStructLock);
            int teamShipCount = (player->team == RED ? shared->redShipCount : shared->blueShipCount);
            pthread_mutex_unlock(&shared->turnStructLock);
            write(clientFd, &teamShipCount, sizeof(teamShipCount));
        }
        else if (shared->gamePhase == PHASE_PLAYING)
        {
            int row = clientMsg.row;
            int col = clientMsg.col;
            char target = enemyShip[row][col];
            char visited[7][7];
            resultMsg msgToClient;

            if (target == 'a' || target == 'b' || target == 'c' || target == 'd')
            {
                char prevChar = target;
                target = HIT;
                bool alive = dfsCheck(enemyShip, visited, prevChar, row, col);

                msgToClient.game_over = false;
                msgToClient.hit = true;
                clientMsg.hit = true;
                clientMsg.sunk = !alive;
                msgToClient.sunk = !alive;
            }
            else
            {
                if (target != HIT || target != MISS)
                {
                    target = MISS;
                    resultMsg msgToClient;
                    msgToClient.game_over = false;
                    msgToClient.hit = false;
                    msgToClient.sunk = false;
                    clientMsg.hit = false;
                    clientMsg.sunk = false;
                }
                else
                {
                    printf("Target attempted to hit a space already hit before...\n");
                }
            }
        }
        else if (shared->gamePhase == PHASE_GAME_OVER)
        {
            // kill child
            _exit(0);
        }

        write(pipefd[1], &clientMsg, sizeof(clientMsg));
    }

    pthread_cond_broadcast(&shared->turnStructCond);
}

void *loggerFunction(void *arg)
{
    SharedData *data = (SharedData *)arg;
    pthread_mutex_lock(&data->turnStructLock);

    while (data->threadTurn != LOGGER)
    {
        pthread_cond_wait(&data->turnStructCond, &data->turnStructLock);
    }

    FILE *logFile = fopen("game.log", "a");
    if (!logFile)
    {
        printf("Error opening game.log for logging\n");
        pthread_mutex_unlock(&data->turnStructLock);
        return NULL;
    }

    while (!gameEnd)
    {
        // Wait for LOGGER turn
        while (data->threadTurn != LOGGER && !gameEnd)
        {
            pthread_cond_wait(&data->turnStructCond, &data->turnStructLock);
        }

        if (gameEnd)
        {
            pthread_mutex_unlock(&data->turnStructLock);
            fclose(logFile);
            break;
        }

        // Read and log turn state data
        Player *currentPlayer = data->gameState.curTurnPlayer;
        Tuple hitTarget = data->gameState.hitTarget;
        bool shipHit = data->gameState.shipHit;
        bool shipDestroyed = data->gameState.shipDestroyed;
        int currentPhase = data->gamePhase;

        pthread_mutex_unlock(&data->turnStructLock);

        // Write data to log file
        fprintf(logFile, "---Turn Log Entry---\n");
        if (currentPlayer != NULL)
        {
            fprintf(logFile, "Current Turn Player: %s (Team: %s, ID: %d)\n",
                    currentPlayer->name,
                    currentPlayer->team == BLUE ? "BLUE" : "RED",
                    currentPlayer->playerId);
        }

        fprintf(logFile, "Game Phase: ");
        switch (currentPhase)
        {
        case PHASE_PLACEMENT:
            fprintf(logFile, "PLACEMENT\n");
            break;
        case PHASE_PLAYING:
            fprintf(logFile, "PLAYING\n");
            fprintf(logFile, "Target: (%d, %d)\n", hitTarget.x, hitTarget.y);
            fprintf(logFile, "Ship Hit: %s\n", shipHit ? "YES" : "NO");
            fprintf(logFile, "Ship Destroyed: %s\n", shipDestroyed ? "YES" : "NO");
            break;
        case PHASE_GAME_OVER:
            fprintf(logFile, "GAME_OVER\n");
            break;
        default:
            fprintf(logFile, "UNKNOWN\n");
        }
        fprintf(logFile, "---End Entry---\n\n");
        fflush(logFile);

        pthread_mutex_lock(&data->turnStructLock);

        // Signal scheduler that logger has finished accessing turnState
        data->threadTurn = SCHEDULER;
        pthread_cond_broadcast(&data->turnStructCond);

        // Wait for next turn
        while (data->threadTurn == SCHEDULER && !gameEnd)
        {
            pthread_cond_wait(&data->turnStructCond, &data->turnStructLock);
        }
    }

    if (logFile)
        fclose(logFile);

    return NULL;
}

void *schedulerFunction(void *arg)
{
    SharedData *data = (SharedData *)arg;
    pthread_mutex_lock(&data->turnStructLock);

    while (data->threadTurn != SCHEDULER)
    {
        pthread_cond_wait(&data->turnStructCond, &data->turnStructLock);
    }

    while (!gameEnd)
    {
        // Wait for SCHEDULER turn
        while (data->threadTurn != SCHEDULER && !gameEnd)
        {
            pthread_cond_wait(&data->turnStructCond, &data->turnStructLock);
        }

        if (gameEnd)
        {
            pthread_mutex_unlock(&data->turnStructLock);
            break;
        }

        // Clear the turn state for the new turn
        data->gameState.hitTarget.x = -1;
        data->gameState.hitTarget.y = -1;
        data->gameState.shipHit = false;
        data->gameState.shipDestroyed = false;
        data->gameState.gameEnd = false;

        // Select next player's turn from the linked list
        if (data->playerQueueHead != NULL)
        {
            if (data->currentPlayerNode == NULL)
            {
                // First time, start with head
                data->currentPlayerNode = data->playerQueueHead;
            }
            else
            {
                // Move to next player in circular linked list
                if (data->currentPlayerNode->next != NULL)
                {
                    data->currentPlayerNode = data->currentPlayerNode->next;
                }
                else
                {
                    // Wrap around to head
                    data->currentPlayerNode = data->playerQueueHead;
                }
            }

            // Check if current player has been disconnected
            // If disconnected, move to next
            while (data->currentPlayerNode && data->currentPlayerNode->player == NULL)
            {
                if (data->currentPlayerNode->next != NULL)
                {
                    data->currentPlayerNode = data->currentPlayerNode->next;
                }
                else
                {
                    data->currentPlayerNode = data->playerQueueHead;
                }
            }

            if (data->currentPlayerNode && data->currentPlayerNode->player != NULL)
            {
                data->gameState.curTurnPlayer = data->currentPlayerNode->player;
            }
        }

        // Signal client handler that turn state is cleared and new player selected
        data->threadTurn = HANDLER;
        pthread_cond_broadcast(&data->turnStructCond);

        // Wait for next turn to begin
        while (data->threadTurn == HANDLER && !gameEnd)
        {
            pthread_cond_wait(&data->turnStructCond, &data->turnStructLock);
        }
    }

    pthread_mutex_unlock(&data->turnStructLock);
    return NULL;
}

int main()
{
    // mutex and condition variable initialisation
    pthread_mutex_init(&lock, NULL);
    pthread_mutex_init(&turnStructLock, NULL);
    pthread_cond_init(&turnStructConditionVar, NULL);

    // all the threads
    pthread_t scoreLoader;
    pthread_t checkForDisconnects;
    pthread_t schedulerThread;
    pthread_t loggerThread;
    pthread_t scoreUpdater;

    int playerCount = 0;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        printf("Failed to create socket\n");
        exit(0);
    }

    struct sockaddr_in server_address = {0};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(6013);

    bind(server_fd, (struct sockaddr *)&server_address, sizeof(server_address));

    listen(server_fd, 1);

    printf("Server listening on port 6013\n");

    FILE *file = fopen("game.log", "a");
    if (!file)
    {
        printf("Error in opening log file\n");
        exit(0);
    }

    fprintf(file, "\n========================\n");
    fprintf(file, "New Game...\n");
    fprintf(file, "Match Loading...\n");
    fclose(file);

    pthread_create(&checkForDisconnects, NULL, pollForDisconnect, &playerCount);

    pthread_mutex_lock(&lock);
    while (playerCount < 4)
    {
        pthread_mutex_unlock(&lock);
        int clientfd;
        clientfd = accept(server_fd, NULL, NULL);
        if (clientfd == -1)
        {
            printf("Error trying to accept clients\n");
            exit(0);
        }
        else
        {
            char buffer[51];
            ssize_t n = read(clientfd, buffer, sizeof(buffer) - 1);
            if (n <= 0)
            {
                printf("Error reading from client\n");
                close(clientfd);
                continue;
            }
            else
            {
                buffer[n] = '\0';
                printf("Player %s joined!\n", buffer);

                Player *newPlayer = malloc(sizeof(Player));
                strncpy(newPlayer->name, buffer, sizeof(newPlayer->name) - 1);
                newPlayer->playerId = playerCount;
                newPlayer->client_fd = clientfd;
                newPlayer->score = 0;
                if (playerCount % 2 == 0)
                {
                    newPlayer->team = BLUE;
                }
                else
                {
                    newPlayer->team = RED;
                }

                write(clientfd, &newPlayer->playerId, sizeof(newPlayer->playerId));

                pthread_mutex_lock(&lock);
                playerQueue[playerCount] = newPlayer;
                playerCount++;
                pthread_mutex_unlock(&lock);

                pthread_create(&scoreLoader, NULL, load_score, newPlayer);
                pthread_detach(scoreLoader);
                pthread_mutex_lock(&lock);
            }
        }
    }
    pthread_mutex_unlock(&lock);

    // Initialise shared memory

    SharedData *shared = mmap(
        NULL,
        sizeof(SharedData),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1,
        0);

    if (shared == MAP_FAILED)
    {
        printf("Error creating Shared Memory\n");
        exit(1);
    }

    pthread_mutexattr_t turnStateLock;
    pthread_condattr_t turnStateCond;

    pthread_mutexattr_init(&turnStateLock);
    pthread_mutexattr_setpshared(&turnStateLock, PTHREAD_PROCESS_SHARED);

    pthread_condattr_init(&turnStateCond);
    pthread_condattr_setpshared(&turnStateCond, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&shared->turnStructLock, &turnStateLock);
    pthread_cond_init(&shared->turnStructCond, &turnStateCond);
    shared->threadTurn = threadTurn;
    shared->gameState = gameState;
    shared->redShipCount = 0;
    shared->blueShipCount = 0;
    shared->gamePhase = gamePhase;
    shared->activePlayerCount = playerCount;
    memset(shared->redShips, 0, sizeof shared->redShips);
    memset(shared->blueShips, 0, sizeof shared->blueShips);

    // Initialize circular linked list of players
    PlayerNode *prevNode = NULL;
    for (int i = 0; i < playerCount; i++)
    {
        PlayerNode *newNode = malloc(sizeof(PlayerNode));
        newNode->player = playerQueue[i];
        newNode->next = NULL;

        if (i == 0)
        {
            shared->playerQueueHead = newNode;
        }
        else
        {
            prevNode->next = newNode;
        }
        prevNode = newNode;
    }
    // Make it circular - last node points to head
    if (prevNode != NULL)
    {
        prevNode->next = shared->playerQueueHead;
    }
    shared->currentPlayerNode = NULL;

    // threads
    pthread_create(&loggerThread, NULL, loggerFunction, shared);
    pthread_create(&schedulerThread, NULL, schedulerFunction, shared);

    // Create client handlers

    pid_t parent = getpid();
    setup_sigchild();

    for (int i = 0; i < playerCount; i++)
    {
        if (pipe(pipes[playerQueue[i]->playerId]) == -1)
        {
            printf("Error creating pipe\n");
            exit(0);
        }
        if (getpid() == parent)
        {
            pid_t pid = fork();

            if (pid == 0)
            {
                clientHandler(shared, playerQueue[i], pipes[playerQueue[i]->playerId]);
                exit(0);
            }
        }
    }
    // inform disconnect thread that the game started
    gameStart = true;
    int globalShipCount = 0;

    while (true)
    {
        pthread_mutex_lock(&shared->turnStructLock);
        while (shared->threadTurn != HANDLER)
        {
            pthread_cond_wait(&shared->turnStructCond, &shared->turnStructLock);
        }

        // get current player info
        Player *currentPlayer = (shared->gameState).curTurnPlayer;
        teamList curTeam = currentPlayer->team;
        int id = currentPlayer->playerId;
        pthread_mutex_unlock(&shared->turnStructLock);

        if (playerCount <= 0)
        {
            for (int i = 0; i < 4; i++)
            {
                if (pipes[i][0] != -1)
                {
                    close(pipes[i][0]);
                }

                if (pipes[i][1] != -1)
                {
                    close(pipes[i][1]);
                }
            }

            file = fopen("game.log", "a");
            if (!file)
            {
                printf("Error in opening log file\n");
                exit(0);
            }
            fprintf(file, "All players disconnected.\n");
            break;
        }

        msg clientMessage;

        read(pipes[id][0], &clientMessage, sizeof(clientMessage));

        // if player disconnected
        if (clientMessage.disconnected)
        {
            pthread_mutex_lock(&shared->turnStructLock);
            shared->gameState.hitTarget.x = -1;
            shared->gameState.hitTarget.y = -1;
            shared->gameState.shipHit = false;
            shared->gameState.shipDestroyed = false;
            shared->gameState.gameEnd = false;
            shared->threadTurn = SCHEDULER;
            pthread_mutex_unlock(&shared->turnStructLock);
            pthread_cond_broadcast(&shared->turnStructCond);
            close(pipes[id][0]);
            close(pipes[id][1]);
            continue;
        }

        pthread_mutex_lock(&shared->turnStructLock);
        if (shared->gamePhase == PHASE_PLACEMENT)
        {

            // maybe client handlers send game phase to client?
            // disconnect thread will also check for phases to send different "poison pipe messages"

            int length = clientMessage.ship_id - 'a' + 2;
            char (*targetArr)[7];

            if (curTeam == RED)
            {
                targetArr = shared->redShips;
            }
            else if (curTeam == BLUE)
            {
                targetArr = shared->blueShips;
            }
            else
            {
                printf("Invalid Team\n");
                exit(0);
            }
            pthread_mutex_unlock(&shared->turnStructLock);

            if (clientMessage.dir != 'v' && clientMessage.dir != 'h')
            {
                printf("Invalid direction\n");
                exit(0);
            }

            int col, row;

            for (int p = 0; p < length; p++)
            {

                col = clientMessage.col + p * (clientMessage.dir == 'h');
                row = clientMessage.row + p * (clientMessage.dir == 'v');

                if (targetArr[row][col] != '\0')
                {
                    printf("Invalid operation\n");
                }
                else
                {
                    targetArr[row][col] = clientMessage.ship_id;
                    pthread_mutex_lock(&shared->turnStructLock);
                    curTeam == RED ? shared->redShipCount++ : shared->blueShipCount++;
                    pthread_mutex_unlock(&shared->turnStructLock);
                }
            }
            globalShipCount++;
            if (globalShipCount == 8)
            {
                shared->gamePhase = PHASE_PLAYING;
            }
        }
        else if (shared->gamePhase == PHASE_PLAYING)
        {
            shared->gameState.hitTarget.x = clientMessage.row;
            shared->gameState.hitTarget.y = clientMessage.col;
            shared->gameState.shipHit = clientMessage.hit;
            shared->gameState.shipDestroyed = clientMessage.sunk;
            // decrement count on enemy team if a ship is sunk
            if (clientMessage.sunk)
                curTeam == RED ? shared->blueShipCount-- : shared->redShipCount--;

            if (shared->blueShipCount == 0 || shared->redShipCount == 0)
            {
                shared->gamePhase = PHASE_GAME_OVER;
            }
            pthread_mutex_unlock(&shared->turnStructLock);
        }
        else if (shared->gamePhase == PHASE_GAME_OVER)
        {
            pthread_mutex_unlock(&shared->turnStructLock);
            char (*name)[51] = malloc(sizeof(char[2][51]));
            short pCount = 0;
            file = fopen("game.log", "a");
            if (!file)
            {
                printf("Error in opening log file\n");
                exit(0);
            }

            fprintf(file, "Winning team: %s \n", curTeam == BLUE ? "BLUE" : "RED");
            fprintf(file, "Winning players:\n");
            pthread_mutex_lock(&lock);
            for (int i = 0; i < playerCount; i++)
            {
                if (playerQueue[i]->team == curTeam)
                {
                    strcpy(name[pCount++], playerQueue[i]->name);
                }
            }
            pthread_create(&scoreUpdater, NULL, updateScore, name);
            for (int i = 0; i < playerCount; i++)
            {
                if (playerQueue[i]->team == curTeam)
                {
                    fprintf(file, "%s", playerQueue[i]->name);
                    int closeStatus1 = close(pipes[playerQueue[i]->playerId][0]);
                    if (closeStatus1 != 0)
                    {
                        printf("Error closing pipe for player %s\n", playerQueue[i]->name);
                        continue;
                    }

                    int closeStatus2 = close(pipes[playerQueue[i]->playerId][1]);
                    if (closeStatus2 != 0)
                    {
                        printf("Error closing pipe for player %s\n", playerQueue[i]->name);
                        continue;
                    }
                }
            }

            pthread_mutex_unlock(&lock);
            pthread_join(scoreUpdater, NULL);
            free(name);
            break;
        }
        else
        {
            pthread_mutex_unlock(&shared->turnStructLock);
            printf("Error, can't ascertain game state.\n");
            exit(0);
        }

        // change gameState

        shared->threadTurn = LOGGER;
        pthread_cond_broadcast(&shared->turnStructCond);
    }

    // stop polling for disconnects
    gameEnd = true;
    pthread_mutex_destroy(&lock);
    pthread_mutexattr_destroy(&turnStateLock);
    pthread_condattr_destroy(&turnStateCond);
    pthread_detach(checkForDisconnects);

    for (int i = 0; i < playerCount; i++)
    {
        if (playerQueue[i] != NULL)
        {
            close(playerQueue[i]->client_fd);
            free(playerQueue[i]);
        }
    }
    free(shared);
    close(server_fd);

    file = fopen("game.log", "a");
    if (!file)
    {
        printf("Error in opening log file\n");
        exit(0);
    }
    fprintf(file, "Game End\n");

    pthread_mutex_destroy(&turnStructLock);
    pthread_cond_destroy(&turnStructConditionVar);

    return 0;
}
