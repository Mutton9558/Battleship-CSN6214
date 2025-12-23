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