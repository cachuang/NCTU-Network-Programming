#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>

#define MAXLINE 15000
#define MAX_CLIENT_NUM 10
#define INDEX "index.html"

extern char **environ;

using namespace std;

struct HTTPClient
{
    string method, request_file, protocol;
    int sockfd;
};

int execute(int sockfd, char *pwd)
{
    int status;

    setenv("QUERY_STRING", "", 1);
    setenv("CONTENT_LENGTH", "foo", 1);
    setenv("REQUEST_METHOD", "GET", 1);
    setenv("SCRIPT_NAME", "/~chengan0703/hw3.cgi", 1);
    setenv("REMOTE_HOST", "nplinux1.cs.nctu.edu.tw", 1);
    setenv("REMOTE_ADDR", "140.113.168.191", 1);
    setenv("AUTH_TYPE", "foo", 1);
    setenv("REMOTE_USER", "foo", 1);
    setenv("REMOTE_IDENT", "foo", 1);

    if( fork() == 0 )
    {
        dup2(sockfd, 1); 
        dup2(sockfd, 2);
        
        if(execl("./printenv.cgi", "printenv.cgi", NULL) < 0)  
        {
            cout << "exec fail" << endl;
            exit(-1);
        }  
    }
    else 
        wait(&status); 

    close(sockfd); 
    // FD_CLR(sockfd, &allset);
    // client[i].sockfd = -1;
}

int main(int argc, char *argv[])
{
    int listenfd, connfd, sockfd, maxfd, maxi, nready, n;
    char buffer[MAXLINE];
    struct sockaddr_in server_addr;
    struct HTTPClient client[MAX_CLIENT_NUM];
    fd_set allset, rset;

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

    maxfd = listenfd;
    maxi = -1;
    for(int i = 0; i < MAX_CLIENT_NUM; i++)
        client[i].sockfd = -1;
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);

    setenv("CONTENT_LENGTH", "foo", 1);
    setenv("REQUEST_METHOD", "GET", 1);
    setenv("SCRIPT_NAME", "/~chengan0703/hw3.cgi", 1);
    setenv("REMOTE_HOST", "nplinux1.cs.nctu.edu.tw", 1);
    setenv("REMOTE_ADDR", "140.113.168.191", 1);
    setenv("AUTH_TYPE", "foo", 1);
    setenv("REMOTE_USER", "npdemo", 1);
    setenv("REMOTE_IDENT", "foo", 1);

    while(1)
    {
        int i;
        char *pch;

        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);

        if(FD_ISSET(listenfd, &rset))
        {
            connfd = accept(listenfd, (struct sockaddr *) NULL, NULL);
            FD_SET(connfd, &allset);

            for(i = 0; i < MAX_CLIENT_NUM; i++)
                if(client[i].sockfd < 0) {
                    client[i].sockfd = connfd;
                    break;
                }

            if(connfd > maxfd)
                maxfd = connfd;

            if(i > maxi)
                maxi = i;

            if(--nready <= 0)
                continue;
        }

        for(int i = 0; i <= maxi; i++)
        {
            if( (sockfd = client[i].sockfd) < 0 )
                continue;

            if(FD_ISSET(sockfd, &rset))
            {
                if( (n = read(sockfd, buffer, MAXLINE)) > 0) {

                    buffer[n] = 0;

                    // parse first line of http request only
                    if((pch = strtok(buffer, " ")));
                        client[i].method = string(pch);
                    if((pch = strtok(NULL, " ")));
                        client[i].request_file = string(pch).substr(1);   // strip "/"
                    if((pch = strtok(NULL, "\r\n")));
                        client[i].protocol = string(pch);

                    // hande GET method only
                    if(client[i].method != "GET")
                        continue;

                    // set default page if not specify
                    if(client[i].request_file == "")
                        client[i].request_file = INDEX;

                    // url contain "?"
                    if( (n = client[i].request_file.find("?")) != string::npos ) {
                        string query_string = client[i].request_file.substr(n + 1);
                        client[i].request_file =  client[i].request_file.substr(0, n);
                        setenv("QUERY_STRING", query_string.c_str(), 1);
                    }

                    string fileType =  client[i].request_file.substr(client[i].request_file.rfind(".") + 1);

                    cout << client[i].method << " " << client[i].request_file << " " << fileType << endl;

                    // request file exist
                    if( access(client[i].request_file.c_str(), F_OK ) != -1 ) {

                        char http200[] = "HTTP/1.1 200 OK\r\n";

                        write(sockfd, http200, strlen(http200));

                        if(fileType == "html") 
                        {
                            char contentType[] = "Content-Type:text/html\r\n\r\n";
                            char line[MAXLINE];
                            FILE *file = fopen(client[i].request_file.c_str(), "r");
                            
                            write(sockfd, contentType, strlen(contentType));

                            while(fgets(line, sizeof(line), file))
                                write(sockfd, line, strlen(line));
                        }
                        else if(fileType == "cgi")
                        {
                            char file_path[MAXLINE] = "";

                            strcat(file_path, "./");
                            strcat(file_path, client[i].request_file.c_str());
                            
                            if( fork() == 0 )
                            {
                                if( fork() == 0 )
                                {
                                    dup2(sockfd, 1); 
                                    dup2(sockfd, 2);
                                    
                                    if(execl(file_path, client[i].request_file.c_str(), NULL) < 0) 
                                        exit(-1);
                                }
                                else
                                    exit(0);
                            }
                            else 
                                wait(NULL); 
                        }

                        close(sockfd);
                        FD_CLR(sockfd, &allset);
                        client[i].sockfd = -1;
                    }
                    else
                    {
                        char http404[] = 
                        "HTTP/1.1 404 Not Found\r\nContent-Type:text/html\r\n\r\n"
                        "<head><title>404 - Not Found</title></head><body><h1>404 - Not Found</h1></body></html>";
                        
                        write(sockfd, http404, strlen(http404));

                        close(sockfd);
                        FD_CLR(sockfd, &allset);
                        client[i].sockfd = -1;
                    }
                }
                else 
                {
                    printf("Client has closed the connection.\n");
                    close(sockfd);
                    FD_CLR(sockfd, &allset);
                    client[i].sockfd = -1;
                }

                if(--nready <= 0)
                    break;
            }
        }
    }
}
