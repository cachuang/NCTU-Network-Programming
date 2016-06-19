#include "Client.h"

using namespace std;

Client::Client()
{
    id = sockfd = status = server_port = socks_server_port = -1;
}

int Client::setConnection()
{
    struct hostent *hptr;

    file = fopen(filename, "r");
    
    if(!file) {
        strcpy(errorMsg, strerror(errno));
        return -1;
    }

    bzero(&socks_server_addr, sizeof(socks_server_addr));
    socks_server_addr.sin_family = AF_INET;
    socks_server_addr.sin_port = htons(socks_server_port);

    if( (hptr = gethostbyname(socks_server_ip)) == NULL ) {
        strcpy(errorMsg, strerror(errno));
        return -1;
    }

    socks_server_addr.sin_addr = *(struct in_addr *)(hptr->h_addr);

    if( (sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
        strcpy(errorMsg, strerror(errno));
        return -1;
    }

    int flag = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);

    if( connect(sockfd, (struct sockaddr *) &socks_server_addr, sizeof(socks_server_addr)) < 0 ) {
        if (errno != EINPROGRESS) {
            strcpy(errorMsg, strerror(errno));
            return -1;
        };
    }

    return 0;
}

void Client::printText(const char *text)
{
    printf("<script>document.all['m%d'].innerHTML += '%s';</script>\n", id, text);
    fflush(stdout);
}