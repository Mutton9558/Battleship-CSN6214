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
#include <fcntl.h>
#include <sys/select.h>

#define HIT 'X'
#define MISS 'O'
#define PHASE_PLACEMENT 1
#define PHASE_PLAYING 2
#define PHASE_GAME_OVER 3

const int PORT = 8080;

enum threadList
{
    HANDLER,
    SCHEDULER,
    LOGGER
};

typedef enum
{
    BLUE,
    RED
} teamList;

typedef struct
{
    int x;
    int y;
} Tuple;

typedef struct
{
    char name[51];
    teamList team;
    int client_fd;
    int playerId;
    int score;
    bool disconnected;
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
    int gamePhase;
} turnState;

typedef struct
{
    enum threadList threadTurn;
    pthread_mutex_t turnStructLock;
    pthread_cond_t turnStructCond;
    turnState gameState;
    char redTeamMembers[2][51];
    char blueTeamMembers[2][51];
    char blueShips[7][7];
    char redShips[7][7];
    int redShipCount;
    int blueShipCount;
    PlayerNode *playerQueueHead;
    PlayerNode *currentPlayerNode;
    teamList winningTeam;
    char winnerNames[2][51];
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
int redPlayersCount = 0;
int bluePlayersCount = 0;
enum threadList threadTurn = SCHEDULER;

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
    bool playerFound = false;
    while (fgets(line, 63, file))
    {
        if (sscanf(line, "%50[^:]: %d", name, &score) == 2)
        {
            if (strcmp(name, player->name) == 0)
            {
                player->score = score;
                playerFound = true;
            }
        }
    }
    fclose(file);

    if (!playerFound)
    {
        FILE *scoreFile = fopen("score.txt", "a");
        if (!scoreFile)
        {
            printf("Error in opening score file\n");
            exit(0);
        }
        fprintf(scoreFile, "%s: %d\n", player->name, 0);
        fclose(scoreFile);
    }

    FILE *logFile = fopen("game.log", "a");
    if (!logFile)
    {
        printf("Error in opening log file\n");
        exit(0);
    }
    char team[5];
    if (player->team == BLUE)
    {
        strncpy(team, "Blue", sizeof(team));
    }
    else
    {
        strncpy(team, "Red", sizeof(team));
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
    return NULL;
}

void *pollForDisconnect(void *arg)
{
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

                        FILE *logFile = fopen("game.log", "a");
                        if (!logFile)
                        {
                            printf("Error in opening log file\n");
                            free(name);
                            exit(0);
                        }
                        fprintf(logFile, "Player %s disconnected.\n", name);

                        if (!gameStart)
                        {
                            free(playerQueue[i]);
                            for (int j = i; j < (*playerCount) - 1; j++)
                            {
                                (playerQueue[(*playerCount) - 1]->team == RED) ? redPlayersCount-- : bluePlayersCount--;
                                playerQueue[j] = playerQueue[j + 1];
                                playerQueue[j]->playerId = j;
                                playerQueue[j]->team = (playerQueue[j]->team == RED) ? BLUE : RED;
                                fprintf(logFile, "Player %s reassigned to team %s\n.", playerQueue[j]->name, playerQueue[j]->team == RED ? "RED" : "BLUE");
                            }

                            playerQueue[(*playerCount) - 1] = NULL;
                            (*playerCount)--;
                        }
                        else
                        {
                            playerQueue[i]->disconnected = true;
                            for (int j = 0; j < (*playerCount); j++)
                            {
                                if (playerQueue[j]->team == RED && playerQueue[j]->disconnected)
                                {
                                    redPlayersCount--;
                                }
                                else
                                {
                                    bluePlayersCount--;
                                }
                            }
                        }
                        fflush(logFile);
                        fclose(logFile);

                        pthread_mutex_unlock(&lock);
                    }
                }
            }
        }
    }
    return NULL;
}

