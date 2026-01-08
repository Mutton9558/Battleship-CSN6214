// Shawn and Imran's part
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

const int PORT = 8080;

typedef struct
{
    int x;
    int y;
} Tuple;

// typedef struct {
//     Tuple shipPos[5];
// } Ship;

// typedef struct{
//     char name[4];
//     Ship ships[4]
// } Team;

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
    int score;
} Player;

typedef struct
{
    Player *curTurnPlayer;
    Tuple hitTarget;
    bool shipDestroyed;
    bool shipHit;
    bool gameEnd;
    Player *playersDisconnected[4];
} turnState;

enum threadList
{
    HANDLER,
    SCHEDULER,
    LOGGER
};

enum threadList threadTurn = HANDLER;

Player *playerQueue[4];
turnState gameState;
bool matchmakingDone = false;
pthread_mutex_t lock;
pthread_mutex_t turnStructLock;
pthread_cond_t turnStructConditionVar;

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
    fprintf("Player %s joined.\n", player->name);
    fprintf("Player %s assigned team %s.\n", player->team);

    return NULL;
}

void *pollForDisconnect(void *arg)
{

    while (!matchmakingDone)
    {

        struct pollfd poller[4];

        // avoid race conditions/incorrect read
        pthread_mutex_lock(&lock);
        int *playerCount = (int *)arg;
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
                        printf("Player %s disconnected...\n", playerQueue[i]->name);
                        free(playerQueue[i]);
                        // shift a player to fill in that player's pos and remove last item of the array
                        playerQueue[i] = playerQueue[count - 1];
                        *playerCount--;
                        pthread_mutex_unlock(&lock);
                    }
                }
            }
        }
    }

    return NULL;
}

// void *disconnectMidGame(void *arg)
// {
//     bool *gameEnd = (bool *)arg;
//     while (!(*gameEnd))
//         ;
//     printf("Game ending\n");
//     return NULL;
// }

// void *clientHandling(void *arg)
// {
//     bool gameEnd = false;
//     pid_t threadPid = getpid();
//     pid_t clientSessions[4];
//     bool turnEnd = false;
//     bool validStart = false;
//     Player *curPlayer = gameState.curTurnPlayer;

//     int n = sizeof(playerQueue) / sizeof(playerQueue[0]);
//     for (int i = 0; i < n; i++)
//     {
//         if (getpid() == threadPid)
//         {
//             pid_t pid = fork();
//             if (pid < 0)
//             {
//                 printf("Error making client session for player %s", playerQueue[i]->name);
//                 exit(0);
//             }
//             else if (pid != threadPid)
//             {
//                 clientSessions[i] = pid;
//                 // create pipe also
//             }
//         }
//     }

//     pid_t curPid = getpid();

//     while (!gameEnd)
//     {

//         if (curPid == threadPid)
//         {
//             // parent

//             pthread_mutex_lock(&turnStructLock);
//             while (threadTurn != HANDLER)
//             {
//                 pthread_cond_wait(&turnStructConditionVar, &turnStructLock);
//             }
//             curPlayer = gameState.curTurnPlayer;
//             pthread_mutex_unlock(&turnStructLock);

//             // handles if user disconnected during logger + scheduler thread execution

//             validStart = true;

//             while (!turnEnd)
//             {
//                 sleep(1);
//             }
//             validStart = false;
//             // read from all pipes and see if there are msgs in the pipe
//             pthread_mutex_lock(&turnStructLock);
//             // change turnStruct
//             threadTurn = LOGGER;
//             pthread_mutex_unlock(&turnStructLock);
//         }
//         else
//         {
//             while (!validStart)
//                 ;
//             // child
//             int n = sizeof(clientSessions) / sizeof(clientSessions[0]);
//             Player *associatedPlayer;

//             for (int i = 0; i < n; i++)
//             {
//                 if (curPid == clientSessions[i])
//                 {
//                     associatedPlayer = playerQueue[i];
//                 }
//                 if (associatedPlayer == curPlayer)
//                 {
//                     // tcp write to indicate current turn
//                     // then blocking read to wait for respond
//                     // at the meantime create thread to listen for disconnect
//                     turnEnd = true;
//                     sleep(1);
//                 }
//                 else
//                 {
//                     // tcp write to indicate not their turn
//                     while (!turnEnd)
//                     {
//                         sleep(1);
//                     }
//                     // poll for disconnects
//                     sleep(1);
//                 }
//             }
//         }
//     }
// }

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
    pthread_t clientHandlerThread;
    pthread_t loggerThread;

    pthread_create(&checkForDisconnects, NULL, pollForDisconnect, &playerCount);
    pthread_detach(checkForDisconnects);
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
                newPlayer->client_fd = clientfd;
                newPlayer->score = 0;
                if (playerCount % 2 == 0)
                {
                    strncpy(newPlayer->team, BLUE, sizeof(BLUE) - 1);
                }
                else
                {
                    strncpy(newPlayer->team, RED, sizeof(RED) - 1);
                }
                pthread_mutex_lock(&lock);
                playerQueue[playerCount] = newPlayer;
                playerCount++;
                pthread_create(&scoreLoader, NULL, load_score, newPlayer);
                pthread_detach(scoreLoader);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    matchmakingDone = true;
    pthread_mutex_destroy(&lock);

    for (int i = 0; i < playerCount; i++)
    {
        close(playerQueue[i]->client_fd);
        free(playerQueue[i]);
    }
    close(server_fd);
    pthread_mutex_destroy(&turnStructLock);

    return 0;
}
