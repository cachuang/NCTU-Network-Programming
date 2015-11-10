#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <string>
#include "readline.h"

#define MAXLINE 15000
#define MAX_CMD_NUM 15000
#define MAX_CMD_LEN 300
#define MAX_CLIENT_NUM 100

using namespace std;

struct clientData
{
    clientData(int _id, int _sockfd, char *_ip, int _port, struct sockaddr_in _sockaddr, char *_name, vector < pair<int, int > > _pipeCounter):id(_id), sockfd(_sockfd), ip(_ip), port(_port), sockaddr(_sockaddr), name(_name), pipeCounter(_pipeCounter) {} 
    
    int id;
    int sockfd;
    char *ip;
    int port;
    struct sockaddr_in sockaddr;
    char *name;
    vector < pair<int, int > > pipeCounter;
};

void broadcast(const vector <struct clientData> &clients, char *message)
{
    for(int i = 0; i < clients.size(); i++)
        write(clients[i].sockfd, message, strlen(message));
}

int execute(char *command[], int readfd, int& stdout, int& stderr)
{
    int pipes[2], pipes2[2], status;
    
    pipe(pipes);
    pipe(pipes2);
    
    //printf("Open fd: %d %d\n", pipes[0], pipes[1]);

    if( fork() == 0 )
    {
        dup2(readfd, 0);
        dup2(pipes[1], 1); 
        dup2(pipes2[1], 2);
        close(readfd);
        close(pipes[0]);
        close(pipes[1]);
        close(pipes2[0]);
        close(pipes2[1]);
        
        if(execvp(command[0], command) < 0)    // exec fail
            exit(-1); 
    }
    else 
        wait(&status);
        
    close(pipes[1]);
    close(pipes2[1]);
           
    //printf("exec exit code: %d\n", WEXITSTATUS(status));
        
    if(status == 0)                           // exec succeed
    {
        stdout = pipes[0];
        stderr = pipes2[0];
        return 0; 
    }                 
    else if(WEXITSTATUS(status) == 255)       // exec fail
    {
        close(pipes[0]);
        close(pipes2[0]);
        return -1;
    } 
    else                                      // exec error
    { 
        stdout = pipes[0];
        stderr = pipes2[0];
        return 1;
    }               
}

int checkCounter(vector< pair<int, int> >& pipeCounter)
{
    char output[MAXLINE];
    int merge[2];
    bool newPipe = false;
    
    for(int i = 0; i < pipeCounter.size(); i++)
    {
        if(pipeCounter[i].first == 0)
        {
            if(!newPipe) {
                pipe(merge);
                newPipe = true;
            }
            int n = read(pipeCounter[i].second, output, sizeof(output));
            output[n] = 0;                    
            write(merge[1], output, strlen(output));
            close(pipeCounter[i].second);
            pipeCounter.erase(pipeCounter.begin() + i--);
        }
    }
        
    if(newPipe) {
        close(merge[1]); 
        return merge[0];
    }
    else 
        return -1;     
}