bool dfsCheck(char board[7][7], bool visited[7][7], char target, int x, int y)
{
    if (x < 0 || x > 6 || y < 0 || y > 6)
        return false;

    if (visited[x][y])
        return false;

    visited[x][y] = true;

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
    char myShip[7][7];
    char enemyShip[7][7];
    bool gameStarted = true;
    write(clientFd, &gameStarted, sizeof(bool));
    write(clientFd, &shared->gameState.curTurnPlayer->score, sizeof(shared->gameState.curTurnPlayer->score));
    write(clientFd, &team, sizeof(teamList));
    if (team == RED)
    {
        write(clientFd, shared->redTeamMembers, sizeof(shared->redTeamMembers));
    }
    else
    {
        write(clientFd, shared->blueTeamMembers, sizeof(shared->redTeamMembers));
    }

    while (true)
    {
        int curTurnPlayerId;
        do
        {
            pthread_mutex_lock(&shared->turnStructLock);
            curTurnPlayerId = shared->gameState.curTurnPlayer->playerId;
            write(clientFd, &shared->gameState.gamePhase, sizeof(shared->gameState.gamePhase));
            pthread_mutex_unlock(&shared->turnStructLock);
            write(clientFd, &curTurnPlayerId, sizeof(curTurnPlayerId));
            sleep(1);
        } while (id != curTurnPlayerId);

        msg clientMsg;

        if (team == RED)
        {
            pthread_mutex_lock(&shared->turnStructLock);
            memcpy(myShip, shared->redShips, sizeof(shared->redShips));
            memcpy(enemyShip, shared->blueShips, sizeof(shared->blueShips));
            pthread_mutex_unlock(&shared->turnStructLock);
        }
        else
        {
            pthread_mutex_lock(&shared->turnStructLock);
            memcpy(myShip, shared->blueShips, sizeof(shared->blueShips));
            memcpy(enemyShip, shared->redShips, sizeof(shared->redShips));
            pthread_mutex_unlock(&shared->turnStructLock);
        }

        write(clientFd, myShip, sizeof(myShip));
        write(clientFd, enemyShip, sizeof(enemyShip));

        if (shared->gameState.gamePhase == PHASE_PLACEMENT)
        {
            pthread_mutex_lock(&shared->turnStructLock);
            int teamShipCount = (team == RED ? shared->redShipCount : shared->blueShipCount);
            pthread_mutex_unlock(&shared->turnStructLock);
            write(clientFd, &teamShipCount, sizeof(teamShipCount));
            ssize_t n = read(clientFd, &clientMsg, sizeof(msg));
            if (n <= 0)
            {
                printf("Client disconnected\n");
                fflush(stdout);
                clientMsg.disconnected = true;
                write(pipefd[1], &clientMsg, sizeof(clientMsg));
                _exit(-1);
            }
        }
        else if (shared->gameState.gamePhase == PHASE_PLAYING)
        {
            ssize_t n = read(clientFd, &clientMsg, sizeof(msg));
            if (n <= 0)
            {
                printf("Client disconnected\n");
                fflush(stdout);
                clientMsg.disconnected = true;
                write(pipefd[1], &clientMsg, sizeof(clientMsg));
                _exit(-1);
            }
            int row = clientMsg.row;
            int col = clientMsg.col;
            char target = enemyShip[row][col];
            bool visited[7][7] = {false};
            resultMsg msgToClient;

            if (target == 'a' || target == 'b' || target == 'c' || target == 'd')
            {
                char prevChar = target;
                if (team == RED)
                {
                    shared->blueShips[row][col] = HIT;
                }
                else
                {
                    shared->redShips[row][col] = HIT;
                }
                enemyShip[row][col] = HIT;
                bool alive = dfsCheck(enemyShip, visited, prevChar, row, col);

                msgToClient.game_over = false;
                msgToClient.hit = true;
                clientMsg.hit = true;
                clientMsg.sunk = !alive;
                msgToClient.sunk = !alive;
            }
            else
            {
                if (target != HIT && target != MISS)
                {
                    if (team == RED)
                    {
                        shared->blueShips[row][col] = MISS;
                    }
                    else
                    {
                        shared->redShips[row][col] = MISS;
                    }
                    msgToClient.game_over = false;
                    msgToClient.hit = false;
                    msgToClient.sunk = false;
                    clientMsg.hit = false;
                    clientMsg.sunk = false;
                }
                else
                {
                    printf("Target attempted to hit a space already hit before...\n");
                    fflush(stdout);
                }
            }
            write(clientFd, &msgToClient, sizeof(msgToClient));
        }
        else if (shared->gameState.gamePhase == PHASE_GAME_OVER)
        {
            char winnerNames[2][51];
            pthread_mutex_lock(&shared->turnStructLock);
            while (shared->winnerNames[0][0] == '\0')
            {
                pthread_cond_wait(&shared->turnStructCond, &shared->turnStructLock);
            }
            pthread_mutex_unlock(&shared->turnStructLock);
            memcpy(winnerNames, shared->winnerNames, sizeof(shared->winnerNames));

            printf("Winner names %s\n", winnerNames[0]);
            printf("Winner names %s\n", winnerNames[1]);
            fflush(stdout);

            write(clientFd, &shared->winningTeam, sizeof(teamList));
            _exit(0);
        }

        write(pipefd[1], &clientMsg, sizeof(clientMsg));
        sleep(2);
    }

    pthread_cond_broadcast(&shared->turnStructCond);
}

