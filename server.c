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
} SharedData;

typedef struct
{
    bool disconnected;
    int msg_type; // e.g., MSG_PLACE_SHIP = 2
    char ship_id; // 'a', 'b', 'c', 'd'
    int row;
    int col;
    char dir; // 'h' or 'v' if 'n', error occured
} msg;

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

void sigchld_handler(int sig)
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

void setup_sigchld()
{
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; // restart interrupted syscalls
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction");
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
            printf("Error in creating score file");
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
    char line[62];
    int score;
    char name[50];
    while (fgets(line, 62, file))
    {
        if (sscanf(line, "%[^:]: %d", name, &score) == 2)
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
        printf("Error in opening log file");
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

void *updateFileDisconnect(void *arg)
{
    char *name = (char *)arg;
    FILE *logFile = fopen("game.log", "a");
    if (!logFile)
    {
        printf("Error in opening log file");
        free(name);
        exit(0);
    }
    fprintf(logFile, "PLayer %s disconnected.\n", name);
    fclose(logFile);
    free(name);
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
                        if (gameStart)
                        {
                            if (gamePhase == PHASE_PLACEMENT)
                            {
                                msg disconnectedMsg;
                                disconnectedMsg.disconnected = true;
                                write(pipes[playerQueue[i]->playerId][1], &disconnectedMsg, sizeof(disconnectedMsg));
                            }
                        }

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
    free(playerCount);
    return NULL;
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
    write(clientFd, true, sizeof(bool));
    if (team == RED)
    {
        enemyTeam == BLUE;
        pthread_mutex_lock(&shared->turnStructLock);
        memcpy(myShip, shared->redShips, sizeof(shared->redShips));
        memcpy(enemyShip, shared->blueShips, sizeof(shared->blueShips));
        pthread_mutex_unlock(&shared->turnStructLock);
    }
    else
    {
        enemyTeam == RED;
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
        write(clientFd, shared->gamePhase, sizeof(shared->gamePhase));
        pthread_mutex_unlock(&shared->turnStructLock);
        int curTurnPlayerId;
        do
        {
            pthread_mutex_lock(&shared->turnStructLock);
            curTurnPlayerId = shared->gameState.curTurnPlayer->playerId;
            pthread_mutex_unlock(&shared->turnStructLock);
            write(clientFd, curTurnPlayerId, sizeof(curTurnPlayerId));
            sleep(1);
        } while (id != curTurnPlayerId);

        msg clientMsg;
        if (shared->gamePhase == PHASE_PLACEMENT)
        {
            pthread_mutex_lock(&shared->turnStructLock);
            int teamShipCount = (player->team == RED ? shared->redShipCount : shared->blueShipCount);
            pthread_mutex_unlock(&shared->turnStructLock);
            write(clientFd, teamShipCount, sizeof(teamShipCount));

            ssize_t n = read(clientFd, &clientMsg, sizeof(msg));
            if (n <= 0)
            {
                printf("Client disconnected\n");
                exit(-1);
            }
        }
        else if (shared->gamePhase == PHASE_GAME_OVER)
        {
            // kill child
            exit(0);
        }

        write(pipes[id][1], &clientMsg, sizeof(clientMsg));
    }

    pthread_cond_broadcast(&shared->turnStructCond);
}

void loggerFunction(void *arg)
{
    SharedData *data = (SharedData *)arg;
    if (data->gamePhase == PHASE_PLACEMENT)
    {
    }
    else if (data->gamePhase == PHASE_PLAYING)
    {
    }
    else if (data->gamePhase == PHASE_GAME_OVER)
    {
    }
    else
    {
        printf("Invalid phase");
        exit(-1);
    }
    free(data);
}

void updateGameState(SharedData *shared)
{
    //     typedef struct
    // {
    //     enum threadList threadTurn;
    //     pthread_mutex_t turnStructLock;
    //     pthread_cond_t turnStructCond;
    //     turnState gameState;
    //     bool turnDone;
    //     char blueShips[7][7];
    //     char redShips[7][7];
    // } SharedData;

    // typedef struct
    // {
    //     Player *curTurnPlayer;
    //     Tuple hitTarget;
    //     bool shipDestroyed;
    //     bool shipHit;
    //     bool gameEnd;
    // }
    // turnState;
}

int main()
{
    pthread_mutex_init(&lock, NULL);
    pthread_mutex_init(&turnStructLock, NULL);
    pthread_cond_init(&turnStructConditionVar, NULL);
    int playerCount = 0;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        printf("Failed to create socket");
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
        printf("Error in opening log file");
        exit(0);
    }

    fprintf(file, "\n========================\n");
    fprintf(file, "New Game...\n");
    fprintf(file, "Match Loading...\n");
    fclose(file);

    // all the threads
    pthread_t scoreLoader;
    pthread_t checkForDisconnects;
    pthread_t schedulerThread;
    pthread_t loggerThread;

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
                pthread_create(&scoreLoader, NULL, load_score, newPlayer);
                pthread_detach(scoreLoader);
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
        printf("Error creating Shared Memory");
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
    memset(shared->redShips, 0, sizeof shared->redShips);
    memset(shared->blueShips, 0, sizeof shared->blueShips);

    // threads
    pthread_create(&loggerThread, NULL, loggerFunction, &shared);

    // Create client handlers

    pid_t parent = getpid();
    setup_sigchld();

    for (int i = 0; i < playerCount; i++)
    {
        if (pipe(pipes[playerQueue[i]->playerId]) == -1)
        {
            printf("Error creating pipe");
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

    while (!gameEnd)
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
        int n = playerCount;
        pthread_mutex_unlock(&shared->turnStructLock);

        if (shared->gamePhase == PHASE_PLACEMENT)
        {

            msg placementMessage;

            // maybe client handlers send game phase to client?
            // disconnect thread will also check for phases to send different "poison pipe messages"
            read(pipes[id][0], &placementMessage, sizeof(placementMessage));

            pthread_mutex_lock(&shared->turnStructLock);
            // if player disconnected
            if (placementMessage.disconnected)
            {
                pthread_mutex_lock(&shared->turnStructLock);
                shared->gameState.hitTarget.x = -1;
                shared->gameState.hitTarget.y = -1;
                shared->gameState.shipHit = false;
                shared->gameState.shipDestroyed = false;
                shared->gameState.gameEnd = false;
                shared->threadTurn = LOGGER;
                pthread_mutex_unlock(&shared->turnStructLock);
                pthread_cond_broadcast(&shared->turnStructCond);
                close(pipes[id][0]);
                close(pipes[id][1]);
                continue;
            }

            int length = placementMessage.ship_id - 'a' + 2;
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
                printf("Invalid Team");
                exit(0);
            }
            pthread_mutex_unlock(&shared->turnStructLock);

            if (placementMessage.dir != 'v' && placementMessage.dir != 'h')
            {
                printf("Invalid direction");
                exit(0);
            }

            int col, row;

            for (int p = 0; p < length; p++)
            {

                col = placementMessage.col + p * (placementMessage.dir == 'h');
                row = placementMessage.row + p * (placementMessage.dir == 'v');

                if (targetArr[row][col] != '\0')
                {
                    printf("Invalid operation");
                }
                else
                {
                    targetArr[row][col] = placementMessage.ship_id;
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
            // read from client handler
        }
        else if (shared->gamePhase == PHASE_GAME_OVER)
        {
            // player handling
        }
        else
        {
            printf("Error, can't ascertain game state.");
            exit(0);
        }

        // change gameState

        shared->threadTurn = LOGGER;
        pthread_cond_broadcast(&shared->turnStructCond);
    }

    sleep(3);

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
    close(server_fd);

    file = fopen("game.log", "a");
    if (!file)
    {
        printf("Error in opening log file");
        exit(0);
    }
    fprintf(file, "Game End");

    pthread_mutex_destroy(&turnStructLock);
    pthread_cond_destroy(&turnStructConditionVar);

    return 0;
}