int main(int argc, char *argv[], char *envp[])
{
    int listenfd, connfd, maxfd, nready, clientID[MAX_CLIENT_NUM];
    struct sockaddr_in server_addr;
    fd_set allset, rset;
    vector <struct clientData> clients;
    
    memset(clientID, -1, sizeof(clientID));
    
    if(argc != 2) {
        printf("Usage: ./server <port>\n");
        exit(0);
    }
    
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(argv[1]));
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket Error");
        exit(errno);
    }

    if( bind(listenfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("Bind Error");
        exit(errno);
    }

    if( listen(listenfd, 20) < 0 ) {
        perror("Listen Error");
        exit(errno);
    }
    
    maxfd = listenfd;
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);

    while(1)
    {
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        
        // listen fd
        if(FD_ISSET(listenfd, &rset))    
        {
            int id, port;
            socklen_t len;
            struct sockaddr_in clientaddr;
            char ip[INET_ADDRSTRLEN];
            char welcome[] = 
            "****************************************\n"
            "** Welcome to the information server. **\n"
            "****************************************\n";
            char enter[100];
            char prompt[] = "% ";
            char name[] = "no name";
            vector < pair<int, int > > pipeCounter;

            len = sizeof(clientaddr);
            connfd = accept(listenfd, (struct sockaddr *) &clientaddr, &len);
            
            for(int i = 1; i <= MAX_CLIENT_NUM; i++)
                if(clientID[i] == -1) {
                    id = i;
                    clientID[i] = connfd;
                    break;
                }
                
            inet_ntop(AF_INET, &clientaddr.sin_addr, ip, sizeof(ip));
            port = ntohs(clientaddr.sin_port);

            clients.push_back(clientData(id, connfd, ip, port, clientaddr, name, pipeCounter));
               
            FD_SET(connfd, &allset);
            
            write(connfd, welcome, strlen(welcome));

            snprintf(enter, sizeof(enter), "*** User '(no name)' entered from %s/%d. ***\n%% "
            , inet_ntop(AF_INET, &clientaddr.sin_addr, ip, sizeof(ip))
            , ntohs(clientaddr.sin_port));
            
            broadcast(clients, enter);

            if(connfd > maxfd)
                maxfd = connfd;
            if(--nready <= 0)
                continue;
        }
        // check all clients
        for(int i = 0; i < clients.size(); i++)
        {
            if(FD_ISSET(clients[i].sockfd, &rset))
            {
                int n;
                char prompt[] = "% ";
                char buffer[MAXLINE];
                int connfd = clients[i].sockfd;
                
                if( (n = readline(connfd, buffer, MAXLINE)) > 0 )
                {
                    char *pch, *arg[MAX_CMD_NUM], *command[MAX_CMD_NUM], text[MAXLINE];
                    int fd, argN = 0, commandN = 0, readfd = 0, stdout = 1, stderr = 2;
                    
                    strcpy(text, buffer);
                        
                    // get first command
                    arg[0] = new char[MAX_CMD_LEN];          
                    if((pch = strtok(buffer, " \r\n")))
                        strcpy(arg[0], pch);
                        
                    // get arguments
                    while(pch != NULL) 
                    {
                        arg[++argN] = new char[MAX_CMD_LEN];
                        if((pch = strtok(NULL, " \r\n")))
                            strcpy(arg[argN], pch);
                        else
                            arg[argN++] = NULL;
                    }
                        
                    // receive exit
                    if(!strcmp(arg[0], "exit"))
                    {
                        char temp[MAXLINE];
                        snprintf(temp, sizeof(temp), "*** User '%s' left. ***\n%% ", clients[i].name);
                        clientID[clients[i].id] = -1;
                        close(clients[i].sockfd);
                        FD_CLR(clients[i].sockfd, &allset);
                        clients.erase(clients.begin() + i);
                        broadcast(clients, temp);
                         if(--nready <= 0)
                            break;
                        else
                            continue;
                    }
                        
                    // receive setenv    
                    else if(!strcmp(arg[0], "setenv"))    
                        setenv(arg[1], arg[2], 1);
                    
                    // receive printenv
                    else if(!strcmp(arg[0], "printenv"))    
                    {
                        if(argN == 3)         // print specific environment variable
                        {
                            char output[MAXLINE];
                            snprintf(output, sizeof(output), "%s=%s\n", arg[1], getenv(arg[1]));
                            write(clients[i].sockfd, output, strlen(output));
                        }
                        else if(argN == 2)    // print all environment variable
                        {
                            char output[MAXLINE] = "\0";
                            char temp[MAXLINE];
                            for (int i = 0; envp[i] != NULL; i++) {
                                snprintf(temp, sizeof(temp), "%s\n", envp[i]);
                                strcat(output, temp);
                            } 
                            write(clients[i].sockfd, output, strlen(output));
                        }    
                    }
                    // command "who" 
                    else if(!strcmp(arg[0], "who")) 
                    {
                        char online[MAXLINE] = "\0";
                        char temp[MAXLINE];
                        for(int j = 0; j < clients.size(); j++)
                        {
                            if(clients[i].sockfd == clients[j].sockfd)
                                snprintf(temp, sizeof(temp), "%d\t%s\t%s/%d\t<-me\n", clients[i].id, clients[i].name, clients[i].ip, clients[i].port);
                            else 
                                snprintf(temp, sizeof(temp), "%d\t%s\t%s/%d\t\n", clients[j].id, clients[j].name, clients[j].ip, clients[j].port);
                            
                            strcat(online, temp);
                        }
                        write(clients[i].sockfd, online, strlen(online));
                    }
                    // command "name"
                    else if(!strcmp(arg[0], "name"))
                    {
                        bool unique = true;
                        char ip[INET_ADDRSTRLEN];
                        
                        for(int j = 0; j < clients.size(); j++)
                        {
                            if(arg[1] != NULL && !strcmp(clients[j].name, arg[1])) {
                                unique = false;
                                break;
                            }
                        }
                        
                        if(!unique)
                        {
                            char error[MAXLINE];
                            snprintf(error, sizeof(error), "%% *** User '%s' already exists. ***\n", arg[1]);
                            write(clients[i].sockfd, error, strlen(error));
                        }
                        else if(arg[1] != NULL)    // name no empty
                        {
                            char temp[MAXLINE];
                            snprintf(temp, sizeof(temp), "*** User from %s/%d is named '%s'. ***\n%% ", clients[i].ip, clients[i].port, arg[1]);
                            broadcast(clients, temp);
                            clients[i].name = arg[1];
                        }
                    }
                    // command "tell <id> <message>"
                    else if(!strcmp(arg[0], "tell")) 
                    {
                        char message[MAXLINE];
                        char temp[MAXLINE];
                        int id = atoi(arg[1]);
                        
                        if(clientID[id] == -1)
                        {
                            snprintf(temp, sizeof(temp), "*** Error: user #%d does not exist yet. ***\n", id);
                            write(clients[i].sockfd, temp, strlen(temp));
                        }
                        else 
                        {
                            if((pch = strtok(text, " ")))    // tell
                                if((pch = strtok(NULL, " ")))    // id
                                    if((pch = strtok(NULL, "\r\n")))     // message
                                        strcpy(message, pch);
                            snprintf(temp, sizeof(temp), "*** %s told you ***: %s\n%% ", clients[i].name, message);
                            write(clientID[id], temp, strlen(temp));
                        }
                    }
                    // command "yell <message>"
                    else if(!strcmp(arg[0], "yell")) 
                    {
                        char message[MAXLINE];
                        char temp[MAXLINE];
                        int id = atoi(arg[1]);
                        
                        if((pch = strtok(text, " ")))    // yell
                            if((pch = strtok(NULL, "\r\n")))    // message
                                strcpy(message, pch);
                        snprintf(temp, sizeof(temp), "*** %s yelled ***: %s\n%% ", clients[i].name, message);
                        broadcast(clients, temp);
                    }
   
                    else 
                    for(int j = 0; j < argN; j++)
                    {
                        if(arg[j] == NULL)    // last command
                        {
                            char output[MAXLINE];

                            command[commandN] = NULL;
                            commandN = 0;
                            
                            // check numbered-pipe
                            // if there's no zero then return -1, else return new readfd
                            if( (fd = checkCounter(clients[i].pipeCounter)) != -1 )
                                readfd = fd;
                            
                            // read from readfd, stdout and stderr will be set when return
                            // return 0 if succeed, return 1 if error, return -1 if fail   
                            if( execute(command, readfd, stdout, stderr) < 0 ) 
                            {
                                char error[MAXLINE];                        
                                snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                                write(connfd, error, strlen(error));  
                                
                                for(int i = 0; i < clients[i].pipeCounter.size(); i++)
                                    clients[i].pipeCounter[i].first += 1;
                                
                                // ls |1
                                // ctt
                                // cat      
                                if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                    clients[i].pipeCounter.push_back(make_pair(1, readfd));
                                else if(readfd != 0) 
                                    close(readfd);
                                    
                                for(int k = 0; command[k] != NULL; k++)
                                    delete [] command[k];

                                break;
                            }
                            
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];
                            
                            if(readfd != 0)    
                                close(readfd);
                            
                            readfd = stdout;

                            n = read(stderr, output, sizeof(output));
                            output[n] = 0;                    
                            write(connfd, output, strlen(output));
                            n = read(stdout, output, sizeof(output));
                            output[n] = 0;                    
                            write(connfd, output, strlen(output));
                            
                            close(stderr);
                            close(stdout);
                        }
                        else if(!strcmp(arg[j], ">"))    // redirect stdout to file
                        {
                            char output[MAXLINE];
     
                            command[commandN] = NULL;
                            commandN = 0;
                            
                            if( (fd = checkCounter(clients[i].pipeCounter)) != -1 )
                                readfd = fd;
                            
                            if( execute(command, readfd, stdout, stderr) < 0 ) 
                            {
                                char error[MAXLINE];
                                snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                                write(connfd, error, strlen(error)); 
                                
                                for(int i = 0; i < clients[i].pipeCounter.size(); i++)
                                    clients[i].pipeCounter[i].first += 1;
                                
                                if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                    clients[i].pipeCounter.push_back(make_pair(1, readfd));
                                else if(readfd != 0) 
                                    close(readfd);
                                    
                                for(int k = 0; command[k] != NULL; k++)
                                    delete [] command[k];     
                                                               
                                break;   
                            }
                            
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];
                            
                            if(readfd != 0)    
                                close(readfd);
                            
                            FILE *fp = fopen(arg[j+1], "w");
                            
                            if(fp) {           
                                n = read(stdout, output, sizeof(output));
                                output[n] = 0;                    
                                write(fileno(fp), output, strlen(output));
                                fclose(fp);
                            }
                            else {
                                char error[MAXLINE];
                                snprintf(error, sizeof(error), "Invalid file name: [%s].\n", arg[j+1]);
                                write(connfd, error, strlen(error));
                            }
                            
                            n = read(stderr, output, sizeof(output));
                            output[n] = 0;                    
                            write(connfd, output, strlen(output));

                            close(stdout);
                            close(stderr);
                            
                            break;
                        }
                        else if(!strcmp(arg[j], "|"))    // pipe stdout to next command 
                        { 
                            char output[MAXLINE];
                            
                            command[commandN] = NULL;
                            commandN = 0;
                            
                            if( (fd = checkCounter(clients[i].pipeCounter)) != -1 )
                                readfd = fd;
                               
                            if( execute(command, readfd, stdout, stderr) < 0 ) 
                            {
                                char error[MAXLINE];
                                snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                                write(connfd, error, strlen(error)); 
                                
                                for(int i = 0; i < clients[i].pipeCounter.size(); i++)
                                    clients[i].pipeCounter[i].first += 1;
                                    
                                if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                    clients[i].pipeCounter.push_back(make_pair(1, readfd));
                                else if(readfd != 0) 
                                    close(readfd);
                                    
                                for(int k = 0; command[k] != NULL; k++)
                                    delete [] command[k];
                                
                                break;
                            }
                            
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];
                            
                            if(readfd != 0)    
                                close(readfd);
                            
                            readfd = stdout; 
                            
                            n = read(stderr, output, sizeof(output));
                            output[n] = 0;                    
                            write(connfd, output, strlen(output));
                            
                            close(stderr);
                        }
                        else if(arg[j][0] == '|')    // pipe stdout to next N line command 
                        {
                            char output[MAXLINE];
                            int number = 0;
                            
                            command[commandN] = NULL;
                            commandN = 0;
                            
                            if( (fd = checkCounter(clients[i].pipeCounter)) != -1 )
                                readfd = fd;

                            if( execute(command, readfd, stdout, stderr) < 0 ) 
                            {
                                char error[MAXLINE];
                                snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                                write(connfd, error, strlen(error)); 
                                
                                for(int i = 0; i < clients[i].pipeCounter.size(); i++)
                                    clients[i].pipeCounter[i].first += 1;
                                    
                                if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                    clients[i].pipeCounter.push_back(make_pair(1, readfd));
                                else if(readfd != 0) 
                                    close(readfd);
                                    
                                for(int k = 0; command[k] != NULL; k++)
                                    delete [] command[k];
                                    
                                break;
                            }
                            
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];
                            
                            if(readfd != 0)    
                                close(readfd);
                            
                            if((pch = strtok(arg[j], "|")))
                                if( (number = atoi(pch)) > 0 )
                                    clients[i].pipeCounter.push_back(make_pair(number, stdout));
                            
                            if(arg[j+1] != NULL && arg[j+1][0] == '!') {
                                if((pch = strtok(arg[j+1], "!")))
                                    if( (number = atoi(pch)) > 0 )
                                        clients[i].pipeCounter.push_back(make_pair(number, stderr)); 
                            }
                            else {
                                n = read(stderr, output, sizeof(output));
                                output[n] = 0;                    
                                write(connfd, output, strlen(output));      
                                close(stderr);
                            }
                                 
                            break;          
                        }
                        else if(arg[j][0] == '!')    // pipe stderr to next N line command 
                        {
                            char output[MAXLINE];
                            int number = 0;
                            
                            command[commandN] = NULL;
                            commandN = 0;
                            
                            if( (fd = checkCounter(clients[i].pipeCounter)) != -1)
                                readfd = fd;

                            if( execute(command, readfd, stdout, stderr) < 0 ) 
                            {
                                char error[MAXLINE];
                                snprintf(error, sizeof(error), "Unknown command: [%s].\n", command[0]);
                                write(connfd, error, strlen(error));  
                                
                                for(int i = 0; i < clients[i].pipeCounter.size(); i++)
                                    clients[i].pipeCounter[i].first += 1;
                                    
                                if(command[0] != NULL && !strcmp(arg[0], command[0]) && fd != -1) 
                                    clients[i].pipeCounter.push_back(make_pair(1, readfd));
                                else if(readfd != 0) 
                                    close(readfd);
                                
                                for(int k = 0; command[k] != NULL; k++)
                                    delete [] command[k];       
                                                             
                                break;
                            }
                            
                            for(int k = 0; command[k] != NULL; k++)
                                delete [] command[k];    
                                                    
                            if(readfd != 0)    
                                close(readfd);
                            
                            if((pch = strtok(arg[j], "!")))
                                if( (number = atoi(pch)) > 0 )
                                    clients[i].pipeCounter.push_back(make_pair(number, stderr));
                            
                            if(arg[j+1] != NULL && arg[j+1][0] == '|') {
                                if((pch = strtok(arg[j+1], "|"))) 
                                    if( (number = atoi(pch)) > 0 )
                                        clients[i].pipeCounter.push_back(make_pair(number, stdout));
                            }
                            else {    
                                n = read(stdout, output, sizeof(output));
                                output[n] = 0;                    
                                write(connfd, output, strlen(output)); 
                                close(stdout);
                            }
                                 
                            break;          
                        }
                        else 
                        {
                            command[commandN] = new char[MAX_CMD_LEN];
                            strcpy(command[commandN++], arg[j]);
                        }
                    }
                    
                    for(int j = 0; j < clients[i].pipeCounter.size(); j++)
                        clients[i].pipeCounter[j].first -= 1;
                        
                    for(int i = 0; i < argN; i++)
                        delete [] arg[i];
                        
                    write(connfd, prompt, strlen(prompt)); 
                }
                // client close connection
                else
                {
                    char temp[MAXLINE];
                    snprintf(temp, sizeof(temp), "*** User '%s' left. ***\n%% ", clients[i].name);
                    clientID[clients[i].id] = -1;
                    close(clients[i].sockfd);
                    FD_CLR(clients[i].sockfd, &allset);
                    clients.erase(clients.begin() + i);
                    broadcast(clients, temp);
                }
                if(--nready <= 0)
                    break;
            }
        }
    }
    
    return 0;
}

