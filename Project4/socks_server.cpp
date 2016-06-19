#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <csignal>

using namespace std;

#define MAXLINE 15000
#define SOCKS_REQUEST_GRANTED 90
#define SOCKS_REQUEST_REJECT 91
#define CONNECT_COMMAND_CODE 1
#define BIND_COMMAND_CODE 2

#define FIREWALL_CONFIGURE_FILE "socks.conf"

void connectmode(int srcfd, char *dst_ip, unsigned int dst_port, unsigned char *request)
{
    int n, maxfd, nready, dstfd;
    char buffer[MAXLINE];
    struct sockaddr_in server_addr; 
    fd_set allset, rset;

    FD_ZERO(&allset);

    request[0] = 0;
    request[1] = SOCKS_REQUEST_GRANTED;

    // send SOCKS REPLY
    write(srcfd, request, 8);

    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(dst_port);

    if( inet_pton(AF_INET, dst_ip, &server_addr.sin_addr) < 0 ) {
        perror("inet_pton error");
        exit(errno);
    }

    if( (dstfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
        perror("socket error");
        exit(errno);
    }

    if( connect(dstfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0 ) {
        perror("connect error");
        exit(errno);
    }

    maxfd = (srcfd < dstfd) ? dstfd : srcfd;

    FD_SET(srcfd, &allset);
    FD_SET(dstfd, &allset);

    while(1)
    {
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        
        if(FD_ISSET(srcfd, &rset))
        {
            if( (n = read(srcfd, buffer, MAXLINE)) > 0 )
            {
                if(n < 10) {
                    buffer[n] = 0;
                    printf("Src: %s\n", buffer);
                }
                else
                    printf("Src: %.10s\n", buffer);

                write(dstfd, buffer, n);
            }
            else 
            {
                //printf("Src Close connection....\n");
                FD_CLR(srcfd, &allset);
                FD_CLR(dstfd, &allset);
                close(dstfd);
                close(srcfd);
                exit(0);
            }
        }

        if(FD_ISSET(dstfd, &rset))
        {
            if( (n = read(dstfd, buffer, MAXLINE)) > 0 )
            {
                if(n < 10) {
                    buffer[n] = 0;
                    printf("Dst: %s\n", buffer);
                }
                else
                    printf("Dst: %.10s\n", buffer);

                write(srcfd, buffer, n);
            }
            else 
            {
                //printf("Dst Close connection....\n");
                FD_CLR(srcfd, &allset);
                FD_CLR(dstfd, &allset);
                close(dstfd);
                close(srcfd);
                exit(0);
            }
        }
    }
}

void bindmode(int srcfd, unsigned char *request)
{
    int n, maxfd, nready, listenfd, dstfd;
    char buffer[MAXLINE];
    struct sockaddr_in server_addr, temp_addr; 
    socklen_t len;
    fd_set allset, rset;

    FD_ZERO(&allset);

    // bind ftp data port
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(INADDR_ANY);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
        perror("socket error");
        exit(errno);
    }

    if( bind(listenfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0 ) {
        perror("bind error");
        exit(errno);
    }

    if( listen(listenfd, 20) < 0 ) {
        perror("listen error");
        exit(errno);
    }

    len = sizeof(temp_addr);
    getsockname(listenfd, (struct sockaddr *) &temp_addr, &len);

    request[0] = 0;
    request[1] = SOCKS_REQUEST_GRANTED;
    request[2] = ntohs(temp_addr.sin_port) / 256;
    request[3] = ntohs(temp_addr.sin_port) % 256;
    request[4] = request[5] = request[6] = request[7] = 0;

    // send SOCKS REPLY
    write(srcfd, request, 8);

    dstfd = accept(listenfd, (struct sockaddr *) NULL, 0);

    write(srcfd, request, 8);

    maxfd = (srcfd < dstfd) ? dstfd : srcfd;

    FD_SET(srcfd, &allset);
    FD_SET(dstfd, &allset);

    while(1)
    {
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        
        if(FD_ISSET(srcfd, &rset))
        {
            if( (n = read(srcfd, buffer, MAXLINE)) > 0 )
                write(dstfd, buffer, n);
            else 
            {
                close(dstfd);
                close(srcfd);
                exit(0);
            }
        }
        if(FD_ISSET(dstfd, &rset))
        {
            if( (n = read(dstfd, buffer, MAXLINE)) > 0 )
                write(srcfd, buffer, n);
            else 
            {
                close(dstfd);
                close(srcfd);
                exit(0);
            }
        }
    }
}

void handler(int signal)
{
    while(waitpid(-1, NULL, WNOHANG) > 0);

    return;
}

int main(int argc, char **argv)
{
    int listenfd, srcfd, n;
    struct sockaddr_in server_addr, client_addr;
    unsigned char buffer[MAXLINE];

    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(argv[1]));
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
        perror("socket error");
        exit(errno);
    }

    if( bind(listenfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0 ) {
        perror("bind error");
        exit(errno);
    }

    if( listen(listenfd, 20) < 0 ) {
        perror("listen error");
        exit(errno);
    }

    signal(SIGCHLD, handler);

    while(1)
    {
        socklen_t len;
        len = sizeof(client_addr);

        srcfd = accept(listenfd, (struct sockaddr *) &client_addr, &len);
        if(errno == EINTR)  continue;

        if( fork() == 0 )
        {
            close(listenfd);

            // read SOCKS REQUEST
            if( (n = read(srcfd, buffer, MAXLINE)) > 0 )
            {
                unsigned char vn = buffer[0];
                unsigned char cd = buffer[1];
                unsigned int dst_port = buffer[2] << 8 | buffer[3];
                char *userid = (char *) buffer + 8;
                char dst_ip[MAXLINE];

                char src_ip[MAXLINE];
                unsigned int src_port;

                FILE *fp = fopen(FIREWALL_CONFIGURE_FILE, "r");
                char cmd[20], mode[2], target[20], ip[5][5]; 
                char match_rule[100] = "No matching rule";
                bool permit = false;

                snprintf(dst_ip, sizeof(dst_ip), "%u.%u.%u.%u", buffer[4], buffer[5], buffer[6], buffer[7]);
                
                // get soruce ip and port
                inet_ntop(AF_INET, &client_addr.sin_addr, src_ip, sizeof(src_ip));
                src_port = ntohs(client_addr.sin_port);
                
                printf("VN: %u, CD: %u, DST_IP: %s, DST_PORT: %u, USERID: %s\n", vn, cd, dst_ip, dst_port, userid);
                
                if(cd == CONNECT_COMMAND_CODE)
                    printf("[CONNECT MODE] %s/%u -> %s/%u\n", src_ip, src_port, dst_ip, dst_port);
                else if(cd == BIND_COMMAND_CODE)
                    printf("[BIND MODE] %s/%u -> %s/%u\n", src_ip, src_port, dst_ip, dst_port);
                
                for(int i = 0; i < 4; i++)
                    snprintf(ip[i], sizeof(ip[i]), "%u", buffer[i+4]);

                if(fp)
                {
                    while( (fscanf(fp, "%s %s %s", cmd, mode, target)) != EOF )
                    {
                        char *pch;
                        char _ip[5][20], target_ip[20];
                        unsigned char _cd;

                        strcpy(target_ip, target);

                        _cd = (mode[0] == 'c') ? CONNECT_COMMAND_CODE : BIND_COMMAND_CODE;

                        if((pch = strtok(target, ".")));
                            strcpy(_ip[0], pch);
                        if((pch = strtok(NULL, ".")));
                            strcpy(_ip[1], pch);
                        if((pch = strtok(NULL, ".")));
                            strcpy(_ip[2], pch);
                        if((pch = strtok(NULL, "\n")));
                            strcpy(_ip[3], pch);

                        //printf("%s %s %s %s %u\n", _ip[0], _ip[1], _ip[2], _ip[3], _cd);     

                        if( (!strcmp(_ip[0], ip[0]) || _ip[0][0] == '*') &&
                            (!strcmp(_ip[1], ip[1]) || _ip[1][0] == '*') &&
                            (!strcmp(_ip[2], ip[2]) || _ip[2][0] == '*') &&
                            (!strcmp(_ip[3], ip[3]) || _ip[3][0] == '*') && _cd == cd)
                        {
                            snprintf(match_rule, sizeof(match_rule), "%s %s %s", cmd, mode, target_ip);
                            
                            if(!strcmp(cmd, "permit"))
                                permit = true;
                            else if(!strcmp(cmd, "reject"))
                                permit = false;

                            break;
                        }
                    }
                    fclose(fp);
                }
                else
                    printf("Open firewall configure file failed.\n");

                if(permit)
                {
                    printf("SOCKS_REQUEST GRANTED ... ( %s )\n\n", match_rule);
                    
                    if(cd == CONNECT_COMMAND_CODE)
                        connectmode(srcfd, dst_ip, dst_port, buffer);
                    else if(cd == BIND_COMMAND_CODE)
                        bindmode(srcfd, buffer);
                }
                else
                {
                    printf("SOCKS_REQUEST REJECT ... ( %s )\n\n", match_rule);

                    buffer[0] = 0;
                    buffer[1] = SOCKS_REQUEST_REJECT;

                    write(srcfd, buffer, 8);
                }
            }

            //printf("Client has closed the connection.\n");
            close(srcfd);
            exit(0);
        }
            close(srcfd);
    }

    return 0;
}
