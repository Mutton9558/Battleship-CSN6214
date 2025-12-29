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

const int PORT = 8080;

typedef struct
{
    char name[50];
    int client_fd;
    int score;
} Player;

Player *playerQueue[4];
bool matchmakingDone = false;
pthread_mutex_t lock;

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

        int check = poll(poller, count, 100);

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
                        printf("Player %s disconnected\n", playerQueue[i]->name);
                        free(playerQueue[i]);
                        // shift a player to fill in that player's pos and remove last item of the array
                        // Why? Because C is so old that there is no better solutions I guess
                        // Well there was one that involved doing some iteration but that just makes everything slower no?
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

int main()
{
    pthread_mutex_init(&lock, NULL);
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

    pthread_t scoreLoader;
    pthread_t checkForDisconnects;
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

    for (int i = 0; i < playerCount; i++)
    {
        close(playerQueue[i]->client_fd);
        free(playerQueue[i]);
    }
    close(server_fd);

    return 0;
}