void *loggerFunction(void *arg)
{
    SharedData *data = (SharedData *)arg;

    while (!gameEnd)
    {
        pthread_mutex_lock(&data->turnStructLock);

        while (data->threadTurn != LOGGER)
        {
            pthread_cond_wait(&data->turnStructCond, &data->turnStructLock);
        }

        // Read and log turn state data
        Player *currentPlayer = data->gameState.curTurnPlayer;
        Tuple hitTarget = data->gameState.hitTarget;
        bool shipHit = data->gameState.shipHit;
        bool shipDestroyed = data->gameState.shipDestroyed;
        int currentPhase = data->gameState.gamePhase;

        pthread_mutex_unlock(&data->turnStructLock);

        FILE *logFile = fopen("game.log", "a");
        if (!logFile)
        {
            printf("Error opening game.log for logging\n");
            pthread_mutex_unlock(&data->turnStructLock);
            return NULL;
        }

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
        fclose(logFile);

        pthread_mutex_lock(&data->turnStructLock);

        // Signal scheduler that logger has finished accessing turnState
        data->threadTurn = SCHEDULER;
        pthread_mutex_unlock(&data->turnStructLock);
        pthread_cond_broadcast(&data->turnStructCond);
    }
    return NULL;
}

void *schedulerFunction(void *arg)
{
    SharedData *data = (SharedData *)arg;

    while (!gameEnd)
    {
        pthread_mutex_lock(&data->turnStructLock);

        while (data->threadTurn != SCHEDULER)
        {
            pthread_cond_wait(&data->turnStructCond, &data->turnStructLock);
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
            while (data->currentPlayerNode && data->currentPlayerNode->player->disconnected)
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
                printf("Selected player %s\n", data->currentPlayerNode->player->name);
            }
        }

        // Signal client handler that turn state is cleared and new player selected
        data->threadTurn = HANDLER;
        pthread_mutex_unlock(&data->turnStructLock);
        pthread_cond_broadcast(&data->turnStructCond);
    }

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

    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    printf("Server listening on port 6013\n");

    while (true)
    {
        int playerCount = 0;

        FILE *file = fopen("game.log", "a");
        if (!file)
        {
            printf("Error in opening log file\n");
            exit(0);
        }

        fprintf(file, "\n========================\n");
        fprintf(file, "New Game...\n");
        fprintf(file, "Match Loading...\n");
        fflush(file);
        fclose(file);

        pthread_create(&checkForDisconnects, NULL, pollForDisconnect, &playerCount);

        time_t startTime = time(NULL);

        while (true)
        {
            pthread_mutex_lock(&lock);
            int count = playerCount;
            pthread_mutex_unlock(&lock);
            // stop immediately if max reached
            if (count == 4)
                break;

            // stop if timeout reached AND minimum satisfied
            if (count >= 3 &&
                difftime(time(NULL), startTime) >= 60)
                break;

            if (count < 3 && difftime(time(NULL), startTime) >= 60)
            {
                printf("Not enough players\n");
                FILE *file = fopen("game.log", "a");
                if (!file)
                {
                    printf("Error in opening log file\n");
                    exit(0);
                }
                fprintf(file, "\nCannot get enough players...Resetting matchmaking\n");
                fflush(file);
                fclose(file);

                break;
            }

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);

            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int ready = select(server_fd + 1, &readfds, NULL, NULL, &tv);

            if (ready > 0 && FD_ISSET(server_fd, &readfds))
            {
                int clientfd = accept(server_fd, NULL, NULL);
                if (clientfd < 0)
                    continue;

                char buffer[51];
                ssize_t n = read(clientfd, buffer, sizeof(buffer) - 1);
                if (n <= 0)
                {
                    close(clientfd);
                    continue;
                }

                buffer[n] = '\0';

                printf("Player %s joined!\n", buffer);

                Player *newPlayer = malloc(sizeof(Player));
                strncpy(newPlayer->name, buffer, sizeof(newPlayer->name) - 1);
                newPlayer->name[50] = '\0';

                pthread_mutex_lock(&lock);
                newPlayer->playerId = playerCount;
                if (playerCount % 2 == 0)
                {
                    newPlayer->team = BLUE;
                    bluePlayersCount++;
                }
                else
                {
                    newPlayer->team = RED;
                    redPlayersCount++;
                }
                newPlayer->client_fd = clientfd;
                newPlayer->score = 0;
                newPlayer->disconnected = false;
                playerQueue[playerCount++] = newPlayer;
                pthread_mutex_unlock(&lock);

                write(clientfd, &newPlayer->playerId, sizeof(newPlayer->playerId));
                time_t elapsedTime = time(NULL) - startTime;
                write(clientfd, &elapsedTime, sizeof(elapsedTime));

                pthread_create(&scoreLoader, NULL, load_score, newPlayer);
                pthread_detach(scoreLoader);
            }
        }

        printf("Game about to start \n");

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
        shared->gameState.gamePhase = PHASE_PLACEMENT;
        memset(shared->redShips, 0, sizeof shared->redShips);
        memset(shared->blueShips, 0, sizeof shared->blueShips);
        memset(shared->winnerNames[0], 0, sizeof(shared->winnerNames[0]));
        memset(shared->winnerNames[1], 0, sizeof(shared->winnerNames[0]));

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

        int redMemCount = 0;
        int blueMemCount = 0;
        shared->redTeamMembers[0][0] = '\0';
        shared->redTeamMembers[1][0] = '\0';
        shared->blueTeamMembers[0][0] = '\0';
        shared->blueTeamMembers[1][0] = '\0';
        for (int i = 0; i < playerCount; i++)
        {
            if (playerQueue[i]->team == RED)
            {
                memset(shared->redTeamMembers[redMemCount], 0, 51);
                strncpy(shared->redTeamMembers[redMemCount], playerQueue[i]->name, 50);
                redMemCount++;
            }
            else
            {
                memset(shared->blueTeamMembers[blueMemCount], 0, 51);
                strncpy(shared->blueTeamMembers[blueMemCount], playerQueue[i]->name, 50);
                blueMemCount++;
            }
        }

        // threads
        pthread_create(&schedulerThread, NULL, schedulerFunction, shared);
        pthread_create(&loggerThread, NULL, loggerFunction, shared);

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
        printf("Game started\n");

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

            msg clientMessage;
            memset(&clientMessage, 0, sizeof(clientMessage));
            if (shared->gameState.gamePhase != PHASE_GAME_OVER)
            {
                read(pipes[id][0], &clientMessage, sizeof(clientMessage));

                if (clientMessage.disconnected)
                {
                    if (redPlayersCount == 0)
                    {
                        shared->redShipCount = 0;
                        shared->gameState.gamePhase = PHASE_GAME_OVER;
                    }
                    else if (bluePlayersCount == 0)
                    {
                        shared->blueShipCount = 0;
                        shared->gameState.gamePhase = PHASE_GAME_OVER;
                    }

                    pthread_mutex_lock(&shared->turnStructLock);
                    shared->threadTurn = SCHEDULER;
                    pthread_cond_broadcast(&shared->turnStructCond);
                    pthread_mutex_unlock(&shared->turnStructLock);
                    continue;
                }
            }

            pthread_mutex_lock(&shared->turnStructLock);
            if (shared->gameState.gamePhase == PHASE_PLACEMENT)
            {
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
                    }
                }
                pthread_mutex_lock(&shared->turnStructLock);
                curTeam == RED ? shared->redShipCount++ : shared->blueShipCount++;
                pthread_mutex_unlock(&shared->turnStructLock);
                globalShipCount++;
                if (globalShipCount >= 8)
                {
                    shared->gameState.gamePhase = PHASE_PLAYING;
                }
            }
            else if (shared->gameState.gamePhase == PHASE_PLAYING)
            {
                shared->gameState.hitTarget.x = clientMessage.row;
                shared->gameState.hitTarget.y = clientMessage.col;
                shared->gameState.shipHit = clientMessage.hit;
                shared->gameState.shipDestroyed = clientMessage.sunk;
                // decrement count on enemy team if a ship is sunk
                if (clientMessage.sunk)
                {
                    curTeam == RED ? shared->blueShipCount-- : shared->redShipCount--;
                }

                if (shared->blueShipCount == 0 || shared->redShipCount == 0)
                {
                    shared->gameState.gamePhase = PHASE_GAME_OVER;
                }

                pthread_mutex_unlock(&shared->turnStructLock);
            }
            else if (shared->gameState.gamePhase == PHASE_GAME_OVER)
            {

                if (shared->blueShipCount == 0)
                {
                    shared->winningTeam = RED;
                    memcpy(shared->winnerNames, shared->redTeamMembers, sizeof(shared->redTeamMembers));
                }
                else
                {
                    shared->winningTeam = BLUE;
                    memcpy(shared->winnerNames, shared->blueTeamMembers, sizeof(shared->blueTeamMembers));
                }
                pthread_cond_broadcast(&shared->turnStructCond);
                pthread_mutex_unlock(&shared->turnStructLock);

                pthread_create(&scoreUpdater, NULL, updateScore, shared->winnerNames);

                for (int i = 0; i < playerCount; i++)
                {
                    int readPipe = pipes[playerQueue[i]->playerId][0];
                    int writePipe = pipes[playerQueue[i]->playerId][1];
                    int readCloseStatus = close(readPipe);
                    if (readCloseStatus != 0)
                    {
                        printf("Error closing pipe for player %s\n", playerQueue[i]->name);
                        continue;
                    }

                    int writeCloseStatus = close(writePipe);
                    if (writeCloseStatus != 0)
                    {
                        printf("Error closing pipe for player %s\n", playerQueue[i]->name);
                        continue;
                    }
                }

                FILE *gameEndLog = fopen("game.log", "a");
                if (!gameEndLog)
                {
                    printf("Error in opening log file\n");
                    exit(0);
                }

                fprintf(gameEndLog, "Winning team: %s \n", shared->winningTeam == BLUE ? "BLUE" : "RED");
                fprintf(gameEndLog, "Winning players:\n");
                for (int i = 0; i < playerCount; i++)
                {
                    if (playerQueue[i]->team == shared->winningTeam)
                        fprintf(gameEndLog, "%s\n", playerQueue[i]->name);
                }
                fflush(gameEndLog);
                fclose(gameEndLog);

                pthread_join(scoreUpdater, NULL);
                break;
            }
            else
            {
                pthread_mutex_unlock(&shared->turnStructLock);
                printf("Error, can't ascertain game state.\n");
                exit(0);
            }

            pthread_mutex_lock(&shared->turnStructLock);
            // change gameState
            shared->threadTurn = LOGGER;
            pthread_cond_broadcast(&shared->turnStructCond);
            pthread_mutex_unlock(&shared->turnStructLock);
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
        munmap(shared, sizeof(SharedData));

        file = fopen("game.log", "a");
        if (!file)
        {
            printf("Error in opening log file\n");
            exit(0);
        }
        fprintf(file, "Game End\n");

        pthread_mutex_destroy(&turnStructLock);
        pthread_cond_destroy(&turnStructConditionVar);
    }

    close(server_fd);

    return 0;
}
